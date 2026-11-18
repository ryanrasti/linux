#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#include <lkl.h>
#include <lkl_host.h>

#include <sys/stat.h>
#include <fcntl.h>
#ifndef __MINGW32__
#include <sys/mman.h>
#endif
#if defined(__FreeBSD__)
#include <net/if.h>
#include <sys/ioctl.h>
#elif __linux
#include <sys/epoll.h>
#include <sys/ioctl.h>
#elif __MINGW32__
#include <windows.h>
#endif

#include <lkl/linux/mman.h>

#include "test.h"

#ifndef __MINGW32__
#define sleep_ns 87654321
int lkl_test_nanosleep(void)
{
	struct __lkl__kernel_timespec ts = {
		.tv_sec = 0,
		.tv_nsec = sleep_ns,
	};
	struct timespec start, stop;
	long delta;
	long ret;

	clock_gettime(CLOCK_MONOTONIC, &start);
	ret = lkl_sys_nanosleep(&ts, NULL);
	clock_gettime(CLOCK_MONOTONIC, &stop);

	delta = 1e9*(stop.tv_sec - start.tv_sec) +
		(stop.tv_nsec - start.tv_nsec);

	lkl_test_logf("sleep %ld (ret=%ld), expected sleep %d\n",
		      delta, ret, sleep_ns);

	if (ret == 0 && delta > sleep_ns * 0.9)
		return TEST_SUCCESS;

	return TEST_FAILURE;
}
#endif

#ifndef __MINGW32__
/*
 * An idle kernel is tickless: while it sleeps, the host's timer fires for
 * the next timer, not at every tick (HZ times a second: the idle host task
 * kept the tick running), and is never armed further than a host's
 * now + delta can add without wrapping. main() counts the timer's fires.
 */
static void (*timer_fn)(void);
static int timer_fires;
static int (*tickless_timer_set)(void *timer, unsigned long ns);
static unsigned long tickless_max;

static void counting_timer_fn(void)
{
	__sync_fetch_and_add(&timer_fires, 1);
	timer_fn();
}

static void *(*host_timer_alloc)(void (*fn)(void));

/* the kernel has one timer, its clockevent's */
static void *counting_timer_alloc(void (*fn)(void))
{
	timer_fn = fn;
	return host_timer_alloc(counting_timer_fn);
}

static int tickless_timer_set_oneshot(void *timer, unsigned long ns)
{
	if (ns > tickless_max)
		tickless_max = ns;
	return tickless_timer_set(timer, ns);
}

static int lkl_test_tickless_idle(void)
{
	struct __lkl__kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
	long ret;
	int fires;

	tickless_timer_set = lkl_host_ops.timer_set_oneshot;
	lkl_host_ops.timer_set_oneshot = tickless_timer_set_oneshot;
	fires = timer_fires;
	ret = lkl_sys_nanosleep(&ts, NULL);
	fires = timer_fires - fires;
	lkl_host_ops.timer_set_oneshot = tickless_timer_set;

	lkl_test_logf("nanosleep 100 ms = %ld: the timer fired %d times, armed at most %lu ns ahead\n",
		      ret, fires, tickless_max);
	/* ticking, it fires 10 times at HZ 100, 100 at 1000 */
	if (ret || fires > 5 || tickless_max > (~0UL >> 2))
		return TEST_FAILURE;
	return TEST_SUCCESS;
}
#endif

LKL_TEST_CALL(getpid, lkl_sys_getpid, 1)
/*
 * The thread that started the kernel is init itself, not another thread of
 * PID 1's process: its tid is 1 and it has no parent.
 */
LKL_TEST_CALL(gettid, lkl_sys_gettid, 1)
LKL_TEST_CALL(getppid, lkl_sys_getppid, 0)

void check_latency(long (*f)(void), long *min, long *max, long *avg)
{
	int i;
	unsigned long long start, stop, sum = 0;
	static const int count = 1000;
	long delta;

	*min = 1000000000;
	*max = -1;

	for (i = 0; i < count; i++) {
		start = lkl_host_ops.time();
		f();
		stop = lkl_host_ops.time();
		delta = stop - start;
		if (*min > delta)
			*min = delta;
		if (*max < delta)
			*max = delta;
		sum += delta;
	}
	*avg = sum / count;
}

static long native_getpid(void)
{
#ifdef __MINGW32__
	GetCurrentProcessId();
#else
	getpid();
#endif
	return 0;
}

int lkl_test_syscall_latency(void)
{
	long min, max, avg;

	lkl_test_logf("avg/min/max: ");

	check_latency(lkl_sys_getpid, &min, &max, &avg);

	lkl_test_logf("lkl:%ld/%ld/%ld ", avg, min, max);

	check_latency(native_getpid, &min, &max, &avg);

	lkl_test_logf("native:%ld/%ld/%ld\n", avg, min, max);

	return TEST_SUCCESS;
}

#define access_rights 0721

/*
 * lkl_syscall() takes any number: out of range, it fails rather than read
 * a handler from outside the syscall table (stock read it before checking).
 */
static int lkl_test_syscall_bounds(void)
{
	long bad[] = { -1, __lkl__NR_syscalls, (long)(~0UL >> 2) };
	long params[6] = { 0 };
	unsigned int i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		long ret = lkl_syscall(bad[i], params);

		lkl_test_logf("lkl_syscall(%ld) = %ld\n", bad[i], ret);
		if (ret != -LKL_ENOSYS)
			return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

LKL_TEST_CALL(creat, lkl_sys_creat, 0, "/file", access_rights)
LKL_TEST_CALL(close, lkl_sys_close, 0, 0);
LKL_TEST_CALL(failopen, lkl_sys_open, -LKL_ENOENT, "/file2", 0, 0);
LKL_TEST_CALL(umask, lkl_sys_umask, 022,  0777);
LKL_TEST_CALL(umask2, lkl_sys_umask, 0777, 0);
LKL_TEST_CALL(open, lkl_sys_open, 0, "/file", LKL_O_RDWR, 0);
static const char wrbuf[] = "test";
LKL_TEST_CALL(write, lkl_sys_write, sizeof(wrbuf), 0, wrbuf, sizeof(wrbuf));
LKL_TEST_CALL(lseek_cur, lkl_sys_lseek, sizeof(wrbuf), 0, 0, LKL_SEEK_CUR);
LKL_TEST_CALL(lseek_end, lkl_sys_lseek, sizeof(wrbuf), 0, 0, LKL_SEEK_END);
LKL_TEST_CALL(lseek_set, lkl_sys_lseek, 0, 0, 0, LKL_SEEK_SET);

int lkl_test_read(void)
{
	char buf[10] = { 0, };
	long ret;

	ret = lkl_sys_read(0, buf, sizeof(buf));

	lkl_test_logf("lkl_sys_read=%ld buf=%s\n", ret, buf);

	if (ret == sizeof(wrbuf) && !strcmp(wrbuf, buf))
		return TEST_SUCCESS;

	return TEST_FAILURE;
}

int lkl_test_fstat(void)
{
	struct lkl_stat stat;
	long ret;

	ret = lkl_sys_fstat(0, &stat);

	lkl_test_logf("lkl_sys_fstat=%ld mode=%o size=%zd\n", ret, stat.st_mode,
		      stat.st_size);

	if (ret == 0 && stat.st_size == sizeof(wrbuf) &&
	    stat.st_mode == (access_rights | LKL_S_IFREG))
		return TEST_SUCCESS;

	return TEST_FAILURE;
}

LKL_TEST_CALL(mkdir, lkl_sys_mkdir, 0, "/mnt", access_rights)

int lkl_test_stat(void)
{
	struct lkl_stat stat;
	long ret;

	ret = lkl_sys_stat("/mnt", &stat);

	lkl_test_logf("lkl_sys_stat(\"/mnt\")=%ld mode=%o\n", ret,
		      stat.st_mode);

	if (ret == 0 && stat.st_mode == (access_rights | LKL_S_IFDIR))
		return TEST_SUCCESS;

	return TEST_FAILURE;
}

static int lkl_test_pipe2(void)
{
	int pipe_fds[2];
	int READ_IDX = 0, WRITE_IDX = 1;
	const char msg[] = "Hello world!";
	char str[20];
	int msg_len_bytes = strlen(msg) + 1;
	int cmp_res;
	long ret;

	ret = lkl_sys_pipe2(pipe_fds, LKL_O_NONBLOCK);
	if (ret) {
		lkl_test_logf("pipe2: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	ret = lkl_sys_write(pipe_fds[WRITE_IDX], msg, msg_len_bytes);
	if (ret != msg_len_bytes) {
		if (ret < 0)
			lkl_test_logf("write error: %s\n", lkl_strerror(ret));
		else
			lkl_test_logf("short write: %ld\n", ret);
		return TEST_FAILURE;
	}

	ret = lkl_sys_read(pipe_fds[READ_IDX], str, msg_len_bytes);
	if (ret != msg_len_bytes) {
		if (ret < 0)
			lkl_test_logf("read error: %s\n", lkl_strerror(ret));
		else
			lkl_test_logf("short read: %ld\n", ret);
		return TEST_FAILURE;
	}

	cmp_res = memcmp(msg, str, msg_len_bytes);
	if (cmp_res) {
		lkl_test_logf("memcmp failed: %d\n", cmp_res);
		return TEST_FAILURE;
	}

	ret = lkl_sys_close(pipe_fds[0]);
	if (ret) {
		lkl_test_logf("close error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	ret = lkl_sys_close(pipe_fds[1]);
	if (ret) {
		lkl_test_logf("close error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

static int lkl_test_epoll(void)
{
	int epoll_fd, pipe_fds[2];
	int READ_IDX = 0, WRITE_IDX = 1;
	struct lkl_epoll_event wait_on, read_result;
	const char msg[] = "Hello world!";
	long ret;

	memset(&wait_on, 0, sizeof(wait_on));
	memset(&read_result, 0, sizeof(read_result));

	ret = lkl_sys_pipe2(pipe_fds, LKL_O_NONBLOCK);
	if (ret) {
		lkl_test_logf("pipe2 error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	epoll_fd = lkl_sys_epoll_create(1);
	if (epoll_fd < 0) {
		lkl_test_logf("epoll_create error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	wait_on.events = LKL_POLLIN | LKL_POLLOUT;
	wait_on.data = pipe_fds[READ_IDX];

	ret = lkl_sys_epoll_ctl(epoll_fd, LKL_EPOLL_CTL_ADD, pipe_fds[READ_IDX],
				&wait_on);
	if (ret < 0) {
		lkl_test_logf("epoll_ctl error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	/* Shouldn't be ready before we have written something */
	ret = lkl_sys_epoll_wait(epoll_fd, &read_result, 1, 0);
	if (ret != 0) {
		if (ret < 0)
			lkl_test_logf("epoll_wait error: %s\n",
				      lkl_strerror(ret));
		else
			lkl_test_logf("epoll_wait: bad event: 0x%lx\n", ret);
		return TEST_FAILURE;
	}

	ret = lkl_sys_write(pipe_fds[WRITE_IDX], msg, strlen(msg) + 1);
	if (ret < 0) {
		lkl_test_logf("write error: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	/* We expect exactly 1 fd to be ready immediately */
	ret = lkl_sys_epoll_wait(epoll_fd, &read_result, 1, 0);
	if (ret != 1) {
		if (ret < 0)
			lkl_test_logf("epoll_wait error: %s\n",
				      lkl_strerror(ret));
		else
			lkl_test_logf("epoll_wait: bad ev no %ld\n", ret);
		return TEST_FAILURE;
	}

	/* Already tested reading from pipe2 so no need to do it
	 * here */

	return TEST_SUCCESS;
}

LKL_TEST_CALL(chdir_proc, lkl_sys_chdir, 0, "proc");

static int dir_fd;

static int lkl_test_open_cwd(void)
{
	dir_fd = lkl_sys_open(".", LKL_O_RDONLY | LKL_O_DIRECTORY, 0);
	if (dir_fd < 0) {
		lkl_test_logf("failed to open current directory: %s\n",
			      lkl_strerror(dir_fd));
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

/* column where to insert a line break for the list file tests below. */
#define COL_LINE_BREAK 70

static int lkl_test_getdents64(void)
{
	long ret;
	char buf[1024], *pos;
	struct lkl_linux_dirent64 *de;
	int wr;

	de = (struct lkl_linux_dirent64 *)buf;
	ret = lkl_sys_getdents64(dir_fd, de, sizeof(buf));

	wr = lkl_test_logf("%d ", dir_fd);

	if (ret < 0)
		return TEST_FAILURE;

	for (pos = buf; pos - buf < ret; pos += de->d_reclen) {
		de = (struct lkl_linux_dirent64 *)pos;

		wr += lkl_test_logf("%s ", de->d_name);
		if (wr >= COL_LINE_BREAK) {
			lkl_test_logf("\n");
			wr = 0;
		}
	}

	return TEST_SUCCESS;
}

LKL_TEST_CALL(close_dir_fd, lkl_sys_close, 0, dir_fd);
LKL_TEST_CALL(chdir_root, lkl_sys_chdir, 0, "/");
LKL_TEST_CALL(mount_fs_proc, lkl_mount_fs, 0, "proc");
LKL_TEST_CALL(umount_fs_proc, lkl_umount_timeout, 0, "proc", 0, 1000);
LKL_TEST_CALL(lo_ifup, lkl_if_up, 0, 1);

static int lkl_test_mutex(void)
{
	long ret = TEST_SUCCESS;
	/*
	 * Can't do much to verify that this works, so we'll just let Valgrind
	 * warn us on CI if we've made bad memory accesses.
	 */

	struct lkl_mutex *mutex;

	mutex = lkl_host_ops.mutex_alloc(0);
	lkl_host_ops.mutex_lock(mutex);
	lkl_host_ops.mutex_unlock(mutex);
	lkl_host_ops.mutex_free(mutex);

	mutex = lkl_host_ops.mutex_alloc(1);
	lkl_host_ops.mutex_lock(mutex);
	lkl_host_ops.mutex_lock(mutex);
	lkl_host_ops.mutex_unlock(mutex);
	lkl_host_ops.mutex_unlock(mutex);
	lkl_host_ops.mutex_free(mutex);

	return ret;
}

static int lkl_test_semaphore(void)
{
	long ret = TEST_SUCCESS;
	/*
	 * Can't do much to verify that this works, so we'll just let Valgrind
	 * warn us on CI if we've made bad memory accesses.
	 */

	struct lkl_sem *sem = lkl_host_ops.sem_alloc(1);
	lkl_host_ops.sem_down(sem);
	lkl_host_ops.sem_up(sem);
	lkl_host_ops.sem_free(sem);

	return ret;
}

static void test_thread(void *data)
{
	int *pipe_fds = (int*) data;
	char tmp[LKL_PIPE_BUF+1];
	int ret;

	ret = lkl_sys_read(pipe_fds[0], tmp, sizeof(tmp));
	if (ret < 0)
		lkl_test_logf("%s: %s\n", __func__, lkl_strerror(ret));
}

static int lkl_test_syscall_thread(void)
{
	int pipe_fds[2];
	char tmp[LKL_PIPE_BUF+1];
	long ret;
	lkl_thread_t tid;

	ret = lkl_sys_pipe2(pipe_fds, 0);
	if (ret) {
		lkl_test_logf("pipe2: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	ret = lkl_sys_fcntl(pipe_fds[0], LKL_F_SETPIPE_SZ, 1);
	if (ret < 0) {
		lkl_test_logf("fcntl setpipe_sz: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}

	tid = lkl_host_ops.thread_create(test_thread, pipe_fds);
	if (!tid) {
		lkl_test_logf("failed to create thread\n");
		return TEST_FAILURE;
	}

	ret = lkl_sys_write(pipe_fds[1], tmp, sizeof(tmp));
	if (ret != sizeof(tmp)) {
		if (ret < 0)
			lkl_test_logf("write error: %s\n", lkl_strerror(ret));
		else
			lkl_test_logf("short write: %ld\n", ret);
		return TEST_FAILURE;
	}

	ret = lkl_host_ops.thread_join(tid);
	if (ret) {
		lkl_test_logf("failed to join thread\n");
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

#ifndef __MINGW32__
static void thread_get_pid(void *unused)
{
	lkl_sys_getpid();
}

static int lkl_test_many_syscall_threads(void)
{
	lkl_thread_t tid;
	int count = 65, ret;

	while (--count > 0) {
		tid = lkl_host_ops.thread_create(thread_get_pid, NULL);
		if (!tid) {
			lkl_test_logf("failed to create thread\n");
			return TEST_FAILURE;
		}

		ret = lkl_host_ops.thread_join(tid);
		if (ret) {
			lkl_test_logf("failed to join thread\n");
			return TEST_FAILURE;
		}
	}

	return TEST_SUCCESS;
}

struct lkl_test_tgid {
	long tgid;
	long parent_pid;
	int new_thread_group_leader_result;
};

static void thread_get_tgid(void *arg)
{
	struct lkl_test_tgid *tgid_arg = (struct lkl_test_tgid *)arg;

	int ret = lkl_sys_new_thread_group_leader();

	tgid_arg->new_thread_group_leader_result = ret;
	if (!ret) {
		tgid_arg->tgid = lkl_sys_getpid();
		tgid_arg->parent_pid = lkl_sys_getppid();
	}
}

static int lkl_test_new_tgid_threads(void)
{
	lkl_thread_t tid;
	long current_pid;
	int count = 65, ret;
	struct lkl_test_tgid test_tgid;

	current_pid = lkl_sys_getpid();
	if (current_pid < 0) {
		lkl_test_logf("failed to get pid\n");
		return TEST_FAILURE;
	}

	while (--count > 0) {
		tid = lkl_host_ops.thread_create(thread_get_tgid, &test_tgid);
		if (!tid) {
			lkl_test_logf("failed to create thread\n");
			return TEST_FAILURE;
		}

		ret = lkl_host_ops.thread_join(tid);
		if (ret) {
			lkl_test_logf("failed to join thread\n");
			return TEST_FAILURE;
		}

		if (test_tgid.new_thread_group_leader_result) {
			lkl_test_logf("failed to set new thread group leader %x\n",
				test_tgid.new_thread_group_leader_result);
			return TEST_FAILURE;
		}

		if (test_tgid.tgid == current_pid) {
			lkl_test_logf("child tgid %lx == current pid %lx\n",
				test_tgid.tgid, current_pid);
			return TEST_FAILURE;
		}

		if (test_tgid.parent_pid != current_pid) {
			lkl_test_logf("ppid of the child %lx != current pid %lx\n",
				test_tgid.parent_pid, current_pid);
			return TEST_FAILURE;
		}
	}

	return TEST_SUCCESS;
}

struct attach_arg {
	long tid;
	int fd;
	long attach, gettid, getpid, write, bogus, init;
	long pid;
};

static void thread_attach(void *arg)
{
	struct attach_arg *a = arg;

	a->attach = lkl_sys_attach_thread(a->tid);
	a->gettid = lkl_sys_gettid();
	a->getpid = lkl_sys_getpid();
	/* an fd of init's: the thread shares its process's files */
	a->write = lkl_sys_write(a->fd, "x", 1);
}

static void thread_attach_bad(void *arg)
{
	struct attach_arg *a = arg;

	a->bogus = lkl_sys_attach_thread(1 << 30);
	/* only a thread new_thread made, and no host thread took */
	a->init = lkl_sys_attach_thread(1);
}

/* the same in a process other than init's */
static void thread_new_process(void *arg)
{
	struct attach_arg *a = arg;
	lkl_thread_t tid;

	if (lkl_sys_new_thread_group_leader())
		return;
	a->pid = lkl_sys_getpid();
	a->tid = lkl_sys_new_thread();
	tid = lkl_host_ops.thread_create(thread_attach, a);
	if (tid)
		lkl_host_ops.thread_join(tid);
}

/*
 * A host thread joins a process as a thread: one of the process's threads
 * (here init's, then another process's) makes it with new_thread, the host
 * thread takes it with attach_thread as its first call.
 */
static int lkl_test_attach_thread(void)
{
	struct attach_arg a = { 0 }, b = { 0 };
	int pipe_fds[2];
	lkl_thread_t tid;
	long ret;
	char c;

	ret = lkl_sys_pipe2(pipe_fds, 0);
	if (ret) {
		lkl_test_logf("pipe2: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}
	a.fd = b.fd = pipe_fds[1];

	a.tid = lkl_sys_new_thread();
	tid = lkl_host_ops.thread_create(thread_attach, &a);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	tid = lkl_host_ops.thread_create(thread_attach_bad, &a);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	tid = lkl_host_ops.thread_create(thread_new_process, &b);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	ret = lkl_sys_read(pipe_fds[0], &c, 1);

	lkl_test_logf("new_thread %ld attach %ld gettid %ld getpid %ld write %ld read %ld\n",
		      a.tid, a.attach, a.gettid, a.getpid, a.write, ret);
	lkl_test_logf("attach bogus %ld init %ld\n", a.bogus, a.init);
	lkl_test_logf("process %ld: new_thread %ld attach %ld gettid %ld getpid %ld\n",
		      b.pid, b.tid, b.attach, b.gettid, b.getpid);

	lkl_sys_close(pipe_fds[0]);
	lkl_sys_close(pipe_fds[1]);

	if (a.tid <= 1 || a.attach || a.gettid != a.tid || a.getpid != 1 ||
	    a.write != 1 || ret != 1 || a.bogus != -LKL_ESRCH ||
	    a.init != -LKL_EINVAL)
		return TEST_FAILURE;
	if (b.pid <= 1 || b.tid <= b.pid || b.attach || b.gettid != b.tid ||
	    b.getpid != b.pid)
		return TEST_FAILURE;

	return TEST_SUCCESS;
}

struct hostfd_reader {
	int fd;
	long ret;
	char buf[8];
};

static void thread_hostfd_read(void *arg)
{
	struct hostfd_reader *r = arg;

	r->ret = lkl_sys_read(r->fd, r->buf, sizeof(r->buf));
}

/*
 * Host fds as guest files (host_fd): writes, writev and a dup'ed copy's
 * reach the host's fd; fstat says character device; a read blocked on the
 * host leaves the CPU free (another thread's calls go on meanwhile).
 */
static int lkl_test_host_fd(void)
{
	struct hostfd_reader r = { 0 };
	struct lkl_iovec iov[2] = { { "b", 1 }, { "cd", 2 } };
	struct lkl_stat st;
	struct timespec ts = { 0, 10000000 };
	int to_host[2], from_host[2];
	long w, rd, dup, wv, lseek, bad, pid;
	char buf[16] = { 0 };
	lkl_thread_t tid;
	ssize_t n;

	if (pipe(to_host) || pipe(from_host))
		return TEST_FAILURE;
	w = lkl_sys_host_fd(to_host[1], LKL_O_WRONLY);
	rd = lkl_sys_host_fd(from_host[0], LKL_O_RDONLY | LKL_O_CLOEXEC);
	bad = lkl_sys_host_fd(0, 0x10000000);
	if (w < 0 || rd < 0) {
		lkl_test_logf("host_fd: %ld %ld\n", w, rd);
		return TEST_FAILURE;
	}
	if (lkl_sys_fstat(w, &st))
		return TEST_FAILURE;

	lkl_sys_write(w, "a", 1);
	wv = lkl_sys_writev(w, iov, 2);
	dup = lkl_sys_dup3(w, 100, 0);
	lkl_sys_write(dup, "e", 1);
	lseek = lkl_sys_lseek(w, 0, LKL_SEEK_SET);
	n = read(to_host[0], buf, sizeof(buf) - 1);

	/* the read waits on the host, the CPU free for this thread's getpid */
	r.fd = rd;
	tid = lkl_host_ops.thread_create(thread_hostfd_read, &r);
	if (!tid)
		return TEST_FAILURE;
	nanosleep(&ts, NULL);
	pid = lkl_sys_getpid();
	if (write(from_host[1], "xyz", 3) != 3 || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;

	lkl_test_logf("host_fd %ld %ld (bad flags %ld), mode %o; host read \"%s\" (writev %ld, dup %ld, lseek %ld); getpid while reading %ld, then read %ld \"%.3s\"\n",
		      w, rd, bad, st.st_mode, buf, wv, dup, lseek, pid, r.ret,
		      r.buf);
	lkl_sys_close(w);
	lkl_sys_close(rd);
	lkl_sys_close(dup);
	close(to_host[0]);
	close(to_host[1]);
	close(from_host[0]);
	close(from_host[1]);

	if (!LKL_S_ISCHR(st.st_mode) || bad != -LKL_EINVAL || wv != 3 ||
	    dup != 100 || lseek != -LKL_ESPIPE || n != 5 ||
	    strcmp(buf, "abcde") || pid != 1 || r.ret != 3 ||
	    memcmp(r.buf, "xyz", 3))
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * memblk: an erofs image (mkfs.erofs's, if there is one) mapped from the
 * host, added as a disk, mounted at /nix/store, and a file of it read.
 */
static int lkl_test_memblk(void)
{
	char dir[] = "/tmp/lkl-memblk-XXXXXX", path[128], cmd[512];
	char buf[32] = { 0 };
	static const char hello[] = "from host memory\n";
	long dev, bad, ret, mnt, rd, creat = 0;
	struct stat st;
	void *img;
	int fd, ok;

	if (!mkdtemp(dir))
		return TEST_FAILURE;
	snprintf(path, sizeof(path), "%s/tree", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/tree/hello", dir);
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0 || write(fd, hello, sizeof(hello) - 1) < 0)
		return TEST_FAILURE;
	close(fd);
	snprintf(cmd, sizeof(cmd),
		 "mkfs.erofs %s/img %s/tree >/dev/null 2>&1", dir, dir);
	ok = system(cmd) == 0;
	snprintf(cmd, sizeof(cmd), "rm -rf %s/tree", dir);
	if (system(cmd))
		return TEST_FAILURE;
	snprintf(path, sizeof(path), "%s/img", dir);
	if (!ok) {
		lkl_test_logf("no mkfs.erofs\n");
		rmdir(dir);
		return TEST_SKIP;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st))
		return TEST_FAILURE;
	img = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	unlink(path);
	rmdir(dir);
	if (img == MAP_FAILED)
		return TEST_FAILURE;

	bad = lkl_sys_memblk_add((long)img, 1000);
	dev = lkl_sys_memblk_add((long)img, st.st_size);
	if (dev == -LKL_ENOSYS) {
		lkl_test_logf("no CONFIG_BLOCK\n");
		munmap(img, st.st_size);
		return TEST_SKIP;
	}
	ret = lkl_sys_mknodat(LKL_AT_FDCWD, "/memblk0", LKL_S_IFBLK | 0600, dev);
	lkl_sys_mkdir("/nix", 0755);
	lkl_sys_mkdir("/nix/store", 0755);
	mnt = lkl_sys_mount("/memblk0", "/nix/store", "erofs", LKL_MS_RDONLY,
			    NULL);
	if (mnt == -LKL_ENODEV) {
		lkl_test_logf("no CONFIG_EROFS_FS\n");
		munmap(img, st.st_size);
		return TEST_SKIP;
	}
	fd = lkl_sys_open("/nix/store/hello", LKL_O_RDONLY, 0);
	rd = fd < 0 ? fd : lkl_sys_read(fd, buf, sizeof(buf) - 1);
	if (fd >= 0)
		lkl_sys_close(fd);
	creat = lkl_sys_open("/nix/store/new", LKL_O_CREAT | LKL_O_WRONLY, 0644);
	lkl_sys_umount("/nix/store", 0);
	munmap(img, st.st_size);

	lkl_test_logf("image %ld bytes: memblk_add %#lx (1000 bytes: %ld), mknod %ld, mount %ld, read %ld \"%s\", creat %ld\n",
		      (long)st.st_size, dev, bad, ret, mnt, rd, buf, creat);
	if (bad != -LKL_EINVAL || dev <= 0 || ret || mnt ||
	    rd != sizeof(hello) - 1 || strcmp(buf, hello) ||
	    creat != -LKL_EROFS)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/* the kernel's context switches so far (/proc/stat's ctxt) */
static long context_switches(void)
{
	char buf[4096], *p;
	long fd, n;

	fd = lkl_sys_open("/proc/stat", LKL_O_RDONLY, 0);
	if (fd < 0)
		return fd;
	n = lkl_sys_read(fd, buf, sizeof(buf) - 1);
	lkl_sys_close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strstr(buf, "\nctxt ");
	return p ? atol(p + 6) : -1;
}

static void thread_getpids(void *arg)
{
	long *switches = arg;
	long c0 = context_switches();
	int i;

	for (i = 0; i < 1000; i++)
		lkl_sys_getpid();
	*switches = context_switches() - c0;
}

/*
 * A host task leaving the kernel switches to another task only if one is
 * runnable: not to the idle host task, nor for the fair server (a deadline
 * entity counted in nr_running). A thread's 1000 getpids switched 3000
 * times.
 */
static int lkl_test_syscall_switches(void)
{
	lkl_thread_t tid;
	long switches = -1, ret;

	ret = lkl_sys_mkdir("/proc", 0555);
	if (ret == 0 || ret == -LKL_EEXIST)
		ret = lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
	if (ret) {
		lkl_test_logf("mount proc: %s\n", lkl_strerror(ret));
		return TEST_FAILURE;
	}
	tid = lkl_host_ops.thread_create(thread_getpids, &switches);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	lkl_sys_umount("/proc", 0);

	lkl_test_logf("1000 getpid: %ld context switches\n", switches);
	return switches >= 0 && switches < 10 ? TEST_SUCCESS : TEST_FAILURE;
}
#endif

static void thread_quit_immediately(void *unused)
{
}

static int lkl_test_join(void)
{
	lkl_thread_t tid = lkl_host_ops.thread_create(thread_quit_immediately, NULL);
	int ret = lkl_host_ops.thread_join(tid);

	if (ret == 0) {
		lkl_test_logf("joined %ld\n", tid);
		return TEST_SUCCESS;
	} else {
		lkl_test_logf("failed joining %ld\n", tid);
		return TEST_FAILURE;
	}
}

static const char *boot_log;

#ifdef LKL_CONFIG_KASAN_KUNIT_TEST

static int lkl_test_kasan(void)
{
	char *log = strdup(boot_log);
	char *line = NULL;
	int p, f, s, t;
	int num_lines, result = TEST_FAILURE;

	line = strtok(log, "\n");
	for (num_lines = 0; line; num_lines++) {
		if (sscanf(line,
			   "[ %*f] # kasan: pass:%d fail:%d skip:%d total:%d",
			   &p, &f, &s, &t) == 4) {
			lkl_test_logf("%s", line);
			result = (f == 0 ? TEST_SUCCESS : TEST_FAILURE);
			goto out;
		}

		line = strtok(NULL, "\n");
	}

	lkl_test_logf("no kasan test output in %d log lines\n", num_lines);
out:
	free(log);
	return result;
}
#endif

#ifdef LKL_HOST_CONFIG_MMU

// TODO: for some reason <lkl/linux/mman.h> doesn't contain definition of
// LKL_MAP_FAILED macro. Thus, temporarily define it here.
#ifndef LKL_MAP_FAILED
#define LKL_MAP_FAILED ((void *)-1)
#endif

static int lkl_test_shared_mmap(void)
{
	int fd = lkl_sys_open("/tmp_file_", LKL_O_RDWR | LKL_O_CREAT, LKL_S_IRWXU);

	if (fd < 0)
		return TEST_FAILURE;

	unsigned long mem_size = 15 * 4096;

	if (lkl_sys_ftruncate(fd, mem_size) < 0)
		return TEST_FAILURE;

	int mem_prot = LKL_PROT_WRITE | LKL_PROT_READ;
	// Super important to use LKL_MAP_POPULATE to force populating pages in the
	// mapping as in the LKL context we don't have a way to fault in the pages.
	int mem_flags =  LKL_MAP_SHARED | LKL_MAP_POPULATE;
	void *mem1 = lkl_sys_mmap(0, mem_size, mem_prot, mem_flags, fd, 0);

	if (mem1 == LKL_MAP_FAILED)
		return TEST_FAILURE;

	memset(mem1, 0x17, mem_size);

	void *mem2 = lkl_sys_mmap(0, mem_size, mem_prot, mem_flags, fd, 0);

	if (mem2 == LKL_MAP_FAILED)
		return TEST_FAILURE;

	// This should not happen
	if (mem1 == mem2)
		return TEST_FAILURE;

	if (memcmp(mem1, mem2, mem_size) != 0)
		return TEST_FAILURE;

	if (lkl_sys_munmap((unsigned long)mem1, mem_size) != 0)
		return TEST_FAILURE;

	if (lkl_sys_munmap((unsigned long)mem2, mem_size) != 0)
		return TEST_FAILURE;

	return TEST_SUCCESS;
}

static int lkl_test_private_mmap(void)
{
	unsigned long mem_size = 3 * 4096;
	int mem_prot = LKL_PROT_WRITE | LKL_PROT_READ;
	// Super important to use LKL_MAP_POPULATE to force populating pages in the
	// mapping as in the LKL context we don't have a way to fault in the pages.
	int mem_flags =  LKL_MAP_ANONYMOUS | LKL_MAP_PRIVATE | LKL_MAP_POPULATE;
	void *mem = lkl_sys_mmap(0, mem_size, mem_prot, mem_flags, -1, 0);

	if (mem == LKL_MAP_FAILED)
		return TEST_FAILURE;

	*(unsigned int *)mem = 13;

	if (lkl_sys_munmap((unsigned long)mem, mem_size) < 0)
		return TEST_FAILURE;

	return TEST_SUCCESS;
}
#endif // LKL_HOST_CONFIG_MMU

#ifdef LKL_HOST_CONFIG_LKL_MMU_TEST
static int lkl_test_kunit_mmu(void)
{
	char *log = strdup(boot_log);
	char *line = NULL;
	int n;
	char c, d;

	line = strtok(log, "\n");
	while (line) {
		if (sscanf(line, "[ %*f] ok %d lkl_m%c%c", &n, &c, &d) == 3 &&
			c == 'm' && d == 'u') {
			lkl_test_logf("%s", line);
			return TEST_SUCCESS;
		}

		line = strtok(NULL, "\n");
	}

	free(log);

	return TEST_FAILURE;
}
#endif // LKL_HOST_CONFIG_LKL_MMU_TEST

#ifdef LKL_HOST_CONFIG_LKL_PCI_KUNIT_TEST
static int lkl_test_kunit_pci(void)
{
	char *log = strdup(boot_log);
	char *line = NULL;
	int n;
	char c;

	line = strtok(log, "\n");
	while (line) {
		if (sscanf(line, "[ %*f] ok %d lkl_pci%c", &n, &c) == 1) {
			lkl_test_logf("%s", line);
			free(log);
			return TEST_SUCCESS;
		}

		line = strtok(NULL, "\n");
	}

	free(log);

	return TEST_FAILURE;
}
#endif // LKL_HOST_CONFIG_LKL_PCI_KUNIT_TEST

#ifdef LKL_HOST_CONFIG_LKL_PCI_MSI_KUNIT_TEST
static int lkl_test_kunit_pci_msi(void)
{
	char *log = strdup(boot_log);
	char *line = NULL;
	int n;
	char c;

	line = strtok(log, "\n");
	while (line) {
		if (sscanf(line, "[ %*f] ok %d lkl_pci_msi%c", &n, &c) == 1) {
			lkl_test_logf("%s", line);
			free(log);
			return TEST_SUCCESS;
		}

		line = strtok(NULL, "\n");
	}

	free(log);

	return TEST_FAILURE;
}
#endif // LKL_HOST_CONFIG_LKL_PCI_MSI_KUNIT_TEST

#define CMD_LINE "mem=32M loglevel=8 "

static int lkl_test_start_kernel(void)
{
	int ret;

	ret = lkl_start_kernel(CMD_LINE);

	boot_log = lkl_test_get_log();

	return ret == 0 ? TEST_SUCCESS : TEST_FAILURE;
}


#ifndef __MINGW32__
/*
 * A host task that exits in its syscall: the host's task_exit (here this)
 * is called on its thread instead of the call returning, with its process's
 * pid and wait status if it was the last thread. The thread ends, or, for
 * one that set exit_jmp (init's, which runs the tests), jumps back.
 */
struct exit_report {
	int calls, pid, status;
};
static __thread struct exit_report *exit_report;
static __thread jmp_buf *exit_jmp;

static void test_task_exit(int pid, int status)
{
	if (exit_report) {
		exit_report->calls++;
		exit_report->pid = pid;
		exit_report->status = status;
	}
	if (exit_jmp)
		longjmp(*exit_jmp, 1);
	lkl_host_ops.thread_exit();
}

static void thread_exits(void *arg)
{
	exit_report = arg;
	lkl_sys_exit(5);
	/* not reached */
	exit_report->calls = -1;
}

struct exit_process {
	struct exit_report leader, thread;
	long pid, tid;
	int fds[2];
};

/* the second thread of the process: blocked in a read when it's killed */
static void thread_blocked(void *arg)
{
	struct exit_process *p = arg;
	char c;

	exit_report = &p->thread;
	if (lkl_sys_attach_thread(p->tid))
		return;
	lkl_sys_read(p->fds[0], &c, 1);
	/* not reached: the read ends with the thread */
	p->thread.calls = -1;
}

static void process_exits(void *arg)
{
	struct exit_process *p = arg;
	struct timespec ts = { 0, 20000000 };
	lkl_thread_t tid;

	exit_report = &p->leader;
	if (lkl_sys_new_thread_group_leader())
		return;
	p->pid = lkl_sys_getpid();
	p->tid = lkl_sys_new_thread();
	tid = lkl_host_ops.thread_create(thread_blocked, p);
	if (!tid)
		return;
	/* (if the thread isn't reading yet, it dies at its next call) */
	nanosleep(&ts, NULL);
	lkl_sys_exit_group(7);
	p->leader.calls = -1;
}

/*
 * Exits in a syscall: a thread of init's process (exit, not its last); a
 * process of two threads (exit_group from one, which kills the other in
 * its read): the last to go reports the process's end, which init reaps.
 */
static int lkl_test_task_exit(void)
{
	struct exit_report t = { 0 };
	struct exit_process p = { 0 };
	lkl_thread_t tid;
	int status = 0;
	long ret;

	tid = lkl_host_ops.thread_create(thread_exits, &t);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	lkl_test_logf("exit(5), a thread of init's: task_exit %d time(s), pid %d status %d\n",
		      t.calls, t.pid, t.status);

	ret = lkl_sys_pipe2(p.fds, 0);
	if (ret)
		return TEST_FAILURE;
	tid = lkl_host_ops.thread_create(process_exits, &p);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	/* the blocked thread's: it's last, so its host thread may still be ending */
	while (!__atomic_load_n(&p.thread.calls, __ATOMIC_SEQ_CST))
		;
	ret = lkl_sys_wait4(p.pid, &status, 0, NULL);
	lkl_sys_close(p.fds[0]);
	lkl_sys_close(p.fds[1]);
	lkl_test_logf("process %ld, exit_group(7): leader task_exit %d pid %d status %#x; thread %d pid %d status %#x; wait4 %ld status %#x\n",
		      p.pid, p.leader.calls, p.leader.pid, p.leader.status,
		      p.thread.calls, p.thread.pid, p.thread.status, ret, status);

	if (t.calls != 1 || t.pid || t.status)
		return TEST_FAILURE;
	if (p.leader.calls != 1 || p.leader.pid || p.thread.calls != 1 ||
	    p.thread.pid != p.pid || p.thread.status != 7 << 8 ||
	    ret != p.pid || status != 7 << 8)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * The halt's other thread (stop_kernel): a process of its own, made before
 * init's end, since after it no new task can be made. It first checks that
 * init's end closed init's files: it reads a pipe whose write end only init
 * holds, which ends (EOF) when init's exit has closed it.
 */
static struct lkl_sem *halt_go;
static volatile long survivor_read = 1, halt_thread_ret;
static __thread int halt_late;

static void thread_until_halt(void *arg)
{
	int *fds = arg;
	char c;

	if (lkl_sys_new_thread_group_leader() ||
	    lkl_sys_unshare(LKL_CLONE_FILES) || lkl_sys_close(fds[1]))
		return;
	survivor_read = lkl_sys_read(fds[0], &c, 1);

	lkl_host_ops.sem_down(halt_go);
	halt_late = 1;
	while ((halt_thread_ret = lkl_sys_getpid()) >= 0)
		;
}

static lkl_thread_t halt_thread;

/*
 * init's end runs as any process's: exit_group(3) closes its files, and the
 * host learns its wait status by task_exit(1, 3 << 8). Then only the halt
 * can be called.
 */
static int lkl_test_init_exit(void)
{
	struct exit_report r = { 0 };
	struct timespec ts = { 0, 1000000 };
	jmp_buf jb;
	int fds[2], i;
	long ret;

	if (lkl_sys_pipe2(fds, 0))
		return TEST_FAILURE;
	halt_go = lkl_host_ops.sem_alloc(0);
	halt_thread = lkl_host_ops.thread_create(thread_until_halt, fds);
	if (!halt_go || !halt_thread)
		return TEST_FAILURE;
	/* until it's reading */
	nanosleep(&ts, NULL);
	nanosleep(&ts, NULL);

	exit_report = &r;
	exit_jmp = &jb;
	if (!setjmp(jb))
		lkl_sys_exit_group(3);
	exit_jmp = NULL;
	exit_report = NULL;

	for (i = 0; i < 1000 && survivor_read == 1; i++)
		nanosleep(&ts, NULL);
	ret = lkl_sys_getpid();
	lkl_test_logf("exit_group(3): task_exit %d time(s), pid %d status %#x; the pipe's reader read %ld; getpid then: %ld\n",
		      r.calls, r.pid, r.status, survivor_read, ret);

	if (r.calls != 1 || r.pid != 1 || r.status != 3 << 8 ||
	    survivor_read != 0 || ret != -LKL_ESRCH)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * Halt with another thread waiting for the CPU: a thread making syscalls in
 * a loop is parked in lkl_cpu_get() when the halt takes the CPU, and still
 * counts in the CPU's shutdown gate until it runs again. Once the halt has
 * started, its sem_downs return 20 ms late, as on a host that runs one
 * thread at a time and has yet to run it. The halt must wait for it without
 * spinning: a spin costs that time in CPU (and on such a host never
 * ends, since the thread it waits for can't run). init has ended: the
 * thread is a process of its own (init_exit's), and the halt is made by a
 * thread without a task.
 */
#define HALT_LATE_NS 20000000
static void (*halt_sem_down)(struct lkl_sem *sem);
static volatile int halting;

static void late_sem_down(struct lkl_sem *sem)
{
	struct timespec ts = { 0, HALT_LATE_NS };

	halt_sem_down(sem);
	if (halt_late && halting)
		nanosleep(&ts, NULL);
}

static int lkl_test_stop_kernel(void)
{
	struct timespec c0, c1;
	long ret, cpu_ns;

	/* (if init_exit failed its thread isn't there to halt with) */
	if (!halt_thread || survivor_read)
		return TEST_FAILURE;
	halt_sem_down = lkl_host_ops.sem_down;
	lkl_host_ops.sem_down = late_sem_down;
	lkl_host_ops.sem_up(halt_go);
	/* until it's making syscalls */
	while (!halt_thread_ret)
		;

	halting = 1;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
	ret = lkl_sys_halt();
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
	lkl_host_ops.thread_join(halt_thread);
	lkl_host_ops.sem_down = halt_sem_down;

	cpu_ns = (c1.tv_sec - c0.tv_sec) * 1000000000L +
		 (c1.tv_nsec - c0.tv_nsec);
	lkl_test_logf("lkl_sys_halt() = %ld, CPU %ld us; the thread's last syscall: %ld\n",
		      ret, cpu_ns / 1000, halt_thread_ret);

	if (ret || halt_thread_ret >= 0 || cpu_ns > HALT_LATE_NS / 2)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}
#else
LKL_TEST_CALL(stop_kernel, lkl_sys_halt, 0);
#endif

struct lkl_test tests[] = {
	LKL_TEST(mutex),
	LKL_TEST(semaphore),
	LKL_TEST(join),
	LKL_TEST(start_kernel),
#ifdef LKL_CONFIG_KASAN_KUNIT_TEST
	LKL_TEST(kasan),
#endif
	LKL_TEST(getpid),
	LKL_TEST(gettid),
	LKL_TEST(getppid),
	LKL_TEST(syscall_latency),
	LKL_TEST(syscall_bounds),
	LKL_TEST(umask),
	LKL_TEST(umask2),
	LKL_TEST(creat),
	LKL_TEST(close),
	LKL_TEST(failopen),
	LKL_TEST(open),
	LKL_TEST(write),
	LKL_TEST(lseek_cur),
	LKL_TEST(lseek_end),
	LKL_TEST(lseek_set),
	LKL_TEST(read),
	LKL_TEST(fstat),
	LKL_TEST(mkdir),
	LKL_TEST(stat),
#ifndef __MINGW32__
	LKL_TEST(nanosleep),
	LKL_TEST(tickless_idle),
#endif
	LKL_TEST(pipe2),
	LKL_TEST(epoll),
	LKL_TEST(mount_fs_proc),
	LKL_TEST(chdir_proc),
	LKL_TEST(open_cwd),
	LKL_TEST(getdents64),
	LKL_TEST(close_dir_fd),
	LKL_TEST(chdir_root),
	LKL_TEST(umount_fs_proc),
	LKL_TEST(lo_ifup),
	LKL_TEST(syscall_thread),
	/*
	 * Wine has an issue where the FlsCallback is not called when
	 * the thread terminates which makes testing the automatic
	 * syscall threads cleanup impossible under wine.
	 */
#ifndef __MINGW32__
	LKL_TEST(many_syscall_threads),
	LKL_TEST(new_tgid_threads),
	LKL_TEST(attach_thread),
	LKL_TEST(syscall_switches),
	LKL_TEST(host_fd),
	LKL_TEST(memblk),
#endif
#ifdef LKL_HOST_CONFIG_MMU
	LKL_TEST(shared_mmap),
	LKL_TEST(private_mmap),
#endif
#ifdef LKL_HOST_CONFIG_LKL_MMU_TEST
	LKL_TEST(kunit_mmu),
#endif
#ifdef LKL_HOST_CONFIG_LKL_PCI_KUNIT_TEST
	LKL_TEST(kunit_pci),
#endif
#ifdef LKL_HOST_CONFIG_LKL_PCI_MSI_KUNIT_TEST
	LKL_TEST(kunit_pci_msi),
#endif
#ifndef __MINGW32__
	LKL_TEST(task_exit),
	LKL_TEST(init_exit),
#endif
	LKL_TEST(stop_kernel),
};

int main(int argc, const char **argv)
{
	int ret;

	lkl_host_ops.print = lkl_test_log;
#ifndef __MINGW32__
	host_timer_alloc = lkl_host_ops.timer_alloc;
	lkl_host_ops.timer_alloc = counting_timer_alloc;
	lkl_host_ops.task_exit = test_task_exit;
#endif

	if (lkl_init(&lkl_host_ops) < 0) {
		printf("%s\n", lkl_test_get_log());
		return 1;
	}

	ret = lkl_test_run(tests, sizeof(tests)/sizeof(struct lkl_test),
			"boot");

	lkl_cleanup();

	return ret;
}
