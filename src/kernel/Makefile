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
# coupled), and vms_bg_pollfd.c (the BGn: readiness poll fd, vms-22a -- pure
# Linux anon_inode/->poll fd machinery with no NetBSD analogue; the rest of BGn:
# moved to ../kernel-core/vms_bg.c behind the exec_socket_* seam, vms-9951).
# vms_l2.c (rd vms-7eb, auth slice of vms-1e4) joins kernel-core alongside
# vms_bg.c and for the SAME reason stays Linux-only for now (see its own
# header): a host-socket-coupled per-process handle list not yet ported to
# the exec_list_*/exec_lock_* seam. It is NOT in the NetBSD kmodule's SRCS.
#
# vms_pe.c (FC-P0.1) is the OPPOSITE case, and it is the point of the cluster
# seam: the LAN port layer of the executive-resident VMScluster stack reaches the
# wire ONLY through exec_kbackend.h families SS14..SS18, so the SAME source is in
# the NetBSD kmodule's SRCS from its first commit -- no "Linux first, NetBSD
# later", which is exactly what produced vms_l2.c's NetBSD hole. Its include
# discipline is enforced by tools/ci/cluster_core_includes_gate.sh.
vms-y := vms_module.o \
         ../kernel-core/vms_access.o \
         ../kernel-core/vms_ast.o \
         ../kernel-core/vms_bg.o \
         ../kernel-core/vms_devtab.o \
         ../kernel-core/vms_eflag.o \
         ../kernel-core/vms_l2.o \
         ../kernel-core/vms_lnm.o \
         ../kernel-core/vms_lock.o \
         ../kernel-core/vms_mbx.o \
         ../kernel-core/vms_pe.o \
         ../kernel-core/vms_proctab.o \
         ../kernel-core/vmsfs_acp.o \
         ../vmsfs/ods2/ods2_reader.o \
         ../vmsfs/ods2/ods2_edit.o \
         vms_p0.o \
         vms_p1.o \
         vms_bg_pollfd.o \
         vms_bg_datafd.o \
         vms_bg_forkinherit.o

# The GENUINE ODS-2 codec, kernel-resident, gives the Files-11 ACP $MOUNT
# (kernel-core/vmsfs_acp.c, vms-127) its home-block/SCB validation. Only
# ods2_reader.o is needed: it carries the PURE parse/validate surface
# (ods2_home_parse / ods2_fh2_parse / ods2_fh2_map_walk / ods2_scb_parse, plus
# the inline checksum in vmsfs/ods2.h), with no block-I/O seam -- vmsfs_acp.c
# does its own block reads through the exec_blockdev_read_block shim, so neither
# ods2_block_*.o nor the writer is linked here. ods2_edit.o (vms-c60) joins it:
# the pure EDIT twins of those parsers (FM2 map append, FH2 EOF/HIBLK set +
# checksum reseal, storage-bitmap bit alloc) the IO$_WRITEVBLK / implicit-extend
# path uses -- also pure (no block-I/O seam), so vmsfs_acp.c sequences the raw
# exec_blockdev_write_block writes around them. -DOVMX_ODS2_KERNEL selects the
# codec's kernel string/type vocabulary (ods2_kcompat.h). This is the OUT-OF-TREE
# / QEMU-test module; the in-tree bootable overlay (distro/kernel/drivers-ovmx/
# vms) now ALSO carries the codec (vms-5f0: ods2_reader.o + ods2_edit.o, with the
# subdir'd "vmsfs/ods2.h" made flatten-safe), because the atomic flip makes PID 1
# $MOUNT SYS$DISK via the ACP. This object set MUST stay mirrored with
# distro/kernel/drivers-ovmx/vms/Kbuild's vms-y -- add a codec .c to both.

# -I$(src)/../kernel-core: the substrate-agnostic executive core + the shim
#   contracts (exec_kbackend.h, exec_list.h, exec_hash.h). The relocated core
#   files include them, and also include vms_internal.h (the shared kernel struct
#   header, still here in src/kernel) resolved via -I$(src). The Linux backend
#   impls (exec_kbackend_linux.h, exec_list_linux.h, exec_hash_linux.h) live here
#   in src/kernel and are selected by OVMX_KBACKEND_LINUX below.
# -I$(src)/../vmsfs/include + -I$(src)/../vmsfs/ods2: the codec's public header
#   ("vmsfs/ods2.h") and its private vocabulary header (ods2_kcompat.h), for the
#   ods2_reader.o above and vmsfs_acp.c's codec use (vms-127).
# -DOVMX_KTEST_FAULT_INJECT (rd vms-5f82): compiles the TEST-ONLY ACP block-I/O
#   fault-injection knob into THIS out-of-tree QEMU-test module ONLY. The bootable
#   overlay (distro/kernel/drivers-ovmx/vms/Kbuild) deliberately does NOT define
#   it, so the shipped executive carries no fault-injection code. It lets a kernel
#   test under a real /dev/vms force a genuine ACP block op to fail and observe the
#   executive's REAL per-device error accounting (vms_device.errcnt) move -- the
#   increment stays on the real error path; only the failure SOURCE is injected.
ccflags-y := -I$(src) -I$(src)/../kernel-core \
             -I$(src)/../vmsfs/include -I$(src)/../vmsfs/ods2 \
             -DOVMX_KBACKEND_LINUX -DOVMX_ODS2_KERNEL -DOVMX_KTEST_FAULT_INJECT

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
