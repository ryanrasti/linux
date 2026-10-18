// SPDX-License-Identifier: GPL-2.0
/*
 * Block devices backed by host operations (lkl_ops->blk_*).
 *
 * A blk-mq driver whose hardware is the host: queue_rq submits a request
 * (blk_submit: its data's pieces, which the host reads or fills in place)
 * and returns; the host works on it meanwhile, as long as that takes (a
 * disk over the network, say), and raises the interrupt as requests
 * complete, whose work ends them (blk_complete). The machine runs on in the
 * meantime: only what waits for the request waits. A request the host could
 * do at once (a disk in its memory) it has done by then: ended here.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <asm/host_ops.h>

/* a request's data: at most this many pieces (queue_limits.max_segments) */
#define HOSTBLK_SEGS	128
#define HOSTBLK_DEPTH	64

struct hostblk {
	int id;
	int irq;			/* raised as requests complete */
	struct blk_mq_tag_set set;
	struct gendisk *disk;
	struct work_struct done;	/* ends those completed */
};

/* each request's: its data's pieces, for the host */
struct hostblk_cmd {
	struct lkl_blk_seg seg[HOSTBLK_SEGS];
};

static int hostblk_major;

static blk_status_t hostblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct hostblk *hb = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct hostblk_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct req_iterator iter;
	struct bio_vec bv;
	int op, n = 0, err;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		op = LKL_BLK_READ;
		break;
	case REQ_OP_WRITE:
		op = LKL_BLK_WRITE;
		break;
	case REQ_OP_FLUSH:
		op = LKL_BLK_FLUSH;
		break;
	default:
		return BLK_STS_NOTSUPP;
	}
	if (rq->cmd_flags & REQ_FUA)
		op |= LKL_BLK_FUA;
	/* (contiguous pieces as one: at most the request's segments) */
	rq_for_each_bvec(bv, rq, iter) {
		void *base = bvec_virt(&bv);

		if (n && cmd->seg[n - 1].base + cmd->seg[n - 1].len == base) {
			cmd->seg[n - 1].len += bv.bv_len;
			continue;
		}
		if (WARN_ON_ONCE(n == HOSTBLK_SEGS))
			return BLK_STS_IOERR;
		cmd->seg[n].base = base;
		cmd->seg[n++].len = bv.bv_len;
	}
	blk_mq_start_request(rq);
	err = lkl_ops->blk_submit(hb->id, rq->tag, op, blk_rq_pos(rq), cmd->seg, n);
	if (err < 0)
		return errno_to_blk_status(err);
	/* (done already: a disk in the host's memory) */
	if (err)
		blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

/* the requests the host has completed, ended */
static void hostblk_done(struct work_struct *work)
{
	struct hostblk *hb = container_of(work, struct hostblk, done);
	unsigned long id;
	long ret;

	while (lkl_ops->blk_complete(hb->id, &id, &ret) > 0) {
		struct request *rq = blk_mq_tag_to_rq(hb->set.tags[0], id);

		if (WARN_ON_ONCE(!rq))
			continue;
		blk_mq_end_request(rq, errno_to_blk_status(ret));
	}
}

static irqreturn_t hostblk_interrupt(int irq, void *dev_id)
{
	struct hostblk *hb = dev_id;

	schedule_work(&hb->done);
	return IRQ_HANDLED;
}

static void hostblk_notify(void *arg)
{
	lkl_trigger_irq(((struct hostblk *)arg)->irq);
}

static const struct blk_mq_ops hostblk_mq_ops = {
	.queue_rq = hostblk_queue_rq,
};

static const struct block_device_operations hostblk_fops = {
	.owner = THIS_MODULE,
};

static int __init hostblk_add(int id)
{
	struct queue_limits lim = {
		.features = BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA,
		.max_segments = HOSTBLK_SEGS,
	};
	struct hostblk *hb;
	int err;

	hb = kzalloc(sizeof(*hb), GFP_KERNEL);
	if (!hb)
		return -ENOMEM;
	hb->id = id;
	INIT_WORK(&hb->done, hostblk_done);
	hb->irq = lkl_get_free_irq("hostblk");
	err = hb->irq;
	if (err < 0)
		goto out_free;
	err = request_irq(hb->irq, hostblk_interrupt, 0, "hostblk", hb);
	if (err)
		goto out_irq;
	err = lkl_ops->blk_connect(id, hostblk_notify, hb);
	if (err)
		goto out_request;

	hb->set.ops = &hostblk_mq_ops;
	hb->set.nr_hw_queues = 1;
	hb->set.queue_depth = HOSTBLK_DEPTH;
	hb->set.numa_node = NUMA_NO_NODE;
	hb->set.cmd_size = sizeof(struct hostblk_cmd);
	err = blk_mq_alloc_tag_set(&hb->set);
	if (err)
		goto out_request;

	hb->disk = blk_mq_alloc_disk(&hb->set, &lim, hb);
	if (IS_ERR(hb->disk)) {
		err = PTR_ERR(hb->disk);
		goto out_set;
	}
	hb->disk->major = hostblk_major;
	hb->disk->first_minor = id;
	hb->disk->minors = 1;
	hb->disk->fops = &hostblk_fops;
	hb->disk->private_data = hb;
	snprintf(hb->disk->disk_name, DISK_NAME_LEN, "hostblk%d", id);
	set_capacity(hb->disk, lkl_ops->blk_capacity(id) >> SECTOR_SHIFT);

	err = add_disk(hb->disk);
	if (err)
		goto out_disk;
	return 0;

out_disk:
	put_disk(hb->disk);
out_set:
	blk_mq_free_tag_set(&hb->set);
out_request:
	free_irq(hb->irq, hb);
out_irq:
	lkl_put_irq(hb->irq, "hostblk");
out_free:
	kfree(hb);
	return err;
}

static int __init hostblk_init(void)
{
	int i, n, err;

	if (!lkl_ops->blk_count || !lkl_ops->blk_submit)
		return 0;

	n = lkl_ops->blk_count();
	if (n <= 0)
		return 0;

	hostblk_major = register_blkdev(0, "hostblk");
	if (hostblk_major < 0)
		return hostblk_major;

	for (i = 0; i < n; i++) {
		err = hostblk_add(i);
		if (err)
			pr_err("hostblk%d: failed to add disk: %d\n", i, err);
	}

	return 0;
}
device_initcall(hostblk_init);
