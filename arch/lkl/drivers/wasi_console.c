// SPDX-License-Identifier: GPL-2.0
/*
 * The console on WASI's stdio: an hvc backend whose output is fd_write to
 * stdout and whose input is fd_read from stdin, when poll_oneoff says it
 * has some (hvc polls: no interrupt). The kernel is linked into the
 * program (a library OS), so it calls WASI's imports itself.
 */
#include <linux/init.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/tty.h>
#include <linux/timekeeping.h>
#include "../../../drivers/tty/hvc/hvc_console.h"

#define WASI(name) \
	__attribute__((import_module("wasi_snapshot_preview1"), import_name(#name)))

struct wasi_iovec {
	const void *buf;
	u32 len;
};

struct wasi_subscription {
	u64 userdata;
	u8 tag;			/* 0: clock, 1: fd_read */
	u8 pad[7];
	union {
		struct {
			u32 id;
			u8 pad[4];
			u64 timeout;
			u64 precision;
			u16 flags;
		} clock;
		struct {
			u32 fd;
		} fd;
	} u;
};

struct wasi_event {
	u64 userdata;
	u16 error;
	u8 type;
	u8 pad[5];
	u64 nbytes;
	u16 flags;
	u8 pad2[6];
};

WASI(fd_write) u16 wasi_fd_write(u32 fd, const struct wasi_iovec *iovs,
				 u32 n, u32 *written);
WASI(fd_read) u16 wasi_fd_read(u32 fd, const struct wasi_iovec *iovs,
			       u32 n, u32 *read);
WASI(fd_fdstat_get) u16 wasi_fd_fdstat_get(u32 fd, void *stat);
WASI(poll_oneoff) u16 wasi_poll_oneoff(const struct wasi_subscription *in,
				       struct wasi_event *out, u32 n, u32 *got);

static ssize_t wasi_put_chars(u32 vtermno, const u8 *buf, size_t count)
{
	struct wasi_iovec iov = { buf, count };
	u32 written;

	if (wasi_fd_write(1, &iov, 1, &written))
		return -EIO;
	return written;
}

/*
 * stdin has bytes (or its end) now: a poll with a zero timeout beside it.
 * node:wasi reports stdin beside no clock shorter than 1 ms, so when the
 * console hasn't polled for a while (idle: hvc's polls back off), a poll
 * that saw nothing is retried with 1 ms. A runtime that refuses the poll
 * (node:wasi again, for a file: stdin hasn't the right to be polled) has
 * it read anyway, and the read blocks the module until input.
 */
static bool wasi_stdin_poll(u64 timeout)
{
	struct wasi_subscription in[2] = {
		{ .userdata = 1, .tag = 1, .u.fd.fd = 0 },
		{ .userdata = 2, .tag = 0, .u.clock = { .id = 1, .timeout = timeout } },
	};
	struct wasi_event out[2];
	u32 got, i;

	if (wasi_poll_oneoff(in, out, 2, &got))
		return true;
	for (i = 0; i < got; i++)
		if (out[i].userdata == 1 && !out[i].error)
			return true;
	return false;
}

static bool wasi_stdin_ready(void)
{
	static u64 last;
	u64 now = ktime_get_ns(), was = last;

	last = now;
	return wasi_stdin_poll(0) ||
	       (now - was > 10 * NSEC_PER_MSEC && wasi_stdin_poll(NSEC_PER_MSEC));
}

/*
 * stdin's end: the tty's end-of-file character (VEOF, ^D), once, which
 * ends a canonical read as a terminal's ^D would; after it, no input
 */
static bool wasi_stdin_ended;

static ssize_t wasi_get_chars(u32 vtermno, u8 *buf, size_t count)
{
	struct wasi_iovec iov = { buf, count };
	u32 read;

	if (wasi_stdin_ended || !wasi_stdin_ready())
		return 0;
	if (wasi_fd_read(0, &iov, 1, &read))
		return -EIO;
	if (!read && count) {
		wasi_stdin_ended = true;
		buf[0] = 4;
		return 1;
	}
	return read;
}

/* fd is a terminal's: WASI's filetype character_device */
static bool wasi_isatty(u32 fd)
{
	struct { u8 filetype; u8 rest[23]; } st;

	return !wasi_fd_fdstat_get(fd, &st) && st.filetype == 2;
}

/*
 * the console's first open: a terminal's settings only where WASI's stdio
 * is a terminal; otherwise no echo of piped input, and output as written
 * (no \r before \n). Before any input is read (hvc polls once opened).
 */
static int wasi_notifier_add(struct hvc_struct *hp, int data)
{
	struct tty_struct *tty = tty_port_tty_get(&hp->port);
	static bool set;
	struct ktermios t;

	if (!tty || set)
		goto out;
	set = true;
	t = tty->termios;
	if (!wasi_isatty(0))
		t.c_lflag &= ~(ECHO | ECHONL);
	if (!wasi_isatty(1))
		t.c_oflag &= ~OPOST;
	tty_set_termios(tty, &t);
out:
	tty_kref_put(tty);
	return 0;
}

static const struct hv_ops wasi_hv_ops = {
	.get_chars = wasi_get_chars,
	.put_chars = wasi_put_chars,
	.notifier_add = wasi_notifier_add,
};

static int __init wasi_console_init(void)
{
	hvc_instantiate(0, 0, &wasi_hv_ops);
	return 0;
}
console_initcall(wasi_console_init);

static int __init wasi_console_dev_init(void)
{
	return PTR_ERR_OR_ZERO(hvc_alloc(0, 0, &wasi_hv_ops, 256));
}
device_initcall(wasi_console_dev_init);
