// SPDX-License-Identifier: GPL-2.0-only
/*
 * exec of a dynamically linked program: its libraries (wasm modules with a
 * dylink.0 section), which the host links with it. The host can't read the
 * machine's files, so the kernel sends them, as it sends the program: the
 * host decides which path to try next (the needed libraries breadth first,
 * each in its requester's run paths, then /lib and /usr/lib), the kernel
 * opens it and streams it, and says whether it couldn't.
 */
#include <asm/exobox.h>
#include <asm/wasm_imports.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/slab.h>

int exobox_exec_libraries(void)
{
	char *path = kmalloc(PATH_MAX, GFP_KERNEL);
	int err = 0, ret;

	if (!path)
		return -ENOMEM;
	while ((ret = wasm_user_exec_lib(path, PATH_MAX, err)) > 0) {
		struct file *file;

		path[PATH_MAX - 1] = 0;
		file = filp_open(path, O_RDONLY | O_LARGEFILE, 0);
		if (IS_ERR(file)) {
			err = PTR_ERR(file);
			continue;
		}
		err = wasm_stream_file(file);
		fput(file);
	}
	kfree(path);
	return ret;
}
