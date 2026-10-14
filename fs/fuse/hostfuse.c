// SPDX-License-Identifier: GPL-2.0
/*
 * hostfuse: filesystems served by the host (lkl_ops->fuse_*), as virtio-fs's
 * are by virtiofsd, without virtio: each FUSE request goes to the host as a
 * list of its pieces (header, arguments, then any data, in the page cache's
 * own pages), and the host writes its reply into such a list. No buffer of
 * the request's size: without an MMU that would be contiguous, which a
 * megabyte's write may not find. mount -t hostfuse TAG DIR mounts the host's
 * filesystem TAG.
 *
 * Requests may be sent where they can't be ended (see virtio_fs.c), so a
 * worker sends them. The host answers synchronously, as for hostblk.
 */
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <asm/host_ops.h>
#include "fuse_i.h"

struct hostfuse {
	int conn;			/* the host's handle */
	spinlock_t lock;
	struct list_head queue;		/* requests to send, in order */
	struct work_struct work;
};

/* seg: len bytes of a request's pages, a piece each; returns how many */
static int hostfuse_page_segs(struct fuse_args_pages *ap,
			      struct lkl_fuse_seg *seg, unsigned int len)
{
	unsigned int i, n;
	int segs = 0;

	for (i = 0; i < ap->num_pages && len; i++) {
		n = min(ap->descs[i].length, len);
		seg[segs].base = page_address(ap->pages[i]) + ap->descs[i].offset;
		seg[segs++].len = n;
		len -= n;
	}
	return segs;
}

/*
 * The reply's arguments, len bytes, already in place: their sizes (as
 * fuse_dev_do_write sets them), and a short read's pages zeroed past it
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

			for (j = 0; j < ap->num_pages; j++) {
				unsigned int dl = ap->descs[j].length;

				if (rest < dl)
					zero_user_segment(ap->pages[j],
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

static void hostfuse_send(struct hostfuse *hf, struct fuse_req *req)
{
	struct fuse_args *args = req->args;
	struct fuse_args_pages *ap = container_of(args, typeof(*ap), args);
	bool reply = test_bit(FR_ISREPLY, &req->flags);
	/* (a header and the arguments each way, and the pages) */
	unsigned int i, max = 2 + args->in_numargs + args->out_numargs +
		(args->in_pages || args->out_pages ? ap->num_pages : 0);
	struct lkl_fuse_seg *in, *out;
	int in_n = 0, out_n = 0;
	long ret;

	in = kmalloc_array(2 * max, sizeof(*in), GFP_NOFS);
	if (!in) {
		req->out.h.error = -ENOMEM;
		goto end;
	}
	out = in + max;

	req->in.h.len = sizeof(req->in.h) +
		fuse_len_args(args->in_numargs, (struct fuse_arg *)args->in_args);
	in[in_n++] = (struct lkl_fuse_seg){ &req->in.h, sizeof(req->in.h) };
	for (i = 0; i < args->in_numargs; i++) {
		unsigned int n = args->in_args[i].size;

		if (i == args->in_numargs - 1 && args->in_pages)
			in_n += hostfuse_page_segs(ap, in + in_n, n);
		else
			in[in_n++] = (struct lkl_fuse_seg){
				(void *)args->in_args[i].value, n };
	}
	if (reply) {
		out[out_n++] = (struct lkl_fuse_seg){ &req->out.h,
						      sizeof(req->out.h) };
		for (i = 0; i < args->out_numargs; i++) {
			unsigned int n = args->out_args[i].size;

			if (i == args->out_numargs - 1 && args->out_pages)
				out_n += hostfuse_page_segs(ap, out + out_n, n);
			else
				out[out_n++] = (struct lkl_fuse_seg){
					args->out_args[i].value, n };
		}
	}

	ret = lkl_ops->fuse_request(hf->conn, in, in_n, out, out_n);
	kfree(in);
	if (!reply)
		goto end;
	if (ret < (long)sizeof(req->out.h)) {
		req->out.h.error = ret < 0 ? ret : -EIO;
		goto end;
	}
	ret = hostfuse_reply(req, ret - sizeof(req->out.h));
	if (ret)
		req->out.h.error = ret;
end:
	fuse_request_end(req);
}

static void hostfuse_work(struct work_struct *work)
{
	struct hostfuse *hf = container_of(work, struct hostfuse, work);
	struct fuse_req *req;

	for (;;) {
		spin_lock(&hf->lock);
		req = list_first_entry_or_null(&hf->queue, struct fuse_req, list);
		if (req)
			list_del_init(&req->list);
		spin_unlock(&hf->lock);
		if (!req)
			return;
		hostfuse_send(hf, req);
	}
}

static void hostfuse_send_req(struct fuse_iqueue *fiq, struct fuse_req *req)
{
	struct hostfuse *hf = fiq->priv;

	if (req->in.h.opcode != FUSE_NOTIFY_REPLY)
		req->in.h.unique = fuse_get_unique(fiq);
	clear_bit(FR_PENDING, &req->flags);
	spin_lock(&hf->lock);
	list_add_tail(&req->list, &hf->queue);
	spin_unlock(&hf->lock);
	schedule_work(&hf->work);
}

/* no reply, so straight to the host */
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

	struct lkl_fuse_seg seg = { &req, sizeof(req) };

	lkl_ops->fuse_request(hf->conn, &seg, 1, NULL, 0);
	kfree(link);
}

/* the host answers each request in one go: nothing to interrupt */
static void hostfuse_send_interrupt(struct fuse_iqueue *fiq,
				    struct fuse_req *req)
{
}

static void hostfuse_release(struct fuse_iqueue *fiq)
{
	struct hostfuse *hf = fiq->priv;

	flush_work(&hf->work);
	if (lkl_ops->fuse_disconnect)
		lkl_ops->fuse_disconnect(hf->conn);
	kfree(hf);
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

	/* as virtiofs's: the host checks access (as virtiofsd does) */
	ctx->rootmode = S_IFDIR;
	ctx->default_permissions = 1;
	ctx->allow_other = 1;
	ctx->max_read = UINT_MAX;
	ctx->blksize = 4096;
	ctx->destroy = true;
	ctx->no_control = true;
	ctx->no_force_umount = true;
	ctx->fudptr = NULL;

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

static int hostfuse_get_tree(struct fs_context *fsc)
{
	struct hostfuse *hf;
	struct fuse_conn *fc;
	struct fuse_mount *fm;
	struct super_block *sb;
	int conn, err;

	if (!fsc->source)
		return invalf(fsc, "hostfuse: no filesystem named");
	conn = lkl_ops->fuse_connect(fsc->source);
	if (conn < 0)
		return conn;

	hf = kzalloc(sizeof(*hf), GFP_KERNEL);
	fc = kzalloc(sizeof(*fc), GFP_KERNEL);
	fm = kzalloc(sizeof(*fm), GFP_KERNEL);
	if (!hf || !fc || !fm) {
		kfree(hf);
		kfree(fc);
		kfree(fm);
		if (lkl_ops->fuse_disconnect)
			lkl_ops->fuse_disconnect(conn);
		return -ENOMEM;
	}
	hf->conn = conn;
	spin_lock_init(&hf->lock);
	INIT_LIST_HEAD(&hf->queue);
	INIT_WORK(&hf->work, hostfuse_work);

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
	if (!lkl_ops->fuse_connect || !lkl_ops->fuse_request)
		return 0;
	return register_filesystem(&hostfuse_type);
}
module_init(hostfuse_init);
