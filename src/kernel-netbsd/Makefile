# Makefile - out-of-tree build of the OVMX/NetBSD `vms' pseudo-device
# (rd vms-bfe, epic vms-8e8). NetBSD (bmake) + bsd.kmodule.mk ONLY; this is not
# a GNU-make or Linux build file. The harness (tests/netbsd/) runs `make` here
# INSIDE a booted NetBSD guest that has the `comp' + `syssrc' sets installed, so
# the kernel sources at /usr/src/sys and the toolchain are present.
#
# Pin NETBSDSRCDIR so bsd.klinks.mk resolves S=/usr/src/sys deterministically
# even though we build outside the source tree (in a scratch dir). Must be set
# before <bsd.kmodule.mk> is pulled in.
NETBSDSRCDIR?=	/usr/src

KMOD=		vms
SRCS=		vms_netbsd.c

# vms_ping.h sits next to vms_netbsd.c in this directory.
CPPFLAGS+=	-I${.CURDIR}

.include <bsd.kmodule.mk>
