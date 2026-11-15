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

/* wasi:filesystem (asm/wasi.h: the ABI) */
#define FS(name) WASI_IMPORT("wasi:filesystem/types", name)
#define DESC(name) FS("[method]descriptor." name)

enum {
	WASI_TYPE_UNKNOWN,
	WASI_TYPE_BLOCK_DEVICE,
	WASI_TYPE_CHARACTER_DEVICE,
	WASI_TYPE_DIRECTORY,
	WASI_TYPE_FIFO,
	WASI_TYPE_SYMBOLIC_LINK,
	WASI_TYPE_REGULAR_FILE,
	WASI_TYPE_SOCKET,
};

/* descriptor-flags */
#define WASI_READ 1
#define WASI_WRITE 2
#define WASI_MUTATE_DIRECTORY 32

/* open-flags */
#define WASI_OPEN_CREATE 1
#define WASI_OPEN_DIRECTORY 2
#define WASI_OPEN_EXCLUSIVE 4

/* new-timestamp's cases */
enum { WASI_TIME_NO_CHANGE, WASI_TIME_NOW, WASI_TIME_SET };

/* option<datetime> */
struct wasi_time {
	u8 is_some;
	u64 seconds;
	u32 nanoseconds;
};

/* result<descriptor-stat, error-code> */
struct wasi_ret_stat {
	u8 is_err;
	union {
		struct {
			u8 type;
			u64 nlink;
			u64 size;
			struct wasi_time atim, mtim, ctim;
		} ok;
		u8 err;
	};
};

/* result<_, error-code>, and result<descriptor-flags, error-code> */
struct wasi_ret_u8 {
	u8 is_err;
	u8 val;			/* the flags, or the error */
};

/* result<own<descriptor>, error-code> (or a directory-entry-stream) */
struct wasi_ret_handle {
	u8 is_err;
	union {
		s32 ok;
		u8 err;
	};
};

/* result<filesize, error-code> */
struct wasi_ret_size {
	u8 is_err;
	union {
		u64 ok;
		u8 err;
	};
};

/* result<tuple<list<u8>, bool>, error-code>: the bytes, and the file's end */
struct wasi_ret_read {
	u8 is_err;
	union {
		struct {
			u8 *ptr;
			u32 len;
			u8 end;
		} ok;
		u8 err;
	};
};

/* result<metadata-hash-value, error-code> */
struct wasi_ret_hash {
	u8 is_err;
	union {
		struct {
			u64 lower, upper;
		} ok;
		u8 err;
	};
};

/* result<string, error-code> */
struct wasi_ret_string {
	u8 is_err;
	union {
		struct {
			char *ptr;
			u32 len;
		} ok;
		u8 err;
	};
};

/* result<option<directory-entry>, error-code> */
struct wasi_ret_entry {
	u8 is_err;
	union {
		struct {
			u8 is_some;
			struct {
				u8 type;
				char *name;
				u32 len;
			} entry;
		} ok;
		u8 err;
	};
};

/* list<tuple<own<descriptor>, string>>'s */
struct wasi_preopen {
	s32 fd;
	const char *name;
	u32 len;
};

WASI_IMPORT("wasi:filesystem/preopens", "get-directories")
void __wasm_import_wasi_filesystem_preopens_get_directories(void *);
DESC("read")
void __wasm_import_wasi_filesystem_types_method_descriptor_read(s32, s64, s64, void *);
DESC("write")
void __wasm_import_wasi_filesystem_types_method_descriptor_write(s32, const void *, u32, s64, void *);
DESC("stat")
void __wasm_import_wasi_filesystem_types_method_descriptor_stat(s32, void *);
DESC("stat-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_stat_at(s32, s32, const char *, u32, void *);
DESC("metadata-hash")
void __wasm_import_wasi_filesystem_types_method_descriptor_metadata_hash(s32, void *);
DESC("metadata-hash-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_metadata_hash_at(s32, s32, const char *, u32, void *);
DESC("open-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_open_at(s32, s32, const char *, u32, s32, s32, void *);
DESC("get-flags")
void __wasm_import_wasi_filesystem_types_method_descriptor_get_flags(s32, void *);
DESC("set-size")
void __wasm_import_wasi_filesystem_types_method_descriptor_set_size(s32, s64, void *);
DESC("set-times-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_set_times_at(s32, s32, const char *, u32,
									  s32, s64, s32, s32, s64, s32, void *);
DESC("sync")
void __wasm_import_wasi_filesystem_types_method_descriptor_sync(s32, void *);
DESC("sync-data")
void __wasm_import_wasi_filesystem_types_method_descriptor_sync_data(s32, void *);
DESC("read-directory")
void __wasm_import_wasi_filesystem_types_method_descriptor_read_directory(s32, void *);
FS("[method]directory-entry-stream.read-directory-entry")
void __wasm_import_wasi_filesystem_types_method_directory_entry_stream_read_directory_entry(s32, void *);
DESC("create-directory-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_create_directory_at(s32, const char *, u32, void *);
DESC("remove-directory-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_remove_directory_at(s32, const char *, u32, void *);
DESC("unlink-file-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_unlink_file_at(s32, const char *, u32, void *);
DESC("rename-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_rename_at(s32, const char *, u32, s32,
								       const char *, u32, void *);
DESC("link-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_link_at(s32, s32, const char *, u32, s32,
								     const char *, u32, void *);
DESC("symlink-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_symlink_at(s32, const char *, u32,
									const char *, u32, void *);
DESC("readlink-at")
void __wasm_import_wasi_filesystem_types_method_descriptor_readlink_at(s32, const char *, u32, void *);
FS("[resource-drop]descriptor")
void __wasm_import_wasi_filesystem_types_descriptor_drop(s32);
FS("[resource-drop]directory-entry-stream")
void __wasm_import_wasi_filesystem_types_directory_entry_stream_drop(s32);

#define WASI_FS(name, ...) \
	__wasm_import_wasi_filesystem_types_method_descriptor_##name(__VA_ARGS__)

/* wasi:filesystem's error-code (its order) as Linux's errno */
static const u8 wasi_errnos[] = {
	EACCES, EAGAIN, EALREADY, EBADF, EBUSY, EDEADLK, EDQUOT, EEXIST, EFBIG,
	EILSEQ, EINPROGRESS, EINTR, EINVAL, EIO, EISDIR, ELOOP, EMLINK,
	EMSGSIZE, ENAMETOOLONG, ENODEV, ENOENT, ENOLCK, ENOMEM, ENOSPC,
	ENOTDIR, ENOTEMPTY, ENOTRECOVERABLE, EOPNOTSUPP, ENOTTY, ENXIO,
	EOVERFLOW, EPERM, EPIPE, EROFS, ESPIPE, ETXTBSY, EXDEV,
};
static_assert(ARRAY_SIZE(wasi_errnos) == 37);

static int wasi_err(u8 e)
{
	return e < ARRAY_SIZE(wasi_errnos) ? -wasi_errnos[e] : -EIO;
}

/* a result<_, error-code>'s errno */
static int wasi_ret(const struct wasi_ret_u8 *r)
{
	return r->is_err ? wasi_err(r->val) : 0;
}

/* a file's attributes: its stat and its identity (the metadata hash's) */
struct wasifs_stat {
	u32 ino;		/* 0: the host has none */
	u8 type;
	u64 nlink;
	u64 size;
	struct timespec64 atime, mtime, ctime;
};

static struct timespec64 wasi_timespec(const struct wasi_time *t)
{
	return t->is_some ? (struct timespec64){ t->seconds, t->nanoseconds }
			  : (struct timespec64){};
}

/*
 * the hash as an inode number (i_ino's 32 bits): its lower half's (the
 * upper may be derived from it: Wasmtime's is the lower xor a constant)
 */
static u32 wasi_ino(const struct wasi_ret_hash *h)
{
	return h->is_err ? 0 : (u32)(h->ok.lower ^ (h->ok.lower >> 32));
}

static int wasifs_stat_of(const struct wasi_ret_stat *r, const struct wasi_ret_hash *h,
			  struct wasifs_stat *st)
{
	if (r->is_err)
		return wasi_err(r->err);
	st->ino = wasi_ino(h);
	st->type = r->ok.type;
	st->nlink = r->ok.nlink;
	st->size = r->ok.size;
	st->atime = wasi_timespec(&r->ok.atim);
	st->mtime = wasi_timespec(&r->ok.mtim);
	st->ctime = wasi_timespec(&r->ok.ctim);
	return 0;
}

/* path's attributes, below the descriptor fd (a symbolic link's own) */
static int wasi_stat_at(s32 fd, const char *path, struct wasifs_stat *st)
{
	struct wasi_ret_stat r;
	struct wasi_ret_hash h;

	WASI_FS(stat_at, fd, 0, path, strlen(path), &r);
	if (!r.is_err)
		WASI_FS(metadata_hash_at, fd, 0, path, strlen(path), &h);
	return wasifs_stat_of(&r, &h, st);
}

/* the descriptor's own */
static int wasi_stat(s32 fd, struct wasifs_stat *st)
{
	struct wasi_ret_stat r;
	struct wasi_ret_hash h;

	WASI_FS(stat, fd, &r);
	if (!r.is_err)
		WASI_FS(metadata_hash, fd, &h);
	return wasifs_stat_of(&r, &h, st);
}

static void wasi_close(s32 fd)
{
	__wasm_import_wasi_filesystem_types_descriptor_drop(fd);
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
	case WASI_TYPE_DIRECTORY:
		return S_IFDIR | 0755;
	case WASI_TYPE_SYMBOLIC_LINK:
		return S_IFLNK | 0777;
	case WASI_TYPE_BLOCK_DEVICE:
		return S_IFBLK | 0644;
	case WASI_TYPE_CHARACTER_DEVICE:
		return S_IFCHR | 0644;
	case WASI_TYPE_FIFO:
		return S_IFIFO | 0644;
	case WASI_TYPE_SOCKET:
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

static int wasifs_test(struct inode *inode, void *data)
{
	const struct wasifs_stat *st = data;

	return inode->i_ino == st->ino &&
	       (inode->i_mode & S_IFMT) == (wasi_mode(st->type) & S_IFMT);
}

static int wasifs_set(struct inode *inode, void *data)
{
	inode->i_ino = ((const struct wasifs_stat *)data)->ino;
	return 0;
}

static struct inode *wasifs_iget(struct super_block *sb, struct wasifs_stat *st)
{
	struct inode *inode;

	if (st->ino) {
		inode = iget5_locked(sb, st->ino, wasifs_test, wasifs_set, st);
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
static int wasifs_open_path(struct dentry *dentry, s32 oflags, s32 flags, s32 *fd)
{
	WASIFS_PATH(path, dentry);
	struct wasi_ret_handle r;

	if (IS_ERR(path))
		return PTR_ERR(path);
	WASI_FS(open_at, root_fd(dentry->d_sb), 0, path, strlen(path), oflags,
		flags, &r);
	if (r.is_err)
		return wasi_err(r.err);
	*fd = r.ok;
	return 0;
}

static int wasifs_create(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, bool excl)
{
	s32 fd;
	int err = wasifs_open_path(dentry, WASI_OPEN_CREATE | WASI_OPEN_EXCLUSIVE,
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
	struct wasi_ret_u8 r;
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	WASI_FS(create_directory_at, root_fd(dir->i_sb), path, strlen(path), &r);
	err = wasi_ret(&r);
	if (err)
		return err;
	inc_nlink(dir);
	return wasifs_instantiate(dentry);
}

static int wasifs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, const char *target)
{
	WASIFS_PATH(path, dentry);
	struct wasi_ret_u8 r;
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	WASI_FS(symlink_at, root_fd(dir->i_sb), target, strlen(target), path,
		strlen(path), &r);
	err = wasi_ret(&r);
	return err ?: wasifs_instantiate(dentry);
}

static int wasifs_link(struct dentry *old, struct inode *dir, struct dentry *new)
{
	WASIFS_PATH(from, old);
	WASIFS_PATH(to, new);
	struct inode *inode = d_inode(old);
	struct wasi_ret_u8 r;
	int err;

	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	WASI_FS(link_at, root_fd(dir->i_sb), 0, from, strlen(from),
		root_fd(dir->i_sb), to, strlen(to), &r);
	err = wasi_ret(&r);
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
	struct wasi_ret_u8 r;
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	WASI_FS(unlink_file_at, root_fd(dir->i_sb), path, strlen(path), &r);
	err = wasi_ret(&r);
	if (!err)
		drop_nlink(d_inode(dentry));
	return err;
}

static int wasifs_rmdir(struct inode *dir, struct dentry *dentry)
{
	WASIFS_PATH(path, dentry);
	struct wasi_ret_u8 r;
	int err;

	if (IS_ERR(path))
		return PTR_ERR(path);
	WASI_FS(remove_directory_at, root_fd(dir->i_sb), path, strlen(path), &r);
	err = wasi_ret(&r);

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
	s32 fd = root_fd(old_dir->i_sb);
	struct wasi_ret_u8 r;
	int err;

	/* NOREPLACE holds by the dentry (the VFS checked it negative); no EXCHANGE */
	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (IS_ERR(from))
		return PTR_ERR(from);
	if (IS_ERR(to))
		return PTR_ERR(to);
	WASI_FS(rename_at, fd, from, strlen(from), fd, to, strlen(to), &r);
	err = wasi_ret(&r);
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
	struct wasi_ret_string r;
	char *target;

	if (!dentry)
		return ERR_PTR(-ECHILD);
	{
		WASIFS_PATH(path, dentry);

		if (IS_ERR(path))
			return path;
		WASI_FS(readlink_at, root_fd(inode->i_sb), path, strlen(path), &r);
	}
	if (r.is_err)
		return ERR_PTR(wasi_err(r.err));
	target = r.ok.len < PATH_MAX ? kmemdup_nul(r.ok.ptr, r.ok.len, GFP_KERNEL)
				     : ERR_PTR(-ENAMETOOLONG);
	lkl_ops->mem_free(r.ok.ptr);
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
	struct wasi_ret_u8 r;

	WASI_FS(set_size, fd, size, &r);
	return wasi_ret(&r);
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

/* an attribute's change of a time: new-timestamp's case */
static s32 wasi_new_time(struct iattr *attr, unsigned int valid, unsigned int set)
{
	if (!(attr->ia_valid & valid))
		return WASI_TIME_NO_CHANGE;
	return attr->ia_valid & set ? WASI_TIME_SET : WASI_TIME_NOW;
}

static int wasifs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	s32 atim = wasi_new_time(attr, ATTR_ATIME, ATTR_ATIME_SET);
	s32 mtim = wasi_new_time(attr, ATTR_MTIME, ATTR_MTIME_SET);
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
	if ((atim || mtim) && !(attr->ia_valid & ATTR_SIZE)) {
		WASIFS_PATH(path, dentry);
		struct wasi_ret_u8 r;

		if (IS_ERR(path))
			return PTR_ERR(path);
		WASI_FS(set_times_at, root_fd(inode->i_sb), 0, path, strlen(path),
			atim, attr->ia_atime.tv_sec, attr->ia_atime.tv_nsec,
			mtim, attr->ia_mtime.tv_sec, attr->ia_mtime.tv_nsec, &r);
		err = wasi_ret(&r);
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
static s32 file_fd(struct file *file)
{
	return (uintptr_t)file->private_data;
}

static int wasifs_open(struct inode *inode, struct file *file)
{
	s32 flags = 0, fd;
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
		struct wasi_ret_read r;
		size_t got;

		WASI_FS(read, file_fd(iocb->ki_filp), want, iocb->ki_pos, &r);
		if (r.is_err) {
			if (!done)
				done = wasi_err(r.err);
			break;
		}
		got = copy_to_iter(r.ok.ptr, r.ok.len, to);
		lkl_ops->mem_free(r.ok.ptr);
		if (got != r.ok.len) {
			if (!done)
				done = -EFAULT;
			break;
		}
		iocb->ki_pos += got;
		done += got;
		if (got < want || r.ok.end)
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
			struct wasi_ret_size r;

			WASI_FS(write, file_fd(file), buf + off, n - off, iocb->ki_pos, &r);
			if (r.is_err) {
				ret = wasi_err(r.err);
				goto stop;
			}
			off += r.ok;
			iocb->ki_pos += r.ok;
			done += r.ok;
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
	struct wasi_ret_u8 r;

	if (datasync)
		WASI_FS(sync_data, fd, &r);
	else
		WASI_FS(sync, fd, &r);
	return wasi_ret(&r);
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
	s32 stream;		/* 0: none */
	loff_t pos;
	struct {
		u8 type;
		char *name;	/* NULL: none */
		u32 len;
	} next;
};

static void wasifs_dir_next_free(struct wasifs_dir *d)
{
	lkl_ops->mem_free(d->next.name);
	d->next.name = NULL;
}

static void wasifs_dir_stream_drop(struct wasifs_dir *d)
{
	wasifs_dir_next_free(d);
	if (d->stream)
		__wasm_import_wasi_filesystem_types_directory_entry_stream_drop(d->stream);
	d->stream = 0;
}

static int wasifs_dir_open(struct inode *inode, struct file *file)
{
	struct wasifs_dir *d = kzalloc(sizeof(*d), GFP_KERNEL);
	int err;

	if (!d)
		return -ENOMEM;
	err = wasifs_open_path(file->f_path.dentry, WASI_OPEN_DIRECTORY, WASI_READ,
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
static u32 wasifs_entry_ino(struct file *file, const char *name, u32 len)
{
	WASIFS_PATH(dir, file->f_path.dentry);
	struct wasi_ret_hash h;
	char *path;
	u32 ino;

	if (IS_ERR(dir))
		return 0;
	path = strcmp(dir, ".") ? kasprintf(GFP_KERNEL, "%s/%.*s", dir, len, name)
				: kmemdup_nul(name, len, GFP_KERNEL);
	if (!path)
		return 0;
	WASI_FS(metadata_hash_at, root_fd(file->f_path.dentry->d_sb), 0, path,
		strlen(path), &h);
	ino = wasi_ino(&h);
	kfree(path);
	return ino;
}

static int wasifs_iterate(struct file *file, struct dir_context *ctx)
{
	struct wasifs_dir *d = file->private_data;

	if (!dir_emit_dots(file, ctx))
		return 0;
	if (!d->stream || d->pos != ctx->pos) {
		struct wasi_ret_handle r;

		wasifs_dir_stream_drop(d);
		WASI_FS(read_directory, d->fd, &r);
		if (r.is_err)
			return wasi_err(r.err);
		d->stream = r.ok;
		d->pos = 2;
	}
	for (;;) {
		if (!d->next.name) {
			struct wasi_ret_entry r;

			__wasm_import_wasi_filesystem_types_method_directory_entry_stream_read_directory_entry(
				d->stream, &r);
			if (r.is_err)
				return wasi_err(r.err);
			if (!r.ok.is_some)
				return 0;
			d->next.type = r.ok.entry.type;
			d->next.name = r.ok.entry.name;
			d->next.len = r.ok.entry.len;
		}
		if (d->pos >= ctx->pos &&
		    !dir_emit(ctx, d->next.name, d->next.len,
			      wasifs_entry_ino(file, d->next.name, d->next.len) ?: 1,
			      wasi_dtype(d->next.type)))
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

static const struct super_operations wasifs_sops = {
	.statfs = wasifs_statfs,
};

/* the preopens (wasi:filesystem/preopens'), got at the first mount, kept */
static struct wasi_preopen *preopens;
static u32 preopens_n;

/* the preopen named `name`: its descriptor, or -ENOENT */
static int wasi_preopen(const char *name, s32 *found)
{
	size_t len = strlen(name);
	u32 i;

	if (!preopens) {
		struct { struct wasi_preopen *ptr; u32 len; } l;

		__wasm_import_wasi_filesystem_preopens_get_directories(&l);
		preopens = l.ptr;
		preopens_n = l.len;
	}
	for (i = 0; i < preopens_n; i++) {
		if (preopens[i].len == len && !memcmp(preopens[i].name, name, len)) {
			*found = preopens[i].fd;
			return 0;
		}
	}
	return -ENOENT;
}

static int wasifs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct wasifs_sb *fs;
	struct wasi_ret_u8 flags;
	struct wasifs_stat st;
	struct inode *root;
	s32 fd;
	int err;

	if (!fc->source)
		return invalfc(fc, "the source is a preopen's name");
	err = wasi_preopen(fc->source, &fd);
	if (err)
		return invalfc(fc, "no preopen %s", fc->source);
	WASI_FS(get_flags, fd, &flags);
	err = wasi_ret(&flags);
	if (err)
		return err;
	if (!(flags.val & WASI_MUTATE_DIRECTORY))
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

static int __init wasifs_init(void)
{
	return register_filesystem(&wasifs_type);
}
fs_initcall(wasifs_init);
