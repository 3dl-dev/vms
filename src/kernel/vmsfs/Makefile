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

vmsfs-y := vmsfs_super.o \
           vmsfs_inode.o \
           vmsfs_file.o \
           vmsfs_dir.o \
           vmsfs_version.o \
           vmsfs_blkdev.o

ccflags-y := -I$(src) -I$(src)/..

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
