# Makefile - out-of-tree build of the OVMX/NetBSD `vms' pseudo-device
# (rd vms-bfe P2b + vms-4b4 P2c, epic vms-8e8). NetBSD (bmake) + bsd.kmodule.mk
# ONLY; this is not a GNU-make or Linux build file. The harness (tests/netbsd/)
# runs `make' here INSIDE a booted NetBSD guest that has the `comp' + `syssrc'
# sets installed, so the kernel sources at /usr/src/sys and the toolchain are
# present.
#
# Pin NETBSDSRCDIR so bsd.klinks.mk resolves S=/usr/src/sys deterministically
# even though we build outside the source tree. Must be set before
# <bsd.kmodule.mk> is pulled in.
NETBSDSRCDIR?=	/usr/src

KMOD=		vms

# The module is the NetBSD BACKEND glue (vms_netbsd.c) + the OVMX intrusive-list
# implementation (exec_list_netbsd.c) + THE SAME executive facility sources the
# Linux vms.ko builds: ../kernel-core/vms_eflag.c (event flags, P2c),
# ../kernel-core/vms_ast.c + vms_access.c (ASTs + access modes, P4-A),
# ../kernel-core/vms_mbx.c (executive-resident mailboxes MBAn:, P4-A) and
# ../kernel-core/vms_proctab.c (the executive process table -- $GETJPI/$SETPRN/
# $PROCESS_SCAN/$HIBER/$WAKE/$SETIDENT, P4-A rd vms-ca7) and
# ../kernel-core/vms_lock.c (the distributed lock manager -- $ENQ/$DEQ/$CONVERT/
# $GETLKI, P4-A rd vms-ff7, the LAST executive facility). There is exactly ONE
# implementation of each facility; this build compiles them with the NetBSD
# backend. (vms_mbx.c links vms_ast_notify_arrival, so vms_ast.c must be present
# -- it is, above. vms_proctab.c binds the host task + the intrusive process
# hash, so exec_hash_netbsd.c -- the NetBSD hash impl -- is listed too. vms_lock.c
# is the sole exec_rbtree consumer -- its lock-ID red-black tree -- so
# exec_rbtree_netbsd.c, OVMX's own intrusive tree, is listed with it. vms_lnm.c
# (rd vms-72da) is the executive-resident logical-name tables LNM$SYSTEM/GROUP/JOB
# -- the LAST facility to join SRCS; it is the sole exec_arena consumer (the
# read-only-publishable arena). Its host-mm glue -- exec_arena_alloc/free
# (uvm_km_alloc(WIRED)) and the cdevsw d_mmap (vms_mmap) that maps the arena's
# wired pages read-only into a process -- is quarantined in the dedicated backend
# TU vms_lnm_arena_netbsd.c, which includes <uvm/uvm_extern.h> but NOT the
# executive's exec_rbtree_netbsd.h (their <sys/rbtree.h> rb_left/rb_right macros
# would collide), listed with the other _netbsd backend TUs above.)
SRCS=		vms_netbsd.c \
		vms_lnm_arena_netbsd.c \
		exec_list_netbsd.c \
		exec_hash_netbsd.c \
		exec_rbtree_netbsd.c \
		vms_eflag.c \
		vms_ast.c \
		vms_access.c \
		vms_mbx.c \
		vms_proctab.c \
		vms_lock.c \
		vms_lnm.c \
		vms_devtab.c \
		vmsfs_acp.c \
		ods2_reader.c \
		ods2_edit.c \
		vms_blockdev_netbsd.c

# vms-d5d: vmsfs_acp.c (the Files-11 ODS-2 ACP: channel + IO$_ACCESS/READVBLK/
# WRITEVBLK/$MOUNT handlers) + ods2_edit.c (the pure on-disk EDIT helpers its
# implicit-extend path calls) join the loadable module so the VAX runtime reads
# SYS$DISK over the executive ACP, not the vmsfs.ko VFS mount. Their raw block
# I/O rides vms_blockdev_netbsd.c (the exec_blockdev_* seam + single-unit
# resolve). Built -DOVMX_ODS2_KERNEL (below); the SAME set the elf32-vax
# compile-audit (tools/cross-vax/build-vms-module-vax.sh) proves ILP32-clean.

# vms_eflag.c lives in the shared core dir; tell bmake where to find it.
.PATH:		${.CURDIR}/../kernel-core
.PATH:		${.CURDIR}/../vmsfs/ods2

# -I${.CURDIR}            : this dir's headers -- the NetBSD backend
#                          (exec_kbackend_netbsd.h, exec_list_netbsd.h), the
#                          NetBSD struct twin (vms_internal.h), the wire contract
#                          (vms_ping.h, vms_eflag_nb.h). Searched BEFORE the core
#                          dir so `#include "vms_internal.h"' from the core file
#                          resolves to the NetBSD twin (there is deliberately no
#                          -I into src/kernel, whose Linux vms_internal.h would
#                          not compile in a NetBSD kernel TU).
# -I../kernel-core        : the shim contracts (exec_kbackend.h, exec_list.h) and
#                          the shared facility source.
# -DOVMX_KBACKEND_NETBSD  : select the NetBSD backend inside exec_kbackend.h /
#                          exec_list.h.
CPPFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../kernel-core -DOVMX_KBACKEND_NETBSD
# vms-d5d: -DOVMX_ODS2_KERNEL selects the real block-I/O + ODS-2 validation in
# vmsfs_acp.c/exec_kbackend_netbsd.h (fail-honest SS$_DEVNOTMOUNT otherwise);
# -I../vmsfs/include supplies the genuine ODS-2 codec header vmsfs/ods2.h.
CPPFLAGS+=	-DOVMX_ODS2_KERNEL -I${.CURDIR}/../vmsfs/include
# vms-618: -DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE compiles OUT vms_devtab.c's two
# TABLE-READ disk resolvers (vms_devtab_disk_backing / vms_ioctl_disk_resolve),
# because this substrate needs resolvers that also lazily OPEN + cache the
# backing device vnode (and that must NOT hold the INITIALIZE target open --
# NetBSD allows one open of a block device). Those definitions live in
# vms_blockdev_netbsd.c; the guard's full reasoning is in vms_devtab.c. NOTHING
# ownership-shaped is substrate-local: the table itself and $ALLOC/$DALLOC/
# $ASSIGN/$DASSGN/$GETDVI/$DEVICE_SCAN have exactly ONE implementation.
CPPFLAGS+=	-DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE

.include <bsd.kmodule.mk>
