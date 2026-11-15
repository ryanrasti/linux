// SPDX-License-Identifier: GPL-2.0
/*
 * wasifs: a WASI preopened directory as a filesystem, through WASI's
 * preview 1 file calls, which the kernel makes itself (it is linked into
 * the program, as the console is).
 *
 *   mount -t wasifs <preopen's name> <dir>
 *
 * The source is a preopen's name as the runtime gives it
 * (fd_prestat_dir_name): the mount finds its descriptor by that name.
 *
 * WASI has no inode handles, only paths under a descriptor, so the
 * filesystem is a path client (as a simple 9p or FUSE one): an operation
 * names its file by the dentry's path below the mount's root, resolved by
 * the runtime from the preopen. Inodes are keyed by the host's (dev, ino),
 * so hard links share one. The dcache is trusted for positive dentries;
 * negative ones are looked up again each time, attributes are fetched
 * again by stat and by open. An open file holds a descriptor of its own;
 * reads and writes go to the host directly (pread/pwrite: no page cache;
 * musl's mmap of a file is a pread copy).
 *
 * WASI has no owners or permissions: files are root's, directories 0755,
 * files 0644, symbolic links 0777, and chmod or chown to anything else is
 * EPERM. A preopen without the right to create files mounts read-only.
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

#define WASI(name) \
	__attribute__((import_module("wasi_snapshot_preview1"), import_name(#name)))

#define WASIFS_MAGIC 0x77617369	/* "wasi" */

/* preview 1's types (wasi_snapshot_preview1.witx) */
struct wasi_filestat {
	u64 dev;
	u64 ino;
	u8 filetype;
	u64 nlink;
	u64 size;
	u64 atim;
	u64 mtim;
	u64 ctim;
};

struct wasi_fdstat {
	u8 filetype;
	u16 flags;
	u64 rights_base;
	u64 rights_inheriting;
};

struct wasi_prestat {
	u8 tag;			/* 0: a directory */
	u32 name_len;
};

struct wasi_dirent {
	u64 next;
	u64 ino;
	u32 namlen;
	u8 type;
};

struct wasi_iovec {
	void *buf;
	u32 len;
};

enum {
	WASI_FILETYPE_BLOCK_DEVICE = 1,
	WASI_FILETYPE_CHARACTER_DEVICE = 2,
	WASI_FILETYPE_DIRECTORY = 3,
	WASI_FILETYPE_REGULAR_FILE = 4,
	WASI_FILETYPE_SOCKET_DGRAM = 5,
	WASI_FILETYPE_SOCKET_STREAM = 6,
	WASI_FILETYPE_SYMBOLIC_LINK = 7,
};

#define WASI_OFLAGS_CREAT 1
#define WASI_OFLAGS_DIRECTORY 2
#define WASI_OFLAGS_EXCL 4

#define WASI_FSTFLAGS_ATIM 1
#define WASI_FSTFLAGS_ATIM_NOW 2
#define WASI_FSTFLAGS_MTIM 4
#define WASI_FSTFLAGS_MTIM_NOW 8

#define R(n) (1ull << (n))
#define WASI_RIGHT_PATH_CREATE_FILE R(10)
/* a file's rights to read, and to write (wasi-libc's split) */
#define WASI_RIGHTS_READ (R(1) | R(2) | R(5) | R(7) | R(21) | R(27))
#define WASI_RIGHTS_WRITE (R(0) | R(2) | R(4) | R(5) | R(6) | R(8) | R(21) | R(22) | R(23))
/*
 * a directory's: all a file's or a directory's (no sockets'), less a
 * file's writing (which opens it to write: EISDIR)
 */
#define WASI_RIGHTS_DIR ((R(28) - 1) & ~WASI_RIGHTS_WRITE)

WASI(fd_prestat_get) u16 wasi_fd_prestat_get(u32 fd, struct wasi_prestat *buf);
WASI(fd_prestat_dir_name) u16 wasi_fd_prestat_dir_name(u32 fd, char *path, u32 len);
WASI(fd_fdstat_get) u16 wasi_fd_fdstat_get(u32 fd, struct wasi_fdstat *buf);
WASI(fd_filestat_get) u16 wasi_fd_filestat_get(u32 fd, struct wasi_filestat *buf);
WASI(fd_filestat_set_size) u16 wasi_fd_filestat_set_size(u32 fd, u64 size);
WASI(fd_pread) u16 wasi_fd_pread(u32 fd, const struct wasi_iovec *iovs, u32 n,
				 u64 offset, u32 *nread);
WASI(fd_pwrite) u16 wasi_fd_pwrite(u32 fd, const struct wasi_iovec *iovs, u32 n,
				   u64 offset, u32 *nwritten);
WASI(fd_readdir) u16 wasi_fd_readdir(u32 fd, void *buf, u32 len, u64 cookie,
				     u32 *used);
WASI(fd_sync) u16 wasi_fd_sync(u32 fd);
WASI(fd_datasync) u16 wasi_fd_datasync(u32 fd);
WASI(fd_close) u16 wasi_fd_close(u32 fd);
WASI(path_open) u16 wasi_path_open(u32 fd, u32 dirflags, const char *path,
				   u32 path_len, u16 oflags, u64 rights_base,
				   u64 rights_inheriting, u16 fdflags, u32 *opened);
WASI(path_filestat_get) u16 wasi_path_filestat_get(u32 fd, u32 flags,
						   const char *path, u32 path_len,
						   struct wasi_filestat *buf);
WASI(path_filestat_set_times) u16 wasi_path_filestat_set_times(u32 fd, u32 flags,
							       const char *path,
							       u32 path_len, u64 atim,
							       u64 mtim, u16 fstflags);
WASI(path_create_directory) u16 wasi_path_create_directory(u32 fd, const char *path,
							   u32 path_len);
WASI(path_remove_directory) u16 wasi_path_remove_directory(u32 fd, const char *path,
							   u32 path_len);
WASI(path_unlink_file) u16 wasi_path_unlink_file(u32 fd, const char *path,
						 u32 path_len);
WASI(path_rename) u16 wasi_path_rename(u32 fd, const char *old_path, u32 old_len,
				       u32 new_fd, const char *new_path, u32 new_len);
WASI(path_link) u16 wasi_path_link(u32 old_fd, u32 old_flags, const char *old_path,
				   u32 old_len, u32 new_fd, const char *new_path,
				   u32 new_len);
WASI(path_symlink) u16 wasi_path_symlink(const char *old_path, u32 old_len, u32 fd,
					 const char *new_path, u32 new_len);
WASI(path_readlink) u16 wasi_path_readlink(u32 fd, const char *path, u32 path_len,
					   char *buf, u32 buf_len, u32 *used);

/* WASI's errno (its numbering, 1..76) as Linux's */
static const u8 wasi_errnos[] = {
	0, E2BIG, EACCES, EADDRINUSE, EADDRNOTAVAIL, EAFNOSUPPORT, EAGAIN,
	EALREADY, EBADF, EBADMSG, EBUSY, ECANCELED, ECHILD, ECONNABORTED,
	ECONNREFUSED, ECONNRESET, EDEADLK, EDESTADDRREQ, EDOM, EDQUOT, EEXIST,
	EFAULT, EFBIG, EHOSTUNREACH, EIDRM, EILSEQ, EINPROGRESS, EINTR, EINVAL,
	EIO, EISCONN, EISDIR, ELOOP, EMFILE, EMLINK, EMSGSIZE, EMULTIHOP,
	ENAMETOOLONG, ENETDOWN, ENETRESET, ENETUNREACH, ENFILE, ENOBUFS, ENODEV,
	ENOENT, ENOEXEC, ENOLCK, ENOLINK, ENOMEM, ENOMSG, ENOPROTOOPT, ENOSPC,
	ENOSYS, ENOTCONN, ENOTDIR, ENOTEMPTY, ENOTRECOVERABLE, ENOTSOCK,
	EOPNOTSUPP, ENOTTY, ENXIO, EOVERFLOW, EOWNERDEAD, EPERM, EPIPE, EPROTO,
	EPROTONOSUPPORT, EPROTOTYPE, ERANGE, EROFS, ESPIPE, ESRCH, ESTALE,
	ETIMEDOUT, ETXTBSY, EXDEV,
	EACCES,			/* notcapable: the runtime withheld the right */
};
static_assert(ARRAY_SIZE(wasi_errnos) == 77);

static int wasi_err(u16 e)
{
	return e < ARRAY_SIZE(wasi_errnos) ? -wasi_errnos[e] : -EIO;
}

struct wasifs_sb {
	u32 fd;			/* the preopen's */
};

static u32 root_fd(struct super_block *sb)
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

static struct timespec64 wasi_time(u64 ns)
{
	return ns_to_timespec64(ns);
}

static const struct inode_operations wasifs_dir_iops;
static const struct inode_operations wasifs_file_iops;
static const struct inode_operations wasifs_symlink_iops;
static const struct file_operations wasifs_dir_fops;
static const struct file_operations wasifs_file_fops;
static const struct dentry_operations wasifs_dops;

static umode_t wasi_mode(u8 filetype)
{
	switch (filetype) {
	case WASI_FILETYPE_DIRECTORY:
		return S_IFDIR | 0755;
	case WASI_FILETYPE_SYMBOLIC_LINK:
		return S_IFLNK | 0777;
	case WASI_FILETYPE_BLOCK_DEVICE:
		return S_IFBLK | 0644;
	case WASI_FILETYPE_CHARACTER_DEVICE:
		return S_IFCHR | 0644;
	case WASI_FILETYPE_SOCKET_DGRAM:
	case WASI_FILETYPE_SOCKET_STREAM:
		return S_IFSOCK | 0644;
	default:
		return S_IFREG | 0644;
	}
}

/* the host's attributes into the inode (all but its type, fixed at iget) */
static void wasifs_fill(struct inode *inode, const struct wasi_filestat *st)
{
	set_nlink(inode, st->nlink);
	i_size_write(inode, st->size);
	inode->i_blocks = (st->size + 511) >> 9;
	inode_set_atime_to_ts(inode, wasi_time(st->atim));
	inode_set_mtime_to_ts(inode, wasi_time(st->mtim));
	inode_set_ctime_to_ts(inode, wasi_time(st->ctim));
}

static int wasifs_test(struct inode *inode, void *data)
{
	const struct wasi_filestat *st = data;

	return inode->i_ino == st->ino && inode->i_generation == (u32)st->dev &&
	       (inode->i_mode & S_IFMT) == (wasi_mode(st->filetype) & S_IFMT);
}

static int wasifs_set(struct inode *inode, void *data)
{
	const struct wasi_filestat *st = data;

	inode->i_ino = st->ino;
	inode->i_generation = st->dev;
	return 0;
}

static struct inode *wasifs_iget(struct super_block *sb, struct wasi_filestat *st)
{
	struct inode *inode;

	if (st->ino) {
		inode = iget5_locked(sb, st->ino ^ st->dev, wasifs_test, wasifs_set, st);
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
	inode->i_mode = wasi_mode(st->filetype);
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

static int wasifs_stat(struct dentry *dentry, struct wasi_filestat *st)
{
	WASIFS_PATH(path, dentry);

	if (IS_ERR(path))
		return PTR_ERR(path);
	return wasi_err(wasi_path_filestat_get(root_fd(dentry->d_sb), 0, path,
					       strlen(path), st));
}

/* the dentry's inode, from the host: after it was made */
static int wasifs_instantiate(struct dentry *dentry)
{
	struct wasi_filestat st;
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
	struct wasi_filestat st;
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

static int wasifs_open_path(struct dentry *dentry, u16 oflags, u64 rights, u32 *fd)
{
	WASIFS_PATH(path, dentry);

	if (IS_ERR(path))
		return PTR_ERR(path);
	return wasi_err(wasi_path_open(root_fd(dentry->d_sb), 0, path, strlen(path),
				       oflags, rights,
				       oflags & WASI_OFLAGS_DIRECTORY ? rights : 0,
				       0, fd));
}

static int wasifs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	u32 fd;
	int err = wasifs_open_path(dentry, WASI_OFLAGS_CREAT | WASI_OFLAGS_EXCL,
				   WASI_RIGHTS_READ | WASI_RIGHTS_WRITE, &fd);

	if (err)
		return err;
	wasi_fd_close(fd);
	return wasifs_instantiate(dentry);
}

static int wasifs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode)
{
	WASIFS_PATH(path, dentry);
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	err = wasi_err(wasi_path_create_directory(root_fd(dir->i_sb), path,
						  strlen(path)));
	if (err)
		return err;
	inc_nlink(dir);
	return wasifs_instantiate(dentry);
}

static int wasifs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *target)
{
	WASIFS_PATH(path, dentry);
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	err = wasi_err(wasi_path_symlink(target, strlen(target), root_fd(dir->i_sb),
					 path, strlen(path)));
	return err ?: wasifs_instantiate(dentry);
}

static int wasifs_link(struct dentry *old, struct inode *dir, struct dentry *new)
{
	WASIFS_PATH(from, old);
	WASIFS_PATH(to, new);
	struct inode *inode = d_inode(old);
	int err;

	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	err = wasi_err(wasi_path_link(root_fd(dir->i_sb), 0, from, strlen(from),
				      root_fd(dir->i_sb), to, strlen(to)));
	if (err)
		return err;
	inc_nlink(inode);
	ihold(inode);
	d_instantiate(new, inode);
	return 0;
}

static int wasifs_unlink(struct inode *dir, struct dentry *dentry)
{
	WASIFS_PATH(path, dentry);
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	err = wasi_err(wasi_path_unlink_file(root_fd(dir->i_sb), path, strlen(path)));
	if (!err)
		drop_nlink(d_inode(dentry));
	return err;
}

static int wasifs_rmdir(struct inode *dir, struct dentry *dentry)
{
	WASIFS_PATH(path, dentry);
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	err = wasi_err(wasi_path_remove_directory(root_fd(dir->i_sb), path,
						  strlen(path)));
	if (err)
		return err;
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
	u32 fd = root_fd(old_dir->i_sb);
	int err;

	/* NOREPLACE holds by the dentry (the VFS checked it negative); no EXCHANGE */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	err = wasi_err(wasi_path_rename(fd, from, strlen(from), fd, to, strlen(to)));
	if (err)
		return err;
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
	char *target;
	u32 used;
	int err;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	target = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!target)
		return ERR_PTR(-ENOMEM);
	{
		WASIFS_PATH(path, dentry);

		err = IS_ERR(path) ? PTR_ERR(path) :
		      wasi_err(wasi_path_readlink(root_fd(inode->i_sb), path,
						  strlen(path), target,
						  PATH_MAX - 1, &used));
	}
	if (err) {
		kfree(target);
		return ERR_PTR(err);
	}
	target[used] = 0;
	set_delayed_call(done, kfree_link, target);
	return target;
}

/* the inode's attributes from the host again */
static int wasifs_refresh(struct dentry *dentry)
{
	struct wasi_filestat st;
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

static int wasifs_truncate(struct dentry *dentry, struct iattr *attr)
{
	u32 fd;
	int err;

	/* an open file's own descriptor, when it may write */
	if ((attr->ia_valid & ATTR_FILE) && (attr->ia_file->f_mode & FMODE_WRITE))
		return wasi_err(wasi_fd_filestat_set_size(
			(uintptr_t)attr->ia_file->private_data, attr->ia_size));
	err = wasifs_open_path(dentry, 0, WASI_RIGHTS_WRITE, &fd);
	if (err)
		return err;
	err = wasi_err(wasi_fd_filestat_set_size(fd, attr->ia_size));
	wasi_fd_close(fd);
	return err;
}

static int wasifs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	u16 fst = 0;
	u64 atim = 0, mtim = 0;
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
	if (attr->ia_valid & ATTR_ATIME) {
		fst |= attr->ia_valid & ATTR_ATIME_SET ? WASI_FSTFLAGS_ATIM
						       : WASI_FSTFLAGS_ATIM_NOW;
		atim = timespec64_to_ns(&attr->ia_atime);
	}
	if (attr->ia_valid & ATTR_MTIME) {
		fst |= attr->ia_valid & ATTR_MTIME_SET ? WASI_FSTFLAGS_MTIM
						       : WASI_FSTFLAGS_MTIM_NOW;
		mtim = timespec64_to_ns(&attr->ia_mtime);
	}
	/* (a truncation sets the times itself) */
	if (fst && !(attr->ia_valid & ATTR_SIZE)) {
		WASIFS_PATH(path, dentry);

		if (IS_ERR(path))
			return PTR_ERR(path);
		err = wasi_err(wasi_path_filestat_set_times(root_fd(inode->i_sb), 0,
							    path, strlen(path),
							    atim, mtim, fst));
		if (err)
			return err;
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
static u32 file_fd(struct file *file)
{
	return (uintptr_t)file->private_data;
}

static int wasifs_open(struct inode *inode, struct file *file)
{
	u64 rights = 0;
	u32 fd;
	int err;

	if (file->f_mode & FMODE_READ)
		rights |= WASI_RIGHTS_READ;
	if (file->f_mode & FMODE_WRITE)
		rights |= WASI_RIGHTS_WRITE;
	err = wasifs_open_path(file->f_path.dentry, 0, rights, &fd);
	if (err)
		return err;
	file->private_data = (void *)(uintptr_t)fd;
	return wasifs_refresh(file->f_path.dentry);
}

static int wasifs_release(struct inode *inode, struct file *file)
{
	wasi_fd_close(file_fd(file));
	return 0;
}

/* (a bounce buffer: the iterator may be any kind, a kernel_read's too) */
#define WASIFS_IO_MAX (64 * 1024)

static ssize_t wasifs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	size_t len = min(iov_iter_count(to), (size_t)WASIFS_IO_MAX);
	ssize_t done = 0;
	void *buf;

	if (!len)
		return 0;
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	while (iov_iter_count(to)) {
		struct wasi_iovec iov = { buf, min(iov_iter_count(to), len) };
		u32 got;
		u16 e = wasi_fd_pread(file_fd(iocb->ki_filp), &iov, 1, iocb->ki_pos, &got);

		if (e) {
			if (!done)
				done = wasi_err(e);
			break;
		}
		if (copy_to_iter(buf, got, to) != got) {
			if (!done)
				done = -EFAULT;
			break;
		}
		iocb->ki_pos += got;
		done += got;
		if (got < iov.len)
			break;
	}
	kfree(buf);
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
			struct wasi_iovec iov = { buf + off, n - off };
			u32 put;
			u16 e = wasi_fd_pwrite(file_fd(file), &iov, 1, iocb->ki_pos, &put);

			if (e) {
				ret = wasi_err(e);
				goto stop;
			}
			off += put;
			iocb->ki_pos += put;
			done += put;
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

static int wasifs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return wasi_err(datasync ? wasi_fd_datasync(file_fd(file))
				 : wasi_fd_sync(file_fd(file)));
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

static int wasifs_dir_open(struct inode *inode, struct file *file)
{
	u32 fd;
	int err = wasifs_open_path(file->f_path.dentry, WASI_OFLAGS_DIRECTORY,
				   WASI_RIGHTS_DIR, &fd);

	if (err)
		return err;
	file->private_data = (void *)(uintptr_t)fd;
	return 0;
}

static unsigned char wasi_dtype(u8 filetype)
{
	return fs_umode_to_dtype(wasi_mode(filetype));
}

/*
 * readdir: the file position past "." and ".." is 2 + WASI's cookie of the
 * next entry. A buffer the runtime filled may end in a cut entry: the next
 * call starts at that entry's cookie.
 */
#define WASIFS_DIRBUF 4096

static int wasifs_iterate(struct file *file, struct dir_context *ctx)
{
	char *buf;
	int err = 0;

	if (!dir_emit_dots(file, ctx))
		return 0;
	buf = kmalloc(WASIFS_DIRBUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		u32 used, off = 0;
		u16 e = wasi_fd_readdir(file_fd(file), buf, WASIFS_DIRBUF,
					ctx->pos - 2, &used);

		if (e) {
			err = wasi_err(e);
			break;
		}
		while (off + sizeof(struct wasi_dirent) <= used) {
			struct wasi_dirent d;
			const char *name = buf + off + sizeof(d);

			memcpy(&d, buf + off, sizeof(d));
			if (off + sizeof(d) + d.namlen > used)
				break;
			off += sizeof(d) + d.namlen;
			if (!(d.namlen == 1 && name[0] == '.') &&
			    !(d.namlen == 2 && name[0] == '.' && name[1] == '.') &&
			    !dir_emit(ctx, name, d.namlen, d.ino ?: 1, wasi_dtype(d.type)))
				goto out;
			ctx->pos = 2 + d.next;
		}
		/* a buffer not full is the directory's end */
		if (used < WASIFS_DIRBUF)
			break;
		if (!off) {
			err = -ENAMETOOLONG;
			break;
		}
	}
out:
	kfree(buf);
	return err;
}

static const struct file_operations wasifs_dir_fops = {
	.open = wasifs_dir_open,
	.release = wasifs_release,
	.iterate_shared = wasifs_iterate,
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.fsync = wasifs_fsync,
};

static int wasifs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	/* (WASI has no statfs: the type and limits only) */
	buf->f_type = WASIFS_MAGIC;
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = NAME_MAX;
	return 0;
}

static const struct super_operations wasifs_sops = {
	.statfs = wasifs_statfs,
};

/* the preopen named `name`: its descriptor, or -ENOENT */
static int wasi_preopen(const char *name, u32 *found)
{
	size_t len = strlen(name);
	struct wasi_prestat ps;
	char *buf;
	u32 fd;
	int err = -ENOENT;

	buf = kmalloc(len + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	/* (preopens are numbered from 3, up to the first that isn't one) */
	for (fd = 3; !wasi_fd_prestat_get(fd, &ps); fd++) {
		if (ps.tag || ps.name_len != len ||
		    wasi_fd_prestat_dir_name(fd, buf, len) || memcmp(buf, name, len))
			continue;
		*found = fd;
		err = 0;
		break;
	}
	kfree(buf);
	return err;
}

static int wasifs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct wasifs_sb *fs;
	struct wasi_fdstat fdst;
	struct wasi_filestat st;
	struct inode *root;
	u32 fd;
	int err;

	if (!fc->source)
		return invalfc(fc, "the source is a preopen's name");
	err = wasi_preopen(fc->source, &fd);
	if (err)
		return invalfc(fc, "no preopen %s", fc->source);
	err = wasi_err(wasi_fd_fdstat_get(fd, &fdst));
	if (err)
		return err;
	if (!(fdst.rights_base & WASI_RIGHT_PATH_CREATE_FILE))
		sb->s_flags |= SB_RDONLY;
	err = wasi_err(wasi_fd_filestat_get(fd, &st));
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

static int __init wasifs_init(void)
{
	return register_filesystem(&wasifs_type);
}
fs_initcall(wasifs_init);
