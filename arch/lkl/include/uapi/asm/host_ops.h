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
 * @timer_set_oneshot - arm the timer to fire once, after delta ns.
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
 * @blk_count - number of host block devices (see arch/lkl/drivers/hostblk.c)
 * @blk_capacity - size of a host block device in bytes
 * @blk_rw - read or write len bytes at the given 512-byte sector, directly to
 * or from buf; returns 0 or a negative errno
 * @blk_flush - make completed writes durable; returns 0 or a negative errno
 *
 * @copy_from_user - copy n bytes from user memory; returns the number of bytes
 * not copied. Used instead of memcpy when user memory is not part of the
 * kernel's address space (e.g. a separate wasm memory per process); needs
 * CONFIG_UACCESS_MEMCPY=n
 * @copy_to_user - the reverse of copy_from_user
 *
 * @user_thread_create - create the host thread of a task cloned from
 * userspace (fork by a host task's syscall); called on the parent's host
 * thread, inside the syscall. The thread runs f(arg), which returns once the
 * kernel has first scheduled the task and made it the thread's host task,
 * with the CPU released; the host then resumes the child's user context,
 * where clone returns 0. Returns a thread handle or 0 if the thread could not
 * be created. Without it, clone from userspace fails with -ENOSYS.
 * @vm_thread_create - the same for a task cloned with CLONE_VM (a thread, or
 * vfork's child): it shares the caller's memory, so the host starts it at the
 * function its libc left at the top of stack (the clone's new stack pointer),
 * instead of resuming a copy. Without it, clone with CLONE_VM fails with
 * -EINVAL.
 *
 * @signal_handler - run a process's signal handler (a function pointer in the
 * process) for sig; called on the way back from a syscall, on the process's
 * host thread, with the CPU released. info: for an SA_SIGINFO handler, which
 * takes (sig, info, ucontext), the siginfo_t to copy to the process (NULL
 * for a plain handler). mask: the signal mask (sigset_t) that sigreturn
 * restores, as the ucontext's uc_sigmask, which the handler may change.
 * Without it, processes get no signals delivered. Returns nonzero if the
 * handler moved the thread out of the syscall it was in (as by changing the
 * context's program counter natively): the syscall then ends with -EINTR
 * rather than restarting.
 * @task_exit - the calling host thread's task exited during a syscall (exit,
 * exit_group, a fatal signal), with the given wait status. The thread should
 * end without returning; if it returns, lkl_syscall returns 0 and the thread's
 * next syscall gets a new host task.
 *
 * @exec_load - make sure the host has the program identified by key (a file
 * and its version), to run it with exec. First called with data == NULL: 0
 * means the host has it; 1 asks for the file's contents, which follow in
 * order, len bytes at a time at off of size. Returns 0 once the host has the
 * program, or a negative errno (-ENOEXEC: not a program it can run).
 * @exec_lib - the shared libraries for key's program, after exec_load: returns
 * 1 with the path of the next file to try in path (size bytes), 0 when it has
 * them all, or a negative errno. The kernel opens that path and sends the
 * file with exec_load, then calls again with err (0 or why it couldn't) and,
 * if it sent the file, loaded: the library's key. Optional.
 * @exec - replace the calling process's program with key's, with the given
 * argv and envp (args: argc then envc NUL-terminated strings), from its
 * return to user mode on. Called past exec's point of no return; an error
 * kills the process. Without it, there are no wasm executables.
 *
 * @random - fill buf with len cryptographically random bytes; returns 0 or a
 * negative errno. Seeds the kernel's RNG, as a CPU's random instructions
 * would (see asm/archrandom.h). Optional.
 */
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

	int (*blk_count)(void);
	unsigned long long (*blk_capacity)(int disk);
	int (*blk_rw)(int disk, int write, unsigned long long sector, void *buf,
		      unsigned long len);
	int (*blk_flush)(int disk);

	unsigned long (*copy_from_user)(void *to, const void *from,
					unsigned long n);
	unsigned long (*copy_to_user)(void *to, const void *from,
				      unsigned long n);

	lkl_thread_t (*user_thread_create)(void (*f)(void *), void *arg);
	lkl_thread_t (*vm_thread_create)(void (*f)(void *), void *arg,
					 unsigned long stack);
	int (*signal_handler)(unsigned long handler, int sig, const void *info,
			      void *mask);
	void (*task_exit)(int status);

	int (*exec_load)(const char *key, const void *data, unsigned long off,
			 unsigned long len, unsigned long size);
	int (*exec_lib)(const char *key, const char *loaded, int err, char *path,
			unsigned long size);
	int (*exec)(const char *key, const char *args, unsigned long len,
		    int argc, int envc);

	int (*random)(void *buf, unsigned long len);
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

int lkl_printf(const char *fmt, ...);
void lkl_bug(const char *fmt, ...);

#endif
