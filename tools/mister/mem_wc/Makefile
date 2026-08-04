# Out-of-tree build for the mem_wc write-combining /dev/mem driver.
#
# Build against the MiSTer kernel source, cross-compiled with the same Arm
# 10.2-2020.11 toolchain the kernel itself was built with (check /proc/version
# on the device):
#
#   make KDIR=/path/to/Linux-Kernel_MiSTer
#
# KDIR must be a kernel tree that has had `make modules_prepare` run against the
# DEVICE'S OWN /proc/config.gz, so vermagic matches exactly. The running kernel
# has CONFIG_MODVERSIONS and CONFIG_MODULE_SIG off, so no symbol-CRC matching
# and no signing are needed -- only the vermagic string. See README.md.

obj-m := mem_wc.o

# Kernel source / build tree. Override on the command line as needed.
KDIR ?= /lib/modules/$(shell uname -r)/build

ARCH          ?= arm
CROSS_COMPILE ?= /opt/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean

.PHONY: all clean
