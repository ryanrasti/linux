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

#define CLI(iface, name) WASI_IMPORT("wasi:cli/" iface, name)

CLI("stdin", "get-stdin") s32 __wasm_import_wasi_cli_stdin_get_stdin(void);
CLI("stdout", "get-stdout") s32 __wasm_import_wasi_cli_stdout_get_stdout(void);
CLI("terminal-stdin", "get-terminal-stdin")
void __wasm_import_wasi_cli_terminal_stdin_get_terminal_stdin(void *);
CLI("terminal-stdout", "get-terminal-stdout")
void __wasm_import_wasi_cli_terminal_stdout_get_terminal_stdout(void *);
CLI("terminal-input", "[resource-drop]terminal-input")
void __wasm_import_wasi_cli_terminal_input_terminal_input_drop(s32);
CLI("terminal-output", "[resource-drop]terminal-output")
void __wasm_import_wasi_cli_terminal_output_terminal_output_drop(s32);

/* option<own<terminal-...>> */
struct wasi_ret_option {
	u8 is_some;
	s32 handle;
};

/* the streams, each got once (0: not yet) */
static s32 wasi_stdin, wasi_stdout;

static s32 wasi_stdin_get(void)
{
	if (!wasi_stdin)
		wasi_stdin = __wasm_import_wasi_cli_stdin_get_stdin();
	return wasi_stdin;
}

static s32 wasi_stdout_get(void)
{
	if (!wasi_stdout)
		wasi_stdout = __wasm_import_wasi_cli_stdout_get_stdout();
	return wasi_stdout;
}

/*
 * what the host waits on for the console (poll_watch): stdin's bytes and
 * stdout's room, each set while hvc has found none, 0 once ready
 */
enum { W_IN, W_OUT, WATCHES };
static unsigned int watched[WATCHES];
static s32 pollable[WATCHES];	/* each made once it's first waited on */
static int wasi_console_irq = -1;

static void wasi_console_watch(int k)
{
	if (wasi_console_irq < 0)
		return;
	if (!pollable[k])
		pollable[k] = k == W_IN ?
			__wasm_import_wasi_io_streams_method_input_stream_subscribe(wasi_stdin_get()) :
			__wasm_import_wasi_io_streams_method_output_stream_subscribe(wasi_stdout_get());
	watched[k] = pollable[k];
}

static ssize_t wasi_put_chars(u32 vtermno, const u8 *buf, size_t count)
{
	struct wasi_ret_u64 room;
	struct wasi_ret_stream r;
	s32 out = wasi_stdout_get();

	__wasm_import_wasi_io_streams_method_output_stream_check_write(out, &room);
	if (room.is_err) {
		wasi_stream_failed(&room.err);
		return -EIO;
	}
	if (!room.ok) {
		/* (hvc keeps it, and writes it again once there's room) */
		wasi_console_watch(W_OUT);
		return 0;
	}
	count = min_t(u64, count, room.ok);
	__wasm_import_wasi_io_streams_method_output_stream_write(out, buf, count, &r);
	if (!r.is_err)
		__wasm_import_wasi_io_streams_method_output_stream_flush(out, &r);
	if (r.is_err) {
		wasi_stream_failed(&r.err);
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
	struct wasi_ret_list r;

	if (wasi_stdin_ended)
		return 0;
	__wasm_import_wasi_io_streams_method_input_stream_read(wasi_stdin_get(), count, &r);
	if (r.is_err) {
		if (!r.err.closed) {
			wasi_stream_failed(&r.err);
			return -EIO;
		}
		wasi_stdin_ended = true;
		if (!count)
			return 0;
		buf[0] = 4;
		return 1;
	}
	count = r.ok.len;
	memcpy(buf, r.ok.ptr, count);
	lkl_ops->mem_free(r.ok.ptr);
	/* none yet: polled again once there is */
	if (!count)
		wasi_console_watch(W_IN);
	return count;
}

/* stdin or stdout is a terminal's: WASI's terminal-stdin, terminal-stdout */
static bool wasi_isatty(void (*get)(void *), void (*drop)(s32))
{
	struct wasi_ret_option t;

	get(&t);
	if (t.is_some)
		drop(t.handle);
	return t.is_some;
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
	if (!wasi_isatty(__wasm_import_wasi_cli_terminal_stdin_get_terminal_stdin,
			 __wasm_import_wasi_cli_terminal_input_terminal_input_drop))
		t.c_lflag &= ~(ECHO | ECHONL);
	if (!wasi_isatty(__wasm_import_wasi_cli_terminal_stdout_get_terminal_stdout,
			 __wasm_import_wasi_cli_terminal_output_terminal_output_drop))
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
