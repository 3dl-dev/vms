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

# vms_eflag.o is built from ../kernel-core/vms_eflag.c: the event-flag facility
# is substrate-agnostic and LIVES in src/kernel-core/ (rd vms-ec4, Phase B). The
# Linux vms.ko is the Linux BACKEND for that shared source -- it provides the
# exec_*/exec_list_* primitives (exec_kbackend_linux.h / exec_list_linux.h here
# in src/kernel) that the core file compiles against. Kbuild builds the object
# in place under src/kernel-core; the header search paths below let the core
# file find both the shared contracts and this dir's shared struct header.
vms-y := vms_module.o \
         vms_access.o \
         vms_ast.o \
         ../kernel-core/vms_eflag.o \
         vms_lock.o \
         vms_devtab.o \
         vms_proctab.o \
         vms_lnm.o \
         vms_mbx.o \
         vms_p0.o \
         vms_p1.o

# -I$(src)/../kernel-core: the substrate-agnostic executive core + the shim
#   contracts (exec_kbackend.h, exec_list.h). The relocated core file
#   ../kernel-core/vms_eflag.c includes them, and also includes vms_internal.h
#   (the shared kernel struct header, still here in src/kernel) resolved via
#   -I$(src). The Linux backend impls (exec_kbackend_linux.h, exec_list_linux.h)
#   live here in src/kernel and are selected by OVMX_KBACKEND_LINUX below.
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
