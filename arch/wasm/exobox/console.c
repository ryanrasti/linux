// SPDX-License-Identifier: GPL-2.0
/*
 * hvc0 as direct host calls (CONFIG_EXOBOX_CONSOLE): output is written
 * to the host at once; input is the host's buffer, read when the host
 * raises the console's irq.
 */
#include <linux/init.h>
#include <asm/irq.h>
#include "hvc_console.h"
#include "exobox.h"

void exobox_import(kernel, console_write)(const u8 *buf, size_t len);
/* bytes the host has for the console, up to len; 0: none */
int exobox_import(kernel, console_read)(u8 *buf, size_t len);
/* the irq the host raises when it has console input */
void exobox_import(kernel, console_irq)(int irq);

static ssize_t hvc_wasm_put(u32 vtermno, const u8 *buf, size_t count)
{
	wasm_kernel_console_write(buf, count);
	return count;
}

static ssize_t hvc_wasm_get(u32 vtermno, u8 *buf, size_t count)
{
	return wasm_kernel_console_read(buf, count);
}

static const struct hv_ops hvc_wasm_ops = {
	.get_chars = hvc_wasm_get,
	.put_chars = hvc_wasm_put,
	.notifier_add = notifier_add_irq,
	.notifier_del = notifier_del_irq,
	.notifier_hangup = notifier_hangup_irq,
};

static int __init hvc_wasm_init(void)
{
	int irq = wasm_alloc_irq();

	if (irq < 0)
		return irq;
	wasm_kernel_console_irq(irq);
	return PTR_ERR_OR_ZERO(hvc_alloc(0, irq, &hvc_wasm_ops, 256));
}
device_initcall(hvc_wasm_init);

static int __init hvc_wasm_console_init(void)
{
	hvc_instantiate(0, 0, &hvc_wasm_ops);
	return 0;
}
console_initcall(hvc_wasm_console_init);
