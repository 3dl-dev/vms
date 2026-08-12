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

# vms_eflag.o / vms_ast.o / vms_access.o are built from ../kernel-core/: these
# substrate-agnostic executive facilities LIVE in src/kernel-core/ -- event flags
# (rd vms-ec4, Phase B) and, now, AST delivery and access-mode enforcement (rd
# vms-5b2, Phase D). The Linux vms.ko is the Linux BACKEND for that shared source
# -- it provides the exec_*/exec_list_* primitives (exec_kbackend_linux.h /
# exec_list_linux.h here in src/kernel) that the core files compile against.
# Kbuild builds each object in place under src/kernel-core; the header search
# paths below let the core files find both the shared contracts and this dir's
# shared struct header. (vms_lnm.c stays here in src/kernel for now: it is
# coupled to the Linux mm layer -- vmalloc_user/remap_vmalloc_range/vm_flags and
# smp_wmb barriers -- which the exec_* shim does not yet cover; its extraction
# waits on a memory-mapping seam, a later phase.)
vms-y := vms_module.o \
         ../kernel-core/vms_access.o \
         ../kernel-core/vms_ast.o \
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
