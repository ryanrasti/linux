// SPDX-License-Identifier: GPL-2.0
/*
 * Transparent Socket Impersonation for an in-process kernel: a program's
 * AF_INET sockets are served by the host for everything that isn't the
 * guest's own (loopback), with no network device, no routing and no
 * protocol of the host's in the kernel.
 *
 * The structure is libkrunfw's TSI (net/tsi/af_tsi.c): a socket with two
 * personalities. The outer socket, the program's, keeps AF_INET's face
 * (getsockopt(SO_DOMAIN) says AF_INET, getsockname and getpeername answer
 * in sockaddr_in) over an inner AF_INET socket of the guest's own, used as
 * long as the socket stays local: bind, a connect to 127.0.0.0/8 or
 * 0.0.0.0, and listen (local first). What TSI sends over vsock to a proxy,
 * here is a direct call to the host, and what TSI's proxy sends back is the
 * host calling into the kernel (lkl_net_*, asm/host_ops.h):
 *
 * - connect to any other address: host op net_connect(sock, pid, addr,
 *   len), the CPU held, which returns at once; the host answers later with
 *   lkl_net_done(sock, err). Then the socket is host-backed: its transport is
 *   one end of a kernel AF_UNIX socketpair, and the other end is the
 *   host's ("end"), which it reads and writes directly (lkl_net_recv,
 *   lkl_net_send: nonblocking, -EAGAIN), told of readiness by host op
 *   net_ready(end) from the end's callbacks.
 * - listen: the inner socket listens (the guest's own clients connect to it
 *   as usual), and, unless it is bound to loopback, host op net_listen
 *   offers the address to the host, which hands in inbound connections with
 *   lkl_net_accept(sock, peer) -> end.
 * - a datagram to a non-loopback address (the program's resolver queries,
 *   to the nameserver /etc/resolv.conf names): host op net_sendto, answered
 *   with lkl_net_reply(sock, from, data) datagrams.
 *
 * Every net_* op that returns 0 hands the host a reference to sock, given
 * back with lkl_net_done(sock, err). A socket the guest closes while the
 * host holds one is reported with net_ready(sock).
 *
 * The family is never registered (sock_register): its number, AF_TSI, is
 * only the outer sock's sk_family. Sockets are made here, from the program's
 * socket() (tsi_handler, called at the top of run_syscall), with sock_alloc
 * and sock_alloc_file, so net/socket.c is untouched.
 *
 * Every wakeup reaches the program through the outer socket's wait queue:
 * the inner socket's and the transport's callbacks also wake it, and poll
 * registers on it alone. So a poll or an epoll registration made before a
 * connect sees the connection once it exists.
 *
 * The host's calls into the kernel run as a kernel thread of their own
 * ("tsi"), parked like host0 (syscalls.c), which the calling host thread
 * borrows with the CPU: a task of no process of the program's, so its
 * calls are invisible to the program (no thread of init's to be killed at
 * an exec, no child to wait for).
 */
#include <linux/in.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/syscalls.h>
#include <linux/uio.h>
#include <net/af_unix.h>
#include <net/inet_sock.h>
#include <net/sock.h>
#include <net/tcp_states.h>
#include <asm/cpu.h>
#include <asm/ioctls.h>
#include <asm/unistd.h>
#include <asm/host_ops.h>
#include <asm/sched.h>
#include <asm/syscalls.h>

/* the outer socket's personality */
enum { TSI_HYBRID, TSI_INET, TSI_HOST };
/* a host-backed stream's connection */
enum { TSI_NONE, TSI_CONNECTING, TSI_CONNECTED, TSI_FAILED };

/* the original callbacks of a socket whose wakeups also wake the outer one */
struct tsi_hook {
	void (*data_ready)(struct sock *sk);
	void (*write_space)(struct sock *sk);
	void (*state_change)(struct sock *sk);
	void (*error_report)(struct sock *sk);
};

/* a host connection lkl_net_accept queued, until the program accepts it */
struct tsi_pending {
	struct list_head list;
	struct socket *transport;
	/* or a host fd (lkl_net_accept_fd), if hosted */
	bool hosted;
	long hostfd;
	struct sockaddr_in peer;
};

struct tsi_sock {
	struct sock sk;
	/* the guest's own AF_INET socket: loopback, bind, listen; options */
	struct socket *isocket;
	/* host-backed: the guest end of the socketpair */
	struct socket *transport;
	/* host-backed by a host fd instead (lkl_net_done_fd): no transport */
	bool hosted;
	long hostfd;
	struct tsi_hook ihook, thook;
	int status;
	int conn;
	bool released;
	/* the host holds a reference for a listen (net_listen) */
	bool host_listening;
	/* datagrams the host holds (net_sendto), each a reference */
	int dgram_refs;
	/* host-backed: the peer (a stream's, or a datagram socket's connect) */
	struct sockaddr_in peer;
	/* host-backed: the local address getsockname answers (0: the inner's) */
	struct sockaddr_in local;
	struct list_head pending;
	int npending;
	int backlog;
};

#define tsi_sk(__sk) ((struct tsi_sock *)(__sk))

static struct proto tsi_proto = {
	.name = "TSI",
	.owner = THIS_MODULE,
	.obj_size = sizeof(struct tsi_sock),
};

static const struct proto_ops tsi_stream_ops;
static const struct proto_ops tsi_dgram_ops;

/* hostfd.c's I/O and poll by fd */
long lkl_hostfd_io(long fd, char __user *buf, size_t len, int write,
		   int nonblock);
__poll_t lkl_hostfd_poll_fd(struct file *file, long fd, __poll_t want,
			    poll_table *wait);

/* the task the host's calls run as: see the top */
static struct task_struct *tsi_task;

static bool tsi_is_local(const struct sockaddr_in *sin)
{
	return ipv4_is_loopback(sin->sin_addr.s_addr) ||
	       sin->sin_addr.s_addr == htonl(INADDR_ANY);
}

static int tsi_check_addr(const struct sockaddr *addr, int len)
{
	if (len < (int)sizeof(struct sockaddr_in))
		return -EINVAL;
	if (addr->sa_family != AF_INET)
		return -EAFNOSUPPORT;
	return 0;
}

/* everyone waiting on the program's socket, for any event: they look again */
static void tsi_wake(struct tsi_sock *tsk)
{
	struct socket_wq *wq;

	rcu_read_lock();
	wq = rcu_dereference(tsk->sk.sk_wq);
	if (skwq_has_sleeper(wq))
		wake_up_interruptible_all(&wq->wait);
	rcu_read_unlock();
}

/*
 * The inner socket's and the transport's callbacks: their own, then the
 * outer socket's waiters. sk_user_data is the outer socket while hooked.
 */
static void tsi_hooked(struct sock *sk, int which)
{
	struct tsi_sock *tsk;
	struct tsi_hook *h;

	read_lock_bh(&sk->sk_callback_lock);
	tsk = sk->sk_user_data;
	if (!tsk) {
		read_unlock_bh(&sk->sk_callback_lock);
		return;
	}
	/*
	 * The transport's, or the inner socket's; or a connection the inner
	 * listener made, which TCP cloned from it, callbacks and user data
	 * too, until the program accepts it (tsi_accept).
	 */
	h = tsk->transport && tsk->transport->sk == sk ? &tsk->thook :
							   &tsk->ihook;
	switch (which) {
	case 0:
		h->data_ready(sk);
		break;
	case 1:
		h->write_space(sk);
		break;
	case 2:
		h->state_change(sk);
		break;
	default:
		h->error_report(sk);
	}
	tsi_wake(tsk);
	read_unlock_bh(&sk->sk_callback_lock);
}

static void tsi_data_ready(struct sock *sk)
{
	tsi_hooked(sk, 0);
}

static void tsi_write_space(struct sock *sk)
{
	tsi_hooked(sk, 1);
}

static void tsi_state_change(struct sock *sk)
{
	tsi_hooked(sk, 2);
}

static void tsi_error_report(struct sock *sk)
{
	tsi_hooked(sk, 3);
}

static void tsi_hook(struct tsi_sock *tsk, struct socket *s, struct tsi_hook *h)
{
	struct sock *sk = s->sk;

	write_lock_bh(&sk->sk_callback_lock);
	h->data_ready = sk->sk_data_ready;
	h->write_space = sk->sk_write_space;
	h->state_change = sk->sk_state_change;
	h->error_report = sk->sk_error_report;
	sk->sk_user_data = tsk;
	sk->sk_data_ready = tsi_data_ready;
	sk->sk_write_space = tsi_write_space;
	sk->sk_state_change = tsi_state_change;
	sk->sk_error_report = tsi_error_report;
	write_unlock_bh(&sk->sk_callback_lock);
}

/* the callbacks given back, then the socket released */
static void tsi_unhook_release(struct socket *s, struct tsi_hook *h)
{
	struct sock *sk;

	if (!s)
		return;
	sk = s->sk;
	write_lock_bh(&sk->sk_callback_lock);
	if (sk->sk_user_data) {
		sk->sk_user_data = NULL;
		sk->sk_data_ready = h->data_ready;
		sk->sk_write_space = h->write_space;
		sk->sk_state_change = h->state_change;
		sk->sk_error_report = h->error_report;
	}
	write_unlock_bh(&sk->sk_callback_lock);
	sock_release(s);
}

/* ---- the host's end of a connection ---- */

/* the end's callbacks: the host is told, and only wakes a waker */
static void tsi_end_ready(struct sock *sk)
{
	void *end;

	read_lock_bh(&sk->sk_callback_lock);
	end = sk->sk_user_data;
	if (end && lkl_ops->net_ready)
		lkl_ops->net_ready(end);
	read_unlock_bh(&sk->sk_callback_lock);
}

/*
 * A connection: a socketpair, the guest's end hooked to the outer socket
 * later (*guest), the host's end (*end) reporting to the host.
 */
static int tsi_socketpair(struct socket **guest, struct socket **end)
{
	struct socket *a, *b;
	struct sock *sk;
	int err;

	err = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &a);
	if (err)
		return err;
	err = sock_create_kern(&init_net, AF_UNIX, SOCK_STREAM, 0, &b);
	if (err)
		goto free_a;
	err = a->ops->socketpair(a, b);
	if (err)
		goto free_b;
	sk = b->sk;
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = b;
	sk->sk_data_ready = tsi_end_ready;
	sk->sk_write_space = tsi_end_ready;
	sk->sk_state_change = tsi_end_ready;
	sk->sk_error_report = tsi_end_ready;
	write_unlock_bh(&sk->sk_callback_lock);
	*guest = a;
	*end = b;
	return 0;

free_b:
	sock_release(b);
free_a:
	sock_release(a);
	return err;
}

/* ---- the outer socket ---- */

static struct sock *tsi_sk_alloc(struct socket *sock, int type)
{
	struct tsi_sock *tsk;
	struct sock *sk;

	sk = sk_alloc(&init_net, AF_TSI, GFP_KERNEL, &tsi_proto, 0);
	if (!sk)
		return NULL;
	sock_init_data(sock, sk);
	/* SOL_SOCKET's setsockopt is ours too (getsockopt's: tsi_handler) */
	set_bit(SOCK_CUSTOM_SOCKOPT, &sock->flags);
	sk->sk_protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
	sock->ops = type == SOCK_STREAM ? &tsi_stream_ops : &tsi_dgram_ops;
	tsk = tsi_sk(sk);
	tsk->status = TSI_HYBRID;
	tsk->conn = TSI_NONE;
	INIT_LIST_HEAD(&tsk->pending);
	return sk;
}

static int tsi_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk;
	struct tsi_pending *p, *n;
	struct socket *inner, *transport;
	LIST_HEAD(pending);
	bool host_holds;

	if (!sk)
		return 0;
	tsk = tsi_sk(sk);
	lock_sock(sk);
	tsk->released = true;
	host_holds = tsk->conn == TSI_CONNECTING || tsk->host_listening ||
		     tsk->dgram_refs;
	list_splice_init(&tsk->pending, &pending);
	tsk->npending = 0;
	inner = tsk->isocket;
	transport = tsk->transport;
	sk->sk_shutdown = SHUTDOWN_MASK;
	release_sock(sk);

	tsi_unhook_release(transport, &tsk->thook);
	tsi_unhook_release(inner, &tsk->ihook);
	tsk->transport = NULL;
	tsk->isocket = NULL;
	list_for_each_entry_safe(p, n, &pending, list) {
		if (p->hosted) {
			if (lkl_ops->net_fd_close)
				lkl_ops->net_fd_close(p->hostfd);
		} else {
			sock_release(p->transport);
		}
		kfree(p);
	}
	if (tsk->hosted) {
		tsk->hosted = false;
		if (lkl_ops->net_fd_close)
			lkl_ops->net_fd_close(tsk->hostfd);
	}
	skb_queue_purge(&sk->sk_receive_queue);
	if (host_holds && lkl_ops->net_ready)
		lkl_ops->net_ready(sk);

	sock_orphan(sk);
	sock->sk = NULL;
	sock_put(sk);
	return 0;
}

static int tsi_bind(struct socket *sock, struct sockaddr *addr, int len)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	int err = tsi_check_addr(addr, len);

	if (err)
		return err;
	if (!tsk->isocket)
		return -EINVAL;
	return tsk->isocket->ops->bind(tsk->isocket, addr, len);
}

static int tsi_getname(struct socket *sock, struct sockaddr *addr, int peer)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;

	if (tsk->status == TSI_HOST) {
		if (peer) {
			if (sock->type == SOCK_STREAM &&
			    tsk->conn != TSI_CONNECTED)
				return -ENOTCONN;
			*sin = tsk->peer;
			return sizeof(*sin);
		}
		if (tsk->local.sin_family || !tsk->isocket) {
			*sin = tsk->local;
			sin->sin_family = AF_INET;
			return sizeof(*sin);
		}
	}
	if (!tsk->isocket)
		return -ENOTCONN;
	return tsk->isocket->ops->getname(tsk->isocket, addr, peer);
}

static int tsi_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	struct sk_buff_head *q = &sock->sk->sk_receive_queue;

	/*
	 * A datagram the host queued: FIONREAD is its size (glibc's resolver
	 * sizes its buffer for the second of its parallel answers so; the
	 * inner socket's 0 had it truncate the answer and fail).
	 */
	if (sock->type == SOCK_DGRAM && cmd == SIOCINQ &&
	    !skb_queue_empty_lockless(q)) {
		struct sk_buff *skb;
		int amount = 0;

		spin_lock_bh(&q->lock);
		skb = skb_peek(q);
		if (skb)
			amount = skb->len;
		spin_unlock_bh(&q->lock);
		return put_user(amount, (int __user *)arg);
	}
	if (tsk->status == TSI_HOST && tsk->transport &&
	    (cmd == SIOCINQ || cmd == SIOCOUTQ))
		return tsk->transport->ops->ioctl(tsk->transport, cmd, arg);
	if (!tsk->isocket)
		return -ENOIOCTLCMD;
	return tsk->isocket->ops->ioctl(tsk->isocket, cmd, arg);
}

/*
 * SOL_SOCKET: the outer socket's (its timeouts are what its own waits use),
 * the inner's (its answer, while it is the socket that serves), the
 * transport's (its timeouts). Others: the inner's, but TCP's once the
 * socket is host-backed, accepted and ignored.
 */
static int tsi_setsockopt(struct socket *sock, int level, int optname,
			  sockptr_t optval, unsigned int optlen)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	int err = 0, ierr = 0;

	if (level == SOL_SOCKET) {
		if (tsk->isocket)
			ierr = sock_setsockopt(tsk->isocket, level, optname,
					       optval, optlen);
		err = sock_setsockopt(sock, level, optname, optval, optlen);
		if (tsk->transport)
			sock_setsockopt(tsk->transport, level, optname, optval,
					optlen);
		return tsk->status != TSI_HOST && tsk->isocket ? ierr : err;
	}
	if (tsk->status == TSI_HOST && level == SOL_TCP)
		return 0;
	if (!tsk->isocket || !tsk->isocket->ops->setsockopt)
		return level == SOL_TCP ? 0 : -ENOPROTOOPT;
	return tsk->isocket->ops->setsockopt(tsk->isocket, level, optname,
					     optval, optlen);
}

static int tsi_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);

	if (!tsk->isocket || !tsk->isocket->ops->getsockopt)
		return -EOPNOTSUPP;
	return tsk->isocket->ops->getsockopt(tsk->isocket, level, optname,
					     optval, optlen);
}

/*
 * Wait on the outer socket (released while asleep) until cond() or the
 * timeout (*err -EAGAIN) or a signal.
 */
static int tsi_wait(struct sock *sk, long *timeo, bool (*cond)(struct tsi_sock *))
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct tsi_sock *tsk = tsi_sk(sk);
	int err = 0;

	add_wait_queue(sk_sleep(sk), &wait);
	while (!cond(tsk)) {
		if (!*timeo) {
			err = -EAGAIN;
			break;
		}
		if (signal_pending(current)) {
			err = sock_intr_errno(*timeo);
			break;
		}
		sk_wait_event(sk, timeo, cond(tsk), &wait);
	}
	remove_wait_queue(sk_sleep(sk), &wait);
	return err;
}

static bool tsi_not_connecting(struct tsi_sock *tsk)
{
	return READ_ONCE(tsk->conn) != TSI_CONNECTING;
}

/* a host connect's end, the socket locked: 0, or its error (consumed) */
static int tsi_connect_result(struct tsi_sock *tsk)
{
	int err;

	if (tsk->conn == TSI_CONNECTED)
		return 0;
	err = sock_error(&tsk->sk);
	tsk->conn = TSI_NONE;
	tsk->status = TSI_HYBRID;
	tsk->sk.sk_socket->state = SS_UNCONNECTED;
	return err ? err : -ECONNREFUSED;
}

/* blocking I/O on a host connect in progress waits for it, as TCP's does */
static int tsi_wait_connected(struct sock *sk, int nonblock)
{
	struct tsi_sock *tsk = tsi_sk(sk);
	long timeo = sock_sndtimeo(sk, nonblock);
	int err = 0;

	lock_sock(sk);
	if (tsk->conn == TSI_CONNECTING)
		err = tsi_wait(sk, &timeo, tsi_not_connecting);
	if (!err && tsk->conn != TSI_CONNECTED)
		err = tsk->conn == TSI_FAILED ? tsi_connect_result(tsk) :
						 -ENOTCONN;
	release_sock(sk);
	return err;
}

static int tsi_stream_connect(struct socket *sock, struct sockaddr *addr,
			      int len, int flags)
{
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;
	struct socket *inner;
	long timeo;
	int err;

	if (len >= (int)sizeof(sa_family_t) && addr->sa_family == AF_UNSPEC) {
		if (tsk->status == TSI_HOST || !tsk->isocket)
			return -EOPNOTSUPP;
		return tsk->isocket->ops->connect(tsk->isocket, addr, len,
						  flags);
	}
	err = tsi_check_addr(addr, len);
	if (err)
		return err;

	lock_sock(sk);
	switch (tsk->conn) {
	case TSI_CONNECTING:
		if (flags & O_NONBLOCK) {
			err = -EALREADY;
			goto out;
		}
		goto wait;
	case TSI_CONNECTED:
		err = -EISCONN;
		goto out;
	case TSI_FAILED:
		err = tsi_connect_result(tsk);
		goto out;
	}

	inner = tsk->isocket;
	if (tsk->status == TSI_INET || tsi_is_local(sin)) {
		if (!inner) {
			err = -EINVAL;
			goto out;
		}
		/* the inner socket may block: not with the outer locked */
		release_sock(sk);
		err = inner->ops->connect(inner, addr, len, flags);
		lock_sock(sk);
		if (!err || err == -EINPROGRESS || err == -EALREADY ||
		    err == -EISCONN)
			tsk->status = TSI_INET;
		goto out;
	}

	if (!lkl_ops->net_connect) {
		err = -ENETUNREACH;
		goto out;
	}
	/* a local port of the guest's own, for getsockname */
	if (inner && !inet_sk(inner->sk)->inet_num) {
		struct sockaddr_in any = { .sin_family = AF_INET };

		kernel_bind(inner, (struct sockaddr *)&any, sizeof(any));
	}
	tsk->peer = *sin;
	tsk->peer.sin_family = AF_INET;
	memset(tsk->peer.sin_zero, 0, sizeof(tsk->peer.sin_zero));
	tsk->status = TSI_HOST;
	tsk->conn = TSI_CONNECTING;
	sock->state = SS_CONNECTING;
	sk->sk_err = 0;
	sock_hold(sk);
	err = lkl_ops->net_connect(sk, task_tgid_vnr(current),
				   (struct sockaddr *)&tsk->peer,
				   sizeof(tsk->peer));
	if (err) {
		sock_put(sk);
		tsk->status = TSI_HYBRID;
		tsk->conn = TSI_NONE;
		sock->state = SS_UNCONNECTED;
		goto out;
	}
	if (flags & O_NONBLOCK) {
		err = -EINPROGRESS;
		goto out;
	}
wait:
	timeo = sock_sndtimeo(sk, flags & O_NONBLOCK);
	err = tsi_wait(sk, &timeo, tsi_not_connecting);
	if (err == -EAGAIN)
		/* SO_SNDTIMEO ran out: still connecting, as TCP's */
		err = -EINPROGRESS;
	else if (!err)
		err = tsi_connect_result(tsk);
out:
	release_sock(sk);
	return err;
}

static int tsi_accept(struct socket *sock, struct socket *newsock,
		      struct proto_accept_arg *arg)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk = tsi_sk(sk), *ntsk;
	struct tsi_pending *p = NULL;
	struct socket *nsock = NULL;
	struct sock *nsk;
	long timeo = sock_rcvtimeo(sk, arg->flags & O_NONBLOCK);
	int err = 0;

	if (!tsk->isocket || tsk->isocket->sk->sk_state != TCP_LISTEN)
		return -EINVAL;
	/* either queue: the inner socket's (its wakeups wake ours) or ours */
	add_wait_queue(sk_sleep(sk), &wait);
	for (;;) {
		lock_sock(sk);
		p = list_first_entry_or_null(&tsk->pending, struct tsi_pending,
					     list);
		if (p) {
			list_del(&p->list);
			tsk->npending--;
		}
		release_sock(sk);
		if (p)
			break;
		err = kernel_accept(tsk->isocket, &nsock, O_NONBLOCK);
		if (err != -EAGAIN)
			break;
		if (!timeo)
			break;
		if (signal_pending(current)) {
			err = sock_intr_errno(timeo);
			break;
		}
		timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, timeo);
	}
	remove_wait_queue(sk_sleep(sk), &wait);
	if (!p && err)
		return err;

	nsk = tsi_sk_alloc(newsock, SOCK_STREAM);
	if (!nsk) {
		if (p) {
			if (p->hosted) {
				if (lkl_ops->net_fd_close)
					lkl_ops->net_fd_close(p->hostfd);
			} else {
				sock_release(p->transport);
			}
			kfree(p);
		} else {
			sock_release(nsock);
		}
		return -ENOMEM;
	}
	ntsk = tsi_sk(nsk);
	if (p) {
		ntsk->transport = p->transport;
		ntsk->hosted = p->hosted;
		ntsk->hostfd = p->hostfd;
		ntsk->peer = p->peer;
		tsi_getname(sock, (struct sockaddr *)&ntsk->local, 0);
		ntsk->status = TSI_HOST;
		ntsk->conn = TSI_CONNECTED;
		if (!p->hosted)
			tsi_hook(ntsk, ntsk->transport, &ntsk->thook);
		kfree(p);
	} else {
		struct sock *csk = nsock->sk;

		/* TCP's clone of the listener's: its callbacks given back */
		write_lock_bh(&csk->sk_callback_lock);
		csk->sk_user_data = NULL;
		csk->sk_data_ready = tsk->ihook.data_ready;
		csk->sk_write_space = tsk->ihook.write_space;
		csk->sk_state_change = tsk->ihook.state_change;
		csk->sk_error_report = tsk->ihook.error_report;
		write_unlock_bh(&csk->sk_callback_lock);
		ntsk->isocket = nsock;
		ntsk->status = TSI_INET;
		tsi_hook(ntsk, nsock, &ntsk->ihook);
	}
	newsock->state = SS_CONNECTED;
	return 0;
}

static __poll_t tsi_stream_poll(struct file *file, struct socket *sock,
				poll_table *wait)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	struct socket *inner = READ_ONCE(tsk->isocket);
	__poll_t mask = 0;

	sock_poll_wait(file, sock, wait);
	if (READ_ONCE(tsk->status) == TSI_HOST) {
		switch (READ_ONCE(tsk->conn)) {
		case TSI_CONNECTING:
			return 0;
		case TSI_CONNECTED:
			if (tsk->hosted)
				return lkl_hostfd_poll_fd(file, tsk->hostfd,
							  EPOLLIN | EPOLLOUT,
							  wait);
			return tsk->transport->ops->poll(file, tsk->transport,
							 NULL);
		default:
			/* as TCP's after a refused connect */
			mask = EPOLLOUT | EPOLLWRNORM | EPOLLHUP | EPOLLIN |
			       EPOLLRDNORM | EPOLLRDHUP;
			if (READ_ONCE(tsk->sk.sk_err))
				mask |= EPOLLERR;
			return mask;
		}
	}
	if (inner)
		mask = inner->ops->poll(file, inner, NULL);
	if (!list_empty(&tsk->pending))
		mask |= EPOLLIN | EPOLLRDNORM;
	return mask;
}

static int tsi_listen(struct socket *sock, int backlog)
{
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct sockaddr_in addr;
	int err;

	if (!tsk->isocket || tsk->status == TSI_HOST)
		return -EINVAL;
	/* local first: the guest's own clients connect to the inner socket */
	err = tsk->isocket->ops->listen(tsk->isocket, backlog);
	if (err)
		return err;
	lock_sock(sk);
	tsk->status = TSI_INET;
	tsk->backlog = backlog;
	sk->sk_state = TCP_LISTEN;
	if (!tsk->host_listening && lkl_ops->net_listen &&
	    tsk->isocket->ops->getname(tsk->isocket, (struct sockaddr *)&addr,
				       0) >= 0 &&
	    !ipv4_is_loopback(addr.sin_addr.s_addr)) {
		sock_hold(sk);
		if (lkl_ops->net_listen(sk, task_tgid_vnr(current),
					(struct sockaddr *)&addr,
					sizeof(addr)) == 0)
			tsk->host_listening = true;
		else
			/* the host doesn't take it: the guest's alone */
			sock_put(sk);
	}
	release_sock(sk);
	return 0;
}

static int tsi_shutdown(struct socket *sock, int how)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);

	if (tsk->status == TSI_HOST) {
		if (tsk->hosted) {
			if (how < SHUT_RD || how > SHUT_RDWR)
				return -EINVAL;
			if (lkl_ops->net_fd_shutdown)
				lkl_ops->net_fd_shutdown(tsk->hostfd, how);
			return 0;
		}
		if (!tsk->transport)
			return sock->type == SOCK_DGRAM ? 0 : -ENOTCONN;
		return tsk->transport->ops->shutdown(tsk->transport, how);
	}
	if (!tsk->isocket)
		return -ENOTCONN;
	return tsk->isocket->ops->shutdown(tsk->isocket, how);
}

/*
 * A host-fd-backed connection's send or receive: the host's write or read
 * of its fd, a segment of msg at a time (the program's own buffers: no
 * MMU, so a user address is the host's), as a host_fd file's. A receive is
 * one read; a send all of msg, unless nonblocking or cut short. Neither
 * MSG_PEEK nor out-of-band data: the host fd has no such call.
 */
static int tsi_hostfd_msg(struct socket *sock, struct msghdr *msg, int flags,
			  int write)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	int nonblock = (flags & MSG_DONTWAIT) ||
		       (sock->file && (READ_ONCE(sock->file->f_flags) & O_NONBLOCK));
	long done = 0, ret = 0;

	if (flags & (MSG_PEEK | MSG_OOB))
		return -EOPNOTSUPP;
	if (!user_backed_iter(&msg->msg_iter))
		return -EINVAL;
	while (iov_iter_count(&msg->msg_iter)) {
		void __user *base = iter_iov_addr(&msg->msg_iter);
		size_t seg = iter_iov_len(&msg->msg_iter);

		ret = lkl_hostfd_io(tsk->hostfd, base, seg, write, nonblock);
		if (ret <= 0)
			break;
		iov_iter_advance(&msg->msg_iter, ret);
		done += ret;
		if (!write || (size_t)ret < seg)
			break;
	}
	if (write && ret == -EPIPE && !done && !(flags & MSG_NOSIGNAL))
		send_sig(SIGPIPE, current, 0);
	return done ? done : ret;
}

static int tsi_stream_sendmsg(struct socket *sock, struct msghdr *msg,
			      size_t len)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	int err;

	if (tsk->status != TSI_HOST)
		return tsk->isocket ?
			tsk->isocket->ops->sendmsg(tsk->isocket, msg, len) :
			-ENOTCONN;
	err = tsi_wait_connected(sock->sk, msg->msg_flags & MSG_DONTWAIT);
	if (err)
		return err == -EAGAIN || err == -ENOTCONN ? err : -EPIPE;
	/* a connected TCP socket ignores the address; AF_UNIX's would not */
	msg->msg_name = NULL;
	msg->msg_namelen = 0;
	if (tsk->hosted)
		return tsi_hostfd_msg(sock, msg, msg->msg_flags, 1);
	return tsk->transport->ops->sendmsg(tsk->transport, msg, len);
}

static int tsi_stream_recvmsg(struct socket *sock, struct msghdr *msg,
			      size_t len, int flags)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	int err;

	if (tsk->status != TSI_HOST)
		return tsk->isocket ?
			tsk->isocket->ops->recvmsg(tsk->isocket, msg, len,
						   flags) :
			-ENOTCONN;
	err = tsi_wait_connected(sock->sk, flags & MSG_DONTWAIT);
	if (err)
		return err;
	/* TCP's recvfrom gives no address (AF_UNIX's would give its own) */
	msg->msg_name = NULL;
	if (tsk->hosted)
		return tsi_hostfd_msg(sock, msg, flags, 0);
	return tsk->transport->ops->recvmsg(tsk->transport, msg, len, flags);
}

/* ---- datagrams ---- */

/* the sender of a datagram the host queued (lkl_net_reply) */
struct tsi_dgram_cb {
	struct sockaddr_in from;
};

#define TSI_CB(skb) ((struct tsi_dgram_cb *)(skb)->cb)

static int tsi_dgram_connect(struct socket *sock, struct sockaddr *addr,
			     int len, int flags)
{
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct sockaddr_in *sin = (struct sockaddr_in *)addr;
	int err;

	if (!tsk->isocket)
		return -EINVAL;
	if (len >= (int)sizeof(sa_family_t) && addr->sa_family == AF_UNSPEC) {
		lock_sock(sk);
		tsk->status = TSI_HYBRID;
		release_sock(sk);
		return tsk->isocket->ops->connect(tsk->isocket, addr, len,
						  flags);
	}
	err = tsi_check_addr(addr, len);
	if (err)
		return err;
	if (tsi_is_local(sin)) {
		err = tsk->isocket->ops->connect(tsk->isocket, addr, len,
						 flags);
		if (!err) {
			lock_sock(sk);
			tsk->status = TSI_INET;
			release_sock(sk);
		}
		return err;
	}
	/* the host's peer: only recorded, as UDP's connect only records */
	lock_sock(sk);
	tsk->peer = *sin;
	memset(tsk->peer.sin_zero, 0, sizeof(tsk->peer.sin_zero));
	tsk->status = TSI_HOST;
	release_sock(sk);
	return 0;
}

static int tsi_dgram_sendmsg(struct socket *sock, struct msghdr *msg,
			     size_t len)
{
	struct sock *sk = sock->sk;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct sockaddr_in to;
	void *buf;
	int err;

	if (msg->msg_name) {
		err = tsi_check_addr(msg->msg_name, msg->msg_namelen);
		if (err)
			return err;
		to = *(struct sockaddr_in *)msg->msg_name;
	} else if (tsk->status == TSI_HOST) {
		to = tsk->peer;
	} else {
		return tsk->isocket ?
			tsk->isocket->ops->sendmsg(tsk->isocket, msg, len) :
			-EDESTADDRREQ;
	}
	if (tsi_is_local(&to)) {
		if (!tsk->isocket)
			return -EINVAL;
		return tsk->isocket->ops->sendmsg(tsk->isocket, msg, len);
	}

	if (!lkl_ops->net_sendto)
		return -ENETUNREACH;
	if (len > 65507)
		return -EMSGSIZE;
	buf = kmalloc(len ? len : 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	if (!copy_from_iter_full(buf, len, &msg->msg_iter)) {
		kfree(buf);
		return -EFAULT;
	}
	to.sin_family = AF_INET;
	memset(to.sin_zero, 0, sizeof(to.sin_zero));
	lock_sock(sk);
	tsk->dgram_refs++;
	sock_hold(sk);
	err = lkl_ops->net_sendto(sk, task_tgid_vnr(current),
				  (struct sockaddr *)&to, sizeof(to), buf, len);
	if (err) {
		tsk->dgram_refs--;
		sock_put(sk);
	}
	release_sock(sk);
	kfree(buf);
	return err ? err : len;
}

/* one datagram, the host's or the inner socket's, without waiting */
static int tsi_dgram_try(struct sock *sk, struct msghdr *msg, size_t len,
			 int flags)
{
	struct tsi_sock *tsk = tsi_sk(sk);
	struct sk_buff *skb;
	size_t copied;
	int err;

	err = sock_error(sk);
	if (err)
		return err;
	skb = skb_recv_datagram(sk, flags | MSG_DONTWAIT, &err);
	if (skb) {
		copied = min_t(size_t, len, skb->len);
		if (copied < skb->len)
			msg->msg_flags |= MSG_TRUNC;
		err = skb_copy_datagram_msg(skb, 0, msg, copied);
		if (!err && msg->msg_name) {
			memcpy(msg->msg_name, &TSI_CB(skb)->from,
			       sizeof(struct sockaddr_in));
			msg->msg_namelen = sizeof(struct sockaddr_in);
		}
		if (!err)
			err = flags & MSG_TRUNC ? skb->len : copied;
		skb_free_datagram(sk, skb);
		return err;
	}
	if (!tsk->isocket)
		return -EAGAIN;
	return tsk->isocket->ops->recvmsg(tsk->isocket, msg, len,
					  flags | MSG_DONTWAIT);
}

static int tsi_dgram_recvmsg(struct socket *sock, struct msghdr *msg,
			     size_t len, int flags)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct sock *sk = sock->sk;
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	int err;

	add_wait_queue(sk_sleep(sk), &wait);
	for (;;) {
		err = tsi_dgram_try(sk, msg, len, flags);
		if (err != -EAGAIN || !timeo)
			break;
		if (signal_pending(current)) {
			err = sock_intr_errno(timeo);
			break;
		}
		timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, timeo);
	}
	remove_wait_queue(sk_sleep(sk), &wait);
	return err;
}

static __poll_t tsi_dgram_poll(struct file *file, struct socket *sock,
			       poll_table *wait)
{
	struct tsi_sock *tsk = tsi_sk(sock->sk);
	struct socket *inner = READ_ONCE(tsk->isocket);
	__poll_t mask = EPOLLOUT | EPOLLWRNORM;

	sock_poll_wait(file, sock, wait);
	if (inner)
		mask |= inner->ops->poll(file, inner, NULL);
	if (!skb_queue_empty_lockless(&sock->sk->sk_receive_queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(sock->sk->sk_err))
		mask |= EPOLLERR;
	return mask;
}

static const struct proto_ops tsi_stream_ops = {
	.family = PF_INET,
	.owner = THIS_MODULE,
	.release = tsi_release,
	.bind = tsi_bind,
	.connect = tsi_stream_connect,
	.socketpair = sock_no_socketpair,
	.accept = tsi_accept,
	.getname = tsi_getname,
	.poll = tsi_stream_poll,
	.ioctl = tsi_ioctl,
	.listen = tsi_listen,
	.shutdown = tsi_shutdown,
	.setsockopt = tsi_setsockopt,
	.getsockopt = tsi_getsockopt,
	.sendmsg = tsi_stream_sendmsg,
	.recvmsg = tsi_stream_recvmsg,
	.mmap = sock_no_mmap,
};

static const struct proto_ops tsi_dgram_ops = {
	.family = PF_INET,
	.owner = THIS_MODULE,
	.release = tsi_release,
	.bind = tsi_bind,
	.connect = tsi_dgram_connect,
	.socketpair = sock_no_socketpair,
	.accept = sock_no_accept,
	.getname = tsi_getname,
	.poll = tsi_dgram_poll,
	.ioctl = tsi_ioctl,
	.listen = sock_no_listen,
	.shutdown = tsi_shutdown,
	.setsockopt = tsi_setsockopt,
	.getsockopt = tsi_getsockopt,
	.sendmsg = tsi_dgram_sendmsg,
	.recvmsg = tsi_dgram_recvmsg,
	.mmap = sock_no_mmap,
};

static bool tsi_socket_of(struct socket *sock)
{
	return sock->ops == &tsi_stream_ops || sock->ops == &tsi_dgram_ops;
}

/* ---- the program's syscalls ---- */

/* socket(AF_INET, SOCK_STREAM or SOCK_DGRAM, 0, TCP or UDP) */
static long tsi_sys_socket(long family, long type, long protocol)
{
	int flags = type & ~SOCK_TYPE_MASK;
	struct socket *sock, *inner;
	struct file *file;
	int fd, err;

	type &= SOCK_TYPE_MASK;
	if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK))
		return -EINVAL;
	err = sock_create_kern(&init_net, AF_INET, type, protocol, &inner);
	if (err)
		return err;
	sock = sock_alloc();
	if (!sock) {
		sock_release(inner);
		return -ENFILE;
	}
	sock->type = type;
	if (!tsi_sk_alloc(sock, type)) {
		sock_release(inner);
		sock_release(sock);
		return -ENOMEM;
	}
	tsi_sk(sock->sk)->isocket = inner;
	tsi_hook(tsi_sk(sock->sk), inner, &tsi_sk(sock->sk)->ihook);

	fd = get_unused_fd_flags(flags & O_CLOEXEC);
	if (fd < 0) {
		sock_release(sock);
		return fd;
	}
	/* (on failure it releases the socket) */
	file = sock_alloc_file(sock, flags, NULL);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		return PTR_ERR(file);
	}
	fd_install(fd, file);
	return fd;
}

/*
 * getsockopt(SOL_SOCKET) on an impersonated socket: SO_DOMAIN is AF_INET;
 * while the inner socket serves (loopback, a listen), its answers (SO_ERROR
 * of its connect, SO_ACCEPTCONN); otherwise the outer socket's (SO_ERROR of
 * a host connect).
 */
static long tsi_sys_getsockopt(long fd, long level, long optname, long optval,
			       long optlen)
{
	char __user *uval = (char __user *)optval;
	int __user *ulen = (int __user *)optlen;
	struct socket *sock;
	struct tsi_sock *tsk;
	int err, len, v = AF_INET;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return err;
	if (!tsi_socket_of(sock)) {
		sockfd_put(sock);
		return __sys_getsockopt(fd, level, optname, uval, ulen);
	}
	tsk = tsi_sk(sock->sk);
	if (optname == SO_DOMAIN) {
		err = -EFAULT;
		if (get_user(len, ulen))
			goto out;
		err = -EINVAL;
		if (len < 0)
			goto out;
		len = min_t(int, len, sizeof(v));
		err = -EFAULT;
		if (copy_to_user(uval, &v, len) || put_user(len, ulen))
			goto out;
		err = 0;
	} else if (tsk->status == TSI_INET && tsk->isocket) {
		err = sk_getsockopt(tsk->isocket->sk, SOL_SOCKET, optname,
				    USER_SOCKPTR(uval), USER_SOCKPTR(ulen));
	} else {
		err = sk_getsockopt(sock->sk, SOL_SOCKET, optname,
				    USER_SOCKPTR(uval), USER_SOCKPTR(ulen));
	}
out:
	sockfd_put(sock);
	return err;
}

void *tsi_handler(long no, long *params, void *handler)
{
	long type = params[1] & SOCK_TYPE_MASK, protocol = params[2];

	if (!tsi_task)
		return handler;
	if (no == __NR_socket && params[0] == AF_INET &&
	    ((type == SOCK_STREAM && (!protocol || protocol == IPPROTO_TCP)) ||
	     (type == SOCK_DGRAM && (!protocol || protocol == IPPROTO_UDP))))
		return tsi_sys_socket;
	if (no == __NR_getsockopt && params[1] == SOL_SOCKET)
		return tsi_sys_getsockopt;
	return handler;
}

/* ---- the host's calls (asm/host_ops.h) ---- */

/* the CPU, and the tsi task as current */
static int tsi_enter(void)
{
	int ret;

	if (!tsi_task)
		return -ENODEV;
	ret = lkl_cpu_get();
	if (ret < 0)
		return ret;
	switch_to_host_task(tsi_task);
	return 0;
}

/*
 * The tsi task parks and the CPU goes to the next task (the idle host task
 * if none): left current, it would keep the tick running while the kernel
 * idles (hostfd.c's I/O leaves the same way).
 */
static void tsi_leave(void)
{
	thread_sched_jb();
}

void *lkl_net_done(void *sock, int err)
{
	struct sock *sk = sock;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct socket *guest = NULL, *end = NULL;

	if (tsi_enter())
		return NULL;
	lock_sock(sk);
	if (sk->sk_type == SOCK_DGRAM) {
		tsk->dgram_refs--;
		if (err && !tsk->released) {
			sk->sk_err = -err;
			sk_error_report(sk);
		}
	} else if (tsk->conn == TSI_CONNECTING) {
		if (!err && !tsk->released)
			err = tsi_socketpair(&guest, &end);
		if (tsk->released) {
			/* the program gave up: nothing to connect */
			tsk->conn = TSI_NONE;
		} else if (err) {
			tsk->conn = TSI_FAILED;
			sk->sk_err = -err;
		} else {
			tsk->transport = guest;
			guest->sk->sk_rcvtimeo = sk->sk_rcvtimeo;
			guest->sk->sk_sndtimeo = sk->sk_sndtimeo;
			tsi_hook(tsk, guest, &tsk->thook);
			tsk->conn = TSI_CONNECTED;
			sk->sk_socket->state = SS_CONNECTED;
			sk->sk_state = TCP_ESTABLISHED;
		}
		tsi_wake(tsk);
	} else if (tsk->host_listening) {
		tsk->host_listening = false;
	}
	release_sock(sk);
	sock_put(sk);
	tsi_leave();
	return end;
}

int lkl_net_done_fd(void *sock, long fd)
{
	struct sock *sk = sock;
	struct tsi_sock *tsk = tsi_sk(sk);
	int err = 0;

	if (fd < 0 || sk->sk_type != SOCK_STREAM)
		return -EINVAL;
	err = tsi_enter();
	if (err)
		return err;
	lock_sock(sk);
	if (tsk->conn != TSI_CONNECTING) {
		err = -EINVAL;
	} else if (tsk->released) {
		/* the program gave up: the host keeps fd */
		tsk->conn = TSI_NONE;
		err = -ECONNABORTED;
	} else {
		tsk->hosted = true;
		tsk->hostfd = fd;
		tsk->conn = TSI_CONNECTED;
		sk->sk_socket->state = SS_CONNECTED;
		sk->sk_state = TCP_ESTABLISHED;
	}
	tsi_wake(tsk);
	release_sock(sk);
	sock_put(sk);
	tsi_leave();
	return err;
}

int lkl_net_accept_fd(void *sock, const struct sockaddr *peer, int len,
		      long fd)
{
	struct sock *sk = sock;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct tsi_pending *p;
	int err;

	if (fd < 0 || len < (int)sizeof(struct sockaddr_in))
		return -EINVAL;
	err = tsi_enter();
	if (err)
		return err;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	err = -ENOMEM;
	if (!p)
		goto out;
	lock_sock(sk);
	if (tsk->released || !tsk->host_listening ||
	    tsk->npending >= max(tsk->backlog, 1)) {
		release_sock(sk);
		kfree(p);
		err = -ECONNREFUSED;
		goto out;
	}
	p->hosted = true;
	p->hostfd = fd;
	memcpy(&p->peer, peer, sizeof(p->peer));
	p->peer.sin_family = AF_INET;
	list_add_tail(&p->list, &tsk->pending);
	tsk->npending++;
	tsi_wake(tsk);
	release_sock(sk);
	err = 0;
out:
	tsi_leave();
	return err;
}

void *lkl_net_accept(void *sock, const struct sockaddr *peer, int len)
{
	struct sock *sk = sock;
	struct tsi_sock *tsk = tsi_sk(sk);
	struct tsi_pending *p;
	struct socket *end = NULL;

	if (len < (int)sizeof(struct sockaddr_in) || tsi_enter())
		return NULL;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		goto out;
	lock_sock(sk);
	if (tsk->released || !tsk->host_listening ||
	    tsk->npending >= max(tsk->backlog, 1) ||
	    tsi_socketpair(&p->transport, &end)) {
		release_sock(sk);
		kfree(p);
		end = NULL;
		goto out;
	}
	memcpy(&p->peer, peer, sizeof(p->peer));
	p->peer.sin_family = AF_INET;
	list_add_tail(&p->list, &tsk->pending);
	tsk->npending++;
	tsi_wake(tsk);
	release_sock(sk);
out:
	tsi_leave();
	return end;
}

long lkl_net_reply(void *sock, const struct sockaddr *from, int len,
		   const void *buf, unsigned long size)
{
	struct sock *sk = sock;
	struct sk_buff *skb;
	long err;

	if (len < (int)sizeof(struct sockaddr_in) || size > 65507)
		return -EINVAL;
	err = tsi_enter();
	if (err)
		return err;
	err = -EBADF;
	if (tsi_sk(sk)->released)
		goto out;
	err = -ENOMEM;
	skb = alloc_skb(size, GFP_KERNEL);
	if (!skb)
		goto out;
	skb_put_data(skb, buf, size);
	memcpy(&TSI_CB(skb)->from, from, sizeof(struct sockaddr_in));
	TSI_CB(skb)->from.sin_family = AF_INET;
	err = sock_queue_rcv_skb(sk, skb);
	if (err)
		kfree_skb(skb);
out:
	tsi_leave();
	return err;
}

long lkl_net_recv(void *end, void *buf, unsigned long len)
{
	struct kvec iov = { .iov_base = buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT };
	long ret = tsi_enter();

	if (ret)
		return ret;
	ret = kernel_recvmsg(end, &msg, &iov, 1, len, MSG_DONTWAIT);
	/*
	 * EOF: the program shut its writing down, or closed the socket
	 * altogether (AF_UNIX shuts both of this end's directions down then):
	 * told apart, so the host knows nothing will be read either.
	 */
	if (!ret && len &&
	    READ_ONCE(((struct socket *)end)->sk->sk_shutdown) & SEND_SHUTDOWN)
		ret = -EPIPE;
	tsi_leave();
	return ret;
}

long lkl_net_send(void *end, const void *buf, unsigned long len)
{
	struct kvec iov = { .iov_base = (void *)buf, .iov_len = len };
	struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
	long ret = tsi_enter();

	if (ret)
		return ret;
	ret = kernel_sendmsg(end, &msg, &iov, 1, len);
	tsi_leave();
	return ret;
}

long lkl_net_shutdown(void *end, int how)
{
	long ret = tsi_enter();

	if (ret)
		return ret;
	ret = kernel_sock_shutdown(end, how);
	tsi_leave();
	return ret;
}

void lkl_net_close(void *end, int reset)
{
	struct socket *b = end;
	struct sock *sk;

	if (tsi_enter())
		return;
	sk = b->sk;
	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = NULL;
	write_unlock_bh(&sk->sk_callback_lock);
	/*
	 * A reset: the guest's end sees ECONNRESET, as AF_UNIX gives a peer
	 * closed with data unread, whatever this end had read.
	 */
	if (reset) {
		struct sock *peer = unix_peer_get(sk);

		if (peer) {
			WRITE_ONCE(peer->sk_err, ECONNRESET);
			sock_put(peer);
		}
	}
	sock_release(b);
	tsi_leave();
}

/*
 * The host's own syscall (asm/host_ops.h), as the tsi task: a kernel
 * thread, so no thread of init's process to keep it from ending, or to
 * hold its files open after its exit (capnix: a provider's library read
 * /proc/self/maps, the executor got a host task of init's, and the sandbox
 * never ended). Not the calls that would end the tsi task or make it a
 * host task of a process's.
 */
long lkl_net_syscall(long no, long *params)
{
	long ret;

	switch (no) {
	case __NR_exit:
	case __NR_exit_group:
	case __NR_new_thread:
	case __NR_attach_thread:
	case __NR_new_thread_group_leader:
	case __NR_new_exec:
		return -EINVAL;
	}
	ret = tsi_enter();
	if (ret)
		return ret;
	ret = run_syscall_current(no, params);
	tsi_leave();
	return ret;
}

/* ---- the tsi task ---- */

static DECLARE_COMPLETION(tsi_parked);

/* as host0_park: the task parks, its first host thread ends */
static int tsi_park(void *unused)
{
	set_thread_flag(TIF_HOST_THREAD);
	complete(&tsi_parked);
	thread_sched_jb();
	lkl_ops->thread_exit();
	return 0;
}

static int __init tsi_init(void)
{
	struct task_struct *t;

	t = kthread_run(tsi_park, NULL, "tsi");
	if (IS_ERR(t))
		return PTR_ERR(t);
	wait_for_completion(&tsi_parked);
	tsi_task = t;
	return 0;
}
device_initcall(tsi_init);
