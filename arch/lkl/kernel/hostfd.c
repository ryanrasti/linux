// SPDX-License-Identifier: GPL-2.0
/*
 * Host fds as guest files: host_fd(fd, flags) makes a file of the calling
 * process's whose reads and writes the host does on its own fd, such as
 * the program's stdio. As a guest file it is dup'ed, passed on and closed
 * as any other: dup2 onto it or away from it, a pipe in its place, a child
 * inheriting it.
 *
 * The host's I/O is done outside the CPU, with the task asleep in the
 * kernel (TASK_UNINTERRUPTIBLE, as in a disk read): a read of stdin can
 * wait for as long as it takes while the rest of the kernel runs, or idles
 * tickless if nothing else is runnable.
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <asm/cpu.h>
#include <asm/host_ops.h>
#include <asm/sched.h>

static ssize_t hostfd_rw(struct file *file, char __user *buf, size_t len,
			 int write)
{
	struct task_struct *task = current;
	long ret;

	/* only a host task has a host thread to do the I/O on */
	if (!test_thread_flag(TIF_HOST_THREAD) || !lkl_ops->hostfd_rw)
		return -EINVAL;

	/*
	 * Schedule out, as a host task leaving the kernel does: the CPU goes
	 * to the next task, and this thread comes back here without it.
	 */
	thread_sched_jb();
	ret = lkl_ops->hostfd_rw((long)file->private_data, (void *)buf, len,
				 write);
	/* the kernel halted meanwhile: nothing to return to */
	if (lkl_cpu_get() < 0)
		lkl_ops->thread_exit();
	switch_to_host_task(task);

	return ret;
}

static ssize_t hostfd_read(struct file *file, char __user *buf, size_t len,
			   loff_t *pos)
{
	return hostfd_rw(file, buf, len, 0);
}

static ssize_t hostfd_write(struct file *file, const char __user *buf,
			    size_t len, loff_t *pos)
{
	return hostfd_rw(file, (char __user *)buf, len, 1);
}

static const struct file_operations hostfd_fops = {
	.read	= hostfd_read,
	.write	= hostfd_write,
};

/*
 * host_fd(fd, flags): a new descriptor (the lowest free) for host fd fd;
 * flags are the access mode (O_RDONLY, O_WRONLY, O_RDWR) and O_CLOEXEC.
 */
SYSCALL_DEFINE2(host_fd, int, fd, unsigned int, flags)
{
	struct file *file;
	int gfd;

	if (fd < 0 || (flags & ~(O_ACCMODE | O_CLOEXEC)) ||
	    (flags & O_ACCMODE) == O_ACCMODE)
		return -EINVAL;

	gfd = get_unused_fd_flags(flags & O_CLOEXEC);
	if (gfd < 0)
		return gfd;
	/* an inode of its own, so that it can be typed */
	file = anon_inode_create_getfile("[hostfd]", &hostfd_fops,
					 (void *)(long)fd, flags & O_ACCMODE,
					 NULL);
	if (IS_ERR(file)) {
		put_unused_fd(gfd);
		return PTR_ERR(file);
	}
	/*
	 * A character device, as a terminal or /dev/null is: an anon inode
	 * has no file type, which libuv (Node) takes for an unknown handle,
	 * and loses what is written to it.
	 */
	file_inode(file)->i_mode = S_IFCHR | 0600;
	fd_install(gfd, file);

	return gfd;
}
