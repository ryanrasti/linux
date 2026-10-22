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
#include <net/ipv6.h>
#include <net/sock.h>
#include <asm/wasm_imports.h>
#include "exobox.h"
#include <asm/irq.h>

#define AF_HOSTSOCK	AF_ECONET	/* the guest's AF_INET sockets */
#define AF_HOSTSOCK6	AF_WANPIPE	/* ...and AF_INET6 */

/* what's copied through the kernel at a time, each way */
#define CHUNK		(64 * 1024)

enum mode { UNDECIDED, LOCAL, REMOTE };

struct hsock {
	struct sock sk;			/* first: the sock is ours */
	struct socket *inner;		/* local traffic, and the options' record */
	int family;			/* AF_INET or AF_INET6 */
	enum mode mode;
	int hid;			/* the host's socket (REMOTE) */
	bool listener;			/* a REMOTE listener (and its inner one) */
	bool error_told;		/* a failure's errno is in sk_err, once */
	struct sockaddr_storage peer;	/* a REMOTE datagram socket's connect() */
	int peer_len;
};

#define hs_sk(sk) ((struct hsock *)(sk))

static const struct proto_ops hostsock_stream_ops, hostsock_dgram_ops;

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

static int hostsock_stream_connect(struct socket *sock, struct sockaddr_unsized *uaddr,
				   int len, int flags)
{
	struct sockaddr *addr = (struct sockaddr *)uaddr;
	struct hsock *hs = hs_sk(sock->sk);
	long timeo;
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

	/* REMOTE: connecting, or connected, or failed */
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

static __poll_t hostsock_poll(struct file *file, struct socket *sock,
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

static int __init hostsock_init(void)
{
	int err;

	/* (no network from the host: AF_INET stays the kernel's, loopback only) */
	if (!wasm_kernel_sock_init())
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
	err = sock_register(&hostsock_family4);
	if (err)
		goto out_proto;
	err = sock_register(&hostsock_family6);
	if (err)
		goto out_family4;
	hostsock_on = true;
	return 0;

out_family4:
	sock_unregister(AF_HOSTSOCK);
out_proto:
	proto_unregister(&hostsock_proto);
out_request:
	wasm_kernel_sock_irq(0);
	free_irq(hostsock_irq, NULL);
	return err;
}
/* (after inet: the inner sockets are its) */
late_initcall(hostsock_init);
