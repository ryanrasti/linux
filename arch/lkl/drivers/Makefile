# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_PCI) += pci.o
obj-$(CONFIG_LKL_PCI_KUNIT_TEST) += pci_test.o
obj-$(CONFIG_PCI_MSI) += pci-msi.o
obj-$(CONFIG_LKL_PCI_MSI_KUNIT_TEST) += pci-msi-test.o
obj-$(CONFIG_LKL_WASI) += wasi_bindings.o wasi_console.o
# (wit-bindgen's C: its libc the kernel's)
CFLAGS_wasi_bindings.o += -I $(src)/wasi_libc
obj-$(CONFIG_LKL_WASI_SOCK) += wasi_sock.o
obj-$(CONFIG_LKL_WASI_BLK) += wasi_blk.o
obj-$(CONFIG_LKL_WASI_FS) += wasi_fs.o
