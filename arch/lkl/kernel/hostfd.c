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
 * tickless if nothing else is runnable. A nonblocking file's (O_NONBLOCK,
 * at host_fd or by fcntl later) doesn't wait: the host returns -EAGAIN if
 * its fd isn't ready, as a pipe's read would (tokio reads to EAGAIN after
 * each edge; a read that waited instead stalled its runtime).
 */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <asm/cpu.h>
#include <asm/host_ops.h>
#include <asm/sched.h>

static ssize_t hostfd_rw(struct file *file, char __user *buf, size_t len,
			 int write)
{
	struct task_struct *task = current;
	ssize_t done = 0;
	long ret;
	int flags;

	/* only a host task has a host thread to do the I/O on */
	if (!test_thread_flag(TIF_HOST_THREAD) || !lkl_ops->hostfd_rw)
		return -EINVAL;

	for (;;) {
		flags = write ? LKL_HOSTFD_WRITE : 0;
		/* (f_flags read once a call: an fcntl meanwhile applies to the next) */
		if (READ_ONCE(file->f_flags) & O_NONBLOCK)
			flags |= LKL_HOSTFD_NONBLOCK;
		/*
		 * Schedule out, as a host task leaving the kernel does: the CPU
		 * goes to the next task, and this thread comes back here without
		 * it.
		 */
		thread_sched_jb();
		ret = lkl_ops->hostfd_rw((long)file->private_data, buf + done,
					 len - done, flags);
		/* the kernel halted meanwhile: nothing to return to */
		if (lkl_cpu_get() < 0)
			lkl_ops->thread_exit();
		switch_to_host_task(task);

		/*
		 * A read is one read, and a nonblocking write takes what the fd
		 * takes now. A blocking write is whole, as a pipe's (pipe_write):
		 * the host may take it in pieces, or wait without taking any
		 * (-ERESTARTSYS), and it is asked for the rest until it is done,
		 * or a signal comes, which cuts it short at what was written. A
		 * host whose threads share one CPU (exobox's coroutines, or its
		 * token) can't wait in a write(2) a full pipe doesn't take whole:
		 * it writes what the pipe takes, and waits for the rest outside
		 * the write, briefly (the task sleeps uninterruptibly meanwhile,
		 * out of a signal's reach), then returns.
		 */
		if (!write || (flags & LKL_HOSTFD_NONBLOCK) || !ret)
			break;
		if (ret > 0)
			done += ret;
		else if (ret != -ERESTARTSYS || !done)
			break;
		if (done >= len || signal_pending(current))
			break;
	}

	return done ? done : ret;
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

/*
 * Every poller of a host fd file waits here: lkl_hostfd_ready wakes them
 * all, and each asks the host again. One queue for all of them is enough,
 * as a wakeup only makes a poller look again.
 */
static DECLARE_WAIT_QUEUE_HEAD(hostfd_wait);

/*
 * The host says what the fd has ready now (hostfd_poll), watching it for
 * the rest: so the guest's poll, select and epoll see a host fd, such as a
 * channel the host passed in, become readable without a read blocked on
 * it.
 */
static __poll_t hostfd_poll(struct file *file, poll_table *wait)
{
	__poll_t want = 0, ready;

	if (!lkl_ops->hostfd_poll)
		return EPOLLIN | EPOLLRDNORM | EPOLLOUT | EPOLLWRNORM;
	poll_wait(file, &hostfd_wait, wait);
	if (file->f_mode & FMODE_READ)
		want |= EPOLLIN;
	if (file->f_mode & FMODE_WRITE)
		want |= EPOLLOUT;
	/* (POLLHUP and POLLERR whatever was asked, as poll(2) gives them) */
	ready = lkl_ops->hostfd_poll((long)file->private_data, want) &
		(want | EPOLLHUP | EPOLLERR);
	if (ready & EPOLLIN)
		ready |= EPOLLRDNORM;
	if (ready & EPOLLOUT)
		ready |= EPOLLWRNORM;
	return ready;
}

void lkl_hostfd_ready(int fd)
{
	if (lkl_cpu_get() < 0)
		return;
	wake_up_interruptible_all(&hostfd_wait);
	lkl_cpu_put();
}

static const struct file_operations hostfd_fops = {
	.read	= hostfd_read,
	.write	= hostfd_write,
	.poll	= hostfd_poll,
};

/*
 * host_fd(fd, flags): a new descriptor (the lowest free) for host fd fd;
 * flags are the access mode (O_RDONLY, O_WRONLY, O_RDWR), O_CLOEXEC and
 * O_NONBLOCK.
 */
SYSCALL_DEFINE2(host_fd, int, fd, unsigned int, flags)
{
	struct file *file;
	int gfd;

	if (fd < 0 || (flags & ~(O_ACCMODE | O_CLOEXEC | O_NONBLOCK)) ||
	    (flags & O_ACCMODE) == O_ACCMODE)
		return -EINVAL;

	gfd = get_unused_fd_flags(flags & O_CLOEXEC);
	if (gfd < 0)
		return gfd;
	/* an inode of its own, so that it can be typed */
	file = anon_inode_create_getfile("[hostfd]", &hostfd_fops,
					 (void *)(long)fd,
					 flags & (O_ACCMODE | O_NONBLOCK), NULL);
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
