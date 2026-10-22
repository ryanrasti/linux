obj-y += debug.o exec.o fault.o fork.o preempt.o syscall.o
obj-$(CONFIG_EXOBOX_HOSTFUSE) += hostfuse.o
obj-$(CONFIG_EXOBOX_HOSTSOCK) += hostsock.o
# (FUSE's internals, as virtio_fs.c uses them)
CFLAGS_hostfuse.o := -I$(srctree)/fs/fuse
