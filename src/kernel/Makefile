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

# vms_eflag.o / vms_ast.o / vms_access.o / vms_mbx.o / vms_proctab.o / vms_lock.o
# are built from ../kernel-core/: these substrate-agnostic executive facilities
# LIVE in src/kernel-core/ -- event flags (rd vms-ec4, Phase B), AST delivery and
# access-mode enforcement (rd vms-5b2, Phase D), mailboxes (rd vms-a88, Phase E),
# the process table (rd vms-846b, Phase F), and, now, the LOCK MANAGER (rd
# vms-84a, Phase G -- the 44 KB payoff and the last facility promoted). The Linux
# vms.ko is the Linux BACKEND for that shared source -- it provides the
# exec_*/exec_list_*/exec_hash_*/exec_rbtree_* primitives (exec_kbackend_linux.h /
# exec_list_linux.h / exec_hash_linux.h / exec_rbtree_linux.h here in src/kernel)
# that the core files compile against. Kbuild builds each object in place under
# src/kernel-core; the header search paths below let the core files find both the
# shared contracts and this dir's shared struct header.
#
# vms_proctab.c is the first core facility to bind the host TASK: it lands the
# host-task (exec_current_is_privileged / exec_task_*), RCU-lite (exec_rcu_* /
# exec_free_deferred) and sleepable-mutex (exec_mutex_*) shim ops plus the
# intrusive hash seam (exec_hash.h). Its host-task NEIGHBOUR vms_module.c stays
# here in src/kernel as Linux glue -- registration derives identity from
# current's credentials (current_uid/from_kuid/current->real_parent) and the
# free path uses kfree_rcu; that is module-lifecycle host binding, extracted with
# the vms_module.c -> vms_dispatch.c split (design record §8 row F, second half),
# not with proctab.
#
# (vms_lnm.c and vms_devtab.c likewise stay here for now: vms_lnm is coupled to
# the Linux mm layer -- vmalloc_user/remap_vmalloc_range/vm_flags and smp_wmb
# barriers; vms_devtab is coupled to the block layer -- lookup_bdev/MAJOR/MINOR/
# dev_t -- and to host-task credentials. Those seams are not covered by the
# exec_* shim yet; their extraction waits on later phases.)
vms-y := vms_module.o \
         ../kernel-core/vms_access.o \
         ../kernel-core/vms_ast.o \
         ../kernel-core/vms_eflag.o \
         ../kernel-core/vms_lock.o \
         ../kernel-core/vms_mbx.o \
         ../kernel-core/vms_proctab.o \
         vms_devtab.o \
         vms_lnm.o \
         vms_p0.o \
         vms_p1.o

# -I$(src)/../kernel-core: the substrate-agnostic executive core + the shim
#   contracts (exec_kbackend.h, exec_list.h, exec_hash.h). The relocated core
#   files include them, and also include vms_internal.h (the shared kernel struct
#   header, still here in src/kernel) resolved via -I$(src). The Linux backend
#   impls (exec_kbackend_linux.h, exec_list_linux.h, exec_hash_linux.h) live here
#   in src/kernel and are selected by OVMX_KBACKEND_LINUX below.
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
