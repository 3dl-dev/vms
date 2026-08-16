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
# / vms_devtab.o / vms_lnm.o are built from ../kernel-core/: these substrate-
# agnostic executive facilities
# LIVE in src/kernel-core/ -- event flags (rd vms-ec4, Phase B), AST delivery and
# access-mode enforcement (rd vms-5b2, Phase D), mailboxes (rd vms-a88, Phase E),
# the process table (rd vms-846b, Phase F), the LOCK MANAGER (rd vms-84a, Phase G
# -- the 44 KB payoff), the DEVICE TABLE (rd vms-31b -- the facility Phase E
# deferred, landed once its block-layer + host-task-credential seams existed),
# and, now, the LOGICAL-NAME MANAGER (rd vms-d61 -- the LAST facility to leave
# src/kernel, landed once its store-barrier + userspace-publishable-arena seams
# existed; every executive facility now lives in the shared core). The Linux
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
# vms_devtab.c's block-layer coupling (lookup_bdev/MAJOR/MINOR/dev_t) is now
# behind exec_kbackend.h's block-device seam (exec_blockdev_*, §8) and its
# host-task-credential coupling behind exec_current_uid/gid, so it joins the
# core above (vms-31b). vms_lnm.c was the LAST facility here in src/kernel: it
# is coupled to the Linux mm layer -- vmalloc_user/remap_vmalloc_range/vm_flags
# and smp_wmb barriers. vms-d61 lands its two missing seams -- the STORE BARRIER
# (exec_membar_producer, §9, the seqlock's smp_wmb) and the ALLOCATION half of a
# USERSPACE-PUBLISHABLE ARENA (exec_arena_alloc/free, §10, vmalloc_user/vfree) --
# and moves it into the core below. The one piece that does NOT cross the seam is
# the MMAP-TIME MAPPING (remap_vmalloc_range + clearing VM_MAYWRITE): that is
# host char-device glue, so vms_lnm_mmap stays in the Linux rind (vms_module.c),
# reading the arena base+size back through the facility's accessors. With this
# move EVERY substrate-agnostic executive facility lives in src/kernel-core/;
# what remains here is the Linux glue -- vms_module.c (the /dev/vms char device
# + registration), vms_p0.c/vms_p1.c (the P0/P1 region mappers, still host-mm-
# coupled), and vms_bg.c (the BGn: INET pseudo-device, vms-527 -- a kernel-mode
# driver over the HOST kernel's in-kernel socket API, deliberately not in the
# core because it names the host transport directly; see its own header).
vms-y := vms_module.o \
         ../kernel-core/vms_access.o \
         ../kernel-core/vms_ast.o \
         ../kernel-core/vms_devtab.o \
         ../kernel-core/vms_eflag.o \
         ../kernel-core/vms_lnm.o \
         ../kernel-core/vms_lock.o \
         ../kernel-core/vms_mbx.o \
         ../kernel-core/vms_proctab.o \
         ../kernel-core/vmsfs_acp.o \
         ../vmsfs/ods2/ods2_reader.o \
         vms_p0.o \
         vms_p1.o \
         vms_bg.o

# The GENUINE ODS-2 codec, kernel-resident, gives the Files-11 ACP $MOUNT
# (kernel-core/vmsfs_acp.c, vms-127) its home-block/SCB validation. Only
# ods2_reader.o is needed: it carries the PURE parse/validate surface
# (ods2_home_parse / ods2_fh2_parse / ods2_fh2_map_walk / ods2_scb_parse, plus
# the inline checksum in vmsfs/ods2.h), with no block-I/O seam -- vmsfs_acp.c
# does its own block reads through the exec_blockdev_read_block shim, so neither
# ods2_block_*.o nor the writer is linked here. -DOVMX_ODS2_KERNEL selects the
# codec's kernel string/type vocabulary (ods2_kcompat.h). This is the OUT-OF-TREE
# / QEMU-test module; the in-tree bootable overlay (distro/kernel/drivers-ovmx/
# vms) deliberately does NOT carry the codec yet (flatten-safe-include follow-up),
# so vmsfs_acp.c gates its codec use on OVMX_ODS2_KERNEL and $MOUNT fail-honestly
# refuses there. This build MUST mirror into the bootable Kbuild ONLY the object
# set the bootable module can resolve -- ods2_reader.o is intentionally absent
# from distro/kernel/drivers-ovmx/vms/Kbuild.

# -I$(src)/../kernel-core: the substrate-agnostic executive core + the shim
#   contracts (exec_kbackend.h, exec_list.h, exec_hash.h). The relocated core
#   files include them, and also include vms_internal.h (the shared kernel struct
#   header, still here in src/kernel) resolved via -I$(src). The Linux backend
#   impls (exec_kbackend_linux.h, exec_list_linux.h, exec_hash_linux.h) live here
#   in src/kernel and are selected by OVMX_KBACKEND_LINUX below.
# -I$(src)/../vmsfs/include + -I$(src)/../vmsfs/ods2: the codec's public header
#   ("vmsfs/ods2.h") and its private vocabulary header (ods2_kcompat.h), for the
#   ods2_reader.o above and vmsfs_acp.c's codec use (vms-127).
ccflags-y := -I$(src) -I$(src)/../kernel-core \
             -I$(src)/../vmsfs/include -I$(src)/../vmsfs/ods2 \
             -DOVMX_KBACKEND_LINUX -DOVMX_ODS2_KERNEL

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
