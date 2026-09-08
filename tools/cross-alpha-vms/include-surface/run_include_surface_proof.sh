#!/bin/bash
# run_include_surface_proof.sh — include-surface proof for the alpha-dec-vms
# GCC port's own host sources (vms-714c, R5:
# docs/design-gcc-port-surface-gaps-register.md S1.2).
#
# THE GAP: the port's own libgcc/gcc host sources #include VMS-convention
# header names — a literal "vms/" directory, canonicalized (lowercase
# basename + ".h") member names, e.g. `#include <vms/chfdef.h>` in
# libgcc/config/alpha/vms-gcc_shell_handler.c — the Unix-hosted analog of how
# a native VMS DEC C compiler resolves a bare `#include <chfdef.h>` off the
# STARLET/SYS$STARLET_C text libraries. Before this proof, OVMX shipped its
# CRTL headers (ssdef.h, chfdef.h, ...) as ordinary flat headers with no
# "vms/" directory shape on the include path, so this class of #include
# FAILED for the port's sources UNCHANGED (confirmed: `alpha-dec-vms-gcc -S
# vms-gcc_shell_handler.c` with no extra -I dies
# "vms/chfdef.h: No such file or directory").
#
# THE FIX: src/libvms/include/vms/{ssdef,chfdef,pdscdef}.h — a canonicalized
# include-surface shim satisfying the VMS-convention names the port's own
# sources reference, additive and separate from OVMX's internal CHF/PDSC
# headers (owned by the vms-1fa/vms-2e72 condition-handling lane) so this
# shim cannot regress that lane's ABI.
#
# THE PROOF: fetch the GENUINE, UNPATCHED upstream
# libgcc/config/alpha/vms-gcc_shell_handler.c (GCC 14.2.0, SHA256-pinned) and
# compile it with the real alpha-dec-vms cross cc1 (`-I src/libvms/include`,
# no other change) to real VMS/Alpha assembly. Not a synthetic stand-in: this
# is the port's own host source, verbatim, resolving its own #includes
# unpatched — the "builds unchanged" bar from the gap register's priority
# list (S4, item 3).
#
# Host-container only (Rule-9-clean, like the rest of tools/cross-alpha-vms/):
# the compiler runs on the build host; nothing here touches /dev/vms, the
# alpha runtime, or link.c/imgact/the crtl_rms veneer.
#
#   tools/cross-alpha-vms/include-surface/run_include_surface_proof.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=$(cd "$HERE/.." && pwd)                 # tools/cross-alpha-vms
REPO=$(cd "$CROSS/../.." && pwd)
IMG=${IMG:-ovmx-cross-alpha-vms}

# The exact upstream file this proof compiles, pinned by content hash (Rule 8:
# derived from public GCC source only; fetched, never vendored into the repo —
# same posture as the GCC/binutils tarball fetches in build-toolchain.sh).
GCC_VER=${GCC_VER:-14.2.0}
SRC_URL="https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-${GCC_VER}/libgcc/config/alpha/vms-gcc_shell_handler.c"
SRC_SHA256="53ea32a5c343e19399bb6bdfb7da118073a44ff1f597348e262b039cdbf5d3e8"

# vms-e7c5 pattern: reuse an already-present toolchain image (a CI gate PULLED
# the source-hash-keyed prebuilt image from ghcr, or a prior job in this run
# built it) — do NOT rebuild, which would re-fetch gcc from ftp.gnu.org. A
# missing image still builds from source.
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/3] toolchain image $IMG already present — skipping build (prebuilt/pulled) =="
else
    echo "== [1/3] build alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$CROSS"
fi

echo "== [2/3] fetch the genuine upstream port source (GCC ${GCC_VER}, SHA256-pinned) =="
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
curl -fsSL --retry 3 --max-time 30 -o "$WORK/vms-gcc_shell_handler.c" "$SRC_URL"
echo "${SRC_SHA256}  $WORK/vms-gcc_shell_handler.c" | sha256sum -c -

echo "== [3/3] compile it, UNPATCHED, with the real alpha-dec-vms cross cc1 =="
docker run --rm \
    -v "$REPO/src/libvms/include:/incsurf:ro" \
    -v "$WORK:/src:ro" \
    "$IMG" bash -euxo pipefail -c '
        export PATH=/opt/cross-alpha-vms/bin:$PATH
        W=/tmp/include-surface-proof; mkdir -p "$W"

        # Baseline control: WITHOUT the include-surface shim, the port source
        # #include <vms/chfdef.h> must NOT resolve (proves this is a real,
        # currently-open gap, not a pre-existing capability).
        if alpha-dec-vms-gcc -S -mpointer-size=64 /src/vms-gcc_shell_handler.c \
             -o "$W/baseline.s" 2>"$W/baseline.err"; then
            echo "PROOF FAIL: the unpatched port source compiled WITHOUT the include" >&2
            echo "surface shim — the gap this proof exists to demonstrate is not real." >&2
            exit 3
        fi
        grep -q "vms/chfdef.h: No such file or directory" "$W/baseline.err" \
            || { echo "PROOF FAIL: baseline failed for an unexpected reason:" >&2; cat "$W/baseline.err" >&2; exit 4; }
        echo "== baseline confirmed: vms/chfdef.h unresolved without the shim =="

        # THE PROOF: with -I pointed at the include-surface shim, the SAME
        # unpatched upstream file compiles to genuine VMS/Alpha assembly.
        alpha-dec-vms-gcc -S -mpointer-size=64 -I/incsurf \
            /src/vms-gcc_shell_handler.c -o "$W/out.s"

        grep -q "\.ent __gcc_shell_handler" "$W/out.s" \
            && grep -q "\.pdesc __gcc_shell_handler" "$W/out.s" \
            || { echo "PROOF FAIL: no VMS procedure descriptor emitted for __gcc_shell_handler" >&2; exit 5; }
    '

echo "== PROOF PASSED: the unpatched alpha-dec-vms port source (libgcc/config/alpha/vms-gcc_shell_handler.c, GCC ${GCC_VER}) compiles to real VMS/Alpha assembly against the OVMX include surface =="
