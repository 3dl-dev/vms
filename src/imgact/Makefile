# Makefile — IMGACT.EXE, the OVMX image activator (beads vms-913.2, vms-913.11).
#
# Standalone build (full CMake OVMX_IMGACT integration is bead 913.3).
# Requires a native musl toolchain (e.g. Alpine's `gcc`, which is musl-based).
# Do NOT build on the host — use build-in-podman.sh, which runs this Makefile
# inside an Alpine container per the project's containerized-deps rule.
#
# aarch64 and x86_64 are supported. ARCH defaults to the host machine but may
# be overridden for a cross build, e.g.:
#   make ARCH=x86_64 CC="x86_64-linux-gnu-gcc"

CC      ?= gcc
ARCH    ?= $(shell uname -m | sed -e 's/arm64/aarch64/' -e 's/amd64/x86_64/')
ARCHDIR := arch/$(ARCH)

# IMGACT.EXE is a freestanding static-PIE ELF interpreter that exports no
# symbols (hidden visibility) and self-relocates. It links against nothing.
IMGACT_CFLAGS := -std=gnu11 -O2 -Wall -Wextra \
                 -I../vmslink/include \
                 -fPIC -fvisibility=hidden -ffreestanding \
                 -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables
IMGACT_LDFLAGS := -nostdlib -nostartfiles -shared \
                  -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none

IMGACT_SRC := imgact.c $(ARCHDIR)/start.S

.PHONY: all clean
all: IMGACT.EXE

IMGACT.EXE: $(IMGACT_SRC) $(ARCHDIR)/imgact_arch.h
	$(CC) $(IMGACT_CFLAGS) $(IMGACT_LDFLAGS) -o $@ imgact.c $(ARCHDIR)/start.S

clean:
	rm -f IMGACT.EXE test/*.o test/test_prog 'test/LIBTEST$$SHR.EXE'
