#ifndef _ASM_UAPI_LKL_HOST_OPS_H
#define _ASM_UAPI_LKL_HOST_OPS_H

/* Defined in {posix,nt}-host.c */
struct lkl_mutex;
struct lkl_sem;
struct lkl_tls_key;
typedef unsigned long lkl_thread_t;
struct lkl_jmp_buf {
	unsigned long buf[128];
};
struct lkl_pci_dev;
struct sockaddr;

/**
 * lkl_signal - a caught signal's delivery (signal_handler)
 *
 * @sig - the signal's number
 * @restart - whether the syscall it interrupted runs again after the
 * handler (its -ERESTART* result, the handler's SA_RESTART)
 * @handler - the handler, sa_handler (or sa_sigaction) as the program set it
 * @flags - its sa_flags as the program set them (SA_SIGINFO: the handler
 * takes (sig, info, ucontext); SA_ONSTACK; the kernel has done SA_NODEFER,
 * SA_RESETHAND and sa_mask)
 * @info - the signal's siginfo_t (Linux's layout, 128 bytes)
 * @mask - the sigset_t the handler's sigreturn restores (uc_sigmask): the
 * mask before the delivery; the host writes back the ucontext's
 * @stack - the thread's alternate signal stack (stack_t, as sigaltstack
 * sets it: SS_DISABLE if none), uc_stack; the host writes back the
 * ucontext's
 */
struct lkl_signal {
	int sig;
	int restart;
	unsigned long handler;
	unsigned long flags;
	void *info;
	void *mask;
	void *stack;
};

enum lkl_pci_irq_type {
	LKL_PCI_IRQ_MSI = 1,
	LKL_PCI_IRQ_MSIX = 2,
};

/**
 * lkl_dev_pci_ops - PCI host operations
 *
 * These operations would be a wrapper of userspace PCI drvier and
 * must be provided by a host library or by the application.
 *
 * @add - add a new PCI device; returns a handler or NULL if fails
 * @remove - release resources
 * @init_irq - allocate resources for interrupts
 * @msi_init - allocate resources for MSI/MSI-X interrupts. @irqs contains
 *             one LKL IRQ per hardware vector, with non-positive entries left
 *             unassigned.
 * @msi_teardown - release MSI/MSI-X interrupt resources
 * @read - read the PCI Configuration Space
 * @write - write the PCI Configuration Space
 * @resource_alloc - map BARx and return the mapped address. x is resource_index
 *
 * @map_page - return the DMA address of pages; vaddr might not be page-aligned
 * @unmap_page - cleanup DMA region if needed
 *
 */
struct lkl_dev_pci_ops {
	struct lkl_pci_dev *(*add)(const char *name, void *kernel_ram,
				   unsigned long ram_size);
	void (*remove)(struct lkl_pci_dev *dev);
	int (*irq_init)(struct lkl_pci_dev *dev, int irq);
	int (*msi_init)(struct lkl_pci_dev *dev, int type, int nvec,
			int *irqs);
	void (*msi_teardown)(struct lkl_pci_dev *dev, int type);
	int (*read)(struct lkl_pci_dev *dev, int where, int size, void *val);
	int (*write)(struct lkl_pci_dev *dev, int where, int size, void *val);
	void *(*resource_alloc)(struct lkl_pci_dev *dev,
				unsigned long resource_size,
				int resource_index);
	unsigned long long (*map_page)(struct lkl_pci_dev *dev, void *vaddr,
				       unsigned long size);
	void (*unmap_page)(struct lkl_pci_dev *dev,
			   unsigned long long dma_handle, unsigned long size);
};

enum lkl_prot {
	LKL_PROT_NONE = 0,
	LKL_PROT_READ = 1,
	LKL_PROT_WRITE = 2,
	LKL_PROT_EXEC = 4,
};

/**
 * lkl_host_operations - host operations used by the Linux kernel
 *
 * These operations must be provided by a host library or by the application
 * itself.
 *
 * @virtio_devices - string containg the list of virtio devices in virtio mmio
 * command line format. This string is appended to the kernel command line and
 * is provided here for convenience to be implemented by the host library.
 *
 * @print - optional operation that receives console messages
 *
 * @panic - called during a kernel panic
 *
 * @sem_alloc - allocate a host semaphore an initialize it to count
 * @sem_free - free a host semaphore
 * @sem_up - perform an up operation on the semaphore
 * @sem_down - perform a down operation on the semaphore
 *
 * @mutex_alloc - allocate and initialize a host mutex; the recursive parameter
 * determines if the mutex is recursive or not
 * @mutex_free - free a host mutex
 * @mutex_lock - acquire the mutex
 * @mutex_unlock - release the mutex
 *
 * @thread_create - create a new thread and run f(arg) in its context; returns a
 * thread handle or 0 if the thread could not be created
 * @thread_detach - on POSIX systems, free up resources held by
 * pthreads. Noop on Win32.
 * @thread_exit - terminates the current thread
 * @thread_join - wait for the given thread to terminate. Returns 0
 * for success, -1 otherwise
 * @thread_stack - get the thread stack base and size of the current thread
 *
 * @tls_alloc - allocate a thread local storage key; returns 0 if successful; if
 * destructor is not NULL it will be called when a thread terminates with its
 * argument set to the current thread local storage value
 * @tls_free - frees a thread local storage key; returns 0 if succesful
 * @tls_set - associate data to the thread local storage key; returns 0 if
 * successful
 * @tls_get - return data associated with the thread local storage key or NULL
 * on error
 *
 * @mem_alloc - allocate memory
 * @mem_free - free memory
 * @page_alloc - allocate page aligned memory
 * @page_free - free memory allocated by page_alloc
 *
 * @timer_create - allocate a host timer that runs fn() when the timer
 * fires.
 * @timer_free - disarms and free the timer
 * @timer_set_oneshot - arm the timer to fire once, after delta ns. An idle
 * kernel is tickless: the timer is armed for its next timer only, and with
 * none, as far off as it goes (146 years on 64-bit: at most KTIME_MAX / 2,
 * so the host's now + delta doesn't wrap a signed 64-bit clock).
 *
 * @ioremap - searches for an I/O memory region identified by addr and size and
 * returns a pointer to the start of the address range that can be used by
 * iomem_access
 * @iomem_acess - reads or writes to and I/O memory region; addr must be in the
 * range returned by ioremap
 *
 * @jmp_buf_set - runs the give function and setups a jump back point by saving
 * the context in the jump buffer; jmp_buf_longjmp can be called from the give
 * function or any callee in that function to return back to the jump back
 * point
 *
 * NOTE: we can't return from jmp_buf_set before calling jmp_buf_longjmp or
 * otherwise the saved context (stack) is not going to be valid, so we must pass
 * the function that will eventually call longjmp here
 *
 * @jmp_buf_longjmp - perform a jump back to the saved jump buffer
 *
 * @memcpy - copy memory
 * @memset - set memory
 *
 * @mmap - map anonymous memory at the given address with the given size and
 * protection
 * @munmap - unmap previously mapped memory
 *
 * @pci_ops - pointer to PCI host operations
 *
 * @task_exit - the calling host thread's task exited in its syscall (exit,
 * exit_group, or killed: a fatal signal, such as another thread's
 * exit_group, ends a task at its syscall's start or end). Called with the
 * CPU released and the thread's task forgotten (its TLS cleared); it must
 * not return: the host ends the thread, or jumps out of lkl_syscall() to a
 * frame of its own. If the task was its process's last thread, pid is the
 * process's (its thread group id) and status its wait status (as wait4
 * reports it: exit code << 8, or the fatal signal's number); else both are
 * 0. init's end is pid 1, after its exit has run (its files closed).
 * Optional: without it, the thread ends with thread_exit.
 *
 * @signal_handler - run a caught signal's handler, a function of the
 * program's, at the return of the task's syscall to the program (the
 * kernel's signal delivery: get_signal and the default actions are its
 * own). Called on the task's host thread with the CPU released; the host
 * calls sig->handler as the program would be interrupted at that point (on
 * its stack or, for SA_ONSTACK, sig->stack's, with a ucontext whose
 * uc_sigmask is *sig->mask and uc_stack *sig->stack, and as the restorer
 * the program gave, which the kernel doesn't keep: LKL's sigaction has no
 * sa_restorer). The handler's own syscalls are made as any of the task's.
 * When it returns, *sig->mask and *sig->stack are what sigreturn restores
 * (the ucontext's, which the handler may have changed). Return: whether
 * the interrupted syscall runs again, if sig->restart says it would (its
 * -ERESTART* result, SA_RESTART); 0 ends it with -EINTR (a handler that
 * moved the program on: changed its ucontext's program counter). A handler
 * that doesn't return (siglongjmp) leaves the kernel's frames behind,
 * which is safe: nothing is held. Optional: without it, a caught signal's
 * handler is skipped, as if it had returned at once.
 *
 * @fork - a program's fork (clone making a process, without CLONE_THREAD:
 * fork, vfork, posix_spawn's CLONE_VM | CLONE_VFORK) as a fork of the host
 * process. Called on the parent's host thread inside its clone, holding
 * the CPU (it may not call into the kernel). The host forks its process,
 * the kernel's memory (its data, its pool, the threads' kernel stacks)
 * staying shared between the two (mapped shared before the fork: the
 * kernel itself doesn't assume one process), and in the child starts a
 * thread that calls f(arg), which returns once the kernel has first
 * scheduled the child's task, with the CPU released; then the host resumes
 * the program there as the clone's return in the child, with 0 (the
 * parent's registers at the clone; the stack clone was given, if any).
 * Returns the thread's id, what thread_self() returns on it, unique among
 * all the host processes' threads (the CPU's owner is one); 0 if it failed
 * (clone returns -EAGAIN). If the clone fails after the fork (a signal
 * came, and it restarts), f(arg) calls thread_exit on the child's thread
 * instead of returning, and thread_join(id), from the parent's process,
 * must wait for that: the host ends the child process. The kernel's host
 * ops (semaphores, mutexes, timers) must work across the processes. The
 * child's exit is reported by task_exit, as any task's. Optional: without
 * it, clone fails with -ENOSYS.
 *
 * @hostfd_rw - read or write (flags has LKL_HOSTFD_WRITE) len bytes at buf
 * on host fd fd, for a guest file made by host_fd(fd, flags)
 * (asm/syscalls.h): returns the bytes done or a negative Linux errno. Called
 * on the calling task's host thread with the CPU released, the task asleep
 * in the kernel, so it may block for as long as the I/O takes. A write may
 * also take less than len (what the fd takes without waiting), or wait a
 * while and return -ERESTARTSYS with nothing done: the kernel asks again for
 * the rest of a blocking write until it is whole or a signal is pending
 * (then the call returns what was written, or is restarted or interrupted
 * if nothing was), as a pipe's write. Not with
 * LKL_HOSTFD_NONBLOCK (the file is O_NONBLOCK): then if the fd isn't ready
 * now it returns -EAGAIN at once, as a nonblocking read or write would, and
 * (with hostfd_poll) watches the fd as hostfd_poll does for what isn't
 * ready, calling lkl_hostfd_ready once it may be: an edge-triggered epoll
 * that read to -EAGAIN waits for that wakeup.
 * Without it, the files' reads and writes fail with -EINVAL.
 *
 * Networking (arch/lkl/kernel/tsi.c, CONFIG_LKL_TSI): a program's AF_INET
 * sockets are its own for loopback (127.0.0.0/8, 0.0.0.0) and the host's
 * for any other address. sock is the program's socket, end the host's end
 * of a connection (see lkl_net_* below). Each op is called on the calling
 * task's host thread with the CPU held: it must not block or call into the
 * kernel, only take note and wake whatever will act (the host's own
 * thread), and return at once. Each that returns 0 hands the host a
 * reference to sock, which it gives back with lkl_net_done(sock, err);
 * until then sock stays valid, also after the program closes it.
 *
 * @net_connect - the program connects sock (a stream) to addr (sockaddr_in,
 * len bytes); pid is its process's. Return 0, then lkl_net_done(sock, 0)
 * once connected (it returns the connection's end) or (sock, -errno) for a
 * refusal; or return -errno at once. Meanwhile a blocking connect waits, a
 * nonblocking one returned -EINPROGRESS (poll: POLLOUT, or POLLERR and
 * SO_ERROR). Without it, such connects fail with -ENETUNREACH.
 *
 * @net_listen - the program listens on sock, bound to addr (not loopback:
 * the inner socket listens for the guest's own clients first). Return 0 to
 * serve the address too: each inbound connection is lkl_net_accept(sock,
 * peer, len), until the host's lkl_net_done(sock, err) or net_ready(sock),
 * the program's close. Nonzero: the listen stays the guest's alone.
 *
 * @net_sendto - a datagram of size bytes at buf from sock to addr (not
 * loopback): a query to the program's resolver, or any other. The host
 * copies it. Return 0 (answers come as lkl_net_reply(sock, ...), then
 * lkl_net_done(sock, err), err nonzero being the socket's error once) or
 * -errno, sendmsg's error. Without it, -ENETUNREACH.
 *
 * @net_ready - obj is an end: data arrived, room was made, or the guest's
 * side was shut down or closed (lkl_net_recv/send now get further); or obj
 * is a sock the host holds a reference to: the program closed it (a
 * connect it gave up, a listen it ended; lkl_net_done still gives the
 * reference back).
 *
 * @hostfd_poll - a poll of a guest file made by host_fd(fd, flags): return
 * which of events (POLLIN, POLLOUT) host fd fd has ready now, without
 * waiting. For any not ready, the host watches the fd and calls
 * lkl_hostfd_ready(fd) once one is (the guest's pollers then ask again).
 * Called with the CPU held. Without it, the files poll as always ready.
 *
 * @net_fd_shutdown - the program shut down (how: SHUT_RD, SHUT_WR, SHUT_RDWR)
 * a connection backed by host fd fd (lkl_net_done_fd, lkl_net_accept_fd),
 * which the host may not shut down itself: it asks whoever can.
 *
 * @net_fd_close - the program's last reference to such a connection is
 * gone: the host closes fd (and ends the connection).
 */
/* hostfd_rw's flags */
#define LKL_HOSTFD_WRITE	1
#define LKL_HOSTFD_NONBLOCK	2

struct lkl_host_operations {
	const char *virtio_devices;

	void (*print)(const char *str, int len);
	void (*panic)(void);

	struct lkl_sem* (*sem_alloc)(int count);
	void (*sem_free)(struct lkl_sem *sem);
	void (*sem_up)(struct lkl_sem *sem);
	void (*sem_down)(struct lkl_sem *sem);

	struct lkl_mutex *(*mutex_alloc)(int recursive);
	void (*mutex_free)(struct lkl_mutex *mutex);
	void (*mutex_lock)(struct lkl_mutex *mutex);
	void (*mutex_unlock)(struct lkl_mutex *mutex);

	lkl_thread_t (*thread_create)(void (*f)(void *), void *arg);
	void (*thread_detach)(void);
	void (*thread_exit)(void);
	int (*thread_join)(lkl_thread_t tid);
	lkl_thread_t (*thread_self)(void);
	int (*thread_equal)(lkl_thread_t a, lkl_thread_t b);
	void *(*thread_stack)(unsigned long *size);

	struct lkl_tls_key *(*tls_alloc)(void (*destructor)(void *));
	void (*tls_free)(struct lkl_tls_key *key);
	int (*tls_set)(struct lkl_tls_key *key, void *data);
	void *(*tls_get)(struct lkl_tls_key *key);

	void* (*mem_alloc)(unsigned long);
	void (*mem_free)(void *);
	void* (*page_alloc)(unsigned long size);
	void (*page_free)(void *addr, unsigned long size);

	unsigned long long (*time)(void);

	void* (*timer_alloc)(void (*fn)(void));
	int (*timer_set_oneshot)(void *timer, unsigned long delta);
	void (*timer_free)(void *timer);

	void* (*ioremap)(long addr, int size);
	int (*iomem_access)(const volatile void *addr, void *val, int size,
			    int write);

	void (*jmp_buf_set)(struct lkl_jmp_buf *jmpb, void (*f)(void));
	void (*jmp_buf_longjmp)(struct lkl_jmp_buf *jmpb, int val);

	void* (*memcpy)(void *dest, const void *src, unsigned long count);
	void* (*memset)(void *s, int c, unsigned long count);
	void* (*memmove)(void *dest, const void *src, unsigned long count);

	void* (*mmap)(void *addr, unsigned long size, enum lkl_prot prot);
	int (*munmap)(void *addr, unsigned long size);

	void (*shmem_init)(unsigned long size);
	void *(*shmem_mmap)(void *addr, unsigned long pg_off, unsigned long size,
				enum lkl_prot prot);

	struct lkl_dev_pci_ops *pci_ops;

	void (*task_exit)(int pid, int status);
	long (*hostfd_rw)(int fd, void *buf, unsigned long len, int flags);
	int (*signal_handler)(struct lkl_signal *sig);
	lkl_thread_t (*fork)(void (*f)(void *), void *arg);
	int (*net_connect)(void *sock, int pid, const struct sockaddr *addr, int len);
	int (*net_listen)(void *sock, int pid, const struct sockaddr *addr, int len);
	int (*net_sendto)(void *sock, int pid, const struct sockaddr *addr, int len, const void *buf, unsigned long size);
	void (*net_ready)(void *obj);
	void (*net_fd_shutdown)(long fd, int how);
	void (*net_fd_close)(long fd);
	unsigned int (*hostfd_poll)(int fd, unsigned int events);
};

/**
 * lkl_init - initializes LKL
 *
 * This function needs to be called this before any other LKL function.
 *
 * @lkl_ops - pointer to host operations
 */
int lkl_init(struct lkl_host_operations *lkl_ops);

/**
 * lkl_start_kernel - starts the kernel
 *
 * @cmd_line - format for command line string that is going to be used to
 * generate the Linux kernel command line
 */
int lkl_start_kernel(const char *cmd_line, ...);

/**
 * lkl_cleanup - cleanup LKL
 *
 * To be called after lkl_sys_shutdown. Once this function is called no more LKL
 * calls can be made unless @lkl_init is called again.
 */
void lkl_cleanup(void);

/**
 * lkl_is_running - returns 1 if the kernel is currently running
 */
int lkl_is_running(void);

/*
 * The host's calls for networking (see the net_* ops), each from a thread
 * of its own, never from inside a host op. They take the CPU and run as a
 * kernel thread of their own (tsi), so none blocks on the network: an end
 * is nonblocking.
 *
 * lkl_net_done - give back the reference an op handed over, with the
 * outcome: for a connect, 0 (connected: returns the connection's end, or
 * NULL if the program closed sock meanwhile, or the kernel had no memory)
 * or -errno (the program's connect fails with it); for a listen, the end of
 * the host's serving it; for a datagram, 0 or the socket's error.
 *
 * lkl_net_accept - an inbound connection from peer (sockaddr_in, len bytes)
 * for a sock the host serves (net_listen): its end, queued for the
 * program's accept, which sees peer as getpeername. NULL: refused (the
 * program closed sock, its backlog is full, no memory).
 *
 * lkl_net_done_fd, lkl_net_accept_fd - as lkl_net_done(sock, 0) for a
 * connect and lkl_net_accept, the connection a host fd: the program's
 * socket reads and writes fd itself, as a host_fd file does (hostfd_rw,
 * hostfd_poll), with no end and no copy; its shutdown and close are
 * net_fd_shutdown and net_fd_close. 0, or -errno: nothing took fd (the
 * host still owns it).
 *
 * lkl_net_reply - a datagram for sock, from address from (the resolver's,
 * as the program sent to): 0 or -errno.
 *
 * lkl_net_recv, lkl_net_send - read what the program wrote, write what it
 * will read: bytes done; from recv, 0 once the program shut its writing
 * down (EOF; it may still read), -EPIPE once it closed its socket (after
 * all it wrote was read); -EAGAIN when there's nothing to read or no room
 * (net_ready(end) comes when there is); -EPIPE from send once the program
 * closed; -ECONNRESET once it closed with data unread.
 * lkl_net_shutdown - shutdown(how) of the end: SHUT_WR, the program reads EOF.
 * lkl_net_close - the end released; with reset, the program's next call on
 * its socket fails with ECONNRESET (else it reads EOF).
 *
 * lkl_net_syscall - syscall no with params (as lkl_syscall's) made by the
 * host's own code that serves the network (a library the provider uses:
 * a getrandom, a read of /proc/self/maps for a stack's bounds), run as the
 * kernel task the lkl_net_* calls run as: no thread of the program's (a
 * host thread's first lkl_syscall makes it a thread of init's process,
 * which keeps that process alive and its files open until the thread makes
 * another call, and holds up an exec), its files not the program's. Not
 * exit, exit_group or the host-task calls (new_thread, attach_thread,
 * new_thread_group_leader, new_exec): -EINVAL.
 */
void *lkl_net_done(void *sock, int err);
void *lkl_net_accept(void *sock, const struct sockaddr *peer, int len);
int lkl_net_done_fd(void *sock, long fd);
int lkl_net_accept_fd(void *sock, const struct sockaddr *peer, int len, long fd);
long lkl_net_reply(void *sock, const struct sockaddr *from, int len,
		   const void *buf, unsigned long size);
long lkl_net_recv(void *end, void *buf, unsigned long len);
long lkl_net_send(void *end, const void *buf, unsigned long len);
long lkl_net_shutdown(void *end, int how);
void lkl_net_close(void *end, int reset);
long lkl_net_syscall(long no, long *params);

/*
 * lkl_hostfd_ready - host fd fd, which a guest file of host_fd's polled not
 * ready (hostfd_poll), may be ready now: the guest's pollers ask again.
 * From a thread of the host's own, never from inside a host op.
 */
void lkl_hostfd_ready(int fd);

int lkl_printf(const char *fmt, ...);
void lkl_bug(const char *fmt, ...);

#endif
