# Makefile — IMGACT.EXE, the OVMX image activator (beads vms-913.2, vms-913.11).
#
# Standalone build (full CMake OVMX_IMGACT integration is bead 913.3).
# Requires a native musl toolchain (e.g. Alpine's `gcc`, which is musl-based).
# Do NOT build on the host — use build-in-podman.sh, which runs this Makefile
# inside an Alpine container per the project's containerized-deps rule.
#
# aarch64, x86_64, and alpha are supported. ARCH defaults to the host machine
# but may be overridden for a cross build, e.g.:
#   make ARCH=x86_64 CC="x86_64-linux-gnu-gcc"
#   make ARCH=alpha  CC="alpha-linux-gnu-gcc"

CC      ?= gcc
ARCH    ?= $(shell uname -m | sed -e 's/arm64/aarch64/' -e 's/amd64/x86_64/')
ARCHDIR := arch/$(ARCH)

# IMGACT.EXE is a freestanding static-PIE ELF interpreter that exports no
# symbols (hidden visibility) and self-relocates. It links against nothing.
IMGACT_CFLAGS := -std=gnu11 -O2 -Wall -Wextra \
                 -Iinclude \
                 -I../vmslink/include \
                 -I../libvms/include \
                 -I../kernel \
                 -I../boundary_audit/include \
                 -fPIC -fvisibility=hidden -ffreestanding \
                 -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables
IMGACT_LDFLAGS := -nostdlib -nostartfiles -shared \
                  -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none

# known_images.c (bead vms-913.5) is compiled as an extra freestanding
# translation unit here so IMGACT.EXE can call known_images_lookup() (bead
# vms-30d wiring); imgact.c provides the open/fstat/mmap/munmap/close/
# strncpy/strncmp shim its libc calls need under -nostdlib.
# Alpha-only: the VMS standard-call trampoline (imgact_vms_transfer, vms-f60d).
ARCH_EXTRA_SRC :=
ifeq ($(ARCH),alpha)
ARCH_EXTRA_SRC := $(ARCHDIR)/vms_transfer.S
endif

# imgact_boundary_audit.c + the shared boundary_audit_filter.c (vms-617) are
# compiled as extra freestanding TUs here, exactly like known_images.c: the
# executive-boundary AUDIT tracer installed at the activation site. The shared
# classifier/finding-format .c lives under src/boundary_audit so the hosted
# unit-proof library and this freestanding path use the SAME code (no drift).
IMGACT_SRC := imgact.c known_images.c imgact_acp.c imgact_xfer.c \
              imgact_boundary_audit.c ../boundary_audit/boundary_audit_filter.c \
              $(ARCHDIR)/start.S $(ARCH_EXTRA_SRC)

.PHONY: all clean
all: IMGACT.EXE

IMGACT.EXE: $(IMGACT_SRC) $(ARCHDIR)/imgact_arch.h known_images.h imgact_acp.h imgact_xfer.h imgact_boundary_audit.h ../boundary_audit/include/boundary_audit_filter.h include/ovmx_activation.h
	$(CC) $(IMGACT_CFLAGS) $(IMGACT_LDFLAGS) -o $@ $(IMGACT_SRC)

clean:
	rm -f IMGACT.EXE test/*.o test/test_prog 'test/LIBTEST$$SHR.EXE'
