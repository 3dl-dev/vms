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
# ctype only, no host object).
#
# V2b (rd vms-d69) adds vmsfs_alloc.o / vmsfs_dirscan.o / vmsfs_header.o: the
# remaining block-coupled ODS-2 algorithms -- the storage/FID/cluster allocator,
# the directory-block scanner, and the file-header decode/encode -- promoted onto
# the block/inode seam (vmsfs_bio.h: struct vmsfs_volume + the vmsfs_bget /
# vmsfs_bit_* / vmsfs_le*_to_cpu / vmsfs_now_seconds ops, realized in
# vmsfs_backend_linux.h). After this vmsfs_blkdev.o is the thin VFS + vmsfs_bio
# backend; ALL ODS-2 logic lives in the core. The seam is DESIGNED in
# README-backend.md §3.
# The GENUINE ODS-2 codec (src/vmsfs/ods2/) compiled KERNEL-RESIDENT (rd
# vms-dcd, epic vms-208): the on-disk reader/writer/path plus the KERNEL
# realization of its block-access seam (ods2_block_kern.o, bound to vmsfs_bio.h),
# and vmsfs_ods2ro.o -- a read-only Files-11 presentation that drives the codec
# off a real struct block_device (the foundation-rung proof that the codec runs
# in-kernel; NOT the authoritative $QIO ACP path, which lands in vms.ko later).
# The SAME sources compile userspace for INITIALIZE/vmsfs_master + host ctest.
vmsfs-y := vmsfs_super.o \
           vmsfs_inode.o \
           vmsfs_file.o \
           vmsfs_dir.o \
           ../../kernel-core/vmsfs/vmsfs_version.o \
           ../../kernel-core/vmsfs/vmsfs_map.o \
           ../../kernel-core/vmsfs/vmsfs_name.o \
           ../../kernel-core/vmsfs/vmsfs_alloc.o \
           ../../kernel-core/vmsfs/vmsfs_dirscan.o \
           ../../kernel-core/vmsfs/vmsfs_header.o \
           ../../vmsfs/ods2/ods2_reader.o \
           ../../vmsfs/ods2/ods2_writer.o \
           ../../vmsfs/ods2/ods2_edit.o \
           ../../vmsfs/ods2/ods2_bdev.o \
           ../../vmsfs/ods2/ods2_path.o \
           ../../vmsfs/ods2/ods2_block_kern.o \
           vmsfs_ods2ro.o \
           vmsfs_blkdev.o

# -I$(src)/../../kernel-core/vmsfs: the substrate-agnostic ODS-2 core + the shim
#   contracts (vmsfs_backend.h, vmsfs_core.h). The relocated core files include
#   them, and also include vmsfs_ondisk.h (still here beside this glue) resolved
#   via -I$(src). The Linux backend impl (vmsfs_backend_linux.h) lives here and is
#   selected by OVMX_KBACKEND_LINUX below.
# -I$(src)/../../vmsfs/include: the genuine ODS-2 codec's public header
#   (vmsfs/ods2.h); -I$(src)/../../vmsfs/ods2: its private seam headers
#   (ods2_kcompat.h, ods2_block.h). -DOVMX_ODS2_KERNEL selects the codec's
#   kernel-resident world (kernel types/alloc + the vmsfs_bio block backend).
ccflags-y := -I$(src) -I$(src)/.. -I$(src)/../../kernel-core/vmsfs \
             -I$(src)/../../vmsfs/include -I$(src)/../../vmsfs/ods2 \
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
