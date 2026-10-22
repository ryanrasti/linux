// SPDX-License-Identifier: GPL-2.0
/*
 * Disks as direct host calls (CONFIG_EXOBOX_HOSTBLK): /dev/vda, vdb, ...,
 * one per kernel.blk_count(). Each request's segments are read or written
 * by host calls, and the request completes before queue_rq returns.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include "exobox.h"

int exobox_import(kernel, blk_count)(void);
u64 exobox_import(kernel, blk_size)(int disk);
int exobox_import(kernel, blk_writable)(int disk);
/* 0, or a negative errno */
int exobox_import(kernel, blk_read)(int disk, u64 offset, void *buf, u32 len);
int exobox_import(kernel, blk_write)(int disk, u64 offset, const void *buf, u32 len);

struct hostblk {
	int id;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;
};

static blk_status_t hostblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				     const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct hostblk *hb = rq->q->queuedata;
	u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	blk_status_t status = BLK_STS_OK;
	struct req_iterator iter;
	struct bio_vec bvec;

	blk_mq_start_request(rq);
	switch (req_op(rq)) {
	case REQ_OP_FLUSH:
		break;
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		rq_for_each_segment(bvec, rq, iter) {
			void *buf = bvec_virt(&bvec);
			int ret = req_op(rq) == REQ_OP_READ ?
				wasm_kernel_blk_read(hb->id, pos, buf, bvec.bv_len) :
				wasm_kernel_blk_write(hb->id, pos, buf, bvec.bv_len);

			if (ret) {
				status = BLK_STS_IOERR;
				break;
			}
			pos += bvec.bv_len;
		}
		break;
	default:
		status = BLK_STS_NOTSUPP;
	}
	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops hostblk_mq_ops = {
	.queue_rq = hostblk_queue_rq,
};

static const struct block_device_operations hostblk_fops = {
	.owner = THIS_MODULE,
};

static int __init hostblk_add(int id, int major)
{
	struct queue_limits lim = { .logical_block_size = SECTOR_SIZE };
	struct hostblk *hb = kzalloc(sizeof(*hb), GFP_KERNEL);
	int ret;

	if (!hb)
		return -ENOMEM;
	hb->id = id;
	ret = blk_mq_alloc_sq_tag_set(&hb->tag_set, &hostblk_mq_ops, 16, 0);
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
	int n = wasm_kernel_blk_count(), major, i, ret;

	if (n <= 0)
		return 0;
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
