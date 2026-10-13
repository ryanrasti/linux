// SPDX-License-Identifier: GPL-2.0
/*
 * A vsock transport to the host through host operations (lkl_ops->vsock_*):
 * virtio-vsock packets without a virtio device. As for vsock_loopback,
 * everything above sending and receiving packets is
 * virtio_transport_common's. The host queues packets for us and calls our
 * notify, which raises an interrupt; a work item pulls them in.
 */
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/virtio_vsock.h>
#include <asm/host_ops.h>
#include <asm/irq.h>

static u32 hostvsock_cid;
static int hostvsock_irq;
static struct workqueue_struct *hostvsock_wq;
static struct work_struct hostvsock_rx_work;
static struct virtio_transport hostvsock_transport;

static u32 hostvsock_get_local_cid(void)
{
	return hostvsock_cid;
}

static int hostvsock_send_pkt(struct sk_buff *skb)
{
	int len = skb->len;
	int err;

	err = skb_linearize(skb);
	if (!err)
		err = lkl_ops->vsock_send(virtio_vsock_hdr(skb), skb->data,
					  skb->len);
	/* the host has copied it: sent, as a device's tx completion says */
	virtio_transport_consume_skb_sent(skb, true);
	return err ? err : len;
}

static int hostvsock_cancel_pkt(struct vsock_sock *vsk)
{
	/* nothing is queued: send_pkt hands each packet over at once */
	return 0;
}

static bool hostvsock_allow(u32 cid, u32 port)
{
	return true;
}

static bool hostvsock_seqpacket_allow(u32 remote_cid)
{
	return true;
}

static bool hostvsock_msgzerocopy_allow(void)
{
	return false;
}

static struct virtio_transport hostvsock_transport = {
	.transport = {
		.module                   = THIS_MODULE,

		.get_local_cid            = hostvsock_get_local_cid,

		.init                     = virtio_transport_do_socket_init,
		.destruct                 = virtio_transport_destruct,
		.release                  = virtio_transport_release,
		.connect                  = virtio_transport_connect,
		.shutdown                 = virtio_transport_shutdown,
		.cancel_pkt               = hostvsock_cancel_pkt,

		.dgram_enqueue            = virtio_transport_dgram_enqueue,
		.dgram_allow              = hostvsock_allow,
		.dgram_get_cid            = virtio_transport_dgram_get_cid,
		.dgram_get_port           = virtio_transport_dgram_get_port,
		.dgram_get_length         = virtio_transport_dgram_get_length,
		.dgram_has_data           = virtio_transport_dgram_has_data,

		.stream_dequeue           = virtio_transport_stream_dequeue,
		.stream_enqueue           = virtio_transport_stream_enqueue,
		.stream_has_data          = virtio_transport_stream_has_data,
		.stream_has_space         = virtio_transport_stream_has_space,
		.stream_rcvhiwat          = virtio_transport_stream_rcvhiwat,
		.stream_is_active         = virtio_transport_stream_is_active,
		.stream_allow             = virtio_transport_stream_allow,

		.seqpacket_dequeue        = virtio_transport_seqpacket_dequeue,
		.seqpacket_enqueue        = virtio_transport_seqpacket_enqueue,
		.seqpacket_allow          = hostvsock_seqpacket_allow,
		.seqpacket_has_data       = virtio_transport_seqpacket_has_data,

		.msgzerocopy_allow        = hostvsock_msgzerocopy_allow,

		.notify_poll_in           = virtio_transport_notify_poll_in,
		.notify_poll_out          = virtio_transport_notify_poll_out,
		.notify_recv_init         = virtio_transport_notify_recv_init,
		.notify_recv_pre_block    = virtio_transport_notify_recv_pre_block,
		.notify_recv_pre_dequeue  = virtio_transport_notify_recv_pre_dequeue,
		.notify_recv_post_dequeue = virtio_transport_notify_recv_post_dequeue,
		.notify_send_init         = virtio_transport_notify_send_init,
		.notify_send_pre_block    = virtio_transport_notify_send_pre_block,
		.notify_send_pre_enqueue  = virtio_transport_notify_send_pre_enqueue,
		.notify_send_post_enqueue = virtio_transport_notify_send_post_enqueue,
		.notify_buffer_size       = virtio_transport_notify_buffer_size,
		.notify_set_rcvlowat      = virtio_transport_notify_set_rcvlowat,

		.unsent_bytes             = virtio_transport_unsent_bytes,

		.read_skb                 = virtio_transport_read_skb,
	},

	.send_pkt = hostvsock_send_pkt,
};

/* the host's queued packets, each into an skb of its size */
static void hostvsock_rx(struct work_struct *work)
{
	unsigned long len;

	while ((len = lkl_ops->vsock_recv(NULL, 0))) {
		struct virtio_vsock_hdr *hdr;
		struct sk_buff *skb;

		skb = virtio_vsock_alloc_skb(len, GFP_KERNEL);
		if (!skb)
			break;	/* still queued: the next notify retries */
		if (lkl_ops->vsock_recv(skb->head, len) != len) {
			kfree_skb(skb);
			continue;
		}
		hdr = virtio_vsock_hdr(skb);
		if (le32_to_cpu(hdr->len) != len - sizeof(*hdr)) {
			kfree_skb(skb);
			continue;
		}
		virtio_vsock_skb_rx_put(skb);
		virtio_transport_recv_pkt(&hostvsock_transport, skb);
	}
}

static irqreturn_t hostvsock_interrupt(int irq, void *dev_id)
{
	queue_work(hostvsock_wq, &hostvsock_rx_work);
	return IRQ_HANDLED;
}

/* from the host, on a thread of its own: packets are queued for us */
static void hostvsock_notify(void)
{
	lkl_trigger_irq(hostvsock_irq);
}

static int __init hostvsock_init(void)
{
	int err;

	if (!lkl_ops->vsock_init)
		return 0;

	hostvsock_wq = alloc_workqueue("hostvsock", 0, 0);
	if (!hostvsock_wq)
		return -ENOMEM;
	INIT_WORK(&hostvsock_rx_work, hostvsock_rx);

	hostvsock_irq = lkl_get_free_irq("hostvsock");
	if (hostvsock_irq < 0) {
		err = hostvsock_irq;
		goto out_wq;
	}
	err = request_irq(hostvsock_irq, hostvsock_interrupt, 0, "hostvsock",
			  NULL);
	if (err)
		goto out_irq;

	hostvsock_cid = lkl_ops->vsock_init(hostvsock_notify);
	if (!hostvsock_cid) {
		err = 0;	/* no vsock on this host */
		goto out_request;
	}

	err = vsock_core_register(&hostvsock_transport.transport,
				  VSOCK_TRANSPORT_F_G2H);
	if (err)
		goto out_request;
	return 0;

out_request:
	free_irq(hostvsock_irq, NULL);
out_irq:
	lkl_put_irq(hostvsock_irq, "hostvsock");
out_wq:
	destroy_workqueue(hostvsock_wq);
	return err;
}
device_initcall(hostvsock_init);
