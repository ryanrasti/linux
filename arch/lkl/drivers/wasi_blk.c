// SPDX-License-Identifier: GPL-2.0
/*
 * A read-only block device over a memory buffer in the blob module (the
 * system image, an EROFS filesystem, as a data symbol: __wasi_image, of
 * __wasi_image_size bytes). No host operation: the kernel reads it from its
 * own memory. One disk, /dev/wblk0, mounted as the root.
 */
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/module.h>

extern const char __wasi_image[] __attribute__((weak));
extern const unsigned long __wasi_image_size __attribute__((weak));

static int wblk_major;

static blk_status_t wblk_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
	struct req_iterator iter;
	struct bio_vec bv;

	blk_mq_start_request(rq);
	if (req_op(rq) != REQ_OP_READ) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}
	rq_for_each_segment(bv, rq, iter) {
		void *dst = bvec_kmap_local(&bv);

		memcpy(dst, __wasi_image + pos, bv.bv_len);
		kunmap_local(dst);
		pos += bv.bv_len;
	}
	blk_mq_end_request(rq, BLK_STS_OK);
	return BLK_STS_OK;
}

static const struct blk_mq_ops wblk_mq_ops = {
	.queue_rq = wblk_queue_rq,
};

static const struct block_device_operations wblk_fops = {
	.owner = THIS_MODULE,
};

static struct blk_mq_tag_set wblk_set;

static int __init wblk_init(void)
{
	struct gendisk *disk;
	int ret;

	if (!__wasi_image || !&__wasi_image_size || !__wasi_image_size)
		return 0;	/* no image embedded */

	wblk_major = register_blkdev(0, "wblk");
	if (wblk_major < 0)
		return wblk_major;

	wblk_set.ops = &wblk_mq_ops;
	wblk_set.nr_hw_queues = 1;
	wblk_set.queue_depth = 64;
	wblk_set.numa_node = NUMA_NO_NODE;
	wblk_set.cmd_size = 0;
	wblk_set.flags = BLK_MQ_F_SHOULD_MERGE;
	ret = blk_mq_alloc_tag_set(&wblk_set);
	if (ret)
		goto out_unreg;

	disk = blk_mq_alloc_disk(&wblk_set, NULL, NULL);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto out_tags;
	}
	disk->major = wblk_major;
	disk->first_minor = 0;
	disk->minors = 1;
	disk->fops = &wblk_fops;
	snprintf(disk->disk_name, DISK_NAME_LEN, "wblk0");
	set_capacity(disk, __wasi_image_size >> SECTOR_SHIFT);
	set_disk_ro(disk, 1);
	ret = add_disk(disk);
	if (ret)
		goto out_tags;
	return 0;

out_tags:
	blk_mq_free_tag_set(&wblk_set);
out_unreg:
	unregister_blkdev(wblk_major, "wblk");
	return ret;
}
device_initcall(wblk_init);
