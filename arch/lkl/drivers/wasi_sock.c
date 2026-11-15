// SPDX-License-Identifier: GPL-2.0
/*
 * The guest's AF_INET and AF_INET6 sockets, local or on WASI 0.2's
 * wasi:sockets, by address. A loopback peer is the guest itself: the socket
 * is then an ordinary kernel inet socket inside (its "inner" one). Anything
 * else is wasi:sockets': its connections are the host network's (a
 * runtime's own policy says which: wasmtime's -S inherit-network, Node's),
 * its data in the runtime's buffers. The guest's address is the host's, so
 * a listener on 0.0.0.0:6379 is the host's port 6379. A listener listens in
 * the guest, and is offered to the host: if the runtime lets it bind the
 * port, the host's connections queue on it too.
 *
 * The kernel is linked into the program (a library OS, a component), so it
 * calls wasi:sockets' imports itself. A call never waits (but bind's and
 * listen's finish); what a socket waits for (a connection, bytes, room) is
 * a wasi:io pollable, which the host layer's scheduler waits on with its
 * own (the poll_watch host operation): a ready one raises the interrupt,
 * which sets the socket's state and wakes its waiters.
 *
 * Userspace's AF_INET and AF_INET6 stream and datagram sockets come here
 * (wasi_sock_family(): the arch's syscall table), under retired family
 * numbers (AF_ECONET, AF_WANPIPE), as nothing else registers them. Every
 * socket has its inner inet socket from the start: it keeps the options,
 * and an undecided socket behaves as inet.
 */
#include <linux/inet.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/sockios.h>
#include <asm/ioctls.h>
#include <linux/uio.h>
#include <linux/workqueue.h>
#include <net/ipv6.h>
#include <net/sock.h>
#include <asm/host_ops.h>
#include <asm/wasi_sock.h>

#define AF_WASI_SOCK	AF_ECONET	/* the guest's AF_INET sockets */
#define AF_WASI_SOCK6	AF_WANPIPE	/* ...and AF_INET6 */

/* what's copied through the kernel at a time, each way */
#define CHUNK		(64 * 1024)
/* wasi:sockets' sockets at once (EMFILE beyond) */
#define SOCKETS		64
/* bytes read ahead for MSG_PEEK */
#define PEEK_SIZE	4096

/*
 * wasi:sockets and wasi:io, as the canonical ABI lowers them (the names
 * wit-bindgen gives them; the version the program's world's, host/wit):
 * handles are i32s, results written to a return area
 */
#define WASI_IMPORT(iface, name) \
	__attribute__((import_module(iface "@0.2.12"), import_name(name)))
#define TCP(name) WASI_IMPORT("wasi:sockets/tcp", name)
#define STREAMS(name) WASI_IMPORT("wasi:io/streams", name)

#define ADDR_ARGS s32, s32, s32, s32, s32, s32, s32, s32, s32, s32, s32, s32

WASI_IMPORT("wasi:sockets/instance-network", "instance-network")
s32 __wasm_import_wasi_sockets_instance_network_instance_network(void);
WASI_IMPORT("wasi:sockets/tcp-create-socket", "create-tcp-socket")
void __wasm_import_wasi_sockets_tcp_create_socket_create_tcp_socket(s32, void *);
TCP("[method]tcp-socket.start-bind")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_start_bind(s32, s32, ADDR_ARGS, void *);
TCP("[method]tcp-socket.finish-bind")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_bind(s32, void *);
TCP("[method]tcp-socket.start-connect")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_start_connect(s32, s32, ADDR_ARGS, void *);
TCP("[method]tcp-socket.finish-connect")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_connect(s32, void *);
TCP("[method]tcp-socket.start-listen")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_start_listen(s32, void *);
TCP("[method]tcp-socket.finish-listen")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_listen(s32, void *);
TCP("[method]tcp-socket.accept")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_accept(s32, void *);
TCP("[method]tcp-socket.local-address")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_local_address(s32, void *);
TCP("[method]tcp-socket.remote-address")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_remote_address(s32, void *);
TCP("[method]tcp-socket.set-listen-backlog-size")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_set_listen_backlog_size(s32, s64, void *);
TCP("[method]tcp-socket.shutdown")
void __wasm_import_wasi_sockets_tcp_method_tcp_socket_shutdown(s32, s32, void *);
TCP("[method]tcp-socket.subscribe")
s32 __wasm_import_wasi_sockets_tcp_method_tcp_socket_subscribe(s32);
TCP("[resource-drop]tcp-socket")
void __wasm_import_wasi_sockets_tcp_tcp_socket_drop(s32);
STREAMS("[method]input-stream.read")
void __wasm_import_wasi_io_streams_method_input_stream_read(s32, s64, void *);
STREAMS("[method]input-stream.subscribe")
s32 __wasm_import_wasi_io_streams_method_input_stream_subscribe(s32);
STREAMS("[method]output-stream.check-write")
void __wasm_import_wasi_io_streams_method_output_stream_check_write(s32, void *);
STREAMS("[method]output-stream.write")
void __wasm_import_wasi_io_streams_method_output_stream_write(s32, const void *, u32, void *);
STREAMS("[method]output-stream.flush")
void __wasm_import_wasi_io_streams_method_output_stream_flush(s32, void *);
STREAMS("[method]output-stream.subscribe")
s32 __wasm_import_wasi_io_streams_method_output_stream_subscribe(s32);
STREAMS("[resource-drop]input-stream")
void __wasm_import_wasi_io_streams_input_stream_drop(s32);
STREAMS("[resource-drop]output-stream")
void __wasm_import_wasi_io_streams_output_stream_drop(s32);
WASI_IMPORT("wasi:io/poll", "[method]pollable.block")
void __wasm_import_wasi_io_poll_method_pollable_block(s32);
WASI_IMPORT("wasi:io/poll", "[resource-drop]pollable")
void __wasm_import_wasi_io_poll_pollable_drop(s32);
WASI_IMPORT("wasi:io/error", "[resource-drop]error")
void __wasm_import_wasi_io_error_error_drop(s32);

/* result<_, error-code> */
struct ret_void {
	u8 is_err, err;
};

/* result<handles, error-code> (an error's code the first's low byte) */
struct ret_handles {
	u8 is_err;
	u32 h[3];
};

/* stream-error: last-operation-failed (an error resource), or closed */
struct stream_error {
	u8 closed;
	u32 error;
};

/* result<_, stream-error> */
struct ret_stream {
	u8 is_err;
	struct stream_error err;
};

/* result<list<u8>, stream-error> */
struct ret_list {
	u8 is_err;
	union {
		struct {
			u8 *ptr;
			u32 len;
		} ok;
		struct stream_error err;
	};
};

/* result<u64, stream-error> */
struct ret_u64 {
	u8 is_err;
	union {
		u64 ok;
		struct stream_error err;
	};
};

/*
 * result<ip-socket-address, error-code>: at 4, the variant's tag (or the
 * error's code); at 8, the port; ipv4's address bytes at 10, ipv6's
 * 16-bit words at 16
 */
struct ret_addr {
	u8 r[36];
} __aligned(4);

#define WOULD_BLOCK	8	/* error-code's would-block */

/* wasi:sockets' error code, as an errno */
static int hs_errno(u8 e)
{
	static const short errnos[] = {
		EIO,		/* unknown */
		EACCES,		/* access-denied */
		EOPNOTSUPP,	/* not-supported */
		EINVAL,		/* invalid-argument */
		ENOMEM,		/* out-of-memory */
		ETIMEDOUT,	/* timeout */
		EALREADY,	/* concurrency-conflict */
		EINPROGRESS,	/* not-in-progress */
		EAGAIN,		/* would-block */
		EINVAL,		/* invalid-state */
		EMFILE,		/* new-socket-limit */
		EADDRNOTAVAIL,	/* address-not-bindable */
		EADDRINUSE,	/* address-in-use */
		EHOSTUNREACH,	/* remote-unreachable */
		ECONNREFUSED,	/* connection-refused */
		ECONNRESET,	/* connection-reset */
		ECONNABORTED,	/* connection-aborted */
		EMSGSIZE,	/* datagram-too-large */
		ENOENT,		/* name-unresolvable */
		EAGAIN,		/* temporary-resolver-failure */
		EIO,		/* permanent-resolver-failure */
	};

	return e < ARRAY_SIZE(errnos) ? errnos[e] : EIO;
}

/* a stream's failure, its error resource dropped */
static void hs_stream_failed(const struct stream_error *e)
{
	if (!e->closed)
		__wasm_import_wasi_io_error_error_drop(e->error);
}

/* a socket's state */
#define HS_CONNECTED	1	/* a stream's connection is made */
#define HS_READABLE	2	/* bytes to read */
#define HS_WRITABLE	4	/* room for more to send */
#define HS_EOF		8	/* the peer won't send more */
#define HS_FAILED	16	/* connect failed */
#define HS_ACCEPTABLE	32	/* a listener's connections wait */

/* what a socket waits on: the socket (connect, accept), its streams */
enum { W_SOCK, W_IN, W_OUT, WATCHES };

enum mode { UNDECIDED, LOCAL, REMOTE };

struct hsock {
	struct sock sk;			/* first: the sock is ours */
	struct socket *inner;		/* local traffic, and the options' record */
	int family;			/* AF_INET or AF_INET6 */
	enum mode mode;
	bool listener;			/* a REMOTE listener (and its inner one) */
	bool error_told;		/* a failure's errno is in sk_err, once */
	struct sockaddr_storage peer;	/* a REMOTE datagram socket's connect() */
	int peer_len;
	/* REMOTE, with its wasi:sockets socket (sock: 0 until it has one) */
	int slot;			/* in remote[] */
	u32 sock, in, out;		/* tcp-socket, and its streams once connected */
	u32 pollable[WATCHES];		/* each made once it's first waited on */
	unsigned int state;		/* HS_* */
	int err;			/* HS_FAILED's errno */
	bool connecting;
	u8 *peek;			/* what MSG_PEEK read ahead */
	unsigned long peeked;
};

#define hs_sk(sk) ((struct hsock *)(sk))

static const struct proto_ops wasi_sock_stream_ops, wasi_sock_dgram_ops;

static bool wasi_sock_on;
static int wasi_sock_irq;
static struct work_struct wasi_sock_work;
static s32 network;			/* instance-network's */
static struct hsock *remote[SOCKETS];
/*
 * the pollables the host's scheduler waits on (poll_watch): remote[i]'s at
 * [i], 0 where it doesn't wait; one the host found ready it sets to 0
 */
static u32 watched[SOCKETS][WATCHES];

/* the arch's socket(): AF_INET(6) stream and datagram sockets are ours */
int wasi_sock_family(int family, int type)
{
	type &= SOCK_TYPE_MASK;
	if (!wasi_sock_on || (type != SOCK_STREAM && type != SOCK_DGRAM))
		return family;
	if (family == AF_INET)
		return AF_WASI_SOCK;
	if (family == AF_INET6)
		return AF_WASI_SOCK6;
	return family;
}

/* a failure's errno (connect refused, connection reset), or 0 */
static int hs_error(struct hsock *hs)
{
	return hs->state & HS_FAILED ? -hs->err : 0;
}

/* what hs waits for now: k's pollable is to be waited on */
static bool hs_wants(struct hsock *hs, int k)
{
	switch (k) {
	case W_SOCK:
		return hs->listener ? !(hs->state & HS_ACCEPTABLE) : hs->connecting;
	case W_IN:
		return hs->in && !(hs->state & (HS_READABLE | HS_EOF));
	default:
		return hs->out && !(hs->state & HS_WRITABLE);
	}
}

/* the host's scheduler to wait on what hs waits for (and on nothing else) */
static void hs_watch(struct hsock *hs)
{
	int k;

	for (k = 0; k < WATCHES; k++) {
		if (!hs_wants(hs, k)) {
			watched[hs->slot][k] = 0;
			continue;
		}
		if (!hs->pollable[k])
			hs->pollable[k] = k == W_SOCK ?
				__wasm_import_wasi_sockets_tcp_method_tcp_socket_subscribe(hs->sock) :
				k == W_IN ?
				__wasm_import_wasi_io_streams_method_input_stream_subscribe(hs->in) :
				__wasm_import_wasi_io_streams_method_output_stream_subscribe(hs->out);
		watched[hs->slot][k] = hs->pollable[k];
	}
}

/* bits of hs's state on or off; waiters woken when one comes on */
static void hs_set(struct hsock *hs, unsigned int bits, bool on)
{
	unsigned int was = hs->state;

	hs->state = on ? was | bits : was & ~bits;
	hs_watch(hs);
	if (hs->state & ~was)
		hs->sk.sk_state_change(&hs->sk);
}

/* hs's wasi:sockets socket, made now (a REMOTE socket's first need) */
static int hs_open(struct hsock *hs)
{
	struct ret_handles r;
	int slot;

	if (hs->sock)
		return 0;
	if (hs->sk.sk_type != SOCK_STREAM)	/* (no UDP yet) */
		return -EOPNOTSUPP;
	for (slot = 0; slot < SOCKETS && remote[slot]; slot++)
		;
	if (slot == SOCKETS)
		return -EMFILE;
	__wasm_import_wasi_sockets_tcp_create_socket_create_tcp_socket(
		hs->family == AF_INET6, &r);	/* (ip-address-family: ipv4, ipv6) */
	if (r.is_err)
		return -hs_errno(r.h[0]);
	hs->sock = r.h[0];
	hs->slot = slot;
	remote[slot] = hs;
	return 0;
}

/* done with hs's wasi:sockets socket */
static void hs_close(struct hsock *hs)
{
	int k;

	for (k = 0; k < WATCHES; k++) {
		watched[hs->slot][k] = 0;
		if (hs->pollable[k])
			__wasm_import_wasi_io_poll_pollable_drop(hs->pollable[k]);
		hs->pollable[k] = 0;
	}
	if (hs->in) {
		__wasm_import_wasi_io_streams_input_stream_drop(hs->in);
		__wasm_import_wasi_io_streams_output_stream_drop(hs->out);
		hs->in = hs->out = 0;
	}
	__wasm_import_wasi_sockets_tcp_tcp_socket_drop(hs->sock);
	hs->sock = 0;
	remote[hs->slot] = NULL;
	kfree(hs->peek);
	hs->peek = NULL;
	hs->peeked = 0;
	hs->state = 0;
	hs->connecting = false;
}

/* a Linux sockaddr_in or sockaddr_in6, as wasi:sockets has an address (flat) */
static int hs_flat_addr(const struct sockaddr *addr, int len, s32 a[12])
{
	int i;

	memset(a, 0, 12 * sizeof(*a));
	if (addr->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
		const struct sockaddr_in *in = (const void *)addr;
		const u8 *ip = (const u8 *)&in->sin_addr;

		a[0] = 0;	/* ipv4 */
		a[1] = ntohs(in->sin_port);
		for (i = 0; i < 4; i++)
			a[2 + i] = ip[i];
		return 0;
	}
	if (addr->sa_family == AF_INET6 && len >= (int)sizeof(struct sockaddr_in6)) {
		const struct sockaddr_in6 *in6 = (const void *)addr;

		a[0] = 1;	/* ipv6: port, flow info, address, scope (0s) */
		a[1] = ntohs(in6->sin6_port);
		for (i = 0; i < 8; i++)
			a[3 + i] = ntohs(in6->sin6_addr.s6_addr16[i]);
		return 0;
	}
	return -EAFNOSUPPORT;
}

/* wasi:sockets' address (r's), as a Linux one: its length */
static int hs_sockaddr(const struct ret_addr *ra, struct sockaddr *addr)
{
	const u8 *r = ra->r;
	u16 port = *(const u16 *)(r + 8);
	int i;

	if (r[4] == 0) {	/* ipv4 */
		struct sockaddr_in *in = (void *)addr;

		memset(in, 0, sizeof(*in));
		in->sin_family = AF_INET;
		in->sin_port = htons(port);
		memcpy(&in->sin_addr, r + 10, 4);
		return sizeof(*in);
	}
	{
		struct sockaddr_in6 *in6 = (void *)addr;

		memset(in6, 0, sizeof(*in6));
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(port);
		for (i = 0; i < 8; i++)
			in6->sin6_addr.s6_addr16[i] = htons(*(const u16 *)(r + 16 + 2 * i));
		return sizeof(*in6);
	}
}

/* a start/finish pair's finish, waited for (the whole program) while it would block */
static int hs_finish(struct hsock *hs, void (*finish)(s32, void *))
{
	struct ret_void r;

	for (;;) {
		s32 p;

		finish(hs->sock, &r);
		if (!r.is_err)
			return 0;
		if (r.err != WOULD_BLOCK)
			return -hs_errno(r.err);
		p = __wasm_import_wasi_sockets_tcp_method_tcp_socket_subscribe(hs->sock);
		__wasm_import_wasi_io_poll_method_pollable_block(p);
		__wasm_import_wasi_io_poll_pollable_drop(p);
	}
}

/* start connecting to addr: then connected (or failed) once it's ready */
static int hs_connect(struct hsock *hs, const struct sockaddr *addr, int len)
{
	struct ret_void r;
	s32 a[12];
	int err = hs_flat_addr(addr, len, a);

	if (err)
		return err;
	__wasm_import_wasi_sockets_tcp_method_tcp_socket_start_connect(hs->sock, network,
		a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], &r);
	if (r.is_err)
		return -hs_errno(r.err);
	hs->connecting = true;
	hs_watch(hs);
	return 0;
}

/* a connecting socket's connection, made or failed, once it's ready */
static void hs_connected(struct hsock *hs)
{
	struct ret_handles r;

	__wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_connect(hs->sock, &r);
	if (r.is_err && (u8)r.h[0] == WOULD_BLOCK)
		return;
	hs->connecting = false;
	if (r.is_err) {
		hs->err = hs_errno(r.h[0]);
		hs->state |= HS_FAILED;
		return;
	}
	hs->in = r.h[0];
	hs->out = r.h[1];
	hs->state |= HS_CONNECTED | HS_WRITABLE;
}

/* bind and listen on addr's port, on the host's network */
static int hs_listen(struct hsock *hs, const struct sockaddr *addr, int len,
		     int backlog)
{
	struct ret_void r;
	s32 a[12];
	int err = hs_flat_addr(addr, len, a);

	if (err)
		return err;
	__wasm_import_wasi_sockets_tcp_method_tcp_socket_start_bind(hs->sock, network,
		a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11], &r);
	if (r.is_err)
		return -hs_errno(r.err);
	err = hs_finish(hs, __wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_bind);
	if (err)
		return err;
	__wasm_import_wasi_sockets_tcp_method_tcp_socket_set_listen_backlog_size(hs->sock,
										 backlog, &r);
	__wasm_import_wasi_sockets_tcp_method_tcp_socket_start_listen(hs->sock, &r);
	if (r.is_err)
		return -hs_errno(r.err);
	return hs_finish(hs, __wasm_import_wasi_sockets_tcp_method_tcp_socket_finish_listen);
}

/*
 * on a stream, as much of buf as there's room for now: how many, -EAGAIN
 * (none: wait for HS_WRITABLE), or a negative errno
 */
static long hs_write(struct hsock *hs, const void *buf, unsigned long len)
{
	struct ret_u64 room;
	struct ret_stream r;

	if (!hs->out)
		return -ENOTCONN;
	__wasm_import_wasi_io_streams_method_output_stream_check_write(hs->out, &room);
	if (room.is_err) {
		hs_stream_failed(&room.err);
		return -EPIPE;
	}
	if (!room.ok) {
		hs_set(hs, HS_WRITABLE, false);
		return -EAGAIN;
	}
	len = min_t(u64, len, room.ok);
	__wasm_import_wasi_io_streams_method_output_stream_write(hs->out, buf, len, &r);
	if (!r.is_err)
		__wasm_import_wasi_io_streams_method_output_stream_flush(hs->out, &r);
	if (r.is_err) {
		hs_stream_failed(&r.err);
		return -EPIPE;
	}
	return len;
}

/*
 * from a stream, at most len bytes into buf (peek: left there): how many (0:
 * its end), or -EAGAIN (none yet: wait for HS_READABLE or HS_EOF)
 */
static long hs_read(struct hsock *hs, void *buf, unsigned long len, bool peek)
{
	unsigned long n;

	if (!hs->in)
		return -ENOTCONN;
	/* what peek read ahead first */
	if (!hs->peeked) {
		struct ret_list r;

		if (peek && !hs->peek) {
			hs->peek = kmalloc(PEEK_SIZE, GFP_KERNEL);
			if (!hs->peek)
				return -ENOMEM;
		}
		__wasm_import_wasi_io_streams_method_input_stream_read(hs->in,
			peek ? PEEK_SIZE : len, &r);
		if (r.is_err) {
			hs_stream_failed(&r.err);
			hs_set(hs, HS_READABLE, false);
			hs_set(hs, HS_EOF, true);
			return 0;
		}
		/* (the list is the host's mem_alloc's, as cabi_realloc's) */
		n = r.ok.len;
		if (!n) {
			lkl_ops->mem_free(r.ok.ptr);
			hs_set(hs, HS_READABLE, false);
			return -EAGAIN;
		}
		if (!peek) {
			memcpy(buf, r.ok.ptr, n);
			lkl_ops->mem_free(r.ok.ptr);
			hs_set(hs, HS_READABLE, false);
			return n;
		}
		memcpy(hs->peek, r.ok.ptr, n);
		hs->peeked = n;
		lkl_ops->mem_free(r.ok.ptr);
	}
	n = min(len, hs->peeked);
	memcpy(buf, hs->peek, n);
	if (!peek) {
		memmove(hs->peek, hs->peek + n, hs->peeked - n);
		hs->peeked -= n;
		if (!hs->peeked)
			hs_set(hs, HS_READABLE, false);
	}
	return n;
}

/*
 * A loopback address; or, any: the unspecified one too (a peer there is the
 * guest itself, as Linux has it; a listener there, every address)
 */
static bool hs_loopback(const struct sockaddr *a, int len, bool any)
{
	if (a->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
		__be32 ip = ((const struct sockaddr_in *)a)->sin_addr.s_addr;

		return ipv4_is_loopback(ip) || (any && ipv4_is_zeronet(ip));
	}
	if (a->sa_family == AF_INET6 && len >= SIN6_LEN_RFC2133) {
		const struct in6_addr *ip = &((const struct sockaddr_in6 *)a)->sin6_addr;

		if (ipv6_addr_v4mapped(ip))
			return ipv4_is_loopback(ip->s6_addr32[3]) ||
			       (any && ipv4_is_zeronet(ip->s6_addr32[3]));
		return ipv6_addr_loopback(ip) || (any && ipv6_addr_any(ip));
	}
	return false;
}

/* a peer that's the guest itself */
static bool hs_local_addr(const struct sockaddr *a, int len)
{
	return hs_loopback(a, len, true);
}

/* a published listener's inner one has a connection to accept */
static bool hs_inner_acceptable(struct hsock *hs)
{
	return hs->listener &&
	       (hs->inner->ops->poll(NULL, hs->inner, NULL) & EPOLLIN);
}

/* a published listener's inner one has a connection: wake our waiters */
static void hs_inner_ready(struct sock *inner)
{
	struct hsock *hs;

	sock_def_readable(inner);
	rcu_read_lock();
	/* (its accepted children inherit the callback, not the pointer) */
	hs = __rcu_dereference_sk_user_data_with_flags(inner, SK_USER_DATA_NOCOPY);
	if (hs)
		hs->sk.sk_data_ready(&hs->sk);
	rcu_read_unlock();
}

/*
 * Wait for any of bits in the socket's state (or a failure), as long as
 * *timeo: 0, -EAGAIN (out of time), or -EINTR/-ERESTARTSYS
 */
static int hs_wait(struct hsock *hs, unsigned int bits, long *timeo)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int err = 0;

	add_wait_queue(sk_sleep(&hs->sk), &wait);
	while (!(hs->state & (bits | HS_FAILED)) && !hs_inner_acceptable(hs)) {
		if (!*timeo) {
			err = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			err = sock_intr_errno(*timeo);
			break;
		}
		*timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, *timeo);
	}
	remove_wait_queue(sk_sleep(&hs->sk), &wait);
	return err;
}

/*
 * the interrupt: pollables the host found ready (their watched[] entries
 * 0 while we wait on them); the sockets' state set, their waiters woken
 */
static void wasi_sock_ready(struct work_struct *work)
{
	int i, k;

	for (i = 0; i < SOCKETS; i++) {
		struct hsock *hs = remote[i];
		bool ready[WATCHES];
		unsigned int was;

		if (!hs)
			continue;
		for (k = 0; k < WATCHES; k++)
			ready[k] = hs->pollable[k] && hs_wants(hs, k) && !watched[i][k];
		was = hs->state;
		if (ready[W_SOCK]) {
			if (hs->listener)
				hs->state |= HS_ACCEPTABLE;
			else
				hs_connected(hs);
		}
		if (ready[W_IN])
			hs->state |= HS_READABLE;
		if (ready[W_OUT])
			hs->state |= HS_WRITABLE;
		hs_watch(hs);
		if (hs->state != was)
			hs->sk.sk_state_change(&hs->sk);
	}
}

static irqreturn_t wasi_sock_interrupt(int irq, void *dev_id)
{
	schedule_work(&wasi_sock_work);
	return IRQ_HANDLED;
}

/* (the host's, on a thread of its own: some pollable is ready) */
static void wasi_sock_notify(void)
{
	lkl_trigger_irq(wasi_sock_irq);
}

static struct proto wasi_sock_proto;

/* our sock for sock, with an inner socket (made here, unless given) */
static struct hsock *hs_new(struct net *net, struct socket *sock, int family,
			    int protocol, int kern, struct socket *inner)
{
	struct sock *sk;
	struct hsock *hs;
	int err;

	sk = sk_alloc(net, family == AF_INET ? AF_WASI_SOCK : AF_WASI_SOCK6,
		      GFP_KERNEL, &wasi_sock_proto, kern);
	if (!sk)
		return ERR_PTR(-ENOMEM);
	sock->ops = sock->type == SOCK_STREAM ? &wasi_sock_stream_ops :
						&wasi_sock_dgram_ops;
	sock_init_data(sock, sk);
	/* our setsockopt for SOL_SOCKET too: the inner socket keeps them as well */
	set_bit(SOCK_CUSTOM_SOCKOPT, &sock->flags);
	hs = hs_sk(sk);
	hs->family = family;
	if (!inner) {
		err = __sock_create(net, family, sock->type, protocol, &inner, 1);
		if (err) {
			sk_free(sk);
			sock->sk = NULL;
			return ERR_PTR(err);
		}
	}
	hs->inner = inner;
	/* (SO_PROTOCOL: as inet's; sk_family stays ours, see above) */
	sk->sk_protocol = inner->sk->sk_protocol;
	return hs;
}

static int wasi_sock_create(struct net *net, struct socket *sock, int protocol,
			    int kern, int family)
{
	if (sock->type != SOCK_STREAM && sock->type != SOCK_DGRAM)
		return -ESOCKTNOSUPPORT;
	return PTR_ERR_OR_ZERO(hs_new(net, sock, family, protocol, kern, NULL));
}

static int wasi_sock_create4(struct net *net, struct socket *sock, int protocol,
			     int kern)
{
	return wasi_sock_create(net, sock, protocol, kern, AF_INET);
}

static int wasi_sock_create6(struct net *net, struct socket *sock, int protocol,
			     int kern)
{
	return wasi_sock_create(net, sock, protocol, kern, AF_INET6);
}

static int wasi_sock_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct hsock *hs;

	if (!sk)
		return 0;
	hs = hs_sk(sk);
	if (hs->sock)
		hs_close(hs);
	if (hs->inner) {
		if (hs->listener) {
			write_lock_bh(&hs->inner->sk->sk_callback_lock);
			rcu_assign_sk_user_data(hs->inner->sk, NULL);
			hs->inner->sk->sk_data_ready = sock_def_readable;
			write_unlock_bh(&hs->inner->sk->sk_callback_lock);
		}
		sock_release(hs->inner);
		hs->inner = NULL;
	}
	sock_orphan(sk);
	sock->sk = NULL;
	sock_put(sk);
	return 0;
}

/* (the address is the inner socket's: a REMOTE socket's host picks its own) */
static int wasi_sock_bind(struct socket *sock, struct sockaddr *addr, int len)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode == REMOTE)
		return -EINVAL;
	return hs->inner->ops->bind(hs->inner, addr, len);
}

static int wasi_sock_stream_connect(struct socket *sock, struct sockaddr *addr,
				    int len, int flags)
{
	struct hsock *hs = hs_sk(sock->sk);
	long timeo;
	int err;

	if (hs->mode == LOCAL)
		return hs->inner->ops->connect(hs->inner, addr, len, flags);
	if (hs->mode == UNDECIDED) {
		if (len < (int)sizeof(addr->sa_family))
			return -EINVAL;
		if (hs_local_addr(addr, len)) {
			hs->mode = LOCAL;
			return hs->inner->ops->connect(hs->inner, addr, len, flags);
		}
		if (addr->sa_family != hs->family)
			return -EAFNOSUPPORT;
		hs->mode = REMOTE;
		err = hs_open(hs);
		if (!err)
			err = hs_connect(hs, addr, len);
		if (err)
			return err;
		sock->state = SS_CONNECTING;
	} else if (hs->listener) {
		return -EISCONN;
	}

	/* REMOTE: connecting, or connected, or failed */
	if (hs->state & HS_CONNECTED) {
		if (sock->state == SS_CONNECTED)
			return -EISCONN;
		sock->state = SS_CONNECTED;
		return 0;
	}
	timeo = sock_sndtimeo(sock->sk, flags & O_NONBLOCK);
	err = hs_wait(hs, HS_CONNECTED, &timeo);
	if (err == -EAGAIN)
		return sock->state == SS_CONNECTING && !(flags & O_NONBLOCK) ?
			-ETIMEDOUT : -EINPROGRESS;
	if (err)
		return err;
	err = hs_error(hs);
	if (err) {
		hs->error_told = true;
		sock->state = SS_UNCONNECTED;
		return err;
	}
	sock->state = SS_CONNECTED;
	return 0;
}

static int wasi_sock_dgram_connect(struct socket *sock, struct sockaddr *addr,
				   int len, int flags)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode == UNDECIDED && len >= (int)sizeof(addr->sa_family) &&
	    addr->sa_family != AF_UNSPEC)
		hs->mode = hs_local_addr(addr, len) ? LOCAL : REMOTE;
	if (hs->mode != REMOTE)
		return hs->inner->ops->connect(hs->inner, addr, len, flags);
	if (addr->sa_family == AF_UNSPEC) {
		hs->peer_len = 0;
		sock->state = SS_UNCONNECTED;
		return 0;
	}
	if (len > (int)sizeof(hs->peer))
		return -EINVAL;
	memcpy(&hs->peer, addr, len);
	hs->peer_len = len;
	sock->state = SS_CONNECTED;
	return hs_open(hs);
}

/*
 * A listener in the guest, and offered to the host: published, the host's
 * connections queue here too (REMOTE, a listener); if it won't (not a port
 * it publishes), only the guest's.
 */
static int wasi_sock_listen(struct socket *sock, int backlog)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct sockaddr_storage addr;
	struct sock *inner;
	int len, err;

	if (hs->mode == REMOTE && !hs->listener)
		return -EINVAL;
	err = hs->inner->ops->listen(hs->inner, backlog);
	if (err || hs->mode != UNDECIDED)
		return err;
	hs->mode = LOCAL;
	len = hs->inner->ops->getname(hs->inner, (struct sockaddr *)&addr, 0);
	if (len <= 0 || hs_loopback((struct sockaddr *)&addr, len, false) ||
	    hs_open(hs))
		return 0;
	err = hs_listen(hs, (struct sockaddr *)&addr, len, backlog);
	if (err) {
		hs_close(hs);
		return err == -EACCES || err == -EADDRINUSE ||
		       err == -EADDRNOTAVAIL ? 0 : err;
	}
	hs->mode = REMOTE;
	hs->listener = true;
	hs_watch(hs);
	inner = hs->inner->sk;
	/* (hs_inner_ready() may see hs until a grace period ends) */
	sock_set_flag(&hs->sk, SOCK_RCU_FREE);
	write_lock_bh(&inner->sk_callback_lock);
	__rcu_assign_sk_user_data_with_flags(inner, hs, SK_USER_DATA_NOCOPY);
	inner->sk_data_ready = hs_inner_ready;
	write_unlock_bh(&inner->sk_callback_lock);
	return 0;
}

/* a connection from the guest: the inner listener's */
static int hs_accept_local(struct hsock *hs, struct socket *newsock,
			   struct proto_accept_arg *arg)
{
	struct hsock *child;
	struct socket *inner;
	int err;

	inner = sock_alloc();
	if (!inner)
		return -ENOMEM;
	inner->type = hs->inner->type;
	inner->ops = hs->inner->ops;
	err = hs->inner->ops->accept(hs->inner, inner, arg);
	if (err) {
		sock_release(inner);
		return err;
	}
	child = hs_new(sock_net(&hs->sk), newsock, hs->family, 0, arg->kern, inner);
	if (IS_ERR(child)) {
		sock_release(inner);
		return PTR_ERR(child);
	}
	child->mode = LOCAL;
	newsock->state = SS_CONNECTED;
	return 0;
}

/* a connection from the host's network, or -EAGAIN (none yet) */
static int hs_accept_remote(struct hsock *hs, struct socket *newsock,
			    struct proto_accept_arg *arg)
{
	struct hsock *child;
	struct ret_handles r;
	int slot;

	__wasm_import_wasi_sockets_tcp_method_tcp_socket_accept(hs->sock, &r);
	if (r.is_err) {
		if ((u8)r.h[0] == WOULD_BLOCK)
			hs_set(hs, HS_ACCEPTABLE, false);
		return -hs_errno(r.h[0]);
	}
	for (slot = 0; slot < SOCKETS && remote[slot]; slot++)
		;
	child = slot < SOCKETS ?
		hs_new(sock_net(&hs->sk), newsock, hs->family, 0, arg->kern, NULL) :
		ERR_PTR(-EMFILE);
	if (IS_ERR(child)) {
		__wasm_import_wasi_io_streams_input_stream_drop(r.h[1]);
		__wasm_import_wasi_io_streams_output_stream_drop(r.h[2]);
		__wasm_import_wasi_sockets_tcp_tcp_socket_drop(r.h[0]);
		return PTR_ERR(child);
	}
	child->mode = REMOTE;
	child->sock = r.h[0];
	child->in = r.h[1];
	child->out = r.h[2];
	child->slot = slot;
	remote[slot] = child;
	hs_set(child, HS_CONNECTED | HS_WRITABLE, true);
	newsock->state = SS_CONNECTED;
	return 0;
}

/* a published listener's: the guest's connections, and the host's */
static int wasi_sock_accept(struct socket *sock, struct socket *newsock,
			    struct proto_accept_arg *arg)
{
	struct hsock *hs = hs_sk(sock->sk);
	int flags = arg->flags;
	long timeo;
	int err;

	if (hs->mode == LOCAL)
		return hs_accept_local(hs, newsock, arg);
	if (!hs->listener)
		return -EINVAL;
	timeo = sock_rcvtimeo(sock->sk, flags & O_NONBLOCK);
	for (;;) {
		arg->flags = flags | O_NONBLOCK;
		err = hs_accept_local(hs, newsock, arg);
		arg->flags = flags;
		if (err != -EAGAIN)
			return err;
		err = hs_accept_remote(hs, newsock, arg);
		if (err != -EAGAIN)
			return err;
		err = hs_wait(hs, HS_ACCEPTABLE, &timeo);
		if (err)
			return err;
	}
}

static int wasi_sock_getname(struct socket *sock, struct sockaddr *addr, int peer)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct ret_addr r;

	if (hs->mode != REMOTE)
		return hs->inner->ops->getname(hs->inner, addr, peer);
	if (peer && sock->type == SOCK_DGRAM) {
		if (!hs->peer_len)
			return -ENOTCONN;
		memcpy(addr, &hs->peer, hs->peer_len);
		return hs->peer_len;
	}
	if (peer)
		__wasm_import_wasi_sockets_tcp_method_tcp_socket_remote_address(hs->sock, &r);
	else
		__wasm_import_wasi_sockets_tcp_method_tcp_socket_local_address(hs->sock, &r);
	return r.r[0] ? -hs_errno(r.r[4]) : hs_sockaddr(&r, addr);
}

static __poll_t wasi_sock_poll(struct file *file, struct socket *sock,
			       poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct hsock *hs = hs_sk(sk);
	unsigned int state;
	__poll_t mask = 0;

	if (hs->mode != REMOTE) {
		/*
		 * its error ours: SO_ERROR reads ours (generic SOL_SOCKET), as
		 * after a non-blocking connect
		 */
		int err;

		mask = hs->inner->ops->poll(file, hs->inner, wait);
		err = xchg(&hs->inner->sk->sk_err, 0);
		if (err)
			WRITE_ONCE(sk->sk_err, err);
		if (READ_ONCE(sk->sk_err))
			mask |= EPOLLERR;
		return mask;
	}
	sock_poll_wait(file, sock, wait);
	if (!hs->sock)	/* (a datagram socket that hasn't sent) */
		return EPOLLOUT | EPOLLWRNORM;
	state = hs->state;
	if (hs->listener)
		return state & HS_ACCEPTABLE || hs_inner_acceptable(hs) ?
			EPOLLIN | EPOLLRDNORM : 0;
	if (state & HS_READABLE)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (state & HS_EOF)
		mask |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
	if (sock->type == SOCK_DGRAM ||
	    (state & (HS_CONNECTED | HS_WRITABLE)) == (HS_CONNECTED | HS_WRITABLE))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (state & HS_FAILED) {
		/* (an asynchronous connect's error: SO_ERROR's, once) */
		if (!hs->error_told) {
			hs->error_told = true;
			WRITE_ONCE(sk->sk_err, -hs_error(hs));
		}
		mask |= EPOLLERR | EPOLLHUP | EPOLLOUT;
	}
	if ((state & HS_EOF) && (sk->sk_shutdown & SEND_SHUTDOWN))
		mask |= EPOLLHUP;
	return mask;
}

static int wasi_sock_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode != REMOTE)
		return hs->inner->ops->ioctl(hs->inner, cmd, arg);
	if (cmd == SIOCINQ)
		return put_user((int)hs->peeked, (int __user *)arg);
	return -ENOIOCTLCMD;
}

static int wasi_sock_shutdown(struct socket *sock, int how)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct ret_void r;

	if (hs->mode != REMOTE)
		return hs->inner->ops->shutdown(hs->inner, how);
	if (how < SHUT_RD || how > SHUT_RDWR)
		return -EINVAL;
	if (sock->state != SS_CONNECTED)
		return -ENOTCONN;
	/* (SHUT_RD 0, SHUT_WR 1, SHUT_RDWR 2: as shutdown-type numbers them) */
	__wasm_import_wasi_sockets_tcp_method_tcp_socket_shutdown(hs->sock, how, &r);
	if (r.is_err)
		return -hs_errno(r.err);
	sock->sk->sk_shutdown |= how + 1;	/* (RCV/SEND_SHUTDOWN) */
	return 0;
}

/* SOL_SOCKET's on ours (generic code reads them there) and the inner's too */
static int wasi_sock_setsockopt(struct socket *sock, int level, int optname,
				sockptr_t optval, unsigned int optlen)
{
	struct hsock *hs = hs_sk(sock->sk);
	int err;

	if (level != SOL_SOCKET)
		return hs->inner->ops->setsockopt(hs->inner, level, optname,
						  optval, optlen);
	err = sock_setsockopt(sock, level, optname, optval, optlen);
	if (!err)
		sock_setsockopt(hs->inner, level, optname, optval, optlen);
	return err;
}

static int wasi_sock_getsockopt(struct socket *sock, int level, int optname,
				char __user *optval, int __user *optlen)
{
	struct hsock *hs = hs_sk(sock->sk);

	return hs->inner->ops->getsockopt(hs->inner, level, optname, optval, optlen);
}

static int hs_send_error(struct sock *sk, int err, int flags)
{
	if (err == -EPIPE && !(flags & MSG_NOSIGNAL))
		send_sig(SIGPIPE, current, 0);
	return err;
}

/* send msg on a connected stream */
static int hs_send(struct hsock *hs, struct msghdr *msg, size_t len)
{
	struct sock *sk = &hs->sk;
	long timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);
	size_t sent = 0;
	void *buf;
	int err = 0;

	if (sk->sk_shutdown & SEND_SHUTDOWN)
		return hs_send_error(sk, -EPIPE, msg->msg_flags);
	buf = kmalloc(min_t(size_t, len, CHUNK) ?: 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	do {
		size_t n = min_t(size_t, len - sent, CHUNK);
		long ret;

		if (copy_from_iter(buf, n, &msg->msg_iter) != n) {
			err = -EFAULT;
			break;
		}
		ret = hs_write(hs, buf, n);
		if (ret == -EAGAIN) {
			iov_iter_revert(&msg->msg_iter, n);
			err = hs_wait(hs, HS_WRITABLE, &timeo);
			if (!err)
				err = hs_error(hs);
			if (err)
				break;
			continue;
		}
		if (ret < 0) {
			iov_iter_revert(&msg->msg_iter, n);
			err = ret;
			break;
		}
		if (ret < n)
			iov_iter_revert(&msg->msg_iter, n - ret);
		sent += ret;
	} while (sent < len);
	kfree(buf);
	if (sent)
		return sent;
	return hs_send_error(sk, err, msg->msg_flags);
}

/* from a stream to msg: all the bytes there are, up to len (MSG_WAITALL: len) */
static int hs_recv(struct hsock *hs, struct msghdr *msg, size_t len, int flags)
{
	struct sock *sk = &hs->sk;
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	size_t copied = 0;
	void *buf;
	int err = 0;

	if (!(hs->state & (HS_CONNECTED | HS_EOF | HS_FAILED)))
		return -ENOTCONN;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		return 0;
	buf = kmalloc(CHUNK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		size_t n = min_t(size_t, len - copied, CHUNK);
		long ret = hs_read(hs, buf, n, flags & MSG_PEEK);

		if (ret == -EAGAIN) {
			if (copied && !(flags & MSG_WAITALL))
				break;
			err = hs_wait(hs, HS_READABLE | HS_EOF, &timeo);
			if (err)
				break;
			continue;
		}
		if (ret < 0) {
			err = ret;
			break;
		}
		if (!ret)	/* end of file */
			break;
		if (copy_to_iter(buf, ret, &msg->msg_iter) != ret) {
			err = -EFAULT;
			break;
		}
		copied += ret;
		if (copied == len || (flags & MSG_PEEK) || !(flags & MSG_WAITALL))
			break;
	}
	kfree(buf);
	if (copied)
		return copied;
	if (err == -EAGAIN && hs_error(hs))
		err = hs_error(hs);
	return err;
}

static int wasi_sock_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct sockaddr *to = msg->msg_name;

	if (sock->type == SOCK_DGRAM && hs->mode == UNDECIDED && to)
		hs->mode = hs_local_addr(to, msg->msg_namelen) ? LOCAL : REMOTE;
	if (hs->mode != REMOTE)
		return hs->inner->ops->sendmsg(hs->inner, msg, len);
	if (sock->type == SOCK_STREAM) {
		if (sock->state != SS_CONNECTED && !(hs->state & HS_CONNECTED))
			return hs_send_error(sock->sk, -EPIPE, msg->msg_flags);
		return hs_send(hs, msg, len);
	}
	if (!to && !hs->peer_len)
		return -EDESTADDRREQ;
	return hs_open(hs) ?: -EOPNOTSUPP;	/* (a datagram: no UDP yet) */
}

static int wasi_sock_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			     int flags)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode != REMOTE || !hs->sock)
		return hs->inner->ops->recvmsg(hs->inner, msg, len, flags);
	return hs_recv(hs, msg, len, flags);
}

static const struct proto_ops wasi_sock_stream_ops = {
	.family		= AF_WASI_SOCK,
	.owner		= THIS_MODULE,
	.release	= wasi_sock_release,
	.bind		= wasi_sock_bind,
	.connect	= wasi_sock_stream_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= wasi_sock_accept,
	.getname	= wasi_sock_getname,
	.poll		= wasi_sock_poll,
	.ioctl		= wasi_sock_ioctl,
	.listen		= wasi_sock_listen,
	.shutdown	= wasi_sock_shutdown,
	.setsockopt	= wasi_sock_setsockopt,
	.getsockopt	= wasi_sock_getsockopt,
	.sendmsg	= wasi_sock_sendmsg,
	.recvmsg	= wasi_sock_recvmsg,
	.mmap		= sock_no_mmap,
};

static const struct proto_ops wasi_sock_dgram_ops = {
	.family		= AF_WASI_SOCK,
	.owner		= THIS_MODULE,
	.release	= wasi_sock_release,
	.bind		= wasi_sock_bind,
	.connect	= wasi_sock_dgram_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= wasi_sock_getname,
	.poll		= wasi_sock_poll,
	.ioctl		= wasi_sock_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= wasi_sock_shutdown,
	.setsockopt	= wasi_sock_setsockopt,
	.getsockopt	= wasi_sock_getsockopt,
	.sendmsg	= wasi_sock_sendmsg,
	.recvmsg	= wasi_sock_recvmsg,
	.mmap		= sock_no_mmap,
};

static struct proto wasi_sock_proto = {
	.name		= "WASI_SOCK",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct hsock),
};

static const struct net_proto_family wasi_sock_family4 = {
	.family	= AF_WASI_SOCK,
	.create	= wasi_sock_create4,
	.owner	= THIS_MODULE,
};

static const struct net_proto_family wasi_sock_family6 = {
	.family	= AF_WASI_SOCK6,
	.create	= wasi_sock_create6,
	.owner	= THIS_MODULE,
};

static int __init wasi_sock_init(void)
{
	int err;

	/* (no wait for pollables in the host: AF_INET stays the kernel's, loopback only) */
	if (!lkl_ops->poll_watch)
		return 0;
	network = __wasm_import_wasi_sockets_instance_network_instance_network();
	INIT_WORK(&wasi_sock_work, wasi_sock_ready);
	wasi_sock_irq = lkl_get_free_irq("wasi_sock");
	if (wasi_sock_irq < 0)
		return wasi_sock_irq;
	err = request_irq(wasi_sock_irq, wasi_sock_interrupt, 0, "wasi_sock", NULL);
	if (err)
		goto out_irq;
	err = proto_register(&wasi_sock_proto, 0);
	if (err)
		goto out_request;
	err = sock_register(&wasi_sock_family4);
	if (err)
		goto out_proto;
	err = sock_register(&wasi_sock_family6);
	if (err)
		goto out_family4;
	lkl_ops->poll_watch(&watched[0][0], SOCKETS * WATCHES, wasi_sock_notify);
	wasi_sock_on = true;
	return 0;

out_family4:
	sock_unregister(AF_WASI_SOCK);
out_proto:
	proto_unregister(&wasi_sock_proto);
out_request:
	free_irq(wasi_sock_irq, NULL);
out_irq:
	lkl_put_irq(wasi_sock_irq, "wasi_sock");
	return err;
}
/* (after inet: the inner sockets are its) */
late_initcall(wasi_sock_init);
