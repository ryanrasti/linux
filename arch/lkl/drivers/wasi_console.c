// SPDX-License-Identifier: GPL-2.0
/*
 * The console on WASI's stdio (wasi:cli's streams): an hvc backend whose
 * output is written to stdout and whose input is read from stdin, neither
 * waiting. The kernel is linked into the program (a library OS), so it
 * calls WASI's imports itself. What it waits for (stdin's bytes, stdout's
 * room) is a wasi:io pollable, which the host layer's scheduler waits on
 * with its own (the poll_watch host operation): a ready one raises the
 * console's interrupt, which kicks hvc's thread to poll it again. So hvc
 * polls only when told (the console is notifier driven), never on a
 * timer.
 */
#include <linux/init.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/tty.h>
#include <asm/host_ops.h>
#include <asm/wasi.h>
#include "../../../drivers/tty/hvc/hvc_console.h"

/* the streams, each got once (0: not yet) */
static wasi_io_streams_own_input_stream_t wasi_stdin;
static wasi_io_streams_own_output_stream_t wasi_stdout;

static wasi_io_streams_borrow_input_stream_t wasi_stdin_get(void)
{
	if (!wasi_stdin.__handle)
		wasi_stdin = wasi_cli_stdin_get_stdin();
	return wasi_io_streams_borrow_input_stream(wasi_stdin);
}

static wasi_io_streams_borrow_output_stream_t wasi_stdout_get(void)
{
	if (!wasi_stdout.__handle)
		wasi_stdout = wasi_cli_stdout_get_stdout();
	return wasi_io_streams_borrow_output_stream(wasi_stdout);
}

/*
 * what the host waits on for the console (poll_watch): stdin's bytes and
 * stdout's room, each set while hvc has found none, 0 once ready
 */
enum { W_IN, W_OUT, WATCHES };
static unsigned int watched[WATCHES];
static wasi_io_poll_own_pollable_t pollable[WATCHES];	/* each made once it's first waited on */
static int wasi_console_irq = -1;

static void wasi_console_watch(int k)
{
	if (wasi_console_irq < 0)
		return;
	if (!pollable[k].__handle)
		pollable[k] = k == W_IN ?
			wasi_io_streams_method_input_stream_subscribe(wasi_stdin_get()) :
			wasi_io_streams_method_output_stream_subscribe(wasi_stdout_get());
	watched[k] = pollable[k].__handle;
}

static ssize_t wasi_put_chars(u32 vtermno, const u8 *buf, size_t count)
{
	wasi_io_streams_borrow_output_stream_t out = wasi_stdout_get();
	wasi_io_streams_stream_error_t err;
	wasi_list_u8_t list;
	u64 room;

	if (!wasi_io_streams_method_output_stream_check_write(out, &room, &err)) {
		wasi_stream_failed(&err);
		return -EIO;
	}
	if (!room) {
		/* (hvc keeps it, and writes it again once there's room) */
		wasi_console_watch(W_OUT);
		return 0;
	}
	count = min_t(u64, count, room);
	list = (wasi_list_u8_t){ (u8 *)buf, count };
	if (!wasi_io_streams_method_output_stream_write(out, &list, &err) ||
	    !wasi_io_streams_method_output_stream_flush(out, &err)) {
		wasi_stream_failed(&err);
		return -EIO;
	}
	return count;
}

/*
 * stdin's end: the tty's end-of-file character (VEOF, ^D), once, which
 * ends a canonical read as a terminal's ^D would; after it, no input
 */
static bool wasi_stdin_ended;

static ssize_t wasi_get_chars(u32 vtermno, u8 *buf, size_t count)
{
	wasi_io_streams_stream_error_t err;
	wasi_list_u8_t r;

	if (wasi_stdin_ended)
		return 0;
	if (!wasi_io_streams_method_input_stream_read(wasi_stdin_get(), count, &r, &err)) {
		if (err.tag != WASI_IO_STREAMS_STREAM_ERROR_CLOSED) {
			wasi_stream_failed(&err);
			return -EIO;
		}
		wasi_stdin_ended = true;
		if (!count)
			return 0;
		buf[0] = 4;
		return 1;
	}
	/* (no more than asked for, whatever the host gave) */
	count = min_t(size_t, r.len, count);
	memcpy(buf, r.ptr, count);
	wasi_list_u8_free(&r);
	/* none yet: polled again once there is */
	if (!count)
		wasi_console_watch(W_IN);
	return count;
}

/* stdin is a terminal's: WASI's terminal-stdin */
static bool wasi_stdin_isatty(void)
{
	wasi_cli_terminal_input_own_terminal_input_t t;

	if (!wasi_cli_terminal_stdin_get_terminal_stdin(&t))
		return false;
	wasi_cli_terminal_input_terminal_input_drop_own(t);
	return true;
}

/* ...and stdout: terminal-stdout */
static bool wasi_stdout_isatty(void)
{
	wasi_cli_terminal_output_own_terminal_output_t t;

	if (!wasi_cli_terminal_stdout_get_terminal_stdout(&t))
		return false;
	wasi_cli_terminal_output_terminal_output_drop_own(t);
	return true;
}

/*
 * the console's open: hvc polls it only when kicked (its interrupt), once
 * the host waits on its pollables; and at the first, a terminal's
 * settings only where WASI's stdio is a terminal: otherwise no echo of
 * piped input, and output as written (no \r before \n). Before any input
 * is read (hvc polls once opened).
 */
static int wasi_notifier_add(struct hvc_struct *hp, int data)
{
	struct tty_struct *tty = tty_port_tty_get(&hp->port);
	static bool set;
	struct ktermios t;

	hp->irq_requested = wasi_console_irq >= 0;
	if (!tty || set)
		goto out;
	set = true;
	t = tty->termios;
	if (!wasi_stdin_isatty())
		t.c_lflag &= ~(ECHO | ECHONL);
	if (!wasi_stdout_isatty())
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

void lkl_wasi_release(void)
{
	int k;

	for (k = 0; k < WATCHES; k++) {
		if (pollable[k].__handle)
			wasi_io_poll_pollable_drop_own(pollable[k]);
		pollable[k].__handle = 0;
		watched[k] = 0;
	}
	if (wasi_stdin.__handle)
		wasi_io_streams_input_stream_drop_own(wasi_stdin);
	if (wasi_stdout.__handle)
		wasi_io_streams_output_stream_drop_own(wasi_stdout);
	wasi_stdin.__handle = wasi_stdout.__handle = 0;
	if (IS_ENABLED(CONFIG_LKL_WASI_SOCK))
		wasi_sock_network_drop();
}

static irqreturn_t wasi_console_interrupt(int irq, void *dev_id)
{
	hvc_kick();
	return IRQ_HANDLED;
}

/* (the host's, on a thread of its own: a pollable is ready) */
static void wasi_console_notify(void)
{
	lkl_trigger_irq(wasi_console_irq);
}

static int __init wasi_console_init(void)
{
	hvc_instantiate(0, 0, &wasi_hv_ops);
	return 0;
}
console_initcall(wasi_console_init);

static int __init wasi_console_dev_init(void)
{
	int irq;

	/* (without the host's waits on its pollables: polled on hvc's timer) */
	if (lkl_ops->poll_watch) {
		irq = lkl_get_free_irq("wasi_console");
		if (irq >= 0 && !request_irq(irq, wasi_console_interrupt, 0,
					     "wasi_console", NULL)) {
			wasi_console_irq = irq;
			lkl_ops->poll_watch(watched, WATCHES, wasi_console_notify);
		}
	}
	return PTR_ERR_OR_ZERO(hvc_alloc(0, 0, &wasi_hv_ops, 256));
}
device_initcall(wasi_console_dev_init);
