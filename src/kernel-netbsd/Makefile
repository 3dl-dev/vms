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
# $PROCESS_SCAN/$HIBER/$WAKE/$SETIDENT, P4-A rd vms-ca7). There is exactly ONE
# implementation of each facility; this build compiles them with the NetBSD
# backend. (vms_mbx.c links vms_ast_notify_arrival, so vms_ast.c must be present
# -- it is, above. vms_proctab.c binds the host task + the intrusive process
# hash, so exec_hash_netbsd.c -- the NetBSD hash impl -- is listed too.)
SRCS=		vms_netbsd.c \
		exec_list_netbsd.c \
		exec_hash_netbsd.c \
		vms_eflag.c \
		vms_ast.c \
		vms_access.c \
		vms_mbx.c \
		vms_proctab.c

# vms_eflag.c lives in the shared core dir; tell bmake where to find it.
.PATH:		${.CURDIR}/../kernel-core

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

.include <bsd.kmodule.mk>
