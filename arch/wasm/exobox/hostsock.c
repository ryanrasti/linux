// SPDX-License-Identifier: GPL-2.0
/*
 * hostsock: the guest's AF_INET and AF_INET6 sockets, local or the host's
 * (the kernel.sock_* imports), by address. A loopback peer is the guest itself: the
 * socket is then an ordinary kernel inet socket inside (its "inner" one).
 * Anything else is the host's: its connections and datagrams are the host
 * network's (a host's own policy says which: Node's, the public network
 * only), its data in the host's buffers, its state the host's, which the
 * kernel asks for when it needs it; the host raises an interrupt when a
 * socket's state changes, which wakes its waiters. A listener listens in
 * the guest, and is offered to the host: if it publishes the port, the
 * host's connections queue on it too.
 *
 * Userspace's AF_INET and AF_INET6 stream and datagram sockets come here
 * (hostsock_family(), from exobox_syscall_enter), under retired family
 * numbers (AF_ECONET, AF_WANPIPE), as nothing else registers them. Every
 * socket has its inner inet socket from the start: it keeps the options,
 * and an undecided socket behaves as inet.
 *
 * AF_VSOCK stream sockets are the host's alone, with no inner socket: the
 * channel between the guest and the host itself (not its network, nor its
 * policy), as a VM's vsock is. Their addresses are sockaddr_vm's; the
 * guest is VMADDR_CID_GUEST, the host VMADDR_CID_HOST. A listener's
 * connections are the host's; a connection's peer is the host. They're
 * registered as AF_VSOCK itself (no CONFIG_VSOCKETS: nothing else is).
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
#include <linux/xarray.h>
#include <linux/vm_sockets.h>
#include <net/ipv6.h>
#include <net/sock.h>
#include <asm/wasm_imports.h>
#include "exobox.h"
#include <asm/irq.h>

/*
 * Sockets the host makes, for the guest's AF_INET(6) sockets whose peer
 * isn't the guest itself. Addresses are Linux's
 * sockaddr_in(6), in the kernel's memory. None of these waits: the host
 * works meanwhile, and its sockets' state (sock_state) changes as it goes,
 * each change raising sock_irq's interrupt.
 */
/* what the host has for the guest's sockets: HOSTSOCK_INET (its network) and HOSTSOCK_VSOCK (itself), or 0 (then no sock_irq) */
int exobox_import(kernel, sock_init)(void);
/* the interrupt to raise when sockets' state changed (0: none) */
void exobox_import(kernel, sock_irq)(int irq);
/* a socket (AF_INET(6); SOCK_STREAM or SOCK_DGRAM; AF_VSOCK, SOCK_STREAM): its handle (> 0), or a negative errno */
int exobox_import(kernel, sock_open)(int family, int type);
/* start connecting a stream: 0 (then CONNECTED or FAILED), or a negative errno */
int exobox_import(kernel, sock_connect)(int sock, const void *addr, int addr_len);
/* offer a listener on addr's port to the host: 0 (then LISTENING, or FAILED: -EPERM if not published) */
int exobox_import(kernel, sock_listen)(int sock, const void *addr, int addr_len, int backlog);
/* a connection the host took on a listener: its handle, or -EAGAIN */
int exobox_import(kernel, sock_accept)(int sock);
/* len bytes of buf (a stream: as many as it takes now; a datagram to addr, whole): how many, -EAGAIN, or an errno */
long exobox_import(kernel, sock_send)(int sock, const void *buf, unsigned long len, const void *addr, int addr_len);
/* into buf, at most len: a stream's next (0: its end), or the next datagram (its source at addr); -EAGAIN if none; peek: leave them */
long exobox_import(kernel, sock_recv)(int sock, void *buf, unsigned long len, int peek, void *addr, int *addr_len);
/* the socket's address, or its peer's, at addr: its length, or a negative errno */
int exobox_import(kernel, sock_getname)(int sock, void *addr, int peer);
int exobox_import(kernel, sock_shutdown)(int sock, int how);
void exobox_import(kernel, sock_close)(int sock);
/* a socket whose state changed since, or 0 */
int exobox_import(kernel, sock_changed)(void);
/* its HOSTSOCK_* bits, and with HOSTSOCK_FAILED its errno above HOSTSOCK_ERRNO_SHIFT */
unsigned int exobox_import(kernel, sock_state)(int sock);
/* the bytes a stream has to read (FIONREAD) */
unsigned long exobox_import(kernel, sock_pending)(int sock);

/* a host socket's state (sock_state) */
#define HOSTSOCK_CONNECTED	1	/* a stream's connection is made */
#define HOSTSOCK_LISTENING	2	/* the host publishes the listener */
#define HOSTSOCK_READABLE	4	/* bytes, or datagrams, to read */
#define HOSTSOCK_WRITABLE	8	/* room for more to send */
#define HOSTSOCK_EOF		16	/* the peer won't send more */
#define HOSTSOCK_FAILED		32	/* connect or listen failed, or reset */
#define HOSTSOCK_ACCEPTABLE	64	/* a listener's connections wait */
#define HOSTSOCK_ERRNO_SHIFT	16

/* what the host has (sock_init) */
#define HOSTSOCK_INET		1
#define HOSTSOCK_VSOCK		2

/* the guest's CID (the host is VMADDR_CID_HOST) */
#define VMADDR_CID_GUEST	3

#define AF_HOSTSOCK	AF_ECONET	/* the guest's AF_INET sockets */
#define AF_HOSTSOCK6	AF_WANPIPE	/* ...and AF_INET6 */

/* what's copied through the kernel at a time, each way */
#define CHUNK		(64 * 1024)

enum mode { UNDECIDED, LOCAL, REMOTE };

struct hsock {
	struct sock sk;			/* first: the sock is ours */
	struct socket *inner;		/* local traffic, and the options' record */
	int family;			/* AF_INET, AF_INET6 or AF_VSOCK */
	enum mode mode;
	int hid;			/* the host's socket (REMOTE) */
	bool listener;			/* a REMOTE listener (and its inner one) */
	bool error_told;		/* a failure's errno is in sk_err, once */
	struct sockaddr_storage peer;	/* a REMOTE datagram socket's connect() */
	int peer_len;
	struct sockaddr_vm bound;	/* a vsock's bind() (svm_family 0: none) */
};

#define hs_sk(sk) ((struct hsock *)(sk))

static const struct proto_ops hostsock_stream_ops, hostsock_dgram_ops, hostsock_vsock_ops;

static bool hostsock_on;
static int hostsock_irq;
static DEFINE_XARRAY(hsocks);		/* hid -> hsock */
static struct work_struct hostsock_work;

/* the arch's socket(): AF_INET(6) stream and datagram sockets are ours */
int hostsock_family(int family, int type)
{
	type &= SOCK_TYPE_MASK;
	if (!hostsock_on || (type != SOCK_STREAM && type != SOCK_DGRAM))
		return family;
	if (family == AF_INET)
		return AF_HOSTSOCK;
	if (family == AF_INET6 && IS_ENABLED(CONFIG_IPV6))
		return AF_HOSTSOCK6;
	return family;
}

/*
 * getsockopt(SOL_SOCKET, SO_DOMAIN) on ours: AF_INET(6), not the family
 * they're registered under. (The generic SOL_SOCKET options don't reach
 * our ops; and our sock can't say AF_INET itself: code that sees an inet
 * sock takes it for an inet_sock, as ours isn't.) params: getsockopt's.
 */
bool hostsock_sol_socket(const long *params, long *ret)
{
	int __user *optval = (int __user *)params[3];
	int __user *optlen = (int __user *)params[4];
	struct socket *sock;
	int err, len, family;

	if (!hostsock_on || params[1] != SOL_SOCKET || params[2] != SO_DOMAIN)
		return false;
	sock = sockfd_lookup(params[0], &err);
	if (!sock)
		return false;
	if (sock->ops != &hostsock_stream_ops && sock->ops != &hostsock_dgram_ops) {
		sockfd_put(sock);
		return false;
	}
	family = hs_sk(sock->sk)->family;
	sockfd_put(sock);
	/* (as sk_getsockopt()) */
	if (get_user(len, optlen))
		*ret = -EFAULT;
	else if (len < 0)
		*ret = -EINVAL;
	else {
		len = min_t(int, len, sizeof(family));
		*ret = copy_to_user(optval, &family, len) || put_user(len, optlen) ?
			-EFAULT : 0;
	}
	return true;
}

static unsigned int hs_state(struct hsock *hs)
{
	return wasm_kernel_sock_state(hs->hid);
}

/* a failure's errno (connect refused, connection reset), or 0 */
static int hs_error(unsigned int state)
{
	return state & HOSTSOCK_FAILED ? -(int)(state >> HOSTSOCK_ERRNO_SHIFT) : 0;
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

/* the host's socket for hs, made now (a REMOTE socket's first need) */
static int hs_open(struct hsock *hs)
{
	int hid;

	if (hs->hid)
		return 0;
	hid = wasm_kernel_sock_open(hs->family, hs->sk.sk_type);
	if (hid < 0)
		return hid;
	hs->hid = hid;
	xa_store(&hsocks, hid, hs, GFP_KERNEL);
	return 0;
}

/*
 * Wait for any of bits in the host's state (or a failure), as long as
 * *timeo: 0, -EAGAIN (out of time), or -EINTR/-ERESTARTSYS
 */
/* a published listener's inner one has a connection to accept */
static bool hs_inner_acceptable(struct hsock *hs)
{
	return hs->listener && hs->inner &&
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

static int hs_wait(struct hsock *hs, unsigned int bits, long *timeo)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	int err = 0;

	add_wait_queue(sk_sleep(&hs->sk), &wait);
	while (!(hs_state(hs) & (bits | HOSTSOCK_FAILED)) &&
	       !hs_inner_acceptable(hs)) {
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

/* the host's interrupt: sockets whose state changed; wake who waits on them */
static void hostsock_changed(struct work_struct *work)
{
	int hid;

	while ((hid = wasm_kernel_sock_changed()) > 0) {
		struct hsock *hs = xa_load(&hsocks, hid);

		if (hs)
			hs->sk.sk_state_change(&hs->sk);
	}
}

static irqreturn_t hostsock_interrupt(int irq, void *dev_id)
{
	schedule_work(&hostsock_work);
	return IRQ_HANDLED;
}

static struct proto hostsock_proto;

/* our sock for sock, with an inner socket (made here, unless given) */
static struct hsock *hs_new(struct net *net, struct socket *sock, int family,
			    int protocol, int kern, struct socket *inner)
{
	struct sock *sk;
	struct hsock *hs;
	int err;

	sk = sk_alloc(net, family == AF_INET ? AF_HOSTSOCK : AF_HOSTSOCK6,
		      GFP_KERNEL, &hostsock_proto, kern);
	if (!sk)
		return ERR_PTR(-ENOMEM);
	sock->ops = sock->type == SOCK_STREAM ? &hostsock_stream_ops :
						&hostsock_dgram_ops;
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

static int hostsock_create(struct net *net, struct socket *sock, int protocol,
			   int kern, int family)
{
	if (sock->type != SOCK_STREAM && sock->type != SOCK_DGRAM)
		return -ESOCKTNOSUPPORT;
	return PTR_ERR_OR_ZERO(hs_new(net, sock, family, protocol, kern, NULL));
}

static int hostsock_create4(struct net *net, struct socket *sock, int protocol,
			    int kern)
{
	return hostsock_create(net, sock, protocol, kern, AF_INET);
}

static int hostsock_create6(struct net *net, struct socket *sock, int protocol,
			    int kern)
{
	return hostsock_create(net, sock, protocol, kern, AF_INET6);
}

static int hostsock_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct hsock *hs;

	if (!sk)
		return 0;
	hs = hs_sk(sk);
	if (hs->hid) {
		xa_erase(&hsocks, hs->hid);
		wasm_kernel_sock_close(hs->hid);
		hs->hid = 0;
	}
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
static int hostsock_bind(struct socket *sock, struct sockaddr_unsized *uaddr, int len)
{
	struct sockaddr *addr = (struct sockaddr *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode == REMOTE)
		return -EINVAL;
	return hs->inner->ops->bind(hs->inner, uaddr, len);
}

/* a REMOTE stream's connect(), once asked: connecting, or connected, or failed */
static int hs_connected(struct socket *sock, struct hsock *hs, int flags)
{
	long timeo;
	int err;

	if (hs_state(hs) & HOSTSOCK_CONNECTED) {
		if (sock->state == SS_CONNECTED)
			return -EISCONN;
		sock->state = SS_CONNECTED;
		return 0;
	}
	timeo = sock_sndtimeo(sock->sk, flags & O_NONBLOCK);
	err = hs_wait(hs, HOSTSOCK_CONNECTED, &timeo);
	if (err == -EAGAIN)
		return sock->state == SS_CONNECTING && !(flags & O_NONBLOCK) ?
			-ETIMEDOUT : -EINPROGRESS;
	if (err)
		return err;
	err = hs_error(hs_state(hs));
	if (err) {
		hs->error_told = true;
		sock->state = SS_UNCONNECTED;
		return err;
	}
	sock->state = SS_CONNECTED;
	return 0;
}

static int hostsock_stream_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
				   int len, int flags)
{
	struct sockaddr *addr = (struct sockaddr *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);
	int err;

	if (hs->mode == LOCAL)
		return hs->inner->ops->connect(hs->inner, uaddr, len, flags);
	if (hs->mode == UNDECIDED) {
		if (len < (int)sizeof(addr->sa_family))
			return -EINVAL;
		if (hs_local_addr(addr, len)) {
			hs->mode = LOCAL;
			return hs->inner->ops->connect(hs->inner, uaddr, len, flags);
		}
		if (addr->sa_family != hs->family)
			return -EAFNOSUPPORT;
		hs->mode = REMOTE;
		err = hs_open(hs);
		if (!err)
			err = wasm_kernel_sock_connect(hs->hid, addr, len);
		if (err)
			return err;
		sock->state = SS_CONNECTING;
	} else if (hs->listener) {
		return -EISCONN;
	}
	return hs_connected(sock, hs, flags);
}

static int hostsock_dgram_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
				  int len, int flags)
{
	struct sockaddr *addr = (struct sockaddr *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode == UNDECIDED && len >= (int)sizeof(addr->sa_family) &&
	    addr->sa_family != AF_UNSPEC)
		hs->mode = hs_local_addr(addr, len) ? LOCAL : REMOTE;
	if (hs->mode != REMOTE)
		return hs->inner->ops->connect(hs->inner, uaddr, len, flags);
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
static int hostsock_listen(struct socket *sock, int backlog)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct sockaddr_storage addr;
	long timeo = MAX_SCHEDULE_TIMEOUT;
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
	err = wasm_kernel_sock_listen(hs->hid, &addr, len, backlog);
	if (!err)
		err = hs_wait(hs, HOSTSOCK_LISTENING, &timeo);
	if (!err)
		err = hs_error(hs_state(hs));
	if (err) {
		xa_erase(&hsocks, hs->hid);
		wasm_kernel_sock_close(hs->hid);
		hs->hid = 0;
		return err == -EPERM || err == -EADDRINUSE ||
		       err == -EADDRNOTAVAIL ? 0 : err;
	}
	hs->mode = REMOTE;
	hs->listener = true;
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

/* a published listener's: the guest's connections, and the host's */
static int hostsock_accept(struct socket *sock, struct socket *newsock,
			   struct proto_accept_arg *arg)
{
	struct hsock *hs = hs_sk(sock->sk), *child;
	int flags = arg->flags;
	long timeo;
	int hid, err;

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
		hid = wasm_kernel_sock_accept(hs->hid);
		if (hid != -EAGAIN)
			break;
		err = hs_wait(hs, HOSTSOCK_ACCEPTABLE, &timeo);
		if (err)
			return err;
	}
	if (hid < 0)
		return hid;
	child = hs_new(sock_net(sock->sk), newsock, hs->family, 0, arg->kern, NULL);
	if (IS_ERR(child)) {
		wasm_kernel_sock_close(hid);
		return PTR_ERR(child);
	}
	child->mode = REMOTE;
	child->hid = hid;
	xa_store(&hsocks, hid, child, GFP_KERNEL);
	newsock->state = SS_CONNECTED;
	return 0;
}

static int hostsock_getname(struct socket *sock, struct sockaddr *addr, int peer)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode != REMOTE)
		return hs->inner->ops->getname(hs->inner, addr, peer);
	if (peer && sock->type == SOCK_DGRAM) {
		if (!hs->peer_len)
			return -ENOTCONN;
		memcpy(addr, &hs->peer, hs->peer_len);
		return hs->peer_len;
	}
	return wasm_kernel_sock_getname(hs->hid, addr, peer);
}

/* a REMOTE socket's poll: the host's state */
static __poll_t hs_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct hsock *hs = hs_sk(sk);
	unsigned int state;
	__poll_t mask = 0;

	sock_poll_wait(file, sock, wait);
	if (!hs->hid)	/* (a datagram socket that hasn't sent) */
		return EPOLLOUT | EPOLLWRNORM;
	state = hs_state(hs);
	if (hs->listener)
		return state & HOSTSOCK_ACCEPTABLE || hs_inner_acceptable(hs) ?
			EPOLLIN | EPOLLRDNORM : 0;
	if (state & HOSTSOCK_READABLE)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (state & HOSTSOCK_EOF)
		mask |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
	if (sock->type == SOCK_DGRAM ||
	    (state & (HOSTSOCK_CONNECTED | HOSTSOCK_WRITABLE)) ==
		    (HOSTSOCK_CONNECTED | HOSTSOCK_WRITABLE))
		mask |= EPOLLOUT | EPOLLWRNORM;
	if (state & HOSTSOCK_FAILED) {
		/* (an asynchronous connect's error: SO_ERROR's, once) */
		if (!hs->error_told) {
			hs->error_told = true;
			WRITE_ONCE(sk->sk_err, -hs_error(state));
		}
		mask |= EPOLLERR | EPOLLHUP | EPOLLOUT;
	}
	if ((state & HOSTSOCK_EOF) && (sk->sk_shutdown & SEND_SHUTDOWN))
		mask |= EPOLLHUP;
	return mask;
}

static __poll_t hostsock_poll(struct file *file, struct socket *sock,
			      poll_table *wait)
{
	struct sock *sk = sock->sk;
	struct hsock *hs = hs_sk(sk);
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
	return hs_poll(file, sock, wait);
}

static int hostsock_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode != REMOTE)
		return hs->inner->ops->ioctl(hs->inner, cmd, arg);
	if (cmd == SIOCINQ)
		return put_user((int)wasm_kernel_sock_pending(hs->hid), (int __user *)arg);
	return -ENOIOCTLCMD;
}

static int hostsock_shutdown(struct socket *sock, int how)
{
	struct hsock *hs = hs_sk(sock->sk);
	int err;

	if (hs->mode != REMOTE)
		return hs->inner->ops->shutdown(hs->inner, how);
	if (how < SHUT_RD || how > SHUT_RDWR)
		return -EINVAL;
	if (sock->state != SS_CONNECTED)
		return -ENOTCONN;
	err = wasm_kernel_sock_shutdown(hs->hid, how);
	if (!err)
		sock->sk->sk_shutdown |= how + 1;	/* (RCV/SEND_SHUTDOWN) */
	return err;
}

/* SOL_SOCKET's on ours (generic code reads them there) and the inner's too */
static int hostsock_setsockopt(struct socket *sock, int level, int optname,
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

static int hostsock_getsockopt(struct socket *sock, int level, int optname,
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

/* send msg to the host: addr, a datagram's destination (NULL: connected) */
static int hs_send(struct hsock *hs, struct msghdr *msg, size_t len,
		   const void *addr, int addr_len)
{
	struct sock *sk = &hs->sk;
	long timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);
	size_t sent = 0;
	void *buf;
	int err = 0;

	if (sk->sk_shutdown & SEND_SHUTDOWN)
		return hs_send_error(sk, -EPIPE, msg->msg_flags);
	if (sk->sk_type == SOCK_DGRAM && len > CHUNK)
		return -EMSGSIZE;
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
		ret = wasm_kernel_sock_send(hs->hid, buf, n, addr, addr_len);
		if (ret == -EAGAIN) {
			iov_iter_revert(&msg->msg_iter, n);
			err = hs_wait(hs, HOSTSOCK_WRITABLE, &timeo);
			if (!err)
				err = hs_error(hs_state(hs));
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

/*
 * from the host to msg: a stream's bytes (all there are, up to len; with
 * MSG_WAITALL, len), or one datagram, and its source
 */
static int hs_recv(struct hsock *hs, struct msghdr *msg, size_t len, int flags)
{
	struct sock *sk = &hs->sk;
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	bool stream = sk->sk_type == SOCK_STREAM;
	struct sockaddr_storage from;
	size_t copied = 0;
	void *buf;
	int err = 0;

	if (stream && !(hs_state(hs) & HOSTSOCK_CONNECTED) &&
	    !(hs_state(hs) & (HOSTSOCK_EOF | HOSTSOCK_FAILED)))
		return -ENOTCONN;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		return 0;
	buf = kmalloc(CHUNK, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	for (;;) {
		int from_len = sizeof(from);
		size_t n = min_t(size_t, len - copied, CHUNK);
		long ret = wasm_kernel_sock_recv(hs->hid, buf, stream ? n : CHUNK,
					      flags & MSG_PEEK, &from, &from_len);

		if (ret == -EAGAIN) {
			if (copied && !(flags & MSG_WAITALL))
				break;
			err = hs_wait(hs, HOSTSOCK_READABLE | HOSTSOCK_EOF, &timeo);
			if (err)
				break;
			continue;
		}
		if (ret < 0) {
			err = ret;
			break;
		}
		if (!stream) {
			/* one datagram: what fits, MSG_TRUNC if not all */
			if (ret > len) {
				msg->msg_flags |= MSG_TRUNC;
				n = len;
			} else {
				n = ret;
			}
			if (copy_to_iter(buf, n, &msg->msg_iter) != n)
				err = -EFAULT;
			else if (msg->msg_name) {
				memcpy(msg->msg_name, &from, from_len);
				msg->msg_namelen = from_len;
			}
			copied = flags & MSG_TRUNC ? ret : n;
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
	if (err == -EAGAIN && hs_error(hs_state(hs)))
		err = hs_error(hs_state(hs));
	return err;
}

static int hostsock_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct hsock *hs = hs_sk(sock->sk);
	struct sockaddr *to = msg->msg_name;
	int err;

	if (sock->type == SOCK_DGRAM && hs->mode == UNDECIDED && to)
		hs->mode = hs_local_addr(to, msg->msg_namelen) ? LOCAL : REMOTE;
	if (hs->mode != REMOTE)
		return hs->inner->ops->sendmsg(hs->inner, msg, len);
	if (sock->type == SOCK_STREAM) {
		if (sock->state != SS_CONNECTED)
			return hs_state(hs) & HOSTSOCK_CONNECTED ? hs_send(hs, msg, len, NULL, 0) :
				hs_send_error(sock->sk, -EPIPE, msg->msg_flags);
		return hs_send(hs, msg, len, NULL, 0);
	}
	if (!to) {
		if (!hs->peer_len)
			return -EDESTADDRREQ;
		to = (struct sockaddr *)&hs->peer;
		msg->msg_namelen = hs->peer_len;
	}
	err = hs_open(hs);
	if (err)
		return err;
	return hs_send(hs, msg, len, to, msg->msg_namelen);
}

static int hostsock_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			    int flags)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->mode != REMOTE || !hs->hid)
		return hs->inner->ops->recvmsg(hs->inner, msg, len, flags);
	return hs_recv(hs, msg, len, flags);
}

/*
 * AF_VSOCK: the host's sockets alone (REMOTE from the start, no inner
 * socket), streams only.
 */
static int hs_vsock_addr(const struct sockaddr *addr, int len)
{
	if (len < (int)sizeof(struct sockaddr_vm) || addr->sa_family != AF_VSOCK)
		return -EINVAL;
	return 0;
}

static int hostsock_vsock_create(struct net *net, struct socket *sock,
				 int protocol, int kern)
{
	struct sock *sk;
	struct hsock *hs;

	if (sock->type != SOCK_STREAM)
		return -ESOCKTNOSUPPORT;
	if (protocol && protocol != PF_VSOCK)
		return -EPROTONOSUPPORT;
	sk = sk_alloc(net, AF_VSOCK, GFP_KERNEL, &hostsock_proto, kern);
	if (!sk)
		return -ENOMEM;
	sock->ops = &hostsock_vsock_ops;
	sock_init_data(sock, sk);
	hs = hs_sk(sk);
	hs->family = AF_VSOCK;
	hs->mode = REMOTE;
	return 0;
}

/* (the address a listener listens at: the host makes a connection's) */
static int hostsock_vsock_bind(struct socket *sock, struct sockaddr_unsized *uaddr, int len)
{
	struct sockaddr_vm *addr = (struct sockaddr_vm *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);
	int err = hs_vsock_addr((struct sockaddr *)addr, len);

	if (err)
		return err;
	if (hs->bound.svm_family || hs->hid)
		return -EINVAL;
	if (addr->svm_cid != VMADDR_CID_ANY && addr->svm_cid != VMADDR_CID_GUEST)
		return -EADDRNOTAVAIL;
	hs->bound = *addr;
	return 0;
}

static int hostsock_vsock_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
				  int len, int flags)
{
	struct sockaddr *addr = (struct sockaddr *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);
	int err;

	if (hs->listener)
		return -EISCONN;
	if (!hs->hid) {
		err = hs_vsock_addr(addr, len);
		if (!err)
			err = hs_open(hs);
		if (!err)
			err = wasm_kernel_sock_connect(hs->hid, addr, len);
		if (err)
			return err;
		sock->state = SS_CONNECTING;
	}
	return hs_connected(sock, hs, flags);
}

/* a listener at its bound address, the host's: it makes the connections */
static int hostsock_vsock_listen(struct socket *sock, int backlog)
{
	struct hsock *hs = hs_sk(sock->sk);
	long timeo = MAX_SCHEDULE_TIMEOUT;
	int err;

	if (hs->listener)
		return 0;
	if (!hs->bound.svm_family || hs->hid)
		return -EINVAL;
	err = hs_open(hs);
	if (!err)
		err = wasm_kernel_sock_listen(hs->hid, &hs->bound, sizeof(hs->bound), backlog);
	if (!err)
		err = hs_wait(hs, HOSTSOCK_LISTENING, &timeo);
	if (!err)
		err = hs_error(hs_state(hs));
	if (err) {
		xa_erase(&hsocks, hs->hid);
		wasm_kernel_sock_close(hs->hid);
		hs->hid = 0;
		return err;
	}
	hs->listener = true;
	return 0;
}

static int hostsock_vsock_accept(struct socket *sock, struct socket *newsock,
				 struct proto_accept_arg *arg)
{
	struct hsock *hs = hs_sk(sock->sk), *child;
	long timeo = sock_rcvtimeo(sock->sk, arg->flags & O_NONBLOCK);
	int hid, err;

	if (!hs->listener)
		return -EINVAL;
	while ((hid = wasm_kernel_sock_accept(hs->hid)) == -EAGAIN) {
		err = hs_wait(hs, HOSTSOCK_ACCEPTABLE, &timeo);
		if (err)
			return err;
	}
	if (hid < 0)
		return hid;
	err = hostsock_vsock_create(sock_net(sock->sk), newsock, 0, arg->kern);
	if (err) {
		wasm_kernel_sock_close(hid);
		return err;
	}
	child = hs_sk(newsock->sk);
	child->hid = hid;
	xa_store(&hsocks, hid, child, GFP_KERNEL);
	newsock->state = SS_CONNECTED;
	return 0;
}

static int hostsock_vsock_getname(struct socket *sock, struct sockaddr *addr, int peer)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (hs->hid)
		return wasm_kernel_sock_getname(hs->hid, addr, peer);
	if (peer)
		return -ENOTCONN;
	if (hs->bound.svm_family) {
		memcpy(addr, &hs->bound, sizeof(hs->bound));
	} else {
		struct sockaddr_vm any = { .svm_family = AF_VSOCK,
					   .svm_cid = VMADDR_CID_ANY,
					   .svm_port = VMADDR_PORT_ANY };

		memcpy(addr, &any, sizeof(any));
	}
	return sizeof(struct sockaddr_vm);
}

static __poll_t hostsock_vsock_poll(struct file *file, struct socket *sock,
				    poll_table *wait)
{
	if (!hs_sk(sock->sk)->hid) {
		sock_poll_wait(file, sock, wait);
		return 0;
	}
	return hs_poll(file, sock, wait);
}

static int hostsock_vsock_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (!hs->hid || hs->listener)
		return -ENOTCONN;
	return hs_send(hs, msg, len, NULL, 0);
}

static int hostsock_vsock_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
				  int flags)
{
	if (!hs_sk(sock->sk)->hid)
		return -ENOTCONN;
	return hs_recv(hs_sk(sock->sk), msg, len, flags);
}

static int hostsock_vsock_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct hsock *hs = hs_sk(sock->sk);

	if (cmd == SIOCINQ && hs->hid)
		return put_user((int)wasm_kernel_sock_pending(hs->hid), (int __user *)arg);
	return -ENOIOCTLCMD;
}

static const struct proto_ops hostsock_stream_ops = {
	.family		= AF_HOSTSOCK,
	.owner		= THIS_MODULE,
	.release	= hostsock_release,
	.bind		= hostsock_bind,
	.connect	= hostsock_stream_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= hostsock_accept,
	.getname	= hostsock_getname,
	.poll		= hostsock_poll,
	.ioctl		= hostsock_ioctl,
	.listen		= hostsock_listen,
	.shutdown	= hostsock_shutdown,
	.setsockopt	= hostsock_setsockopt,
	.getsockopt	= hostsock_getsockopt,
	.sendmsg	= hostsock_sendmsg,
	.recvmsg	= hostsock_recvmsg,
	.mmap		= sock_no_mmap,
};

static const struct proto_ops hostsock_dgram_ops = {
	.family		= AF_HOSTSOCK,
	.owner		= THIS_MODULE,
	.release	= hostsock_release,
	.bind		= hostsock_bind,
	.connect	= hostsock_dgram_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= hostsock_getname,
	.poll		= hostsock_poll,
	.ioctl		= hostsock_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= hostsock_shutdown,
	.setsockopt	= hostsock_setsockopt,
	.getsockopt	= hostsock_getsockopt,
	.sendmsg	= hostsock_sendmsg,
	.recvmsg	= hostsock_recvmsg,
	.mmap		= sock_no_mmap,
};

static const struct proto_ops hostsock_vsock_ops = {
	.family		= AF_VSOCK,
	.owner		= THIS_MODULE,
	.release	= hostsock_release,
	.bind		= hostsock_vsock_bind,
	.connect	= hostsock_vsock_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= hostsock_vsock_accept,
	.getname	= hostsock_vsock_getname,
	.poll		= hostsock_vsock_poll,
	.ioctl		= hostsock_vsock_ioctl,
	.listen		= hostsock_vsock_listen,
	.shutdown	= hostsock_shutdown,
	.sendmsg	= hostsock_vsock_sendmsg,
	.recvmsg	= hostsock_vsock_recvmsg,
	.mmap		= sock_no_mmap,
};

static struct proto hostsock_proto = {
	.name		= "HOSTSOCK",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct hsock),
};

static const struct net_proto_family hostsock_family4 = {
	.family	= AF_HOSTSOCK,
	.create	= hostsock_create4,
	.owner	= THIS_MODULE,
};

static const struct net_proto_family hostsock_family6 = {
	.family	= AF_HOSTSOCK6,
	.create	= hostsock_create6,
	.owner	= THIS_MODULE,
};

static const struct net_proto_family hostsock_family_vsock = {
	.family	= AF_VSOCK,
	.create	= hostsock_vsock_create,
	.owner	= THIS_MODULE,
};

static int __init hostsock_init(void)
{
	int has = wasm_kernel_sock_init(), err;

	/* (no network from the host: AF_INET stays the kernel's, loopback only) */
	if (!has)
		return 0;
	INIT_WORK(&hostsock_work, hostsock_changed);
	hostsock_irq = wasm_alloc_irq();
	if (hostsock_irq < 0)
		return hostsock_irq;
	err = request_irq(hostsock_irq, hostsock_interrupt, 0, "hostsock", NULL);
	if (err)
		return err;
	/* (the host raises it when sockets' state changed) */
	wasm_kernel_sock_irq(hostsock_irq);
	err = proto_register(&hostsock_proto, 0);
	if (err)
		goto out_request;
	if (has & HOSTSOCK_VSOCK) {
		err = sock_register(&hostsock_family_vsock);
		if (err)
			goto out_proto;
	}
	if (has & HOSTSOCK_INET) {
		err = sock_register(&hostsock_family4);
		if (err)
			goto out_vsock;
		err = sock_register(&hostsock_family6);
		if (err)
			goto out_family4;
		hostsock_on = true;
	}
	return 0;

out_family4:
	sock_unregister(AF_HOSTSOCK);
out_vsock:
	if (has & HOSTSOCK_VSOCK)
		sock_unregister(AF_VSOCK);
out_proto:
	proto_unregister(&hostsock_proto);
out_request:
	wasm_kernel_sock_irq(0);
	free_irq(hostsock_irq, NULL);
	return err;
}
/* (after inet: the inner sockets are its) */
late_initcall(hostsock_init);
