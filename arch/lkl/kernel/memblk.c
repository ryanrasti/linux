// SPDX-License-Identifier: GPL-2.0
/*
 * memblk: a read-only block device over host memory, such as an erofs
 * image the host mapped before it locked itself down, so that the guest
 * mounts it with no copy at boot and no host call afterwards: a read is a
 * memcpy from the mapping. There is no I/O to wait for, so a bio is done
 * in its submitter's context.
 *
 * A disk is an ordered table of extents, each a run of whole 512-byte
 * sectors that is host memory at an address, or zeros (address 0). One
 * image file mapped whole is a table of one; an image synthesized over the
 * host's own files is many: its metadata in a buffer, each file's data the
 * host's mapping of that file, an ELF's padding zeros. No file's data is
 * copied to make the disk.
 *
 * memblk_add(extents, n), an arch syscall, adds one (memblkN) and returns
 * its device number, for the guest to mknod and mount. The table is copied
 * (the host's may go); the memory it names stays the host's, mapped and
 * unchanged for as long as the kernel runs. Where a block is in host
 * memory is the table's: the host made it, so it finds a block's address
 * itself, and the kernel has no call for it.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/kdev_t.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

/* The host's extent (uapi asm/syscalls.h: struct lkl_memblk_extent). */
struct memblk_extent {
	u64 addr;	/* host address of its first byte; 0: zeros */
	u64 len;	/* bytes, a whole number of 512-byte sectors, not 0 */
};

/*
 * At most this many extents per disk: the kernel's copy is 16 bytes each
 * (16 MiB at most). A closure of a few hundred store paths is tens of
 * thousands of files, each one or two extents.
 */
#define MEMBLK_MAX_EXTENTS	(1UL << 20)

/* An extent, as kept: where it starts in the disk, in bytes, and its memory. */
struct memblk_run {
	u64 start;
	u64 addr;	/* 0: zeros */
};

struct memblk {
	unsigned long n;
	u64 size;	/* the disk's size in bytes: the end of the last run */
	struct memblk_run runs[] __counted_by(n);
};

static int memblk_major;
static int memblk_count;

/* The run holding byte pos (< m->size): the last one starting at or before it. */
static const struct memblk_run *memblk_find(const struct memblk *m, u64 pos)
{
	unsigned long lo = 0, hi = m->n;

	while (hi - lo > 1) {
		unsigned long mid = lo + (hi - lo) / 2;

		if (m->runs[mid].start <= pos)
			lo = mid;
		else
			hi = mid;
	}
	return &m->runs[lo];
}

/* Copies [pos, pos + len) of the disk to dst, across as many runs as it spans. */
static void memblk_copy(const struct memblk *m, char *dst, u64 pos,
			unsigned int len)
{
	const struct memblk_run *r = memblk_find(m, pos);

	while (len) {
		u64 end = r + 1 < m->runs + m->n ? r[1].start : m->size;
		unsigned int k = min_t(u64, len, end - pos);

		if (r->addr)
			memcpy(dst, (const void *)(uintptr_t)
			       (r->addr + (pos - r->start)), k);
		else
			memset(dst, 0, k);
		dst += k;
		pos += k;
		len -= k;
		r++;
	}
}

static void memblk_submit_bio(struct bio *bio)
{
	const struct memblk *m = bio->bi_bdev->bd_disk->private_data;
	u64 pos = (u64)bio->bi_iter.bi_sector << SECTOR_SHIFT;
	struct bio_vec bvec;
	struct bvec_iter iter;

	if (bio_op(bio) != REQ_OP_READ) {
		bio_io_error(bio);
		return;
	}
	/* (the block layer keeps the bio within the disk's capacity) */
	bio_for_each_segment(bvec, bio, iter) {
		char *dst = bvec_kmap_local(&bvec);

		memblk_copy(m, dst, pos, bvec.bv_len);
		kunmap_local(dst);
		pos += bvec.bv_len;
	}
	bio_endio(bio);
}

static const struct block_device_operations memblk_fops = {
	.owner		= THIS_MODULE,
	.submit_bio	= memblk_submit_bio,
};

/* The host's table, checked, as runs. */
static struct memblk *memblk_table(const struct memblk_extent __user *ext,
				   unsigned long n)
{
	struct memblk *m;
	unsigned long i;
	u64 pos = 0;

	if (!n || n > MEMBLK_MAX_EXTENTS)
		return ERR_PTR(-EINVAL);
	m = kvmalloc(struct_size(m, runs, n), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);
	m->n = n;
	for (i = 0; i < n; i++) {
		struct memblk_extent e;

		if (copy_from_user(&e, &ext[i], sizeof(e))) {
			kvfree(m);
			return ERR_PTR(-EFAULT);
		}
		if (!e.len || e.len % SECTOR_SIZE || e.addr + e.len < e.addr ||
		    pos + e.len < pos) {
			kvfree(m);
			return ERR_PTR(-EINVAL);
		}
		m->runs[i] = (struct memblk_run){ .start = pos, .addr = e.addr };
		pos += e.len;
	}
	m->size = pos;
	return m;
}

/*
 * memblk_add(extents, n): a disk of n extents (whole 512-byte sectors of
 * host memory or zeros each, at most MEMBLK_MAX_EXTENTS), in order;
 * returns its device number (new_encode_dev's, as mknod takes it). Disks
 * are never removed.
 */
SYSCALL_DEFINE2(memblk_add, const struct memblk_extent __user *, ext,
		unsigned long, n)
{
	struct queue_limits lim = {
		.features	= BLK_FEAT_SYNCHRONOUS,
	};
	struct gendisk *disk;
	struct memblk *m;
	int err;

	if (memblk_count > MINORMASK)
		return -ENOSPC;
	m = memblk_table(ext, n);
	if (IS_ERR(m))
		return PTR_ERR(m);
	if (!memblk_major) {
		err = register_blkdev(0, "memblk");
		if (err < 0)
			goto free;
		memblk_major = err;
	}

	disk = blk_alloc_disk(&lim, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		goto free;
	}
	disk->major = memblk_major;
	disk->first_minor = memblk_count;
	disk->minors = 1;
	disk->fops = &memblk_fops;
	disk->private_data = m;
	snprintf(disk->disk_name, DISK_NAME_LEN, "memblk%d", memblk_count);
	set_capacity(disk, m->size >> SECTOR_SHIFT);
	set_disk_ro(disk, true);
	err = add_disk(disk);
	if (err) {
		put_disk(disk);
		goto free;
	}

	return new_encode_dev(MKDEV(memblk_major, memblk_count++));
free:
	kvfree(m);
	return err;
}
