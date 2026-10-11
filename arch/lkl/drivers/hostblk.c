// SPDX-License-Identifier: GPL-2.0
/*
 * Block devices backed by host operations (lkl_ops->blk_*).
 *
 * The host owns the storage and copies data straight to and from kernel
 * buffers, so there are no rings or interrupts: each bio is handled
 * synchronously in submit_bio.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/module.h>
#include <asm/host_ops.h>

static int hostblk_major;

static int hostblk_flush(int id)
{
	return lkl_ops->blk_flush ? lkl_ops->blk_flush(id) : 0;
}

static void hostblk_submit_bio(struct bio *bio)
{
	int id = (long)bio->bi_bdev->bd_disk->private_data;
	bool write = op_is_write(bio_op(bio));
	sector_t sector = bio->bi_iter.bi_sector;
	struct bvec_iter iter;
	struct bio_vec bv;
	int err = 0;

	if (bio->bi_opf & REQ_PREFLUSH)
		err = hostblk_flush(id);

	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		bio_for_each_segment(bv, bio, iter) {
			if (err)
				break;
			err = lkl_ops->blk_rw(id, write, sector, bvec_virt(&bv),
					      bv.bv_len);
			sector += bv.bv_len >> SECTOR_SHIFT;
		}
		break;
	default:
		err = -EOPNOTSUPP;
	}

	if (!err && (bio->bi_opf & REQ_FUA))
		err = hostblk_flush(id);

	bio->bi_status = errno_to_blk_status(err);
	bio_endio(bio);
}

static const struct block_device_operations hostblk_fops = {
	.owner = THIS_MODULE,
	.submit_bio = hostblk_submit_bio,
};

static int __init hostblk_add(int id)
{
	struct queue_limits lim = {
		.features = BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA |
			    BLK_FEAT_SYNCHRONOUS,
	};
	struct gendisk *disk;
	int err;

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	disk->major = hostblk_major;
	disk->first_minor = id;
	disk->minors = 1;
	disk->fops = &hostblk_fops;
	disk->private_data = (void *)(long)id;
	snprintf(disk->disk_name, DISK_NAME_LEN, "hostblk%d", id);
	set_capacity(disk, lkl_ops->blk_capacity(id) >> SECTOR_SHIFT);

	err = add_disk(disk);
	if (err)
		put_disk(disk);
	return err;
}

static int __init hostblk_init(void)
{
	int i, n, err;

	if (!lkl_ops->blk_count || !lkl_ops->blk_rw)
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
