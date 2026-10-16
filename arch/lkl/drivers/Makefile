# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_PCI) += pci.o
obj-$(CONFIG_LKL_PCI_KUNIT_TEST) += pci_test.o
obj-$(CONFIG_PCI_MSI) += pci-msi.o
obj-$(CONFIG_LKL_PCI_MSI_KUNIT_TEST) += pci-msi-test.o
obj-$(CONFIG_LKL_HOSTBLK) += hostblk.o
# (virtio_fs.c's transport, with host calls: FUSE's internals, as it uses them)
obj-$(CONFIG_LKL_HOSTFUSE) += hostfuse.o
CFLAGS_hostfuse.o := -I$(srctree)/fs/fuse
obj-$(CONFIG_LKL_HOSTVSOCK) += hostvsock.o
obj-$(CONFIG_LKL_HOSTSOCK) += hostsock.o
