obj-y += debug.o exec.o fault.o fork.o
obj-$(CONFIG_EXOBOX_HOSTFUSE) += hostfuse.o
# (FUSE's internals, as virtio_fs.c uses them)
CFLAGS_hostfuse.o := -I$(srctree)/fs/fuse
