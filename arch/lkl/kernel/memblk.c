// SPDX-License-Identifier: GPL-2.0
/*
 * memblk: a read-only block device over host memory, such as an erofs
 * image the host mapped before it locked itself down, so that the guest
 * mounts it with no copy at boot and no host call afterwards: a read is a
 * memcpy from the mapping. There is no I/O to wait for, so a bio is done
 * in its submitter's context.
 *
 * memblk_add(addr, len), an arch syscall, adds one (memblkN) and returns
 * its device number, for the guest to mknod and mount. The memory stays the
 * host's, mapped and unchanged for as long as the kernel runs.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/syscalls.h>

static int memblk_major;
static int memblk_count;

static void memblk_submit_bio(struct bio *bio)
{
	const char *base = bio->bi_bdev->bd_disk->private_data;
	loff_t pos = (loff_t)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (bio_op(bio) != REQ_OP_READ) {
		bio_io_error(bio);
		return;
	}
	/* (the block layer keeps the bio within the disk's capacity) */
	bio_for_each_segment(bvec, bio, iter) {
		memcpy_to_bvec(&bvec, base + pos);
		pos += bvec.bv_len;
	}
	bio_endio(bio);
}

static const struct block_device_operations memblk_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= memblk_submit_bio,
};

/*
 * memblk_add(addr, len): a disk of len bytes (a whole number of 512-byte
 * sectors) at host address addr; returns its device number
 * (new_encode_dev's, as mknod takes it).
 */
SYSCALL_DEFINE2(memblk_add, unsigned long, addr, unsigned long, len)
{
	struct queue_limits lim = {
		.features	= BLK_FEAT_SYNCHRONOUS,
	};
	struct gendisk *disk;
	int err;

	if (!addr || !len || len % SECTOR_SIZE || addr + len < addr)
		return -EINVAL;
	if (memblk_count > MINORMASK)
		return -ENOSPC;
	if (!memblk_major) {
		err = register_blkdev(0, "memblk");
		if (err < 0)
			return err;
		memblk_major = err;
	}

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk))
		return PTR_ERR(disk);
	disk->major = memblk_major;
	disk->first_minor = memblk_count;
	disk->minors = 1;
	disk->fops = &memblk_fops;
	disk->private_data = (void *)addr;
	snprintf(disk->disk_name, DISK_NAME_LEN, "memblk%d", memblk_count);
	set_capacity(disk, len >> SECTOR_SHIFT);
	set_disk_ro(disk, true);
	err = add_disk(disk);
	if (err) {
		put_disk(disk);
		return err;
	}

	return new_encode_dev(MKDEV(memblk_major, memblk_count++));
}
