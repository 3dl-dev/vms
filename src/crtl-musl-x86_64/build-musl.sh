#!/bin/sh
# build-musl.sh - build OVMX's x86_64 C RTL base: musl with the system-call
# funnel overlaid (vms-003b; see src/internal/ovmx_syscall.c here), and install
# its static libraries over the build container's musl.
#
# The version is the build container's own musl (Ubuntu 24.04 musl-tools 1.2.4),
# so its headers stay authoritative and only libc.a / crt*.o are replaced.
#
# Usage: build-musl.sh <libdir> [workdir]   e.g. /usr/lib/x86_64-linux-musl
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
LIBDIR=${1:?usage: build-musl.sh <libdir> [workdir]}
W=${2:-/tmp/ovmx-musl}
MUSL_VER=1.2.4
MUSL_SHA256=7a35eae33d5372a7c0da1188de798726f68825513b7ae3ebe97aaaa52114f039

rm -rf "$W"; mkdir -p "$W"; cd "$W"
if command -v wget >/dev/null 2>&1; then
    wget -q "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
else
    curl -sSfL -o "musl-${MUSL_VER}.tar.gz" "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
fi
echo "${MUSL_SHA256}  musl-${MUSL_VER}.tar.gz" | sha256sum -c -
tar xzf "musl-${MUSL_VER}.tar.gz"
cd "musl-${MUSL_VER}"
# The kstat layout the file layer fills must be this musl's.
tail -n +4 "$HERE/include/kstat.h" | cmp -s - arch/x86_64/kstat.h ||
    { echo "build-musl: include/kstat.h no longer matches musl ${MUSL_VER} arch/x86_64/kstat.h" >&2; exit 1; }
cp "$HERE/arch/x86_64/syscall_arch.h" arch/x86_64/syscall_arch.h
cp "$HERE/src/internal/ovmx_syscall.c" src/internal/ovmx_syscall.c
./configure --prefix=/usr --libdir="$LIBDIR" --disable-shared >/dev/null
make -j"$(nproc)" lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o lib/rcrt1.o lib/Scrt1.o >/dev/null
nm lib/libc.a 2>/dev/null | grep -q " T __ovmx_syscall$" ||
    { echo "build-musl: __ovmx_syscall not in libc.a" >&2; exit 1; }
for f in libc.a crt1.o crti.o crtn.o rcrt1.o Scrt1.o; do
    install -m 0644 "lib/$f" "$LIBDIR/$f"
done
echo "build-musl: musl ${MUSL_VER} with the OVMX system-call funnel installed in $LIBDIR"
