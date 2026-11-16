// SPDX-License-Identifier: GPL-2.0
/*
 * wasifs: a WASI preopened directory as a filesystem, through
 * wasi:filesystem's descriptors, which the kernel calls itself (it is
 * linked into the program, as the console is).
 *
 *   mount -t wasifs <preopen's name> <dir>
 *
 * The source is a preopen's name as the runtime gives it
 * (wasi:filesystem/preopens): the mount finds its descriptor by that name.
 *
 * WASI has no inode handles, only paths under a descriptor, so the
 * filesystem is a path client (as a simple 9p or FUSE one): an operation
 * names its file by the dentry's path below the mount's root, resolved by
 * the runtime from the preopen. Inodes are keyed by the host's metadata
 * hash, so hard links share one. The dcache is trusted for positive
 * dentries; negative ones are looked up again each time, attributes are
 * fetched again by stat and by open. An open file holds a descriptor of
 * its own; reads and writes go to the host directly, at an offset (no page
 * cache; a mapping of a file is nommu's copy, a shared one written back).
 *
 * WASI has no owners or permissions: files are root's, directories 0755,
 * files 0644, symbolic links 0777, and chmod or chown to anything else is
 * EPERM. A preopen that may not change its directory (no mutate-directory
 * flag) mounts read-only.
 */
#include <linux/init.h>
#include <linux/cleanup.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uio.h>
#include <linux/splice.h>
#include <asm/host_ops.h>
#include <asm/wasi.h>

#define WASIFS_MAGIC 0x77617369	/* "wasi" */

/* wasi:filesystem's descriptor's methods (asm/wasi.h), on a descriptor's handle */
#define WASI_FS(name, fd, ...) \
	wasi_filesystem_types_method_descriptor_##name( \
		(wasi_filesystem_types_borrow_descriptor_t){ fd }, ##__VA_ARGS__)

/* a C string as a WASI one (borrowed: not copied) */
#define WASI_STR(s) (&(wasi_string_t){ (u8 *)(s), strlen(s) })

#define WASI_TYPE(t)	WASI_FILESYSTEM_TYPES_DESCRIPTOR_TYPE_##t
#define WASI_READ	WASI_FILESYSTEM_TYPES_DESCRIPTOR_FLAGS_READ
#define WASI_WRITE	WASI_FILESYSTEM_TYPES_DESCRIPTOR_FLAGS_WRITE
#define WASI_OPEN(f)	WASI_FILESYSTEM_TYPES_OPEN_FLAGS_##f

/* wasi:filesystem's error-code (its order) as Linux's errno */
static const u8 wasi_errnos[] = {
	EACCES, EAGAIN, EALREADY, EBADF, EBUSY, EDEADLK, EDQUOT, EEXIST, EFBIG,
	EILSEQ, EINPROGRESS, EINTR, EINVAL, EIO, EISDIR, ELOOP, EMLINK,
	EMSGSIZE, ENAMETOOLONG, ENODEV, ENOENT, ENOLCK, ENOMEM, ENOSPC,
	ENOTDIR, ENOTEMPTY, ENOTRECOVERABLE, EOPNOTSUPP, ENOTTY, ENXIO,
	EOVERFLOW, EPERM, EPIPE, EROFS, ESPIPE, ETXTBSY, EXDEV,
};
static_assert(ARRAY_SIZE(wasi_errnos) == 37);

static int wasi_err(wasi_filesystem_types_error_code_t e)
{
	return e < ARRAY_SIZE(wasi_errnos) ? -wasi_errnos[e] : -EIO;
}

/* a file's attributes: its stat and its identity (the metadata hash's) */
struct wasifs_stat {
	u64 key;		/* the hash's lower half (0: the host has none) */
	u8 type;
	u64 nlink;
	u64 size;
	struct timespec64 atime, mtime, ctime;
};

static struct timespec64 wasi_timespec(const wasi_filesystem_types_option_datetime_t *t)
{
	return t->is_some ? (struct timespec64){ t->val.seconds, t->val.nanoseconds }
			  : (struct timespec64){};
}

/*
 * A file's identity is the hash's lower half (the upper may be derived from
 * it: Wasmtime's is the lower xor a constant), 0 if the host has none: an
 * inode's key, its 64 bits compared (two files whose inode numbers are the
 * same aren't one); its inode number (i_ino's 32 bits) the key folded.
 */
static u64 wasi_key(bool ok, const wasi_filesystem_types_metadata_hash_value_t *h)
{
	return ok ? h->lower : 0;
}

static u32 wasi_ino(u64 key)
{
	return key ^ (key >> 32);
}

static void wasifs_stat_of(const wasi_filesystem_types_descriptor_stat_t *r, u64 key,
			   struct wasifs_stat *st)
{
	st->key = key;
	st->type = r->type;
	st->nlink = r->link_count;
	st->size = r->size;
	st->atime = wasi_timespec(&r->data_access_timestamp);
	st->mtime = wasi_timespec(&r->data_modification_timestamp);
	st->ctime = wasi_timespec(&r->status_change_timestamp);
}

/* path's attributes, below the descriptor fd (a symbolic link's own) */
static int wasi_stat_at(s32 fd, const char *path, struct wasifs_stat *st)
{
	wasi_filesystem_types_descriptor_stat_t r;
	wasi_filesystem_types_metadata_hash_value_t h;
	wasi_filesystem_types_error_code_t err;
	bool ok;

	if (!WASI_FS(stat_at, fd, 0, WASI_STR(path), &r, &err))
		return wasi_err(err);
	ok = WASI_FS(metadata_hash_at, fd, 0, WASI_STR(path), &h, &err);
	wasifs_stat_of(&r, wasi_key(ok, &h), st);
	return 0;
}

/* the descriptor's own */
static int wasi_stat(s32 fd, struct wasifs_stat *st)
{
	wasi_filesystem_types_descriptor_stat_t r;
	wasi_filesystem_types_metadata_hash_value_t h;
	wasi_filesystem_types_error_code_t err;
	bool ok;

	if (!WASI_FS(stat, fd, &r, &err))
		return wasi_err(err);
	ok = WASI_FS(metadata_hash, fd, &h, &err);
	wasifs_stat_of(&r, wasi_key(ok, &h), st);
	return 0;
}

static void wasi_close(s32 fd)
{
	wasi_filesystem_types_descriptor_drop_own((wasi_filesystem_types_own_descriptor_t){ fd });
}

struct wasifs_sb {
	s32 fd;			/* the preopen's */
};

static s32 root_fd(struct super_block *sb)
{
	return ((struct wasifs_sb *)sb->s_fs_info)->fd;
}

/* a dentry's path below the mount's root ("." for the root), in buf (PATH_MAX) */
static const char *wasifs_path(struct dentry *dentry, char *buf)
{
	char *p = dentry_path_raw(dentry, buf, PATH_MAX);

	if (IS_ERR(p))
		return p;
	return p[1] ? p + 1 : ".";
}

/* the path's buffer, freed at the end of the scope */
DEFINE_FREE(wasifs_name, char *, if (_T) __putname(_T))

#define WASIFS_PATH(var, dentry)					\
	char *var##_buf __free(wasifs_name) = __getname();		\
	const char *var = var##_buf ? wasifs_path(dentry, var##_buf)	\
				    : ERR_PTR(-ENOMEM)

static const struct inode_operations wasifs_dir_iops;
static const struct inode_operations wasifs_file_iops;
static const struct inode_operations wasifs_symlink_iops;
static const struct file_operations wasifs_dir_fops;
static const struct file_operations wasifs_file_fops;
static const struct dentry_operations wasifs_dops;

static umode_t wasi_mode(u8 type)
{
	switch (type) {
	case WASI_TYPE(DIRECTORY):
		return S_IFDIR | 0755;
	case WASI_TYPE(SYMBOLIC_LINK):
		return S_IFLNK | 0777;
	case WASI_TYPE(BLOCK_DEVICE):
		return S_IFBLK | 0644;
	case WASI_TYPE(CHARACTER_DEVICE):
		return S_IFCHR | 0644;
	case WASI_TYPE(FIFO):
		return S_IFIFO | 0644;
	case WASI_TYPE(SOCKET):
		return S_IFSOCK | 0644;
	default:
		return S_IFREG | 0644;
	}
}

/* the host's attributes into the inode (all but its type, fixed at iget) */
static void wasifs_fill(struct inode *inode, const struct wasifs_stat *st)
{
	set_nlink(inode, st->nlink);
	i_size_write(inode, st->size);
	inode->i_blocks = (st->size + 511) >> 9;
	inode_set_atime_to_ts(inode, st->atime);
	inode_set_mtime_to_ts(inode, st->mtime);
	inode_set_ctime_to_ts(inode, st->ctime);
}

/* an inode, with its key */
struct wasifs_inode {
	struct inode vfs_inode;
	u64 key;
};

static struct kmem_cache *wasifs_inode_cache;

static struct wasifs_inode *WASIFS_I(struct inode *inode)
{
	return container_of(inode, struct wasifs_inode, vfs_inode);
}

static int wasifs_test(struct inode *inode, void *data)
{
	const struct wasifs_stat *st = data;

	return WASIFS_I(inode)->key == st->key &&
	       (inode->i_mode & S_IFMT) == (wasi_mode(st->type) & S_IFMT);
}

static int wasifs_set(struct inode *inode, void *data)
{
	const struct wasifs_stat *st = data;

	WASIFS_I(inode)->key = st->key;
	inode->i_ino = wasi_ino(st->key);
	return 0;
}

static struct inode *wasifs_iget(struct super_block *sb, struct wasifs_stat *st)
{
	struct inode *inode;

	if (st->key) {
		inode = iget5_locked(sb, wasi_ino(st->key), wasifs_test, wasifs_set, st);
	} else {
		/* a runtime without inode numbers: an inode each time */
		inode = new_inode(sb);
		if (inode) {
			inode->i_ino = get_next_ino();
			inode->i_state |= I_NEW;
		}
	}
	if (!inode)
		return ERR_PTR(-ENOMEM);
	wasifs_fill(inode, st);
	if (!(inode->i_state & I_NEW))
		return inode;
	inode->i_mode = wasi_mode(st->type);
	switch (inode->i_mode & S_IFMT) {
	case S_IFDIR:
		inode->i_op = &wasifs_dir_iops;
		inode->i_fop = &wasifs_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &wasifs_symlink_iops;
		break;
	case S_IFREG:
		inode->i_op = &wasifs_file_iops;
		inode->i_fop = &wasifs_file_fops;
		break;
	default:
		/* the host's devices and sockets: their nodes, opening nothing */
		inode->i_op = &wasifs_file_iops;
		init_special_inode(inode, inode->i_mode, 0);
	}
	unlock_new_inode(inode);
	return inode;
}

static int wasifs_stat(struct dentry *dentry, struct wasifs_stat *st)
{
	WASIFS_PATH(path, dentry);

	if (IS_ERR(path))
		return PTR_ERR(path);
	return wasi_stat_at(root_fd(dentry->d_sb), path, st);
}

/* the dentry's inode, from the host: after it was made */
static int wasifs_instantiate(struct dentry *dentry)
{
	struct wasifs_stat st;
	struct inode *inode;
	int err = wasifs_stat(dentry, &st);

	if (err)
		return err;
	inode = wasifs_iget(dentry->d_sb, &st);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *wasifs_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	struct wasifs_stat st;
	struct inode *inode = NULL;
	int err;

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	err = wasifs_stat(dentry, &st);
	if (err == -ENOENT)
		return d_splice_alias(NULL, dentry);
	if (err)
		return ERR_PTR(err);
	inode = wasifs_iget(dir->i_sb, &st);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	return d_splice_alias(inode, dentry);
}

/* a negative dentry is looked up again: the host may have made the file since */
static int wasifs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	return d_really_is_positive(dentry);
}

static const struct dentry_operations wasifs_dops = {
	.d_revalidate = wasifs_d_revalidate,
};

/* the dentry's file opened: open-flags oflags, descriptor-flags flags */
static int wasifs_open_path(struct dentry *dentry, u8 oflags, u8 flags, s32 *fd)
{
	WASIFS_PATH(path, dentry);
	wasi_filesystem_types_own_descriptor_t r;
	wasi_filesystem_types_error_code_t err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	if (!WASI_FS(open_at, root_fd(dentry->d_sb), 0, WASI_STR(path), oflags, flags,
		     &r, &err))
		return wasi_err(err);
	*fd = r.__handle;
	return 0;
}

static int wasifs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	s32 fd;
	int err = wasifs_open_path(dentry, WASI_OPEN(CREATE) | WASI_OPEN(EXCLUSIVE),
				   WASI_READ | WASI_WRITE, &fd);

	if (err)
		return err;
	wasi_close(fd);
	return wasifs_instantiate(dentry);
}

static int wasifs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode)
{
	WASIFS_PATH(path, dentry);
	wasi_filesystem_types_error_code_t e;

	if (IS_ERR(path))
		return PTR_ERR(path);
	if (!WASI_FS(create_directory_at, root_fd(dir->i_sb), WASI_STR(path), &e))
		return wasi_err(e);
	inc_nlink(dir);
	return wasifs_instantiate(dentry);
}

static int wasifs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *target)
{
	WASIFS_PATH(path, dentry);
	wasi_filesystem_types_error_code_t e;

	if (IS_ERR(path))
		return PTR_ERR(path);
	if (!WASI_FS(symlink_at, root_fd(dir->i_sb), WASI_STR(target), WASI_STR(path), &e))
		return wasi_err(e);
	return wasifs_instantiate(dentry);
}

static int wasifs_link(struct dentry *old, struct inode *dir, struct dentry *new)
{
	WASIFS_PATH(from, old);
	WASIFS_PATH(to, new);
	struct inode *inode = d_inode(old);
	wasi_filesystem_types_error_code_t e;

	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	if (!WASI_FS(link_at, root_fd(dir->i_sb), 0, WASI_STR(from),
		     (wasi_filesystem_types_borrow_descriptor_t){ root_fd(dir->i_sb) },
		     WASI_STR(to), &e))
		return wasi_err(e);
	inc_nlink(inode);
	ihold(inode);
	d_instantiate(new, inode);
	return 0;
}

static int wasifs_unlink(struct inode *dir, struct dentry *dentry)
{
	WASIFS_PATH(path, dentry);
	wasi_filesystem_types_error_code_t e;

	if (IS_ERR(path))
		return PTR_ERR(path);
	if (!WASI_FS(unlink_file_at, root_fd(dir->i_sb), WASI_STR(path), &e))
		return wasi_err(e);
	drop_nlink(d_inode(dentry));
	return 0;
}

static int wasifs_rmdir(struct inode *dir, struct dentry *dentry)
{
	WASIFS_PATH(path, dentry);
	wasi_filesystem_types_error_code_t e;

	if (IS_ERR(path))
		return PTR_ERR(path);
	if (!WASI_FS(remove_directory_at, root_fd(dir->i_sb), WASI_STR(path), &e))
		return wasi_err(e);
	clear_nlink(d_inode(dentry));
	drop_nlink(dir);
	return 0;
}

static int wasifs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			 struct dentry *old, struct inode *new_dir,
			 struct dentry *new, unsigned int flags)
{
	WASIFS_PATH(from, old);
	WASIFS_PATH(to, new);
	struct inode *victim = d_inode(new);
	s32 fd = root_fd(old_dir->i_sb);
	wasi_filesystem_types_error_code_t e;

	/* NOREPLACE holds by the dentry (the VFS checked it negative); no EXCHANGE */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	if (!WASI_FS(rename_at, fd, WASI_STR(from),
		     (wasi_filesystem_types_borrow_descriptor_t){ fd }, WASI_STR(to), &e))
		return wasi_err(e);
	/* (the links as simple_rename counts them) */
	if (victim) {
		if (S_ISDIR(d_inode(old)->i_mode)) {
			clear_nlink(victim);
			drop_nlink(old_dir);
		} else {
			drop_nlink(victim);
		}
	} else if (S_ISDIR(d_inode(old)->i_mode)) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}
	return 0;
}

static const char *wasifs_get_link(struct dentry *dentry, struct inode *inode,
				   struct delayed_call *done)
{
	wasi_filesystem_types_error_code_t e;
	wasi_string_t r;
	char *target;
	bool ok;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	{
		WASIFS_PATH(path, dentry);

		if (IS_ERR(path))
			return path;
		ok = WASI_FS(readlink_at, root_fd(inode->i_sb), WASI_STR(path), &r, &e);
	}
	if (!ok)
		return ERR_PTR(wasi_err(e));
	target = r.len < PATH_MAX ? kmemdup_nul(r.ptr, r.len, GFP_KERNEL)
				  : ERR_PTR(-ENAMETOOLONG);
	wasi_string_free(&r);
	if (!target)
		return ERR_PTR(-ENOMEM);
	if (IS_ERR(target))
		return target;
	set_delayed_call(done, kfree_link, target);
	return target;
}

/* the inode's attributes from the host again */
static int wasifs_refresh(struct dentry *dentry)
{
	struct wasifs_stat st;
	int err = wasifs_stat(dentry, &st);

	if (!err)
		wasifs_fill(d_inode(dentry), &st);
	return err;
}

static int wasifs_getattr(struct mnt_idmap *idmap, const struct path *path,
			  struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	int err = wasifs_refresh(path->dentry);

	if (err)
		return err;
	generic_fillattr(idmap, request_mask, inode, stat);
	return 0;
}

static int wasi_set_size(s32 fd, loff_t size)
{
	wasi_filesystem_types_error_code_t e;

	return WASI_FS(set_size, fd, size, &e) ? 0 : wasi_err(e);
}

static int wasifs_truncate(struct dentry *dentry, struct iattr *attr)
{
	s32 fd;
	int err;

	/* an open file's own descriptor, when it may write */
	if ((attr->ia_valid & ATTR_FILE) && (attr->ia_file->f_mode & FMODE_WRITE))
		return wasi_set_size((uintptr_t)attr->ia_file->private_data,
				     attr->ia_size);
	err = wasifs_open_path(dentry, 0, WASI_WRITE, &fd);
	if (err)
		return err;
	err = wasi_set_size(fd, attr->ia_size);
	wasi_close(fd);
	return err;
}

/* an attribute's change of a time (to t): a new-timestamp */
static wasi_filesystem_types_new_timestamp_t wasi_new_time(struct iattr *attr, unsigned int valid,
							   unsigned int set, struct timespec64 t)
{
	if (!(attr->ia_valid & valid))
		return (wasi_filesystem_types_new_timestamp_t){
			WASI_FILESYSTEM_TYPES_NEW_TIMESTAMP_NO_CHANGE };
	if (!(attr->ia_valid & set))
		return (wasi_filesystem_types_new_timestamp_t){
			WASI_FILESYSTEM_TYPES_NEW_TIMESTAMP_NOW };
	return (wasi_filesystem_types_new_timestamp_t){
		WASI_FILESYSTEM_TYPES_NEW_TIMESTAMP_TIMESTAMP,
		.val.timestamp = { t.tv_sec, t.tv_nsec } };
}

static int wasifs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	wasi_filesystem_types_new_timestamp_t atim =
		wasi_new_time(attr, ATTR_ATIME, ATTR_ATIME_SET, attr->ia_atime);
	wasi_filesystem_types_new_timestamp_t mtim =
		wasi_new_time(attr, ATTR_MTIME, ATTR_MTIME_SET, attr->ia_mtime);
	int err = setattr_prepare(idmap, dentry, attr);

	if (err)
		return err;
	/* no owners or modes: only the ones there are */
	if ((attr->ia_valid & ATTR_MODE) &&
	    (attr->ia_mode & S_IALLUGO) != (inode->i_mode & S_IALLUGO))
		return -EPERM;
	if ((attr->ia_valid & ATTR_UID) && !uid_eq(attr->ia_uid, inode->i_uid))
		return -EPERM;
	if ((attr->ia_valid & ATTR_GID) && !gid_eq(attr->ia_gid, inode->i_gid))
		return -EPERM;
	if (attr->ia_valid & ATTR_SIZE) {
		err = wasifs_truncate(dentry, attr);
		if (err)
			return err;
	}
	/* (a truncation sets the times itself) */
	if ((atim.tag || mtim.tag) && !(attr->ia_valid & ATTR_SIZE)) {
		WASIFS_PATH(path, dentry);
		wasi_filesystem_types_error_code_t e;

		if (IS_ERR(path))
			return PTR_ERR(path);
		if (!WASI_FS(set_times_at, root_fd(inode->i_sb), 0, WASI_STR(path), &atim, &mtim,
			     &e))
			return wasi_err(e);
	}
	return wasifs_refresh(dentry);
}

static const struct inode_operations wasifs_dir_iops = {
	.lookup = wasifs_lookup,
	.create = wasifs_create,
	.mkdir = wasifs_mkdir,
	.symlink = wasifs_symlink,
	.link = wasifs_link,
	.unlink = wasifs_unlink,
	.rmdir = wasifs_rmdir,
	.rename = wasifs_rename,
	.getattr = wasifs_getattr,
	.setattr = wasifs_setattr,
};

static const struct inode_operations wasifs_file_iops = {
	.getattr = wasifs_getattr,
	.setattr = wasifs_setattr,
};

static const struct inode_operations wasifs_symlink_iops = {
	.get_link = wasifs_get_link,
	.getattr = wasifs_getattr,
};

/* an open file's descriptor is its private_data */
static s32 file_fd(struct file *file)
{
	return (uintptr_t)file->private_data;
}

static int wasifs_open(struct inode *inode, struct file *file)
{
	u8 flags = 0;
	s32 fd;
	int err;

	if (file->f_mode & FMODE_READ)
		flags |= WASI_READ;
	if (file->f_mode & FMODE_WRITE)
		flags |= WASI_WRITE;
	err = wasifs_open_path(file->f_path.dentry, 0, flags, &fd);
	if (err)
		return err;
	file->private_data = (void *)(uintptr_t)fd;
	return wasifs_refresh(file->f_path.dentry);
}

static int wasifs_release(struct inode *inode, struct file *file)
{
	wasi_close(file_fd(file));
	return 0;
}

/* (what one call reads or writes at most) */
#define WASIFS_IO_MAX (64 * 1024)

static ssize_t wasifs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t done = 0;

	while (iov_iter_count(to)) {
		size_t want = min(iov_iter_count(to), (size_t)WASIFS_IO_MAX);
		wasi_filesystem_types_error_code_t e;
		wasi_tuple2_list_u8_bool_t r;
		size_t got, len;

		if (!WASI_FS(read, file_fd(iocb->ki_filp), want, iocb->ki_pos, &r, &e)) {
			if (!done)
				done = wasi_err(e);
			break;
		}
		len = r.f0.len;
		got = copy_to_iter(r.f0.ptr, len, to);
		wasi_list_u8_free(&r.f0);
		if (got != len) {
			if (!done)
				done = -EFAULT;
			break;
		}
		iocb->ki_pos += got;
		done += got;
		if (got < want || r.f1)
			break;
	}
	return done;
}

static ssize_t wasifs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t done = 0, ret;
	size_t len;
	void *buf;

	inode_lock(inode);
	/* an append is at the host's end of the file */
	if (iocb->ki_flags & IOCB_APPEND) {
		ret = wasifs_refresh(file->f_path.dentry);
		if (ret)
			goto out;
	}
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;
	/* (a bounce buffer: the iterator may be any kind, a kernel_write's too) */
	len = min(iov_iter_count(from), (size_t)WASIFS_IO_MAX);
	buf = kmalloc(len, GFP_KERNEL);
	ret = -ENOMEM;
	if (!buf)
		goto out;
	while (iov_iter_count(from)) {
		size_t n = copy_from_iter(buf, min(iov_iter_count(from), len), from);
		size_t off = 0;

		if (!n) {
			ret = -EFAULT;
			break;
		}
		while (off < n) {
			wasi_list_u8_t l = { buf + off, n - off };
			wasi_filesystem_types_error_code_t e;
			u64 w;

			if (!WASI_FS(write, file_fd(file), &l, iocb->ki_pos, &w, &e)) {
				ret = wasi_err(e);
				goto stop;
			}
			off += w;
			iocb->ki_pos += w;
			done += w;
		}
	}
stop:
	kfree(buf);
	if (iocb->ki_pos > i_size_read(inode))
		i_size_write(inode, iocb->ki_pos);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
out:
	inode_unlock(inode);
	if (done)
		return generic_write_sync(iocb, done);
	return ret;
}

static loff_t wasifs_llseek(struct file *file, loff_t offset, int whence)
{
	/* the end, and data and holes, are the host's file's now */
	if (whence != SEEK_SET && whence != SEEK_CUR) {
		int err = wasifs_refresh(file->f_path.dentry);

		if (err)
			return err;
	}
	return generic_file_llseek(file, offset, whence);
}

static int wasi_sync(s32 fd, int datasync)
{
	wasi_filesystem_types_error_code_t e;

	if (datasync ? WASI_FS(sync_data, fd, &e) : WASI_FS(sync, fd, &e))
		return 0;
	return wasi_err(e);
}

static int wasifs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return wasi_sync(file_fd(file), datasync);
}

/* a mapping is the file's copy (nommu): it needs only a mmap to say it may */
static int wasifs_mmap(struct file *file, struct vm_area_struct *vma)
{
	return 0;
}

static const struct file_operations wasifs_file_fops = {
	.open = wasifs_open,
	.release = wasifs_release,
	.read_iter = wasifs_read_iter,
	.write_iter = wasifs_write_iter,
	.llseek = wasifs_llseek,
	.fsync = wasifs_fsync,
	.mmap = wasifs_mmap,
	.splice_read = copy_splice_read,
	.splice_write = iter_file_splice_write,
};

/*
 * an open directory: its descriptor, and its entries read in order (a
 * directory-entry-stream, which has no seek): pos is the file position of
 * the next entry the stream gives, or of next, one read and not yet taken
 * (readdir's buffer was full). The position past "." and ".." is 2 + an
 * entry's index; another position reads the entries again from the first.
 */
struct wasifs_dir {
	s32 fd;
	wasi_filesystem_types_own_directory_entry_stream_t stream;	/* 0: none */
	loff_t pos;
	wasi_filesystem_types_option_directory_entry_t next;
};

static void wasifs_dir_next_free(struct wasifs_dir *d)
{
	wasi_filesystem_types_option_directory_entry_free(&d->next);
	d->next.is_some = false;
}

static void wasifs_dir_stream_drop(struct wasifs_dir *d)
{
	wasifs_dir_next_free(d);
	if (d->stream.__handle)
		wasi_filesystem_types_directory_entry_stream_drop_own(d->stream);
	d->stream.__handle = 0;
}

static int wasifs_dir_open(struct inode *inode, struct file *file)
{
	struct wasifs_dir *d = kzalloc(sizeof(*d), GFP_KERNEL);
	int err;

	if (!d)
		return -ENOMEM;
	err = wasifs_open_path(file->f_path.dentry, WASI_OPEN(DIRECTORY), WASI_READ,
			       &d->fd);
	if (err) {
		kfree(d);
		return err;
	}
	file->private_data = d;
	return 0;
}

static int wasifs_dir_release(struct inode *inode, struct file *file)
{
	struct wasifs_dir *d = file->private_data;

	wasifs_dir_stream_drop(d);
	wasi_close(d->fd);
	kfree(d);
	return 0;
}

static unsigned char wasi_dtype(u8 type)
{
	return fs_umode_to_dtype(wasi_mode(type));
}

/* an entry's inode number, stat's (the metadata hash of the path) */
static u32 wasifs_entry_ino(struct file *file, const u8 *name, u32 len)
{
	WASIFS_PATH(dir, file->f_path.dentry);
	wasi_filesystem_types_metadata_hash_value_t h;
	wasi_filesystem_types_error_code_t e;
	char *path;
	bool ok;
	u32 ino;

	if (IS_ERR(dir))
		return 0;
	path = strcmp(dir, ".") ? kasprintf(GFP_KERNEL, "%s/%.*s", dir, len, name)
				: kmemdup_nul(name, len, GFP_KERNEL);
	if (!path)
		return 0;
	ok = WASI_FS(metadata_hash_at, root_fd(file->f_path.dentry->d_sb), 0, WASI_STR(path),
		     &h, &e);
	ino = wasi_ino(wasi_key(ok, &h));
	kfree(path);
	return ino;
}

static int wasifs_iterate(struct file *file, struct dir_context *ctx)
{
	struct wasifs_dir *d = file->private_data;

	if (!dir_emit_dots(file, ctx))
		return 0;
	if (!d->stream.__handle || d->pos != ctx->pos) {
		wasi_filesystem_types_error_code_t e;

		wasifs_dir_stream_drop(d);
		if (!WASI_FS(read_directory, d->fd, &d->stream, &e))
			return wasi_err(e);
		d->pos = 2;
	}
	for (;;) {
		wasi_filesystem_types_directory_entry_t *next = &d->next.val;

		if (!d->next.is_some) {
			wasi_filesystem_types_error_code_t e;

			if (!wasi_filesystem_types_method_directory_entry_stream_read_directory_entry(
				wasi_filesystem_types_borrow_directory_entry_stream(d->stream),
				&d->next, &e)) {
				d->next.is_some = false;
				return wasi_err(e);
			}
			if (!d->next.is_some)
				return 0;
		}
		if (d->pos >= ctx->pos &&
		    !dir_emit(ctx, (const char *)next->name.ptr, next->name.len,
			      wasifs_entry_ino(file, next->name.ptr, next->name.len) ?: 1,
			      wasi_dtype(next->type)))
			return 0;
		wasifs_dir_next_free(d);
		d->pos++;
		if (d->pos > ctx->pos)
			ctx->pos = d->pos;
	}
}

static int wasifs_dir_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return wasi_sync(((struct wasifs_dir *)file->private_data)->fd, datasync);
}

static const struct file_operations wasifs_dir_fops = {
	.open = wasifs_dir_open,
	.release = wasifs_dir_release,
	.iterate_shared = wasifs_iterate,
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.fsync = wasifs_dir_fsync,
};

static int wasifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	/* (WASI has no statfs: the type and limits only) */
	buf->f_type = WASIFS_MAGIC;
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = NAME_MAX;
	return 0;
}

static struct inode *wasifs_alloc_inode(struct super_block *sb)
{
	struct wasifs_inode *wi = alloc_inode_sb(sb, wasifs_inode_cache, GFP_KERNEL);

	return wi ? &wi->vfs_inode : NULL;
}

static void wasifs_free_inode(struct inode *inode)
{
	kmem_cache_free(wasifs_inode_cache, WASIFS_I(inode));
}

static const struct super_operations wasifs_sops = {
	.alloc_inode = wasifs_alloc_inode,
	.free_inode = wasifs_free_inode,
	.statfs = wasifs_statfs,
};

/* the preopens (wasi:filesystem/preopens'), got at the first mount, kept */
static wasi_filesystem_preopens_list_tuple2_own_descriptor_string_t preopens;

/* the preopen named `name`: its descriptor, or -ENOENT */
static int wasi_preopen(const char *name, s32 *found)
{
	size_t len = strlen(name);
	u32 i;

	if (!preopens.ptr)
		wasi_filesystem_preopens_get_directories(&preopens);
	for (i = 0; i < preopens.len; i++) {
		wasi_string_t *n = &preopens.ptr[i].f1;

		if (n->len == len && !memcmp(n->ptr, name, len)) {
			*found = preopens.ptr[i].f0.__handle;
			return 0;
		}
	}
	return -ENOENT;
}

static int wasifs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	wasi_filesystem_types_descriptor_flags_t flags;
	wasi_filesystem_types_error_code_t e;
	struct wasifs_sb *fs;
	struct wasifs_stat st;
	struct inode *root;
	s32 fd;
	int err;

	if (!fc->source)
		return invalfc(fc, "the source is a preopen's name");
	err = wasi_preopen(fc->source, &fd);
	if (err)
		return invalfc(fc, "no preopen %s", fc->source);
	if (!WASI_FS(get_flags, fd, &flags, &e))
		return wasi_err(e);
	if (!(flags & WASI_FILESYSTEM_TYPES_DESCRIPTOR_FLAGS_MUTATE_DIRECTORY))
		sb->s_flags |= SB_RDONLY;
	err = wasi_stat(fd, &st);
	if (err)
		return err;
	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;
	fs->fd = fd;
	sb->s_fs_info = fs;
	sb->s_magic = WASIFS_MAGIC;
	sb->s_op = &wasifs_sops;
	sb->s_time_gran = 1;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_d_op = &wasifs_dops;
	root = wasifs_iget(sb, &st);
	if (IS_ERR(root))
		return PTR_ERR(root);
	sb->s_root = d_make_root(root);
	return sb->s_root ? 0 : -ENOMEM;
}

static int wasifs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, wasifs_fill_super);
}

static const struct fs_context_operations wasifs_context_ops = {
	.get_tree = wasifs_get_tree,
};

static int wasifs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &wasifs_context_ops;
	return 0;
}

static void wasifs_kill_sb(struct super_block *sb)
{
	/* (the preopen stays the runtime's: not closed) */
	struct wasifs_sb *fs = sb->s_fs_info;

	kill_anon_super(sb);
	kfree(fs);
}

static struct file_system_type wasifs_type = {
	.owner = THIS_MODULE,
	.name = "wasifs",
	.init_fs_context = wasifs_init_fs_context,
	.kill_sb = wasifs_kill_sb,
};

static void wasifs_inode_init_once(void *p)
{
	struct wasifs_inode *wi = p;

	inode_init_once(&wi->vfs_inode);
}

static int __init wasifs_init(void)
{
	wasifs_inode_cache = kmem_cache_create("wasifs_inode", sizeof(struct wasifs_inode), 0,
					       SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					       wasifs_inode_init_once);
	if (!wasifs_inode_cache)
		return -ENOMEM;
	return register_filesystem(&wasifs_type);
}
fs_initcall(wasifs_init);
