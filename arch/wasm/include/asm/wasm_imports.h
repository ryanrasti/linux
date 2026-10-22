#ifndef _WASM_WASM_IMPORTS_H
#define _WASM_WASM_IMPORTS_H

#include <linux/types.h>

#define wasm_import(ns, name)                                   \
	__attribute__((import_module(#ns), import_name(#name))) \
	wasm_##ns##_##name

size_t wasm_import(boot, get_devicetree)(char *buf, size_t size);
int wasm_import(boot, get_initramfs)(char *buf, size_t size);

void wasm_import(kernel, breakpoint)(void);
void wasm_import(kernel, halt_worker)(void);

/* Whole-machine lifecycle event. Values are part of the guest/host ABI. */
enum wasm_machine_termination_reason {
	WASM_MACHINE_TERMINATION_CLEAN = 0,
	WASM_MACHINE_TERMINATION_PANIC = 1,
};

void wasm_import(kernel, terminate_machine)(
	enum wasm_machine_termination_reason reason);

void wasm_import(kernel, boot_console_write)(const char *msg, size_t len);
void wasm_import(kernel, boot_console_close)(void);

void *wasm_import(kernel, return_address)(int level);

/* Unix time in nanoseconds, monotonically advancing during the host session. */
unsigned long long wasm_import(kernel, get_now_nsec)(void);

void wasm_import(kernel, get_stacktrace)(char *buf, size_t size);

enum wasm_user_memory {
	WASM_USER_MEMORY_NONE = 0,
	WASM_USER_MEMORY_SHARE = 1,
	WASM_USER_MEMORY_COPY = 2,
};

/*
 * WASM_USER_MEMORY_COPY is synchronous: success means the destination worker
 * has completed its private snapshot; allocation failure returns -ENOMEM.
 */
int wasm_import(kernel, spawn_worker)(void (*fn)(void *), void *arg,
				      char *name, size_t name_len,
				      enum wasm_user_memory user_memory);

void wasm_import(kernel, run_on_main)(void (*fn)(void *), void *arg);

int wasm_import(user, compile_begin)(u32 len);
int wasm_import(user, compile_write)(u8 *bytes, u32 offset, u32 len);
int wasm_import(user, compile_end)(u32 maximum_memory_pages);
void wasm_import(user, compile_abort)(void);
void wasm_import(user, instantiate)(bool fresh_memory);
void wasm_import(user, call)(void);
void wasm_import(user, switch_entry)(u32 fn, u32 arg);
void wasm_import(user, call_signal_handler)(u32 fn, u32 sig);
int wasm_import(user, call_siginfo_handler)(u32 trampoline, u32 fn, u32 sig);
void wasm_import(user, halt_signal_handler)(void);

int wasm_import(user, read)(void *to, const void __user *from, unsigned long n);
int wasm_import(user, write)(void __user *to, const void *from, unsigned long n);
int wasm_import(user, write_zeroes)(void __user *to, unsigned long n);
int wasm_import(user, futex_atomic_op)(int *oldval, u32 __user *uaddr,
				       int op, int oparg);
int wasm_import(user, futex_atomic_cmpxchg)(u32 *oldval, u32 __user *uaddr,
					    u32 expected, u32 replacement);

#ifdef CONFIG_WASM_JSPI
/* suspends from until a later switch to it; to runs next */
void wasm_import(kernel, jspi_switch)(void *from, void *to);
/* from is dead: to runs next; the kernel then traps, ending this coroutine */
void wasm_import(kernel, jspi_exit)(void *to);
/*
 * exec: the kernel traps next, abandoning the old program's frames (a trap,
 * which no cleanup in them can catch), and the host runs the task again
 * at user_restart, into the new program
 */
void wasm_import(kernel, jspi_restart)(void);
/* suspends until *word is nonzero (an irq) or timeout_ns (-1: none); 2: timed out */
int wasm_import(kernel, jspi_idle)(void *word, long long timeout_ns);
/* lets the event loop run (an irq may arrive); a no-op outside a task */
void wasm_import(kernel, jspi_yield)(void);
/* 0: the host has key's module compiled, and its memory ready; 1: compile it (compile_begin...) */
int wasm_import(user, compile_cached)(const char *key, u32 key_len, u32 maximum_memory_pages);
/* the kernel table's index of the current program's _start */
u32 wasm_import(user, entry)(void);
/* the kernel table's index of the current program's table[fn] */
u32 wasm_import(user, func)(u32 fn);
#endif

#ifdef CONFIG_EXOBOX
/*
 * exec, after the program: the path of the next library to send it, in
 * buf (>0), or 0 (no more), or a negative errno. err: why the last path
 * couldn't be sent (0: it was), for the host to try the next place.
 */
int wasm_import(user, exec_lib)(char *buf, u32 size, int err);
/* fork: the current program's Asyncify (can_fork: whether it has it) */
int wasm_import(user, can_fork)(void);
void wasm_import(user, start_unwind)(void);
void wasm_import(user, stop_unwind)(void);
void wasm_import(user, start_rewind)(void);
void wasm_import(user, stop_rewind)(void);
#endif

#ifdef CONFIG_EXOBOX_HOSTSOCK
/*
 * Sockets the host makes, for the guest's AF_INET(6) sockets whose peer
 * isn't the guest itself (exobox/hostsock.c). Addresses are Linux's
 * sockaddr_in(6), in the kernel's memory. None of these waits: the host
 * works meanwhile, and its sockets' state (sock_state) changes as it goes,
 * each change raising sock_irq's interrupt.
 */
/* the guest has a network: 1 (then sock_irq), or 0: none (loopback only) */
int wasm_import(kernel, sock_init)(void);
/* the interrupt to raise when sockets' state changed (0: none) */
void wasm_import(kernel, sock_irq)(int irq);
/* a socket (AF_INET(6); SOCK_STREAM or SOCK_DGRAM): its handle (> 0), or a negative errno */
int wasm_import(kernel, sock_open)(int family, int type);
/* start connecting a stream: 0 (then CONNECTED or FAILED), or a negative errno */
int wasm_import(kernel, sock_connect)(int sock, const void *addr, int addr_len);
/* offer a listener on addr's port to the host: 0 (then LISTENING, or FAILED: -EPERM if not published) */
int wasm_import(kernel, sock_listen)(int sock, const void *addr, int addr_len, int backlog);
/* a connection the host took on a listener: its handle, or -EAGAIN */
int wasm_import(kernel, sock_accept)(int sock);
/* len bytes of buf (a stream: as many as it takes now; a datagram to addr, whole): how many, -EAGAIN, or an errno */
long wasm_import(kernel, sock_send)(int sock, const void *buf, unsigned long len, const void *addr, int addr_len);
/* into buf, at most len: a stream's next (0: its end), or the next datagram (its source at addr); -EAGAIN if none; peek: leave them */
long wasm_import(kernel, sock_recv)(int sock, void *buf, unsigned long len, int peek, void *addr, int *addr_len);
/* the socket's address, or its peer's, at addr: its length, or a negative errno */
int wasm_import(kernel, sock_getname)(int sock, void *addr, int peer);
int wasm_import(kernel, sock_shutdown)(int sock, int how);
void wasm_import(kernel, sock_close)(int sock);
/* a socket whose state changed since, or 0 */
int wasm_import(kernel, sock_changed)(void);
/* its HOSTSOCK_* bits, and with HOSTSOCK_FAILED its errno above HOSTSOCK_ERRNO_SHIFT */
unsigned int wasm_import(kernel, sock_state)(int sock);
/* the bytes a stream has to read (FIONREAD) */
unsigned long wasm_import(kernel, sock_pending)(int sock);

/* a host socket's state (sock_state) */
#define HOSTSOCK_CONNECTED	1	/* a stream's connection is made */
#define HOSTSOCK_LISTENING	2	/* the host publishes the listener */
#define HOSTSOCK_READABLE	4	/* bytes, or datagrams, to read */
#define HOSTSOCK_WRITABLE	8	/* room for more to send */
#define HOSTSOCK_EOF		16	/* the peer won't send more */
#define HOSTSOCK_FAILED		32	/* connect or listen failed, or reset */
#define HOSTSOCK_ACCEPTABLE	64	/* a listener's connections wait */
#define HOSTSOCK_ERRNO_SHIFT	16
#endif

#ifdef CONFIG_WASM_HOST_DEVICES
void wasm_import(kernel, console_write)(const u8 *buf, size_t len);
/* bytes the host has for the console, up to len; 0: none */
int wasm_import(kernel, console_read)(u8 *buf, size_t len);
/* the irq the host raises when it has console input */
void wasm_import(kernel, console_irq)(int irq);
int wasm_import(kernel, blk_count)(void);
u64 wasm_import(kernel, blk_size)(int disk);
int wasm_import(kernel, blk_writable)(int disk);
/* 0, or a negative errno */
int wasm_import(kernel, blk_read)(int disk, u64 offset, void *buf, u32 len);
int wasm_import(kernel, blk_write)(int disk, u64 offset, const void *buf, u32 len);
#endif

#ifdef CONFIG_VIRTIO_WASM
void wasm_import(virtio, set_features)(u32 id, u64 features);

void wasm_import(virtio, setup)(u32 id, u32 config_irq, u8 *config,
				u32 config_len);
void wasm_import(virtio, reset)(u32 id);

void wasm_import(virtio, enable_vring)(u32 id, u32 index, u32 size,
				       dma_addr_t desc, u32 irq);
void wasm_import(virtio, disable_vring)(u32 id, u32 index);

void wasm_import(virtio, notify)(u32 id, u32 index);
#endif

#undef wasm_import

#endif
