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
# musl source, pinned + checksum-verified. The VENDORED copy beside this script
# comes first, so the build is hermetic: a fetch from musl.libc.org failed a
# merge-queue negative-control shard with "Could not resolve host" (rd vms-244d
# made the alpha musl build hermetic for the same reason). A network fetch is
# the fallback for a tree without the blob: both published hosts, retried with
# backoff. The checksum is enforced whichever way the bytes arrived.
if [ -f "$HERE/musl-${MUSL_VER}.tar.gz" ]; then
    cp "$HERE/musl-${MUSL_VER}.tar.gz" .
else
    n=0
    until [ "$n" -ge 4 ]; do
        for host in musl.libc.org www.musl-libc.org; do
            curl -sSfL --connect-timeout 30 -o "musl-${MUSL_VER}.tar.gz" \
                "https://${host}/releases/musl-${MUSL_VER}.tar.gz" &&
                echo "${MUSL_SHA256}  musl-${MUSL_VER}.tar.gz" | sha256sum -c - >/dev/null 2>&1 &&
                break 2
            rm -f "musl-${MUSL_VER}.tar.gz"
        done
        n=$((n + 1)); sleep $((n * 10))
    done
fi
echo "${MUSL_SHA256}  musl-${MUSL_VER}.tar.gz" | sha256sum -c -
tar xzf "musl-${MUSL_VER}.tar.gz"
cd "musl-${MUSL_VER}"
# The kstat layout the file layer fills must be this musl's.
tail -n +4 "$HERE/include/kstat.h" | cmp -s - arch/x86_64/kstat.h ||
    { echo "build-musl: include/kstat.h no longer matches musl ${MUSL_VER} arch/x86_64/kstat.h" >&2; exit 1; }
cp "$HERE/arch/x86_64/syscall_arch.h" arch/x86_64/syscall_arch.h
cp "$HERE/src/internal/ovmx_syscall.c" src/internal/ovmx_syscall.c
# rd vms-569: cancellation-point calls go through the same funnel.
cp "$HERE/src/thread/x86_64/syscall_cp.s" src/thread/x86_64/syscall_cp.s
# vms-593: the C RTL's clock and time zone are the VMS system's -- the SAME two
# overlays the Alpha DECC$SHR builds (one source: musl 1.2.4 and 1.2.5 carry
# identical clock_gettime.c/__tz.c), as musl's per-arch replacements.
TIME_OVL=${OVMX_MUSL_TIME_OVERLAY:-$HERE/../../tools/cross-alpha-vms/musl-arch/src/time/alpha-dec-vms}
mkdir -p src/time/x86_64
for f in clock_gettime.c __tz.c; do
    cp "$TIME_OVL/$f" "src/time/x86_64/$f" ||
        { echo "build-musl: the vms-593 time overlay $TIME_OVL/$f is missing" >&2; exit 1; }
done
./configure --prefix=/usr --libdir="$LIBDIR" --disable-shared >/dev/null
make -j"$(nproc)" lib/libc.a lib/crt1.o lib/crti.o lib/crtn.o lib/rcrt1.o lib/Scrt1.o >/dev/null
nm lib/libc.a 2>/dev/null | grep -q " T __ovmx_syscall$" ||
    { echo "build-musl: __ovmx_syscall not in libc.a" >&2; exit 1; }
nm lib/libc.a 2>/dev/null | grep -A20 '^syscall_cp.lo:$' | grep -q " U __ovmx_syscall$" ||
    { echo "build-musl: musl's cancellable system call does not go through the funnel (syscall_cp.s overlay)" >&2; exit 1; }
for h in __ovmx_clock_hook __ovmx_tz_hook; do
    nm lib/libc.a 2>/dev/null | grep -qE " [DB] $h$" ||
        { echo "build-musl: $h not in libc.a (vms-593 time overlay not built)" >&2; exit 1; }
done
for f in libc.a crt1.o crti.o crtn.o rcrt1.o Scrt1.o; do
    install -m 0644 "lib/$f" "$LIBDIR/$f"
done
echo "build-musl: musl ${MUSL_VER} with the OVMX system-call funnel installed in $LIBDIR"
