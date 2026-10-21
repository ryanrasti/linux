extra-y := vmlinux.lds

obj-y += \
	binfmt_wasm.o \
	cpuinfo.o \
	early_printk.o \
	fork.o \
	globals.o \
	idle.o \
	irq.o \
	process.o \
	ptrace.o \
	remote_vm.o \
	semaphore.o \
	sections.o \
	setup.o \
	signal.o \
	stacktrace.o \
	syscall.o \
	time.o

obj-$(CONFIG_SMP) += smp.o
obj-$(CONFIG_WASM_JSPI) += sjlj.o sjlj_asm.o
# (the lowering declares the runtime itself)
CFLAGS_REMOVE_sjlj.o += -mllvm -wasm-enable-sjlj

