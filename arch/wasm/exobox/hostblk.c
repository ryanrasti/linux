// SPDX-License-Identifier: GPL-2.0
/*
 * Disks as direct host calls (CONFIG_EXOBOX_HOSTBLK): /dev/vda, vdb, ...,
 * one per kernel.blk_count(). No virtqueues: each request goes to the
 * host as a list of its pieces, in the kernel's memory (the page cache's
 * own pages), which the host reads into or writes from.
 *
 * Asynchronous, as hostfuse.c: a request is submitted to the host, which
 * returns at once. A disk in the host's memory has done it by then (and
 * the request ends here); one whose bytes are elsewhere (a file, the
 * network: a promise) works meanwhile, so the kernel runs other tasks, and
 * raises the disks' interrupt as its requests complete.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/slab.h>
#include <asm/irq.h>
#include "exobox.h"

/* a piece of a request, in the kernel's memory */
struct hostblk_seg {
	void *base;
	u32 len;
};

/* what a request does (blk_submit's op) */
enum { HOSTBLK_READ, HOSTBLK_WRITE, HOSTBLK_FLUSH };

int exobox_import(kernel, blk_count)(void);
u64 exobox_import(kernel, blk_size)(int disk);
int exobox_import(kernel, blk_writable)(int disk);
/* the interrupt to raise as requests complete (a hardware irq number) */
void exobox_import(kernel, blk_irq)(u32 irq);
/*
 * A request on disk: op at offset (bytes), across the n pieces at segs, in
 * order (none for a flush), which stay the kernel's until it completes.
 * Returns at once: 1, done; 0, in progress (then blk_complete gives id);
 * or a negative errno, failed.
 */
int exobox_import(kernel, blk_submit)(int disk, void *id, int op, u64 offset,
				      const struct hostblk_seg *segs, u32 n);
/*
 * A request in progress that has completed since: its id, and 0 or a
 * negative errno; returns 1, or 0 if none has.
 */
int exobox_import(kernel, blk_complete)(void **id, int *ret);

/* a request's pieces at most (the queue's max_segments) */
#define HOSTBLK_SEGS 128

struct hostblk {
	int id;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
};

/* each request's own (blk_mq_rq_to_pdu) */
struct hostblk_req {
	blk_status_t status;
	struct hostblk_seg segs[HOSTBLK_SEGS];
};

/* the request's pieces, contiguous ones as one (at most its phys segments) */
static int hostblk_segs(struct request *rq, struct hostblk_seg *segs)
{
	struct req_iterator iter;
	struct bio_vec bvec;
	int n = 0;

	rq_for_each_bvec(bvec, rq, iter) {
		void *base = bvec_virt(&bvec);

		if (n && segs[n - 1].base + segs[n - 1].len == base) {
			segs[n - 1].len += bvec.bv_len;
			continue;
		}
		if (n == HOSTBLK_SEGS)
			return -EIO;
		segs[n].base = base;
		segs[n++].len = bvec.bv_len;
	}
	return n;
}

static blk_status_t hostblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct hostblk *hb = rq->q->queuedata;
	struct hostblk_req *req = blk_mq_rq_to_pdu(rq);
	int op, n = 0, ret;

	switch (req_op(rq)) {
	case REQ_OP_READ:
		op = HOSTBLK_READ;
		break;
	case REQ_OP_WRITE:
		op = HOSTBLK_WRITE;
		break;
	case REQ_OP_FLUSH:
		op = HOSTBLK_FLUSH;
		break;
	default:
		return BLK_STS_NOTSUPP;
	}
	if (op != HOSTBLK_FLUSH) {
		n = hostblk_segs(rq, req->segs);
		if (n < 0)
			return BLK_STS_IOERR;
	}
	blk_mq_start_request(rq);
	ret = wasm_kernel_blk_submit(hb->id, rq, op,
				     (u64)blk_rq_pos(rq) << SECTOR_SHIFT,
				     req->segs, n);
	if (ret == 0)
		return BLK_STS_OK;	/* (hostblk_interrupt completes it) */
	blk_mq_end_request(rq, ret < 0 ? errno_to_blk_status(ret) : BLK_STS_OK);
	return BLK_STS_OK;
}

static void hostblk_complete(struct request *rq)
{
	struct hostblk_req *req = blk_mq_rq_to_pdu(rq);

	blk_mq_end_request(rq, req->status);
}

static irqreturn_t hostblk_interrupt(int irq, void *dev_id)
{
	void *id;
	int ret;

	while (wasm_kernel_blk_complete(&id, &ret) > 0) {
		struct request *rq = id;

		((struct hostblk_req *)blk_mq_rq_to_pdu(rq))->status =
			errno_to_blk_status(ret);
		blk_mq_complete_request(rq);
	}
	return IRQ_HANDLED;
}

static const struct blk_mq_ops hostblk_mq_ops = {
	.queue_rq = hostblk_queue_rq,
	.complete = hostblk_complete,
};

static const struct block_device_operations hostblk_fops = {
	.owner = THIS_MODULE,
};

static int __init hostblk_add(int id, int major)
{
	struct queue_limits lim = {
		.logical_block_size = SECTOR_SIZE,
		.max_segments = HOSTBLK_SEGS,
		/* (so fsync's flushes reach the host's disk) */
		.features = BLK_FEAT_WRITE_CACHE,
	};
	struct hostblk *hb = kzalloc(sizeof(*hb), GFP_KERNEL);
	int ret;

	if (!hb)
		return -ENOMEM;
	hb->id = id;
	hb->tag_set = (struct blk_mq_tag_set){
		.ops = &hostblk_mq_ops,
		.nr_hw_queues = 1,
		.nr_maps = 1,
		.queue_depth = 16,
		.numa_node = NUMA_NO_NODE,
		.cmd_size = sizeof(struct hostblk_req),
	};
	ret = blk_mq_alloc_tag_set(&hb->tag_set);
	if (ret)
		return ret;
	hb->disk = blk_mq_alloc_disk(&hb->tag_set, &lim, hb);
	if (IS_ERR(hb->disk))
		return PTR_ERR(hb->disk);
	hb->disk->major = major;
	hb->disk->first_minor = id * 16;
	hb->disk->minors = 16;
	hb->disk->fops = &hostblk_fops;
	hb->disk->private_data = hb;
	snprintf(hb->disk->disk_name, DISK_NAME_LEN, "vd%c", 'a' + id);
	set_capacity(hb->disk, wasm_kernel_blk_size(id) >> SECTOR_SHIFT);
	if (!wasm_kernel_blk_writable(id))
		set_disk_ro(hb->disk, true);
	return add_disk(hb->disk);
}

static int __init hostblk_init(void)
{
	int n = wasm_kernel_blk_count(), major, irq, i, ret;

	if (n <= 0)
		return 0;
	irq = wasm_alloc_irq();
	if (irq < 0)
		return irq;
	ret = request_irq(irq, hostblk_interrupt, 0, "hostblk", NULL);
	if (ret)
		return ret;
	wasm_kernel_blk_irq(irqd_to_hwirq(irq_get_irq_data(irq)));
	major = register_blkdev(0, "hostblk");
	if (major < 0)
		return major;
	for (i = 0; i < n && i < 26; i++) {
		ret = hostblk_add(i, major);
		if (ret)
			pr_err("hostblk: disk %d: %d\n", i, ret);
	}
	return 0;
}
device_initcall(hostblk_init);
