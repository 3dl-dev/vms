# Makefile for vmsfs.ko - VMS filesystem kernel module
#
# Build standalone:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
#
# Build with custom kernel:
#   make -C /path/to/kernel M=$(pwd) modules
#
# Install:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules_install
#
# Mount example:
#   mount -t vmsfs -o backing=/var/vmsfs none /vms/sys$disk

obj-m := vmsfs.o

# vmsfs_version.o and vmsfs_map.o are built from ../../kernel-core/vmsfs/: these
# substrate-agnostic ODS-2 algorithms (VMS version resolution + retrieval-map
# VBN<->LBN math, rd vms-544, epic vms-8e8) LIVE in src/kernel-core/vmsfs/ and
# compile against the vmsfs_backend shim. The Linux vmsfs.ko is the Linux BACKEND
# for that shared source -- it provides the vmsfs_backend_linux.h realization
# (host types / string / ctype / errno) the core files compile against, exactly
# as the executive core (src/kernel-core/) is backed by src/kernel here.
# (vmsfs_ondisk.h, already substrate-neutral, stays beside this glue and is
# resolved by -I$(src) -- the same way the executive core reaches vms_internal.h,
# which likewise stayed in src/kernel.
#
# V2 (rd vms-00c) adds vmsfs_name.o: the pure ODS-2 filename FORMAT algorithms
# (split name/type, uppercase, case-blind name match) that were file-static in
# vmsfs_blkdev.c and fit the EXISTING vmsfs_backend vocabulary shim (string /
# ctype only, no host object). The remaining block-coupled ODS-2 algorithms --
# bitmap/cluster allocation and the directory-block scanner -- stay here: they
# are entangled with struct super_block / struct vmsfs_sb_info / struct
# buffer_head / struct inode, and need the block/inode seam the vmsfs_backend
# shim does not cover yet. That seam is DESIGNED in README-backend.md; building
# it and moving those algorithms onto it is the next phase.)
vmsfs-y := vmsfs_super.o \
           vmsfs_inode.o \
           vmsfs_file.o \
           vmsfs_dir.o \
           ../../kernel-core/vmsfs/vmsfs_version.o \
           ../../kernel-core/vmsfs/vmsfs_map.o \
           ../../kernel-core/vmsfs/vmsfs_name.o \
           vmsfs_blkdev.o

# -I$(src)/../../kernel-core/vmsfs: the substrate-agnostic ODS-2 core + the shim
#   contracts (vmsfs_backend.h, vmsfs_core.h). The relocated core files include
#   them, and also include vmsfs_ondisk.h (still here beside this glue) resolved
#   via -I$(src). The Linux backend impl (vmsfs_backend_linux.h) lives here and is
#   selected by OVMX_KBACKEND_LINUX below.
ccflags-y := -I$(src) -I$(src)/.. -I$(src)/../../kernel-core/vmsfs -DOVMX_KBACKEND_LINUX

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
