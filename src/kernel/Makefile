# Makefile for vms.ko kernel module
#
# Build standalone:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#
# Build with custom kernel:
#   make -C /path/to/kernel M=$(pwd) modules
#
# Install:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules_install

obj-m := vms.o

vms-y := vms_module.o \
         vms_access.o \
         vms_ast.o \
         vms_eflag.o \
         vms_lock.o \
         vms_devtab.o \
         vms_proctab.o \
         vms_lnm.o \
         vms_mbx.o \
         vms_p0.o \
         vms_p1.o

# -I$(src)/../kernel-core: the substrate-agnostic executive core + the
#   kernel-backend shim contract (exec_kbackend.h, rd vms-adb). vms_eflag.c
#   includes it; the Linux backend impl (exec_kbackend_linux.h) lives here in
#   src/kernel and is selected by OVMX_KBACKEND_LINUX below.
ccflags-y := -I$(src) -I$(src)/../kernel-core -DOVMX_KBACKEND_LINUX

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
