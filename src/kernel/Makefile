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

vms-y := vms_module.o \
         vms_access.o \
         vms_ast.o \
         vms_eflag.o \
         vms_lock.o \
         vms_devtab.o

ccflags-y := -I$(src)

# Default target: build against running kernel
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
