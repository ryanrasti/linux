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
#include <asm/wasi.h>
#include <asm/wasi_sock.h>

#define AF_WASI_SOCK	AF_ECONET	/* the guest's AF_INET sockets */
#define AF_WASI_SOCK6	AF_WANPIPE	/* ...and AF_INET6 */

/* what's copied through the kernel at a time, each way */
#define CHUNK		(64 * 1024)
/* wasi:sockets' sockets at once (EMFILE beyond) */
#define SOCKETS		64
/* bytes read ahead for MSG_PEEK */
#define PEEK_SIZE	4096

/* wasi:sockets' tcp-socket's methods (asm/wasi.h), on a socket */
#define TCP(name, sock, ...) \
	wasi_sockets_tcp_method_tcp_socket_##name(wasi_sockets_tcp_borrow_tcp_socket(sock), \
						  ##__VA_ARGS__)

#define WOULD_BLOCK	WASI_SOCKETS_NETWORK_ERROR_CODE_WOULD_BLOCK

/* wasi:sockets' error code, as an errno */
static int hs_errno(wasi_sockets_network_error_code_t e)
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
	wasi_sockets_tcp_own_tcp_socket_t sock;
	/* its streams once connected */
	wasi_io_streams_own_input_stream_t in;
	wasi_io_streams_own_output_stream_t out;
	wasi_io_poll_own_pollable_t pollable[WATCHES];	/* each made once it's first waited on */
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
static wasi_sockets_network_own_network_t network;	/* instance-network's (0: not yet) */

/*
 * the host's network, got at its first use, not at boot: a boot done ahead
 * of time (a blob's snapshot) holds no handle of the instance's
 */
static wasi_sockets_network_borrow_network_t network_get(void)
{
	if (!network.__handle)
		network = wasi_sockets_instance_network_instance_network();
	return wasi_sockets_network_borrow_network(network);
}

/* (lkl_wasi_release's: no socket is open, the host's snapshot) */
void wasi_sock_network_drop(void)
{
	if (network.__handle)
		wasi_sockets_network_network_drop_own(network);
	network.__handle = 0;
}
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
		return hs->in.__handle && !(hs->state & (HS_READABLE | HS_EOF));
	default:
		return hs->out.__handle && !(hs->state & HS_WRITABLE);
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
		if (!hs->pollable[k].__handle)
			hs->pollable[k] = k == W_SOCK ? TCP(subscribe, hs->sock) :
				k == W_IN ?
				wasi_io_streams_method_input_stream_subscribe(
					wasi_io_streams_borrow_input_stream(hs->in)) :
				wasi_io_streams_method_output_stream_subscribe(
					wasi_io_streams_borrow_output_stream(hs->out));
		watched[hs->slot][k] = hs->pollable[k].__handle;
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
	wasi_sockets_network_error_code_t e;
	int slot;

	if (hs->sock.__handle)
		return 0;
	if (hs->sk.sk_type != SOCK_STREAM)	/* (no UDP yet) */
		return -EOPNOTSUPP;
	for (slot = 0; slot < SOCKETS && remote[slot]; slot++)
		;
	if (slot == SOCKETS)
		return -EMFILE;
	if (!wasi_sockets_tcp_create_socket_create_tcp_socket(
		hs->family == AF_INET6 ? WASI_SOCKETS_NETWORK_IP_ADDRESS_FAMILY_IPV6 :
					 WASI_SOCKETS_NETWORK_IP_ADDRESS_FAMILY_IPV4,
		&hs->sock, &e))
		return -hs_errno(e);
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
		if (hs->pollable[k].__handle)
			wasi_io_poll_pollable_drop_own(hs->pollable[k]);
		hs->pollable[k].__handle = 0;
	}
	if (hs->in.__handle) {
		wasi_io_streams_input_stream_drop_own(hs->in);
		wasi_io_streams_output_stream_drop_own(hs->out);
		hs->in.__handle = hs->out.__handle = 0;
	}
	wasi_sockets_tcp_tcp_socket_drop_own(hs->sock);
	hs->sock.__handle = 0;
	remote[hs->slot] = NULL;
	kfree(hs->peek);
	hs->peek = NULL;
	hs->peeked = 0;
	hs->state = 0;
	hs->connecting = false;
}

/* ipv6-address: a tuple of its eight 16-bit words, in order */
static_assert(sizeof(wasi_sockets_network_ipv6_address_t) == 8 * sizeof(u16));

/* a Linux sockaddr_in or sockaddr_in6, as wasi:sockets has an address */
static int hs_wasi_addr(const struct sockaddr *addr, int len,
			wasi_sockets_network_ip_socket_address_t *a)
{
	int i;

	memset(a, 0, sizeof(*a));
	if (addr->sa_family == AF_INET && len >= (int)sizeof(struct sockaddr_in)) {
		const struct sockaddr_in *in = (const void *)addr;
		const u8 *ip = (const u8 *)&in->sin_addr;

		a->tag = WASI_SOCKETS_NETWORK_IP_SOCKET_ADDRESS_IPV4;
		a->val.ipv4.port = ntohs(in->sin_port);
		a->val.ipv4.address = (wasi_sockets_network_ipv4_address_t){
			ip[0], ip[1], ip[2], ip[3] };
		return 0;
	}
	if (addr->sa_family == AF_INET6 && len >= (int)sizeof(struct sockaddr_in6)) {
		const struct sockaddr_in6 *in6 = (const void *)addr;
		u16 ip[8];

		/* (flow info and scope 0s) */
		a->tag = WASI_SOCKETS_NETWORK_IP_SOCKET_ADDRESS_IPV6;
		a->val.ipv6.port = ntohs(in6->sin6_port);
		for (i = 0; i < 8; i++)
			ip[i] = ntohs(in6->sin6_addr.s6_addr16[i]);
		memcpy(&a->val.ipv6.address, ip, sizeof(ip));
		return 0;
	}
	return -EAFNOSUPPORT;
}

/* wasi:sockets' address, as a Linux one: its length */
static int hs_sockaddr(const wasi_sockets_network_ip_socket_address_t *a,
		       struct sockaddr *addr)
{
	int i;

	if (a->tag == WASI_SOCKETS_NETWORK_IP_SOCKET_ADDRESS_IPV4) {
		const wasi_sockets_network_ipv4_address_t *ip = &a->val.ipv4.address;
		struct sockaddr_in *in = (void *)addr;
		u8 *to = (u8 *)&in->sin_addr;

		memset(in, 0, sizeof(*in));
		in->sin_family = AF_INET;
		in->sin_port = htons(a->val.ipv4.port);
		to[0] = ip->f0;
		to[1] = ip->f1;
		to[2] = ip->f2;
		to[3] = ip->f3;
		return sizeof(*in);
	}
	{
		struct sockaddr_in6 *in6 = (void *)addr;
		u16 ip[8];

		memcpy(ip, &a->val.ipv6.address, sizeof(ip));
		memset(in6, 0, sizeof(*in6));
		in6->sin6_family = AF_INET6;
		in6->sin6_port = htons(a->val.ipv6.port);
		for (i = 0; i < 8; i++)
			in6->sin6_addr.s6_addr16[i] = htons(ip[i]);
		return sizeof(*in6);
	}
}

/* a start/finish pair's finish, waited for (the whole program) while it would block */
static int hs_finish(struct hsock *hs,
		     bool (*finish)(wasi_sockets_tcp_borrow_tcp_socket_t,
				    wasi_sockets_network_error_code_t *))
{
	wasi_sockets_network_error_code_t e;

	for (;;) {
		wasi_io_poll_own_pollable_t p;

		if (finish(wasi_sockets_tcp_borrow_tcp_socket(hs->sock), &e))
			return 0;
		if (e != WOULD_BLOCK)
			return -hs_errno(e);
		p = TCP(subscribe, hs->sock);
		wasi_io_poll_method_pollable_block(wasi_io_poll_borrow_pollable(p));
		wasi_io_poll_pollable_drop_own(p);
	}
}

/* start connecting to addr: then connected (or failed) once it's ready */
static int hs_connect(struct hsock *hs, const struct sockaddr *addr, int len)
{
	wasi_sockets_network_ip_socket_address_t a;
	wasi_sockets_network_error_code_t e;
	int err = hs_wasi_addr(addr, len, &a);

	if (err)
		return err;
	if (!TCP(start_connect, hs->sock, network_get(), &a, &e))
		return -hs_errno(e);
	hs->connecting = true;
	hs_watch(hs);
	return 0;
}

/* a connecting socket's connection, made or failed, once it's ready */
static void hs_connected(struct hsock *hs)
{
	wasi_sockets_tcp_tuple2_own_input_stream_own_output_stream_t r;
	wasi_sockets_network_error_code_t e;

	if (!TCP(finish_connect, hs->sock, &r, &e)) {
		if (e == WOULD_BLOCK)
			return;
		hs->connecting = false;
		hs->err = hs_errno(e);
		hs->state |= HS_FAILED;
		return;
	}
	hs->connecting = false;
	hs->in = r.f0;
	hs->out = r.f1;
	hs->state |= HS_CONNECTED | HS_WRITABLE;
}

/* bind and listen on addr's port, on the host's network */
static int hs_listen(struct hsock *hs, const struct sockaddr *addr, int len,
		     int backlog)
{
	wasi_sockets_network_ip_socket_address_t a;
	wasi_sockets_network_error_code_t e;
	int err = hs_wasi_addr(addr, len, &a);

	if (err)
		return err;
	if (!TCP(start_bind, hs->sock, network_get(), &a, &e))
		return -hs_errno(e);
	err = hs_finish(hs, wasi_sockets_tcp_method_tcp_socket_finish_bind);
	if (err)
		return err;
	TCP(set_listen_backlog_size, hs->sock, backlog, &e);
	if (!TCP(start_listen, hs->sock, &e))
		return -hs_errno(e);
	return hs_finish(hs, wasi_sockets_tcp_method_tcp_socket_finish_listen);
}

/*
 * on a stream, as much of buf as there's room for now: how many, -EAGAIN
 * (none: wait for HS_WRITABLE), or a negative errno
 */
static long hs_write(struct hsock *hs, const void *buf, unsigned long len)
{
	wasi_io_streams_borrow_output_stream_t out;
	wasi_io_streams_stream_error_t err;
	wasi_list_u8_t list;
	u64 room;

	if (!hs->out.__handle)
		return -ENOTCONN;
	out = wasi_io_streams_borrow_output_stream(hs->out);
	if (!wasi_io_streams_method_output_stream_check_write(out, &room, &err)) {
		wasi_stream_failed(&err);
		return -EPIPE;
	}
	if (!room) {
		hs_set(hs, HS_WRITABLE, false);
		return -EAGAIN;
	}
	len = min_t(u64, len, room);
	list = (wasi_list_u8_t){ (u8 *)buf, len };
	if (!wasi_io_streams_method_output_stream_write(out, &list, &err) ||
	    !wasi_io_streams_method_output_stream_flush(out, &err)) {
		wasi_stream_failed(&err);
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

	if (!hs->in.__handle)
		return -ENOTCONN;
	/* what peek read ahead first */
	if (!hs->peeked) {
		wasi_io_streams_stream_error_t err;
		wasi_list_u8_t r;

		if (peek && !hs->peek) {
			hs->peek = kmalloc(PEEK_SIZE, GFP_KERNEL);
			if (!hs->peek)
				return -ENOMEM;
		}
		if (!wasi_io_streams_method_input_stream_read(
			wasi_io_streams_borrow_input_stream(hs->in), peek ? PEEK_SIZE : len,
			&r, &err)) {
			wasi_stream_failed(&err);
			hs_set(hs, HS_READABLE, false);
			hs_set(hs, HS_EOF, true);
			return 0;
		}
		/* (no more than asked for, whatever the host gave) */
		n = min_t(size_t, r.len, peek ? PEEK_SIZE : len);
		if (!n) {
			wasi_list_u8_free(&r);
			hs_set(hs, HS_READABLE, false);
			return -EAGAIN;
		}
		if (!peek) {
			memcpy(buf, r.ptr, n);
			wasi_list_u8_free(&r);
			hs_set(hs, HS_READABLE, false);
			return n;
		}
		memcpy(hs->peek, r.ptr, n);
		hs->peeked = n;
		wasi_list_u8_free(&r);
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
			ready[k] = hs->pollable[k].__handle && hs_wants(hs, k) && !watched[i][k];
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
	if (hs->sock.__handle)
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
	wasi_sockets_tcp_tuple3_own_tcp_socket_own_input_stream_own_output_stream_t r;
	wasi_sockets_network_error_code_t e;
	struct hsock *child;
	int slot;

	if (!TCP(accept, hs->sock, &r, &e)) {
		if (e == WOULD_BLOCK)
			hs_set(hs, HS_ACCEPTABLE, false);
		return -hs_errno(e);
	}
	for (slot = 0; slot < SOCKETS && remote[slot]; slot++)
		;
	child = slot < SOCKETS ?
		hs_new(sock_net(&hs->sk), newsock, hs->family, 0, arg->kern, NULL) :
		ERR_PTR(-EMFILE);
	if (IS_ERR(child)) {
		wasi_io_streams_input_stream_drop_own(r.f1);
		wasi_io_streams_output_stream_drop_own(r.f2);
		wasi_sockets_tcp_tcp_socket_drop_own(r.f0);
		return PTR_ERR(child);
	}
	child->mode = REMOTE;
	child->sock = r.f0;
	child->in = r.f1;
	child->out = r.f2;
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
	wasi_sockets_network_ip_socket_address_t a;
	wasi_sockets_network_error_code_t e;
	bool ok;

	if (hs->mode != REMOTE)
		return hs->inner->ops->getname(hs->inner, addr, peer);
	if (peer && sock->type == SOCK_DGRAM) {
		if (!hs->peer_len)
			return -ENOTCONN;
		memcpy(addr, &hs->peer, hs->peer_len);
		return hs->peer_len;
	}
	ok = peer ? TCP(remote_address, hs->sock, &a, &e) :
		    TCP(local_address, hs->sock, &a, &e);
	return ok ? hs_sockaddr(&a, addr) : -hs_errno(e);
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
	if (!hs->sock.__handle)	/* (a datagram socket that hasn't sent) */
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
	wasi_sockets_network_error_code_t e;

	if (hs->mode != REMOTE)
		return hs->inner->ops->shutdown(hs->inner, how);
	if (how < SHUT_RD || how > SHUT_RDWR)
		return -EINVAL;
	if (sock->state != SS_CONNECTED)
		return -ENOTCONN;
	/* (SHUT_RD 0, SHUT_WR 1, SHUT_RDWR 2: as shutdown-type numbers them) */
	if (!TCP(shutdown, hs->sock, how, &e))
		return -hs_errno(e);
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

	if (hs->mode != REMOTE || !hs->sock.__handle)
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
