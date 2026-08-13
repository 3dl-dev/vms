# Makefile - out-of-tree build of the OVMX/NetBSD ODS-2 vnode/VFS module
# (rd vms-308, epic vms-8e8). NetBSD (bmake) + bsd.kmodule.mk ONLY; this is not
# a GNU-make or Linux build file. The nightly mount-proof harness (tests/netbsd/)
# runs `make' here INSIDE a booted NetBSD guest that has the `comp' + `syssrc'
# sets installed, so the kernel sources at /usr/src/sys and the toolchain are
# present. (The fast per-PR gate does NOT use this Makefile: it is the
# QEMU-free clang cross-compile in tests/netbsd/crosscompile-vmsfs.sh.)
#
# Pin NETBSDSRCDIR so bsd.klinks.mk resolves S=/usr/src/sys deterministically
# even though we build outside the source tree. Must be set before
# <bsd.kmodule.mk> is pulled in.
NETBSDSRCDIR?=	/usr/src

KMOD=		vmsfs

# The module is the NetBSD ODS-2 VNODE/VFS backend glue (vmsfs_vfsops.c) + THE
# SAME substrate-neutral ODS-2 core sources the Linux vmsfs.ko builds
# (../kernel-core/vmsfs/*.c). There is exactly ONE implementation of each ODS-2
# facility -- the on-disk format, the file-header decode/encode, the version
# resolution, the retrieval-map math, the directory-block scanner and the
# storage allocator; this build compiles them with the NetBSD backend
# (vmsfs_backend_netbsd.h, selected by -DOVMX_KBACKEND_NETBSD). Never a second
# copy of ODS-2 logic (epic vms-8e5 / INV-DRIFT).
SRCS=		vmsfs_vfsops.c \
		vmsfs_version.c \
		vmsfs_map.c \
		vmsfs_name.c \
		vmsfs_header.c \
		vmsfs_alloc.c \
		vmsfs_dirscan.c

# The shared ODS-2 core sources live in the sibling kernel-core dir; tell bmake
# where to find them. The harness stages them at ../kernel-core relative to this
# Makefile (mirroring src/kernel-netbsd/Makefile's layout for the executive).
.PATH:		${.CURDIR}/../kernel-core

# -I${.CURDIR}          : this dir -- the NetBSD backend (vmsfs_backend_netbsd.h)
#                        and the NetBSD glue types (vmsfs_nb.h). Searched FIRST so
#                        the seam-#1/#2 vocabulary resolves to the NetBSD backend.
# -I../kernel-core      : the shared core headers (vmsfs_bio.h, vmsfs_core.h,
#                        vmsfs_backend.h) + the shared facility sources.
# -I../ondisk           : vmsfs_ondisk.h, which physically lives beside the Linux
#                        glue (src/kernel/vmsfs/) and is staged here as ../ondisk.
# -DOVMX_KBACKEND_NETBSD : select the NetBSD backend inside vmsfs_backend.h.
CPPFLAGS+=	-I${.CURDIR} -I${.CURDIR}/../kernel-core -I${.CURDIR}/../ondisk \
		-DOVMX_KBACKEND_NETBSD

.include <bsd.kmodule.mk>
