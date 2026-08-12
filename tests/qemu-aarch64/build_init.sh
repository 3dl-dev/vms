#!/bin/bash
#
# build_init.sh - cross/native-compile the OVMX freestanding init proof program
# for a target architecture and pack it into a single-file (/init) initramfs.
#
#   build_init.sh <aarch64|x86_64> <out-initramfs.cpio.gz>
#
# It links the SAME real OVMX freestanding sources the product ships
# (src/libvmssys/arch/<arch>/{crt0,syscall,sigreturn}.S + the vms_*.c runtime),
# with the SAME flags the CMake `vmssys` target and `vmssys_add_test()` use
# (-ffreestanding -fno-builtin -fno-stack-protector -fno-tree-vectorize, and
# -mno-outline-atomics on aarch64; linked -nostdlib -nostartfiles -static).
# The result is a genuine EM_<arch> freestanding static ELF -- no glibc, no
# dynamic loader -- suitable as PID 1.
#
# aarch64  -> cross toolchain aarch64-linux-gnu-gcc (the positive proof payload)
# x86_64   -> native gcc on the amd64 build host  (the run harness's negative
#             control: a wrong-e_machine /init the arm64 kernel must reject)
set -euo pipefail

ARCH="${1:?usage: build_init.sh <aarch64|x86_64> <out.cpio.gz>}"
OUT="${2:?usage: build_init.sh <aarch64|x86_64> <out.cpio.gz>}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SYS="$REPO/src/libvmssys"
GUEST="$REPO/tests/qemu-aarch64/guest/init_aarch64.c"

case "$ARCH" in
    aarch64)
        CC=aarch64-linux-gnu-gcc
        ARCHFLAGS="-mno-outline-atomics"
        ;;
    x86_64)
        CC=gcc
        ARCHFLAGS=""
        ;;
    *)
        echo "build_init.sh: unsupported arch '$ARCH'" >&2
        exit 2
        ;;
esac

CFLAGS="-ffreestanding -fno-builtin -fno-stack-protector -fno-tree-vectorize -O2 -Wall -Wextra $ARCHFLAGS -I$SYS"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
OBJ="$WORK/obj"; mkdir -p "$OBJ"

# Real arch layer (crt0 provides _start -> __vms_runtime_init -> main).
$CC $CFLAGS -c "$SYS/arch/$ARCH/crt0.S"      -o "$OBJ/crt0.o"
$CC $CFLAGS -c "$SYS/arch/$ARCH/syscall.S"   -o "$OBJ/syscall.o"
$CC $CFLAGS -c "$SYS/arch/$ARCH/sigreturn.S" -o "$OBJ/sigreturn.o"

# Real C runtime sources reachable from crt0's __vms_runtime_init + the helpers
# the proof program calls (vms_strlen/vms_strcmp, the syscall inlines).
for c in vms_string vms_runtime_init vms_stdio vms_snprintf vms_futex; do
    $CC $CFLAGS -c "$SYS/$c.c" -o "$OBJ/$c.o"
done

# The proof program itself.
$CC $CFLAGS -c "$GUEST" -o "$OBJ/init_$ARCH.o"

$CC -nostdlib -nostartfiles -nodefaultlibs -static \
    "$OBJ"/crt0.o "$OBJ"/syscall.o "$OBJ"/sigreturn.o \
    "$OBJ"/vms_string.o "$OBJ"/vms_runtime_init.o "$OBJ"/vms_stdio.o \
    "$OBJ"/vms_snprintf.o "$OBJ"/vms_futex.o \
    "$OBJ"/init_"$ARCH".o -o "$WORK/init"

# Verify the ELF machine matches the requested arch -- an anti-LARP self-check:
# the positive payload MUST be EM_AARCH64, the negative control MUST be EM_X86_64.
READELF="readelf"
command -v "${ARCH}-linux-gnu-readelf" >/dev/null 2>&1 && READELF="${ARCH}-linux-gnu-readelf"
case "$ARCH" in
    aarch64) want="AArch64" ;;
    x86_64)  want="X86-64" ;;
esac
$READELF -h "$WORK/init" | grep -q "Machine:.*$want" || {
    echo "build_init.sh: FAIL $ARCH init is not EM_$want" >&2
    $READELF -h "$WORK/init" | grep Machine >&2
    exit 3
}

# Single-file initramfs: /init is the static proof binary, nothing else needed.
IRD="$WORK/irfs"; mkdir -p "$IRD"
cp "$WORK/init" "$IRD/init"; chmod 0755 "$IRD/init"
( cd "$IRD" && find . | cpio --quiet -o -H newc | gzip -9 ) > "$OUT"

echo "build_init.sh: built $ARCH /init -> $OUT ($(wc -c < "$OUT") bytes, EM_$want)"
