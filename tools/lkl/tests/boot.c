#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <setjmp.h>
#ifndef __MINGW32__
#include <pthread.h>
#endif
#include <lkl.h>
#include <lkl_host.h>

#include <sys/stat.h>
#include <arpa/inet.h>
#include <poll.h>
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
 * host, added as a disk of one extent, mounted at /nix/store, and a file of
 * it read.
 */
static int lkl_test_memblk(void)
{
	char dir[] = "/tmp/lkl-memblk-XXXXXX", path[128], cmd[512];
	char buf[32] = { 0 };
	static const char hello[] = "from host memory\n";
	struct lkl_memblk_extent ext;
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

	ext.addr = (long)img;
	ext.len = 1000;
	bad = lkl_sys_memblk_add(&ext, 1);
	ext.len = st.st_size;
	dev = lkl_sys_memblk_add(&ext, 1);
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

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031 /* (glibc's only with _GNU_SOURCE) */
#endif

struct hostfd_io {
	long fd, ret;
	int write;
	char buf[8];
};

static void thread_hostfd_io(void *arg)
{
	struct hostfd_io *io = arg;

	io->ret = io->write ? lkl_sys_write(io->fd, "x", 1) :
			      lkl_sys_read(io->fd, io->buf, sizeof(io->buf));
}

/*
 * io on another thread; if it still waits after 50 ms, unblock(fd) lets it
 * finish (a write to the pipe, or a read making room). Its result.
 */
static long hostfd_io_bounded(long fd, int out, int unblock)
{
	struct hostfd_io io = { .fd = fd, .ret = -1, .write = out };
	struct timespec ts = { 0, 50000000 };
	char b[4096];
	lkl_thread_t tid;

	tid = lkl_host_ops.thread_create(thread_hostfd_io, &io);
	if (!tid)
		return -1000;
	nanosleep(&ts, NULL);
	if (out ? read(unblock, b, sizeof(b)) < 0 : write(unblock, "y", 1) != 1)
		return -1000;
	if (lkl_host_ops.thread_join(tid))
		return -1000;
	return io.ret;
}

/*
 * A nonblocking host fd file's reads and writes don't wait for the host
 * fd: -EAGAIN while it isn't ready (an empty pipe; a full one), whether
 * O_NONBLOCK came with host_fd or with a later fcntl; once ready, the I/O.
 */
static int lkl_test_hostfd_nonblock(void)
{
	char fill[4096] = { 0 };
	long at_open, by_fcntl, setfl, full, ready;
	long rd = -1, rd2 = -1, wr = -1;
	int p[2], q[2];

	if (pipe(p) || pipe(q))
		return TEST_FAILURE;
	/* a pipe of one page, filled: no room for a write */
	if (fcntl(q[1], F_SETPIPE_SZ, sizeof(fill)) < 0 ||
	    write(q[1], fill, sizeof(fill)) != sizeof(fill))
		return TEST_FAILURE;

	rd = lkl_sys_host_fd(p[0], LKL_O_RDONLY | LKL_O_NONBLOCK);
	at_open = rd < 0 ? rd : hostfd_io_bounded(rd, 0, p[1]);
	/* the unblocking write left a byte: a nonblocking read gets it */
	ready = rd < 0 ? rd : lkl_sys_read(rd, fill, 1);

	rd2 = lkl_sys_host_fd(p[0], LKL_O_RDONLY);
	setfl = lkl_sys_fcntl(rd2, LKL_F_SETFL, LKL_O_NONBLOCK);
	by_fcntl = hostfd_io_bounded(rd2, 0, p[1]);

	wr = lkl_sys_host_fd(q[1], LKL_O_WRONLY);
	lkl_sys_fcntl(wr, LKL_F_SETFL, LKL_O_NONBLOCK);
	full = hostfd_io_bounded(wr, 1, q[0]);

	lkl_test_logf("O_NONBLOCK at host_fd: fd %ld, read of an empty pipe %ld, then %ld; by fcntl (%ld): read %ld; write to a full pipe %ld\n",
		      rd, at_open, ready, setfl, by_fcntl, full);
	lkl_sys_close(rd);
	lkl_sys_close(rd2);
	lkl_sys_close(wr);
	close(p[0]);
	close(p[1]);
	close(q[0]);
	close(q[1]);
	if (rd < 0 || at_open != -LKL_EAGAIN || ready != 1 || setfl ||
	    by_fcntl != -LKL_EAGAIN || full != -LKL_EAGAIN)

/* memblk_extents's file: a pattern with a run of zero blocks in it */
static unsigned char memblk_byte(long i)
{
	return i >= 12288 && i < 28672 ? 0 : (unsigned char)(i * 7 + i / 251 + 1);
}

/*
 * memblk_extents: a disk of many extents of all three kinds. An erofs image
 * (mkfs.erofs's) is cut into them: each run of zero sectors a zeros extent,
 * the rest pieces of 1, 3 and 2 sectors, alternately from the image's host
 * mapping and from a copy of it in another buffer, so that every block the
 * filesystem reads spans extents. It mounts, its files read back, and the
 * raw disk reads back as the image. Malformed tables are refused.
 */
static int lkl_test_memblk_extents(void)
{
	char dir[] = "/tmp/lkl-memblk-XXXXXX", path[128], cmd[512];
	static const char hello[] = "hello, extents\n";
	static const long data_len = 40000, piece[] = { 1, 3, 2 };
	long dev, mnt, nod, empty, odd, rd = 0, raw = 0, n = 0, zeros = 0;
	long i, sectors, k = 0, bad = 0;
	struct lkl_memblk_extent *ext, bad_ext;
	unsigned char *img, *copy, *buf;
	struct stat st;
	int fd, ok;

	if (!mkdtemp(dir))
		return TEST_FAILURE;
	snprintf(path, sizeof(path), "%s/tree", dir);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/tree/data", dir);
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		return TEST_FAILURE;
	for (i = 0; i < data_len; i++) {
		unsigned char b = memblk_byte(i);

		if (write(fd, &b, 1) != 1)
			return TEST_FAILURE;
	}
	close(fd);
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
	if (fd < 0 || fstat(fd, &st) || st.st_size % 512)
		return TEST_FAILURE;
	img = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	unlink(path);
	rmdir(dir);
	if (img == MAP_FAILED)
		return TEST_FAILURE;

	/* The copy and the image stay: the disk is never removed. */
	sectors = st.st_size / 512;
	copy = malloc(st.st_size);
	ext = calloc(sectors, sizeof(*ext));
	buf = malloc(st.st_size);
	if (!copy || !ext || !buf)
		return TEST_FAILURE;
	memcpy(copy, img, st.st_size);
	for (i = 0; i < sectors;) {
		const unsigned char *s = img + i * 512;
		long len = 0;
		int zero = 1;

		for (long j = 0; j < 512; j++)
			zero &= !s[j];
		if (zero) {
			do {
				len++;
				zero = i + len < sectors;
				for (long j = 0; zero && j < 512; j++)
					zero = !s[len * 512 + j];
			} while (zero);
			ext[n].addr = 0;
			zeros++;
		} else {
			len = piece[k % 3];
			if (i + len > sectors)
				len = sectors - i;
			ext[n].addr = (long)(k % 2 ? copy : img) + i * 512;
			k++;
		}
		ext[n++].len = len * 512;
		i += len;
	}

	empty = lkl_sys_memblk_add(ext, 0);
	bad_ext.addr = (long)img;
	bad_ext.len = 1000;
	odd = lkl_sys_memblk_add(&bad_ext, 1);
	dev = lkl_sys_memblk_add(ext, n);
	if (dev == -LKL_ENOSYS) {
		lkl_test_logf("no CONFIG_BLOCK\n");
		return TEST_SKIP;
	}
	nod = lkl_sys_mknodat(LKL_AT_FDCWD, "/memblk-ext", LKL_S_IFBLK | 0600,
			      dev);
	lkl_sys_mkdir("/ext", 0755);
	mnt = lkl_sys_mount("/memblk-ext", "/ext", "erofs", LKL_MS_RDONLY, NULL);
	if (mnt == -LKL_ENODEV) {
		lkl_test_logf("no CONFIG_EROFS_FS\n");
		return TEST_SKIP;
	}

	fd = lkl_sys_open("/ext/data", LKL_O_RDONLY, 0);
	if (fd >= 0) {
		long r;

		while ((r = lkl_sys_read(fd, (char *)buf + rd,
					 data_len + 1 - rd)) > 0)
			rd += r;
		lkl_sys_close(fd);
		for (i = 0; i < rd && i < data_len; i++)
			bad += buf[i] != memblk_byte(i);
	} else {
		rd = fd;
	}
	fd = lkl_sys_open("/ext/hello", LKL_O_RDONLY, 0);
	if (fd >= 0) {
		memset(buf, 0, 64);
		bad += lkl_sys_read(fd, (char *)buf, 63) != sizeof(hello) - 1 ||
		       strcmp((char *)buf, hello);
		lkl_sys_close(fd);
	} else {
		bad++;
	}
	lkl_sys_umount("/ext", 0);

	/* The raw disk, from mid-sector to mid-sector: across every extent. */
	fd = lkl_sys_open("/memblk-ext", LKL_O_RDONLY, 0);
	if (fd >= 0) {
		long r;

		while (raw < st.st_size - 1400 &&
		       (r = lkl_sys_pread64(fd, (char *)buf + raw,
					    st.st_size - 1400 - raw,
					    700 + raw)) > 0)
			raw += r;
		lkl_sys_close(fd);
	} else {
		raw = fd;
	}

	lkl_test_logf("image %ld bytes, %ld extents (%ld of zeros): memblk_add %#lx (none: %ld, 1000 bytes: %ld), mknod %ld, mount %ld, data read %ld (%ld wrong), raw read %ld\n",
		      (long)st.st_size, n, zeros, dev, empty, odd, nod, mnt,
		      rd, bad, raw);
	if (empty != -LKL_EINVAL || odd != -LKL_EINVAL || dev <= 0 || nod ||
	    mnt || rd != data_len || bad || zeros < 1 || n < 8 ||
	    raw != st.st_size - 1400 ||
	    memcmp(buf, img + 700, st.st_size - 1400))
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

#define PROGRAM_MEM (1UL << 30)
/* the posix host's hostfd_poll: poll(2) of the fd, not waiting */
static unsigned int test_hostfd_poll(int fd, unsigned int events)
{
	struct pollfd p = { .fd = fd, .events = events };

	return poll(&p, 1, 0) == 1 ? p.revents : 0;
}

struct hostfd_waiter {
	long epfd, ret;
};

static void thread_hostfd_epoll(void *arg)
{
	struct hostfd_waiter *w = arg;
	struct lkl_epoll_event ev;

	w->ret = lkl_sys_epoll_wait(w->epfd, &ev, 1, 5000);
}

/*
 * A host fd file polls as its host fd is (hostfd_poll): not readable while
 * the host pipe is empty; an epoll_wait on it wakes once the host writes
 * and says so (lkl_hostfd_ready).
 */
static int lkl_test_hostfd_poll(void)
{
	struct hostfd_waiter w = { .ret = -1 };
	struct lkl_epoll_event ev = { .events = LKL_POLLIN };
	struct lkl_pollfd pfd;
	struct timespec ts = { 0, 20000000 };
	long fd, before, early, after;
	int p[2];
	lkl_thread_t tid;

	if (pipe(p))
		return TEST_FAILURE;
	lkl_host_ops.hostfd_poll = test_hostfd_poll;
	fd = lkl_sys_host_fd(p[0], LKL_O_RDONLY);
	pfd = (struct lkl_pollfd){ .fd = fd, .events = LKL_POLLIN };
	before = lkl_sys_poll(&pfd, 1, 0);
	w.epfd = lkl_sys_epoll_create(1);
	lkl_sys_epoll_ctl(w.epfd, LKL_EPOLL_CTL_ADD, fd, &ev);
	tid = lkl_host_ops.thread_create(thread_hostfd_epoll, &w);
	nanosleep(&ts, NULL);
	early = w.ret;
	if (write(p[1], "x", 1) != 1)
		return TEST_FAILURE;
	lkl_hostfd_ready(p[0]);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	pfd.revents = 0;
	after = lkl_sys_poll(&pfd, 1, 0);
	lkl_test_logf("poll before the write %ld; epoll_wait %ld while waiting, %ld after the write; poll %ld (0x%x)\n",
		      before, early, w.ret, after, pfd.revents);
	lkl_sys_close(w.epfd);
	lkl_sys_close(fd);
	close(p[0]);
	close(p[1]);
	lkl_host_ops.hostfd_poll = NULL;
	if (before != 0 || early != -1 || w.ret != 1 || after != 1 ||
	    !(pfd.revents & LKL_POLLIN))
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

#ifdef LKL_CONFIG_LKL_TSI
/*
 * The host side of tsi.c: what the net_* ops were told, as a host would
 * note it for a thread of its own to act on.
 */
static struct {
	void *sock;
	int pid;
	struct lkl_sockaddr_in addr;
	char dgram[64];
	unsigned long dgram_len;
	int connects, listens, sendtos, readies;
	void *ready_obj;
} tsi_host;

static int test_net_note(void *sock, int pid, const struct lkl_sockaddr *addr,
			 int len)
{
	tsi_host.sock = sock;
	tsi_host.pid = pid;
	memcpy(&tsi_host.addr, addr, sizeof(tsi_host.addr));
	return 0;
}

static int test_net_connect(void *sock, int pid,
			    const struct lkl_sockaddr *addr, int len)
{
	test_net_note(sock, pid, addr, len);
	__sync_fetch_and_add(&tsi_host.connects, 1);
	return 0;
}

static int test_net_listen(void *sock, int pid, const struct lkl_sockaddr *addr,
			   int len)
{
	tsi_host.listens++;
	return test_net_note(sock, pid, addr, len);
}

static int test_net_sendto(void *sock, int pid, const struct lkl_sockaddr *addr,
			   int len, const void *buf, unsigned long size)
{
	tsi_host.sendtos++;
	if (size > sizeof(tsi_host.dgram))
		return -LKL_EMSGSIZE;
	memcpy(tsi_host.dgram, buf, size);
	tsi_host.dgram_len = size;
	return test_net_note(sock, pid, addr, len);
}

static void test_net_ready(void *obj)
{
	tsi_host.readies++;
	tsi_host.ready_obj = obj;
}

static struct lkl_sockaddr_in tsi_addr(unsigned int ip, unsigned short port)
{
	struct lkl_sockaddr_in a = { .sin_family = LKL_AF_INET };

	a.sin_addr.lkl_s_addr = htonl(ip);
	a.sin_port = htons(port);
	return a;
}

struct tsi_blocking {
	long fd, ret;
	struct lkl_sockaddr_in to;
};

static void thread_tsi_connect(void *arg)
{
	struct tsi_blocking *b = arg;

	b->ret = lkl_sys_connect(b->fd, (struct lkl_sockaddr *)&b->to,
				 sizeof(b->to));
}

/*
 * The host's own syscalls (lkl_net_syscall) run as the tsi task, of no
 * process of the program's: its pid isn't init's, init's fds aren't its
 * (a close of one is EBADF, and init's stays open), and an exit is
 * refused.
 */
static int lkl_test_tsi_syscall(void)
{
	long none[6] = { 0 }, args[6] = { 0 };
	long pid, fd, closed, still, ex;

	fd = lkl_sys_open("/", LKL_O_RDONLY, 0);
	pid = lkl_net_syscall(__lkl__NR_getpid, none);
	args[0] = fd;
	closed = lkl_net_syscall(__lkl__NR_close, args);
	still = lkl_sys_close(fd);
	ex = lkl_net_syscall(__lkl__NR_exit, none);
	lkl_test_logf("getpid %ld (init's %ld); close of init's fd %ld: %ld, then init's own close %ld; exit %ld\n",
		      pid, lkl_sys_getpid(), fd, closed, still, ex);
	if (pid <= 1 || fd < 0 || closed != -LKL_EBADF || still != 0 ||
	    ex != -LKL_EINVAL)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * A connect to a non-loopback address is the host's (tsi.c): net_connect
 * is told the address and the caller's pid; a nonblocking connect is
 * EINPROGRESS, not ready, until lkl_net_done; then the bytes go both ways
 * through the end (lkl_net_recv/send), the end's shutdown is the guest's
 * EOF, the guest's close is net_ready(end) and the end's EOF. A blocking
 * connect waits for the host's answer: a refusal is its error. The socket
 * says AF_INET (SO_DOMAIN), its peer is the address connected to.
 */
static int lkl_test_tsi_connect(void)
{
	struct lkl_sockaddr_in to = tsi_addr(0x0a010203, 80), peer;
	struct tsi_blocking b = { .ret = 1 };
	struct lkl_pollfd pfd;
	struct timespec ts = { 0, 1000000 };
	int domain = 0, soerr = -1, plen = sizeof(peer), len = sizeof(int);
	long fd, conn, poll0, poll1, w, rd, eof, gone, n;
	char buf[16] = { 0 }, back[16] = { 0 };
	lkl_thread_t tid;
	void *end;
	int i;

	fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM | LKL_O_NONBLOCK, 0);
	lkl_sys_getsockopt(fd, LKL_SOL_SOCKET, LKL_SO_DOMAIN, (char *)&domain,
			   &len);
	conn = lkl_sys_connect(fd, (struct lkl_sockaddr *)&to, sizeof(to));
	pfd = (struct lkl_pollfd){ .fd = fd, .events = LKL_POLLOUT };
	poll0 = lkl_sys_poll(&pfd, 1, 0);
	end = tsi_host.connects ? lkl_net_done(tsi_host.sock, 0) : NULL;
	poll1 = lkl_sys_poll(&pfd, 1, 0);
	len = sizeof(int);
	lkl_sys_getsockopt(fd, LKL_SOL_SOCKET, LKL_SO_ERROR, (char *)&soerr,
			   &len);
	memset(&peer, 0, sizeof(peer));
	lkl_sys_getpeername(fd, (struct lkl_sockaddr *)&peer, &plen);
	w = lkl_sys_write(fd, "hello", 5);
	n = end ? lkl_net_recv(end, buf, sizeof(buf)) : -1;
	if (end)
		lkl_net_send(end, "world", 5);
	rd = lkl_sys_read(fd, back, sizeof(back));
	if (end)
		lkl_net_shutdown(end, 1 /* SHUT_WR */);
	eof = lkl_sys_read(fd, back + 5, sizeof(back) - 5);
	tsi_host.readies = 0;
	lkl_sys_close(fd);
	gone = end ? lkl_net_recv(end, buf + 5, 4) : -1;
	lkl_test_logf("SO_DOMAIN %d, connect %ld (host told %d time(s), pid %d, %x:%d), poll %ld then %ld (0x%x), SO_ERROR %d, peer %x:%d; guest wrote %ld, host read %ld \"%s\"; guest read %ld \"%s\", then %ld at the host's shutdown; after the close net_ready %d (end: %d), recv %ld\n",
		      domain, conn, tsi_host.connects, tsi_host.pid,
		      ntohl(tsi_host.addr.sin_addr.lkl_s_addr),
		      ntohs(tsi_host.addr.sin_port), poll0, poll1, pfd.revents,
		      soerr, ntohl(peer.sin_addr.lkl_s_addr),
		      ntohs(peer.sin_port), w, n, buf, rd, back, eof,
		      tsi_host.readies, tsi_host.ready_obj == end, gone);
	if (end)
		lkl_net_close(end, 0);
	if (domain != LKL_AF_INET || conn != -LKL_EINPROGRESS ||
	    tsi_host.connects != 1 || tsi_host.pid != 1 ||
	    memcmp(&tsi_host.addr, &to, 8) || poll0 != 0 || !end ||
	    poll1 != 1 || !(pfd.revents & LKL_POLLOUT) || soerr != 0 ||
	    memcmp(&peer, &to, 8) || w != 5 || n != 5 || strcmp(buf, "hello") ||
	    rd != 5 || strcmp(back, "world") || eof != 0 ||
	    !tsi_host.readies || tsi_host.ready_obj != end || gone != -LKL_EPIPE)
		return TEST_FAILURE;

	/* blocking, refused */
	b.fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	b.to = tsi_addr(0x0a090909, 81);
	tid = lkl_host_ops.thread_create(thread_tsi_connect, &b);
	for (i = 0; i < 1000 && tsi_host.connects != 2; i++)
		nanosleep(&ts, NULL);
	end = tsi_host.connects == 2 ?
		lkl_net_done(tsi_host.sock, -LKL_ECONNREFUSED) : NULL;
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	lkl_sys_close(b.fd);
	lkl_test_logf("blocking connect: %ld (end %p)\n", b.ret, end);
	if (b.ret != -LKL_ECONNREFUSED || end)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * A listen is the guest's first (a loopback client is accepted from the
 * inner socket) and offered to the host (net_listen, unless bound to
 * loopback), whose inbound connections (lkl_net_accept) are accepted with
 * the peer the host gave; the listener's close is net_ready(sock).
 */
static int lkl_test_tsi_listen(void)
{
	struct lkl_sockaddr_in at = tsi_addr(0, 8080);
	struct lkl_sockaddr_in from = tsi_addr(0x0a050505, 1234);
	struct lkl_sockaddr_in lo = tsi_addr(0x7f000001, 8080), peer, peer2;
	int plen = sizeof(peer), plen2 = sizeof(peer2);
	long fd, client, a1, a2 = -1, rd = -1, bound, listened;
	char buf[8] = { 0 };
	void *end = NULL, *sock;

	memset(&peer, 0, sizeof(peer));
	memset(&peer2, 0, sizeof(peer2));
	fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	bound = lkl_sys_bind(fd, (struct lkl_sockaddr *)&at, sizeof(at));
	listened = lkl_sys_listen(fd, 4);
	sock = tsi_host.listens ? tsi_host.sock : NULL;
	client = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_STREAM, 0);
	lkl_sys_connect(client, (struct lkl_sockaddr *)&lo, sizeof(lo));
	a1 = lkl_sys_accept4(fd, (struct lkl_sockaddr *)&peer, &plen, 0);
	if (sock) {
		end = lkl_net_accept(sock, (struct lkl_sockaddr *)&from,
				     sizeof(from));
		a2 = lkl_sys_accept4(fd, (struct lkl_sockaddr *)&peer2, &plen2,
				     0);
	}
	if (end)
		lkl_net_send(end, "in", 2);
	if (a2 >= 0)
		rd = lkl_sys_read(a2, buf, sizeof(buf));
	tsi_host.readies = 0;
	lkl_sys_close(fd);
	lkl_test_logf("bind %ld listen %ld: net_listen %d time(s) (port %d); loopback client accepted %ld from %x; host's accepted %ld from %x:%d, read %ld \"%s\"; the close: net_ready %d (sock: %d)\n",
		      bound, listened, tsi_host.listens,
		      ntohs(tsi_host.addr.sin_port), a1,
		      ntohl(peer.sin_addr.lkl_s_addr), a2,
		      ntohl(peer2.sin_addr.lkl_s_addr), ntohs(peer2.sin_port),
		      rd, buf, tsi_host.readies, tsi_host.ready_obj == sock);
	if (sock)
		lkl_net_done(sock, 0);
	if (end)
		lkl_net_close(end, 0);
	lkl_sys_close(client);
	lkl_sys_close(a1);
	lkl_sys_close(a2);
	if (bound || listened || tsi_host.listens != 1 ||
	    ntohs(tsi_host.addr.sin_port) != 8080 || a1 < 0 ||
	    ntohl(peer.sin_addr.lkl_s_addr) != 0x7f000001 || !end || a2 < 0 ||
	    memcmp(&peer2, &from, 8) || rd != 2 || strcmp(buf, "in") ||
	    tsi_host.readies != 1 || tsi_host.ready_obj != sock)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * A datagram to a non-loopback address (a resolver's query) is the host's
 * (net_sendto), and its answer (lkl_net_reply) is received from the
 * address the query went to.
 */
static int lkl_test_tsi_dgram(void)
{
	struct lkl_sockaddr_in ns = tsi_addr(0x0a000203, 53), from;
	int flen = sizeof(from);
	long fd, sent, got = -1, replied = -1;
	char buf[16] = { 0 };

	memset(&from, 0, sizeof(from));
	fd = lkl_sys_socket(LKL_AF_INET, LKL_SOCK_DGRAM | LKL_O_NONBLOCK, 0);
	sent = lkl_sys_sendto(fd, "query", 5, 0, (struct lkl_sockaddr *)&ns,
			      sizeof(ns));
	if (tsi_host.sendtos) {
		replied = lkl_net_reply(tsi_host.sock,
					(struct lkl_sockaddr *)&ns, sizeof(ns),
					"answer", 6);
		lkl_net_done(tsi_host.sock, 0);
	}
	got = lkl_sys_recvfrom(fd, buf, sizeof(buf), 0,
			       (struct lkl_sockaddr *)&from, &flen);
	lkl_sys_close(fd);
	lkl_test_logf("sendto %ld: net_sendto %d time(s), %lu bytes \"%.5s\" to %x:%d; reply %ld; recvfrom %ld \"%s\" from %x:%d\n",
		      sent, tsi_host.sendtos, tsi_host.dgram_len,
		      tsi_host.dgram, ntohl(tsi_host.addr.sin_addr.lkl_s_addr),
		      ntohs(tsi_host.addr.sin_port), replied, got, buf,
		      ntohl(from.sin_addr.lkl_s_addr), ntohs(from.sin_port));
	if (sent != 5 || tsi_host.sendtos != 1 || tsi_host.dgram_len != 5 ||
	    memcmp(tsi_host.dgram, "query", 5) || replied || got != 6 ||
	    strcmp(buf, "answer") || memcmp(&from, &ns, 8))
		return TEST_FAILURE;
	return TEST_SUCCESS;
}
#endif

/*
 * The memory reported is the program's (program_mem=1G on the command
 * line), not the kernel's pool (mem=32M): sysinfo's total, and
 * /proc/meminfo's MemTotal, MemFree and MemAvailable, the pool's use
 * counted as used.
 */
static int lkl_test_meminfo(void)
{
	struct lkl_sysinfo si;
	char buf[1024], *p;
	unsigned long total = 0, free = 0, avail = 0;
	long ret, fd, n;

	ret = lkl_sys_sysinfo(&si);
	if (ret)
		return TEST_FAILURE;
	lkl_sys_mkdir("/proc", 0555);
	ret = lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
	fd = lkl_sys_open("/proc/meminfo", LKL_O_RDONLY, 0);
	n = fd < 0 ? fd : lkl_sys_read(fd, buf, sizeof(buf) - 1);
	if (fd >= 0)
		lkl_sys_close(fd);
	lkl_sys_umount("/proc", 0);
	if (ret || n <= 0)
		return TEST_FAILURE;
	buf[n] = 0;
	p = strstr(buf, "MemTotal:");
	if (p)
		total = strtoul(p + 9, NULL, 10);
	p = strstr(buf, "MemFree:");
	if (p)
		free = strtoul(p + 8, NULL, 10);
	p = strstr(buf, "MemAvailable:");
	if (p)
		avail = strtoul(p + 13, NULL, 10);

	lkl_test_logf("sysinfo totalram %lu freeram %lu (unit %u); MemTotal %lu kB, MemFree %lu kB, MemAvailable %lu kB\n",
		      (unsigned long)si.totalram, (unsigned long)si.freeram,
		      si.mem_unit, total, free, avail);
	/* free: all but what the kernel uses of its 32 MB */
	if ((unsigned long)si.totalram * si.mem_unit != PROGRAM_MEM ||
	    (unsigned long)si.freeram * si.mem_unit < PROGRAM_MEM - (32 << 20) ||
	    total != PROGRAM_MEM >> 10 || free > total ||
	    free < total - (32 << 10) || avail < free - (32 << 10) ||
	    avail > total)
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

#define CMD_LINE "mem=32M program_mem=1G loglevel=8 "

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
 * Signals, the kernel's, delivered at a syscall's return: the host's
 * signal_handler (here this, as a native host would: the handler called on
 * the thread's own stack) runs a caught signal's handler, with a ucontext
 * of the mask and alternate stack sigreturn restores. Its return says
 * whether the interrupted call restarts: sig_no_restart overrides it.
 */
struct test_ucontext {
	lkl_sigset_t mask;
	lkl_stack_t stack;
};
static __thread int sig_no_restart;
static __thread struct lkl_signal sig_last;

static int test_signal_handler(struct lkl_signal *s)
{
	struct test_ucontext uc;

	sig_last = *s;
	memcpy(&uc.mask, s->mask, sizeof(uc.mask));
	memcpy(&uc.stack, s->stack, sizeof(uc.stack));
	if (s->flags & LKL_SA_SIGINFO)
		((void (*)(int, void *, void *))s->handler)(s->sig, s->info, &uc);
	else
		((void (*)(int))s->handler)(s->sig);
	memcpy(s->mask, &uc.mask, sizeof(uc.mask));
	return !sig_no_restart;
}

struct sig_record {
	int calls, signo, code;
	long pid, inmask, blocked;
};
static __thread struct sig_record sig_rec;
static __thread int sig_pipe_w = -1;

static void sig_info_handler(int sig, void *info, void *ucp)
{
	lkl_siginfo_t *si = info;
	lkl_sigset_t cur;

	sig_rec.calls++;
	sig_rec.signo = si->si_signo;
	sig_rec.code = si->si_code;
	sig_rec.pid = si->lkl_si_pid;
	/* uc_sigmask: the mask before the signal; the handler's: it blocked */
	sig_rec.inmask = ((struct test_ucontext *)ucp)->mask.sig[0];
	if (!lkl_sys_rt_sigprocmask(LKL_SIG_BLOCK, NULL, &cur, sizeof(cur)))
		sig_rec.blocked = cur.sig[0];
}

static void sig_plain_handler(int sig)
{
	sig_rec.calls++;
	sig_rec.signo = sig;
	/* a byte for a read the signal interrupted, if it restarts */
	if (sig_pipe_w >= 0)
		lkl_sys_write(sig_pipe_w, "s", 1);
}

#define SIGBIT(s) (1UL << ((s) - 1))

static long sig_action(int sig, void *handler, unsigned long flags)
{
	struct lkl_sigaction sa = { 0 };

	sa.lkl_sa_handler = handler;
	sa.sa_flags = flags;
	return lkl_sys_rt_sigaction(sig, &sa, NULL, sizeof(sa.sa_mask));
}

static long sig_mask(int how, unsigned long set)
{
	lkl_sigset_t s = { { set } };

	return lkl_sys_rt_sigprocmask(how, &s, NULL, sizeof(s));
}

static long sig_alarm_us(long us)
{
	struct __lkl__kernel_old_itimerval it = { { 0, 0 }, { 0, us } };

	return lkl_sys_setitimer(LKL_ITIMER_REAL, &it, NULL);
}

static long elapsed_ms(struct timespec *t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0->tv_sec) * 1000 +
	       (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

struct sig_results {
	long pid, kill;
	struct sig_record info;
	struct lkl_signal op;
	long blocked_calls, pending, unblock_calls;
	long sleep, sleep_ms, sleep_calls, rem_ns;
	long read, read_calls, read_noretart;
	long suspend, suspend_calls, suspend_mask;
	long timedwait, timedwait_calls;
	int done;
};

static void signals_process(void *arg)
{
	struct sig_results *r = arg;
	struct __lkl__kernel_timespec ts = { 1, 0 }, rem = { 0, 0 };
	lkl_sigset_t set;
	struct timespec t0;
	int fds[2];
	char c;

	if (lkl_sys_new_thread_group_leader())
		return;
	r->pid = lkl_sys_getpid();

	/* a handler, SA_SIGINFO: at kill's return, with kill's siginfo */
	sig_action(LKL_SIGUSR1, sig_info_handler, LKL_SA_SIGINFO);
	r->kill = lkl_sys_kill(r->pid, LKL_SIGUSR1);
	r->info = sig_rec;
	r->op = sig_last;

	/* blocked: pending, then delivered at the unblocking call's return */
	sig_rec.calls = 0;
	sig_action(LKL_SIGUSR2, sig_plain_handler, 0);
	sig_mask(LKL_SIG_BLOCK, SIGBIT(LKL_SIGUSR2));
	lkl_sys_kill(r->pid, LKL_SIGUSR2);
	r->blocked_calls = sig_rec.calls;
	if (!lkl_sys_rt_sigpending(&set, sizeof(set)))
		r->pending = set.sig[0];
	sig_mask(LKL_SIG_UNBLOCK, SIGBIT(LKL_SIGUSR2));
	r->unblock_calls = sig_rec.calls;

	/* nanosleep interrupted by a handled SIGALRM (setitimer): -EINTR */
	sig_rec.calls = 0;
	sig_action(LKL_SIGALRM, sig_plain_handler, LKL_SA_RESTART);
	sig_alarm_us(20000);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	r->sleep = lkl_sys_nanosleep(&ts, &rem);
	r->sleep_ms = elapsed_ms(&t0);
	r->sleep_calls = sig_rec.calls;
	r->rem_ns = rem.tv_sec * 1000000000L + rem.tv_nsec;

	/*
	 * read restarted after an SA_RESTART handler (which writes the byte
	 * it then reads); not if the host says the handler moved on
	 */
	sig_rec.calls = 0;
	if (lkl_sys_pipe2(fds, 0))
		return;
	sig_pipe_w = fds[1];
	sig_alarm_us(20000);
	r->read = lkl_sys_read(fds[0], &c, 1);
	r->read_calls = sig_rec.calls;
	sig_no_restart = 1;
	sig_alarm_us(20000);
	r->read_noretart = lkl_sys_read(fds[0], &c, 1);
	sig_no_restart = 0;
	sig_pipe_w = -1;
	lkl_sys_close(fds[0]);
	lkl_sys_close(fds[1]);

	/* rt_sigsuspend: the handler runs, -EINTR, the old mask is back */
	sig_rec.calls = 0;
	sig_mask(LKL_SIG_BLOCK, SIGBIT(LKL_SIGALRM));
	sig_alarm_us(20000);
	set.sig[0] = 0;
	r->suspend = lkl_sys_rt_sigsuspend(&set, sizeof(set));
	r->suspend_calls = sig_rec.calls;
	if (!lkl_sys_rt_sigprocmask(LKL_SIG_BLOCK, NULL, &set, sizeof(set)))
		r->suspend_mask = set.sig[0];
	sig_mask(LKL_SIG_UNBLOCK, SIGBIT(LKL_SIGALRM));

	/* rt_sigtimedwait for another signal, interrupted by a handled one */
	sig_rec.calls = 0;
	set.sig[0] = SIGBIT(LKL_SIGUSR2);
	sig_mask(LKL_SIG_BLOCK, SIGBIT(LKL_SIGUSR2));
	sig_alarm_us(20000);
	r->timedwait = lkl_sys_rt_sigtimedwait(&set, NULL, &ts, sizeof(set));
	r->timedwait_calls = sig_rec.calls;

	r->done = 1;
}

/*
 * A process killed by a signal's default action (SIGTERM), when it takes
 * it: sent while blocked (pending), at the unblocking call's return
 */
static void signal_fatal_process(void *arg)
{
	struct exit_process *p = arg;

	exit_report = &p->leader;
	if (lkl_sys_new_thread_group_leader())
		return;
	p->pid = lkl_sys_getpid();
	sig_mask(LKL_SIG_BLOCK, SIGBIT(LKL_SIGTERM));
	lkl_sys_kill(p->pid, LKL_SIGTERM);
	p->tid = 1;
	sig_mask(LKL_SIG_UNBLOCK, SIGBIT(LKL_SIGTERM));
	/* not reached */
	p->leader.calls = -1;
}

static int lkl_test_signals(void)
{
	struct sig_results r = { 0 };
	struct exit_process p = { 0 };
	struct lkl_sigaction init_usr1;
	lkl_thread_t tid;
	int status = 0;
	long ret;

	tid = lkl_host_ops.thread_create(signals_process, &r);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	lkl_sys_wait4(r.pid, NULL, 0, NULL);
	/* the process's actions were its own, not init's */
	if (lkl_sys_rt_sigaction(LKL_SIGUSR1, NULL, &init_usr1,
				 sizeof(init_usr1.sa_mask)))
		return TEST_FAILURE;
	lkl_test_logf("init's SIGUSR1 handler after: %#lx\n",
		      (unsigned long)init_usr1.lkl_sa_handler);
	lkl_test_logf("process %ld: kill(SIGUSR1) %ld: handler %d time(s), signo %d code %d pid %ld, uc_sigmask %#lx, mask in handler %#lx; op: sig %d flags %#lx restart %d\n",
		      r.pid, r.kill, r.info.calls, r.info.signo, r.info.code,
		      r.info.pid, r.info.inmask, r.info.blocked, r.op.sig,
		      r.op.flags, r.op.restart);
	lkl_test_logf("SIGUSR2 blocked: %ld handler calls, pending %#lx; unblocked: %ld\n",
		      r.blocked_calls, r.pending, r.unblock_calls);
	lkl_test_logf("nanosleep(1 s) with SIGALRM in 20 ms: %ld after %ld ms, handler %ld, rem %ld ns\n",
		      r.sleep, r.sleep_ms, r.sleep_calls, r.rem_ns);
	lkl_test_logf("read, SA_RESTART: %ld (handler %ld); the host saying no restart: %ld\n",
		      r.read, r.read_calls, r.read_noretart);
	lkl_test_logf("rt_sigsuspend: %ld, handler %ld, mask after %#lx; rt_sigtimedwait: %ld, handler %ld\n",
		      r.suspend, r.suspend_calls, r.suspend_mask, r.timedwait,
		      r.timedwait_calls);

	tid = lkl_host_ops.thread_create(signal_fatal_process, &p);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	ret = lkl_sys_wait4(p.pid, &status, 0, NULL);
	lkl_test_logf("process %ld, kill(SIGTERM) itself, blocked, then unblocked: task_exit %d pid %d status %#x; wait4 %ld status %#x\n",
		      p.pid, p.leader.calls, p.leader.pid, p.leader.status, ret,
		      status);

	if (init_usr1.lkl_sa_handler)
		return TEST_FAILURE;
	if (!r.done || r.kill || r.info.calls != 1 ||
	    r.info.signo != LKL_SIGUSR1 || r.info.code != LKL_SI_USER ||
	    r.info.pid != r.pid || r.info.inmask & SIGBIT(LKL_SIGUSR1) ||
	    !(r.info.blocked & SIGBIT(LKL_SIGUSR1)) ||
	    r.op.sig != LKL_SIGUSR1 || r.op.flags != LKL_SA_SIGINFO ||
	    r.op.handler != (unsigned long)sig_info_handler || r.op.restart)
		return TEST_FAILURE;
	if (r.blocked_calls || !(r.pending & SIGBIT(LKL_SIGUSR2)) ||
	    r.unblock_calls != 1)
		return TEST_FAILURE;
	if (r.sleep != -LKL_EINTR || r.sleep_ms > 500 || r.sleep_calls != 1 ||
	    r.rem_ns < 500000000L)
		return TEST_FAILURE;
	if (r.read != 1 || r.read_calls != 1 || r.read_noretart != -LKL_EINTR)
		return TEST_FAILURE;
	if (r.suspend != -LKL_EINTR || r.suspend_calls != 1 ||
	    r.suspend_mask != SIGBIT(LKL_SIGALRM) ||
	    r.timedwait != -LKL_EINTR || r.timedwait_calls != 1)
		return TEST_FAILURE;
	if (p.tid != 1 || p.leader.calls != 1 || p.leader.pid != p.pid ||
	    p.leader.status != LKL_SIGTERM || ret != p.pid ||
	    status != LKL_SIGTERM)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * A fork through the host's fork op. The posix host's kernel memory isn't
 * shared, so no host process is forked: the test's op starts the child's
 * thread in this one, which, once its bootstrap returns, runs the
 * program's side of the clone in the child. The op is called inside the
 * clone, on the parent's thread; the child's task bootstraps when first
 * scheduled and is the child process (its pid the clone's result, init its
 * parent, init's files copied); its exit status is wait4's.
 * CLONE_CHILD_SETTID's word is written once, at the child's first
 * schedule, not again at its later switches (it was, at every one).
 */
struct fork_child {
	void (*f)(void *);
	void *arg;
	void (*program)(struct fork_child *c);
	volatile int execd, exiting;
	int fds[4];	/* parent to child, child to parent */
	long pid, ppid, settid_first, settid_later, trips, close;
};
static struct fork_child fork_child;
static int fork_calls;
static pthread_t fork_caller;
static volatile int settid_word;

#define FORK_TRIPS 10

static void fork_child_thread(void *arg)
{
	struct fork_child *c = arg;

	c->f(c->arg);
	/* the program, after its clone returned 0 in the child */
	c->program(c);
}

static void fork_child_program(struct fork_child *c)
{
	char b;
	int i;

	c->settid_first = settid_word;
	settid_word = 0;
	c->pid = lkl_sys_getpid();
	c->ppid = lkl_sys_getppid();
	for (i = 0; i < FORK_TRIPS; i++) {
		if (lkl_sys_read(c->fds[0], &b, 1) != 1 ||
		    lkl_sys_write(c->fds[3], &b, 1) != 1)
			break;
	}
	c->trips = i;
	c->settid_later = settid_word;
	/* its copy of init's files: closing one leaves init's open */
	c->close = lkl_sys_close(c->fds[1]);
	lkl_sys_exit_group(9);
}

static lkl_thread_t test_fork(void (*f)(void *), void *arg)
{
	fork_calls++;
	fork_caller = pthread_self();
	fork_child.f = f;
	fork_child.arg = arg;
	return lkl_host_ops.thread_create(fork_child_thread, &fork_child);
}

static int lkl_test_fork(void)
{
	struct fork_child *c = &fork_child;
	long pid, ret, w = -1;
	int status = 0, i, trips = 0;
	char b = 'x';

	if (lkl_sys_pipe2(&c->fds[0], 0) || lkl_sys_pipe2(&c->fds[2], 0))
		return TEST_FAILURE;
	c->program = fork_child_program;
	pid = lkl_sys_clone(LKL_SIGCHLD | LKL_CLONE_CHILD_SETTID, 0, NULL,
			    (int *)&settid_word, 0);
	if (pid > 0) {
		for (i = 0; i < FORK_TRIPS; i++) {
			if (lkl_sys_write(c->fds[1], &b, 1) != 1 ||
			    lkl_sys_read(c->fds[2], &b, 1) != 1)
				break;
			trips++;
		}
		ret = lkl_sys_wait4(pid, &status, 0, NULL);
		w = lkl_sys_write(c->fds[1], &b, 1);
	} else {
		ret = pid;
	}
	for (i = 0; i < 4; i++)
		lkl_sys_close(c->fds[i]);
	/* a clone making a thread is new_thread's */
	i = lkl_sys_clone(LKL_CLONE_VM | LKL_CLONE_SIGHAND | LKL_CLONE_THREAD,
			  0, NULL, NULL, 0);

	lkl_test_logf("clone = %ld: fork op %d call(s), on the parent's thread %d; child: getpid %ld getppid %ld, settid %ld at first, %ld after %ld/%d round trips (parent %d), close %ld; wait4 %ld status %#x; init's write end after %ld\n",
		      pid, fork_calls, pthread_equal(fork_caller, pthread_self()),
		      c->pid, c->ppid, c->settid_first, c->settid_later,
		      c->trips, FORK_TRIPS, trips, c->close, ret, status, w);
	lkl_test_logf("clone(CLONE_THREAD) = %d\n", i);

	if (pid <= 1 || fork_calls != 1 ||
	    !pthread_equal(fork_caller, pthread_self()) || c->pid != pid ||
	    c->ppid != 1 || c->settid_first != pid || c->settid_later ||
	    c->trips != FORK_TRIPS || trips != FORK_TRIPS || c->close ||
	    ret != pid || status != 9 << 8 || w != 1 || i != -LKL_EINVAL)
		return TEST_FAILURE;
	return TEST_SUCCESS;
}

/*
 * exec's kernel half, new_exec(name), the host having loaded the program:
 * in a process of two threads, after a handler for SIGUSR1, SIGUSR2
 * ignored, SIGUSR1 blocked and pending, a close-on-exec pipe and a plain
 * one, an alternate stack, set_tid_address and a name: the other thread
 * (blocked in a read) ends, the close-on-exec fds are closed, SIGUSR1's
 * action is the default and SIGUSR2's still ignored, SIGUSR1 still blocked
 * and pending, the alternate stack and clear_child_tid gone (the exit
 * doesn't write the word), the name the new one's.
 */
#define PR_SET_NAME 15
#define PR_GET_NAME 16
#define SS_DISABLE 2

struct exec_results {
	struct exit_process p;
	long ret, pid, cloexec_fd, plain_fd, usr1, usr2, blocked, pending;
	long ss_sp, ss_size, ss_flags;
	char comm[16];
	int ctid, ctid_exec;
};

static void exec_process(void *arg)
{
	struct exec_results *r = arg;
	struct exit_process *p = &r->p;
	static char altstack[16384];
	lkl_stack_t ss = { altstack, 0, sizeof(altstack) };
	struct lkl_sigaction sa;
	lkl_sigset_t set;
	struct timespec ts = { 0, 20000000 };
	int cfds[2], fds[2];
	lkl_thread_t tid;

	if (lkl_sys_new_thread_group_leader())
		return;
	p->pid = lkl_sys_getpid();
	if (lkl_sys_pipe2(p->fds, 0) || lkl_sys_pipe2(cfds, LKL_O_CLOEXEC) ||
	    lkl_sys_pipe2(fds, 0))
		return;
	/* (blocked in the other thread too, which would take it) */
	sig_mask(LKL_SIG_BLOCK, SIGBIT(LKL_SIGUSR1));
	p->tid = lkl_sys_new_thread();
	tid = lkl_host_ops.thread_create(thread_blocked, p);
	if (!tid)
		return;
	sig_action(LKL_SIGUSR1, sig_plain_handler, 0);
	sig_action(LKL_SIGUSR2, (void *)1 /* SIG_IGN */, 0);
	lkl_sys_kill(p->pid, LKL_SIGUSR1);
	lkl_sys_sigaltstack(&ss, NULL);
	r->ctid = 1;
	lkl_sys_set_tid_address(&r->ctid);
	lkl_sys_prctl(PR_SET_NAME, (unsigned long)"before", 0, 0, 0);
	/* (if the thread isn't reading yet, it dies at its next call) */
	nanosleep(&ts, NULL);

	r->ret = lkl_sys_new_exec("/nix/store/x-prog/bin/after");

	r->pid = lkl_sys_getpid();
	r->cloexec_fd = lkl_sys_fcntl(cfds[0], LKL_F_GETFD, 0);
	r->plain_fd = lkl_sys_fcntl(fds[0], LKL_F_GETFD, 0);
	lkl_sys_rt_sigaction(LKL_SIGUSR1, NULL, &sa, sizeof(sa.sa_mask));
	r->usr1 = (long)sa.lkl_sa_handler;
	lkl_sys_rt_sigaction(LKL_SIGUSR2, NULL, &sa, sizeof(sa.sa_mask));
	r->usr2 = (long)sa.lkl_sa_handler;
	lkl_sys_rt_sigprocmask(LKL_SIG_BLOCK, NULL, &set, sizeof(set));
	r->blocked = set.sig[0];
	lkl_sys_rt_sigpending(&set, sizeof(set));
	r->pending = set.sig[0];
	lkl_sys_sigaltstack(NULL, &ss);
	r->ss_sp = (long)ss.ss_sp;
	r->ss_size = ss.ss_size;
	r->ss_flags = ss.ss_flags;
	lkl_sys_prctl(PR_GET_NAME, (unsigned long)r->comm, 0, 0, 0);
	/* not written at the exec (the address may be the new program's) */
	r->ctid_exec = r->ctid;
	/* nor at the exit: clear_child_tid is gone */
	r->ctid = 77;
	lkl_sys_close(fds[0]);
	lkl_sys_close(fds[1]);
	lkl_sys_exit(0);
}

/* a vfork's child: its parent's clone returns at its exec, not its exit */
static void vfork_child_program(struct fork_child *c)
{
	struct timespec ts = { 0, 50000000 };

	c->execd = 1;
	lkl_sys_new_exec("prog");
	nanosleep(&ts, NULL);
	c->exiting = 1;
	lkl_sys_exit_group(0);
}

static int lkl_test_exec(void)
{
	struct exec_results r = { 0 };
	struct fork_child *c = &fork_child;
	lkl_thread_t tid;
	long pid, vexiting = -1, vexecd = -1, ret;
	int status = -1;

	tid = lkl_host_ops.thread_create(exec_process, &r);
	if (!tid || lkl_host_ops.thread_join(tid))
		return TEST_FAILURE;
	while (!__atomic_load_n(&r.p.thread.calls, __ATOMIC_SEQ_CST))
		;
	lkl_sys_wait4(r.p.pid, &status, 0, NULL);
	lkl_sys_close(r.p.fds[0]);
	lkl_sys_close(r.p.fds[1]);
	lkl_test_logf("process %ld: new_exec %ld; the other thread: task_exit %d, pid %d; getpid %ld; F_GETFD close-on-exec %ld, plain %ld; SIGUSR1 action %#lx, SIGUSR2 %#lx; blocked %#lx pending %#lx; sigaltstack %#lx size %ld flags %ld; name \"%s\"; clear_child_tid's word %d after the exec, %d after the exit; exit status %#x\n",
		      r.p.pid, r.ret, r.p.thread.calls, r.p.thread.pid, r.pid,
		      r.cloexec_fd, r.plain_fd, r.usr1, r.usr2, r.blocked,
		      r.pending, r.ss_sp, r.ss_size, r.ss_flags, r.comm,
		      r.ctid_exec, r.ctid, status);

	memset(c, 0, sizeof(*c));
	c->program = vfork_child_program;
	pid = lkl_sys_clone(LKL_CLONE_VM | LKL_CLONE_VFORK | LKL_SIGCHLD, 0,
			    NULL, NULL, 0);
	if (pid > 0) {
		vexecd = c->execd;
		vexiting = c->exiting;
		lkl_sys_wait4(pid, NULL, 0, NULL);
	}
	lkl_test_logf("vfork: clone %ld returned with the child exec'd %ld, exiting %ld\n",
		      pid, vexecd, vexiting);

	/*
	 * init's process, as the first program's exec is: its parked thread
	 * (host0) isn't one to kill and wait for (it can't die: the exec hung)
	 */
	ret = lkl_sys_new_exec("/init");
	lkl_test_logf("init's new_exec: %ld, getpid then %ld\n", ret,
		      lkl_sys_getpid());
	if (ret || lkl_sys_getpid() != 1)
		return TEST_FAILURE;

	if (r.ret || r.p.thread.calls != 1 || r.p.thread.pid ||
	    r.pid != r.p.pid || r.cloexec_fd != -LKL_EBADF || r.plain_fd ||
	    r.usr1 != 0 || r.usr2 != 1 ||
	    r.blocked != SIGBIT(LKL_SIGUSR1) ||
	    r.pending != SIGBIT(LKL_SIGUSR1) || r.ss_sp || r.ss_size ||
	    !(r.ss_flags & SS_DISABLE) || strcmp(r.comm, "after") ||
	    r.ctid_exec != 1 || r.ctid != 77 || status != 0)
		return TEST_FAILURE;
	if (pid <= 0 || vexecd != 1 || vexiting)
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
	LKL_TEST(memblk_extents),
	LKL_TEST(meminfo),
#ifdef LKL_CONFIG_LKL_TSI
	LKL_TEST(tsi_connect),
	LKL_TEST(tsi_listen),
	LKL_TEST(tsi_dgram),
	LKL_TEST(tsi_syscall),
#endif
	LKL_TEST(hostfd_poll),
	LKL_TEST(hostfd_nonblock),
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
	LKL_TEST(signals),
	LKL_TEST(fork),
	LKL_TEST(exec),
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
	lkl_host_ops.signal_handler = test_signal_handler;
	lkl_host_ops.fork = test_fork;
#ifdef LKL_CONFIG_LKL_TSI
	lkl_host_ops.net_connect = test_net_connect;
	lkl_host_ops.net_listen = test_net_listen;
	lkl_host_ops.net_sendto = test_net_sendto;
	lkl_host_ops.net_ready = test_net_ready;
#endif
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
