// SPDX-License-Identifier: GPL-2.0
/*
 * hostfuse (CONFIG_EXOBOX_HOSTFUSE): filesystems served by the host with the
 * FUSE protocol, as virtio-fs's are by virtiofsd, without virtio: each
 * request goes to the host as a list of its pieces (header, arguments, then
 * any data, in the page cache's own folios), and the host writes its reply
 * into such a list. Nothing is copied into a buffer of the request's size,
 * and there are no virtqueues. mount -t hostfuse TAG DIR mounts the host's
 * filesystem TAG.
 *
 * Asynchronous, as virtio_fs.c: a request is submitted to the host, which
 * returns at once and works meanwhile (its filesystem is JavaScript's
 * promises), so the kernel runs other tasks. As requests complete the host
 * raises the connection's interrupt, whose work item ends them.
 */
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <asm/irq.h>
#include "fuse_i.h"

#define hostfuse_import(name) \
	__attribute__((import_module("kernel"), import_name(#name))) \
	wasm_kernel_##name

/* a piece of a request or its reply, in the kernel's memory */
struct hostfuse_seg {
	void *base;
	u32 len;
};

/*
 * A connection to the host's filesystem named tag (tag_len bytes): a handle
 * >= 0, or a negative errno (-ENOENT: no such filesystem). The host raises
 * irq (a hardware irq number) when requests have completed.
 */
int hostfuse_import(fuse_connect)(const char *tag, u32 tag_len, u32 irq);
/*
 * A request on conn: the in_n pieces at in, in order, its header first (all
 * read before it returns). If out_n, the request has a reply, written across
 * the out_n pieces at out, in order (its header first; at most their total),
 * which stay the kernel's until the request completes (fuse_complete: id).
 * Returns at once: 0, or a negative errno if it can't be handled.
 */
int hostfuse_import(fuse_submit)(int conn, void *id,
				 const struct hostfuse_seg *in, u32 in_n,
				 const struct hostfuse_seg *out, u32 out_n);
/*
 * A request of conn's that has completed since: its id, and its reply's
 * length or a negative errno; returns 1, or 0 if none has.
 */
int hostfuse_import(fuse_complete)(int conn, void **id, int *ret);
/* the connection's filesystem was unmounted: every request has completed */
void hostfuse_import(fuse_disconnect)(int conn);

struct hostfuse {
	int conn;			/* the host's handle */
	int irq;			/* raised as requests complete */
	irq_hw_number_t hwirq;
	spinlock_t lock;
	struct list_head ending;	/* requests with no reply to wait for */
	struct work_struct done;	/* ends those completed, and ending */
};

/* seg: len bytes of a request's folios, a piece each; returns how many */
static int hostfuse_folio_segs(struct fuse_args_pages *ap,
			       struct hostfuse_seg *seg, unsigned int len)
{
	unsigned int i, n;
	int segs = 0;

	for (i = 0; i < ap->num_folios && len; i++) {
		n = min(ap->descs[i].length, len);
		seg[segs].base = folio_address(ap->folios[i]) + ap->descs[i].offset;
		seg[segs++].len = n;
		len -= n;
	}
	return segs;
}

/*
 * The reply's arguments, len bytes, already in place: their sizes (as
 * fuse_dev_do_write sets them), and a short read's folios zeroed past it
 */
static int hostfuse_reply(struct fuse_req *req, unsigned int len)
{
	struct fuse_args *args = req->args;
	struct fuse_args_pages *ap = container_of(args, typeof(*ap), args);
	unsigned int i, n, last = args->out_numargs - 1;

	if (req->out.h.error || !args->out_numargs)
		return 0;
	for (i = 0; i < args->out_numargs; i++) {
		n = args->out_args[i].size;
		if (i == last && args->out_argvar)
			n = min(n, len);
		else if (n > len)
			return -EIO;
		if (i == last && args->out_pages && args->page_zeroing) {
			unsigned int j, rest = n;

			for (j = 0; j < ap->num_folios; j++) {
				unsigned int dl = ap->descs[j].length;

				if (rest < dl)
					folio_zero_segment(ap->folios[j],
						ap->descs[j].offset + rest,
						ap->descs[j].offset + dl);
				rest -= min(rest, dl);
			}
		}
		args->out_args[i].size = n;
		len -= n;
	}
	return 0;
}

/* the pieces most requests need: on the stack, else allocated */
#define HOSTFUSE_SEGS 8

static int hostfuse_submit(struct hostfuse *hf, struct fuse_req *req)
{
	struct fuse_args *args = req->args;
	struct fuse_args_pages *ap = container_of(args, typeof(*ap), args);
	bool reply = test_bit(FR_ISREPLY, &req->flags);
	/* (a header and the arguments each way, and the folios) */
	unsigned int i, max = 2 + args->in_numargs + args->out_numargs +
		(args->in_pages || args->out_pages ? ap->num_folios : 0);
	struct hostfuse_seg segs[2 * HOSTFUSE_SEGS], *in = segs, *out;
	int in_n = 0, out_n = 0, ret;

	if (max > HOSTFUSE_SEGS) {
		in = kmalloc_array(2 * max, sizeof(*in), GFP_ATOMIC);
		if (!in)
			return -ENOMEM;
		out = in + max;
	} else
		out = in + HOSTFUSE_SEGS;

	req->in.h.len = sizeof(req->in.h) +
		fuse_len_args(args->in_numargs, (struct fuse_arg *)args->in_args);
	in[in_n++] = (struct hostfuse_seg){ &req->in.h, sizeof(req->in.h) };
	for (i = 0; i < args->in_numargs; i++) {
		unsigned int n = args->in_args[i].size;

		if (i == args->in_numargs - 1 && args->in_pages)
			in_n += hostfuse_folio_segs(ap, in + in_n, n);
		else
			in[in_n++] = (struct hostfuse_seg){
				(void *)args->in_args[i].value, n };
	}
	if (reply) {
		out[out_n++] = (struct hostfuse_seg){ &req->out.h,
						      sizeof(req->out.h) };
		for (i = 0; i < args->out_numargs; i++) {
			unsigned int n = args->out_args[i].size;

			if (i == args->out_numargs - 1 && args->out_pages)
				out_n += hostfuse_folio_segs(ap, out + out_n, n);
			else
				out[out_n++] = (struct hostfuse_seg){
					args->out_args[i].value, n };
		}
	}
	ret = wasm_kernel_fuse_submit(hf->conn, reply ? req : NULL,
				      in, in_n, out, out_n);
	if (in != segs)
		kfree(in);
	return ret;
}

/* the host's reply to req, ret bytes or a negative errno, in place */
static void hostfuse_end(struct fuse_req *req, int ret)
{
	if (ret < (int)sizeof(req->out.h))
		req->out.h.error = ret < 0 ? ret : -EIO;
	else {
		ret = hostfuse_reply(req, ret - sizeof(req->out.h));
		if (ret)
			req->out.h.error = ret;
	}
	fuse_request_end(req);
}

static void hostfuse_done(struct work_struct *work)
{
	struct hostfuse *hf = container_of(work, struct hostfuse, done);
	struct fuse_req *req;
	void *id;
	int ret;

	while (wasm_kernel_fuse_complete(hf->conn, &id, &ret) > 0)
		hostfuse_end(id, ret);
	for (;;) {
		spin_lock(&hf->lock);
		req = list_first_entry_or_null(&hf->ending, struct fuse_req, list);
		if (req)
			list_del_init(&req->list);
		spin_unlock(&hf->lock);
		if (!req)
			return;
		fuse_request_end(req);
	}
}

static irqreturn_t hostfuse_interrupt(int irq, void *dev_id)
{
	struct hostfuse *hf = dev_id;

	schedule_work(&hf->done);
	return IRQ_HANDLED;
}

static void hostfuse_send_req(struct fuse_iqueue *fiq, struct fuse_req *req)
{
	struct hostfuse *hf = fiq->priv;
	int ret;

	fuse_request_assign_unique(fiq, req);
	clear_bit(FR_PENDING, &req->flags);
	ret = hostfuse_submit(hf, req);
	if (!ret && test_bit(FR_ISREPLY, &req->flags))
		return;		/* (hostfuse_done ends it) */
	/*
	 * Refused, or no reply wanted: it can't be ended here, where locks
	 * may be held, so the worker ends it
	 */
	req->out.h.error = ret;
	spin_lock(&hf->lock);
	list_add_tail(&req->list, &hf->ending);
	spin_unlock(&hf->lock);
	schedule_work(&hf->done);
}

/* no reply, so the host takes it and that's all */
static void hostfuse_send_forget(struct fuse_iqueue *fiq,
				 struct fuse_forget_link *link)
{
	struct hostfuse *hf = fiq->priv;
	struct {
		struct fuse_in_header h;
		struct fuse_forget_in arg;
	} req = {
		.h = {
			.opcode = FUSE_FORGET,
			.nodeid = link->forget_one.nodeid,
			.unique = fuse_get_unique(fiq),
			.len = sizeof(req),
		},
		.arg = { .nlookup = link->forget_one.nlookup },
	};
	struct hostfuse_seg seg = { &req, sizeof(req) };

	wasm_kernel_fuse_submit(hf->conn, NULL, &seg, 1, NULL, 0);
	kfree(link);
}

/* the host can't stop a request it's working on: nothing to interrupt */
static void hostfuse_send_interrupt(struct fuse_iqueue *fiq,
				    struct fuse_req *req)
{
}

static void hostfuse_free(struct hostfuse *hf)
{
	free_irq(hf->irq, hf);
	irq_dispose_mapping(hf->irq);
	wasm_free_irq(hf->hwirq);
	kfree(hf);
}

static void hostfuse_release(struct fuse_iqueue *fiq)
{
	struct hostfuse *hf = fiq->priv;

	/* (every request has been answered: DESTROY's reply came last) */
	flush_work(&hf->done);
	wasm_kernel_fuse_disconnect(hf->conn);
	hostfuse_free(hf);
}

static const struct fuse_iqueue_ops hostfuse_fiq_ops = {
	.send_forget	= hostfuse_send_forget,
	.send_interrupt	= hostfuse_send_interrupt,
	.send_req	= hostfuse_send_req,
	.release	= hostfuse_release,
};

static int hostfuse_fill_super(struct super_block *sb, struct fs_context *fsc)
{
	struct fuse_mount *fm = get_fuse_mount_super(sb);
	struct fuse_fs_context *ctx = fsc->fs_private;
	int err;

	/* as virtiofs's: the kernel checks permissions */
	ctx->rootmode = S_IFDIR;
	ctx->default_permissions = 1;
	ctx->allow_other = 1;
	ctx->max_read = UINT_MAX;
	ctx->blksize = 4096;
	ctx->destroy = true;
	ctx->no_control = true;
	ctx->no_force_umount = true;

	err = fuse_fill_super_common(sb, ctx);
	if (err)
		return err;
	fuse_send_init(fm);
	return 0;
}

static void hostfuse_kill_sb(struct super_block *sb)
{
	struct fuse_mount *fm = get_fuse_mount_super(sb);

	/* (a failed mount may have no connection yet) */
	if (sb->s_root && fuse_mount_remove(fm))
		fuse_conn_destroy(fm);
	kill_anon_super(sb);
	fuse_mount_destroy(fm);
}

static struct hostfuse *hostfuse_connect(const char *tag)
{
	struct hostfuse *hf = kzalloc(sizeof(*hf), GFP_KERNEL);
	int err;

	if (!hf)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&hf->lock);
	INIT_LIST_HEAD(&hf->ending);
	INIT_WORK(&hf->done, hostfuse_done);
	hf->irq = wasm_alloc_irq();
	if (hf->irq < 0) {
		err = hf->irq;
		kfree(hf);
		return ERR_PTR(err);
	}
	hf->hwirq = irqd_to_hwirq(irq_get_irq_data(hf->irq));
	err = request_irq(hf->irq, hostfuse_interrupt, 0, "hostfuse", hf);
	if (err) {
		irq_dispose_mapping(hf->irq);
		wasm_free_irq(hf->hwirq);
		kfree(hf);
		return ERR_PTR(err);
	}
	hf->conn = wasm_kernel_fuse_connect(tag, strlen(tag), hf->hwirq);
	if (hf->conn < 0) {
		err = hf->conn;
		hostfuse_free(hf);
		return ERR_PTR(err);
	}
	return hf;
}

static int hostfuse_get_tree(struct fs_context *fsc)
{
	struct hostfuse *hf;
	struct fuse_conn *fc;
	struct fuse_mount *fm;
	struct super_block *sb;
	int err;

	if (!fsc->source)
		return invalf(fsc, "hostfuse: no filesystem named");

	fc = kzalloc(sizeof(*fc), GFP_KERNEL);
	fm = kzalloc(sizeof(*fm), GFP_KERNEL);
	err = -ENOMEM;
	if (!fc || !fm)
		goto out_free;
	hf = hostfuse_connect(fsc->source);
	err = PTR_ERR(hf);
	if (IS_ERR(hf))
		goto out_free;

	fuse_conn_init(fc, fm, fsc->user_ns, &hostfuse_fiq_ops, hf);
	fc->release = fuse_free_conn;
	fc->delete_stale = true;
	fc->auto_submounts = true;
	fc->sync_fs = true;

	/* a superblock per mount: each is its own connection */
	fsc->s_fs_info = fm;
	sb = sget_fc(fsc, NULL, set_anon_super_fc);
	if (fsc->s_fs_info)
		fuse_mount_destroy(fm);
	if (IS_ERR(sb))
		return PTR_ERR(sb);

	err = hostfuse_fill_super(sb, fsc);
	if (err) {
		deactivate_locked_super(sb);
		return err;
	}
	sb->s_flags |= SB_ACTIVE;
	fsc->root = dget(sb->s_root);
	return 0;

out_free:
	kfree(fc);
	kfree(fm);
	return err;
}

static void hostfuse_free_fsc(struct fs_context *fsc)
{
	kfree(fsc->fs_private);
}

static const struct fs_context_operations hostfuse_context_ops = {
	.free		= hostfuse_free_fsc,
	.get_tree	= hostfuse_get_tree,
};

static int hostfuse_init_fs_context(struct fs_context *fsc)
{
	struct fuse_fs_context *ctx;

	if (fsc->purpose == FS_CONTEXT_FOR_SUBMOUNT)
		return fuse_init_fs_context_submount(fsc);
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	fsc->fs_private = ctx;
	fsc->ops = &hostfuse_context_ops;
	return 0;
}

static struct file_system_type hostfuse_type = {
	.owner		= THIS_MODULE,
	.name		= "hostfuse",
	.init_fs_context = hostfuse_init_fs_context,
	.kill_sb	= hostfuse_kill_sb,
};

static int __init hostfuse_init(void)
{
	return register_filesystem(&hostfuse_type);
}
module_init(hostfuse_init);
