#!/bin/bash
# build-joint-image.sh (vms-864) — proves a REAL alpha-dec-vms GCC-port crt0
# links ZERO-DEFERRED against the GENUINE alpha DECC$SHR (musl-alpha whole-
# archived + the OVMX bootstrap surface, mk_decc_shr.sh's ALPHA/EVAX branch),
# not the plain-alpine generic-arch fallback (that mismatch is bead vms-2a0 —
# this script exists specifically to NOT fall into it: OVMX_DECC_ARCH=alpha is
# forced, never auto-detected, so the proof cannot silently degrade to the
# generic branch's non-alpha DECC$SHR again).
#
# Runs entirely inside the tools/cross-alpha-vms toolchain container (which
# carries both the alpha-dec-vms cross toolchain AND a host gcc/ar/nm for
# LINK.EXE itself — see tools/cross-alpha-vms/Dockerfile). Containerized,
# build-to-/tmp, Rule-9-clean build/oracle tooling — nothing here runs inside
# an OVMX guest or touches the repo tree.
#
#   IMG=ovmx-cross-alpha-vms tools/cross-alpha-vms/joint-e2e/build-joint-image.sh [OUTDIR]
#
# OUTDIR (default /tmp/joint-e2e-out) receives: LINK.EXE, LIBOTS_SHR.EXE,
# 'DECC$SHR.EXE', crt0.obj, joint_main.obj, joint_e2e.exe, and build.log.
#
# crt0.s here is a REAL alpha-dec-vms cc1 -mpointer-size=64 compile of the
# GCC-port's own libgcc/config/vms/vms-ucrt0.c (GPLv3, gcc-14.2.0 — the exact
# vintage this toolchain builds; see tools/cross-alpha-vms/README.md for the
# pinned version) — captured as source (.s, text) rather than regenerated on
# every run, since the full GCC source tree that produced it is deliberately
# NOT kept in the built toolchain image (Dockerfile: "rm -rf ... gcc-*" after
# `make install-gcc`, to keep the image small). Assembled fresh here by the
# REAL alpha-dec-vms cross `as` on every run — no binary object is checked in.
# joint_main.c is trivial hand-written OVMX proof code, compiled fresh by the
# REAL alpha-dec-vms cc1 on every run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TC_DIR=$(cd "$HERE/.." && pwd)              # tools/cross-alpha-vms
SRC_ROOT=$(cd "$TC_DIR/../.." && pwd)       # repo root
IMG=${IMG:-ovmx-cross-alpha-vms}
OUT=${1:-/tmp/joint-e2e-out}
mkdir -p "$OUT"

# vms-e7c5: if the toolchain image is already present (a CI gate PULLED the
# source-hash-keyed prebuilt image from ghcr, or a prior run built it), reuse
# it — do NOT rebuild, which would re-fetch gcc from ftp.gnu.org and reintroduce
# the outage flake this whole change exists to kill. A missing image still
# builds from source (local runs, or the pull-miss fallback in CI).
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/2] toolchain image $IMG already present — skipping build (prebuilt/pulled) =="
else
    echo "== [1/2] build the alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$TC_DIR"
fi

echo "== [2/2] build the genuine alpha DECC\$SHR + link the joint-e2e proof (in-container) =="
docker run --rm \
    -v "$SRC_ROOT:/src:ro" \
    -v "$HERE:/joint:ro" \
    -v "$OUT:/out" \
    "$IMG" bash -c '
set -euxo pipefail
OUT=/out
PREFIX=/opt/cross-alpha-vms
ALPHA_CC="$PREFIX/bin/alpha-dec-vms-gcc"
ALPHA_AS="$PREFIX/bin/alpha-dec-vms-as"
export PATH="$PREFIX/bin:$PATH"

WORK=/tmp/work
mkdir -p "$WORK"

# ---- 1. musl-alpha libc.a, -g0 (the cross nm needs -g0 to read it; the
#         shipped shareable is byte-identical either way, vms-7b96) ----
echo "-- building musl-alpha libc.a (-g0) --"
OVERLAY=/src/tools/cross-alpha-vms/musl-arch \
    MUSL_EXTRA_CFLAGS=-g0 \
    WORK="$WORK/musl-build" \
    bash /src/tools/cross-alpha-vms/musl-arch/build-musl.sh
LIBC="$WORK/musl-build/musl-1.2.5/lib/libc.a"
MUSL_SRC="$WORK/musl-build/musl-1.2.5"
LIBGCC="$PREFIX/lib/libgcc.a"
test -f "$LIBC" || { echo "FAIL: musl-alpha libc.a not built" >&2; exit 1; }

# ---- 2. LINK.EXE (host tool) ----
echo "-- building LINK.EXE --"
gcc -std=gnu11 -O2 -I/src/src/vmslink/include -o "$WORK/LINK.EXE" /src/src/vmslink/link.c

# ---- 3. LIBOTS$SHR.EXE (the OTS$ integer-divide/block-move runtime the
#         port compiler emits calls to; a SEPARATE shareable, the faithful
#         OpenVMS shape) ----
echo "-- building LIBOTS\$SHR.EXE --"
LINK_EXE="$WORK/LINK.EXE" OUT="$WORK/libots" \
    bash /src/tools/cross-alpha-vms/ots/build-libots.sh

# ---- 4. the GENUINE alpha DECC$SHR — mk_decc_shr.sh ALPHA/EVAX branch,
#         OVMX_DECC_ARCH FORCED (never auto-detected: this is the exact
#         fallback vms-2a0 tracks, and this script exists to prove the real
#         path instead), whole-archiving musl-alpha libc.a + libgcc.a + the
#         OVMX bootstrap surface (decc$main/decc$malloc/C$_EXIT1, vms-864) ----
# vms-2a0 REGRESSION GUARD: with the arch UNSET (auto), the container-format-
# aware detection must resolve to alpha on the genuine libc.a -- it must NOT
# misdetect generic by nm-ing the System V .a container directly (which the
# alpha-dec-vms cross nm rejects "file format not recognized"). This is the ONLY
# place the auto path is exercised; every real caller forces the arch, which is
# exactly why the misdetect lurked. Runs detect-only (exits before any build).
echo "-- vms-2a0 guard: auto-detect must resolve to alpha on the genuine libc.a --"
DET=$(OVMX_DECC_ARCH= OVMX_DECC_DETECT_ONLY=1 \
        NM="$PREFIX/bin/alpha-dec-vms-nm" AR_HOST=ar \
        sh /src/src/vmslink/mk_decc_shr.sh "$WORK/LINK.EXE" /tmp/decc-detect.out "$LIBC" "$LIBGCC" 2>/dev/null | tail -1)
[ "$DET" = alpha ] || { echo "FAIL (vms-2a0): auto-detect resolved [$DET], expected alpha" >&2; exit 1; }
echo "   OK: auto-detect resolves to alpha (container-format-aware, vms-2a0)"

echo "-- building the GENUINE alpha DECC\$SHR (OVMX_DECC_ARCH=alpha, forced) --"
OVMX_DECC_ARCH=alpha \
    NM="$PREFIX/bin/alpha-dec-vms-nm" \
    AR_HOST=ar \
    ALPHA_CC="$ALPHA_CC" \
    ALPHA_MUSL_SRC="$MUSL_SRC" \
    DECC_USE="$WORK/libots/LIBOTS_SHR.EXE" \
    sh /src/src/vmslink/mk_decc_shr.sh "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"

# ---- 5. the real port crt0 + a hello main, both compiled/assembled fresh
#         by the REAL alpha-dec-vms cross toolchain (no object blobs checked
#         into the repo — only the .s/.c sources under tools/cross-alpha-vms/
#         joint-e2e/) ----
echo "-- assembling crt0.obj (real port vms-ucrt0.c -> crt0.s, cross as) --"
"$ALPHA_AS" -o "$OUT/crt0.obj" /joint/crt0.s

echo "-- compiling joint_main.obj (cross cc1, -mpointer-size=64) --"
"$ALPHA_CC" -mpointer-size=64 -g0 -c /joint/joint_main.c -o "$OUT/joint_main.obj"

# ---- 6. the JOINT-E2E IMAGE: real crt0 + real main, --use the genuine
#         alpha DECC$SHR, STRICT (no --allow-undefined; expect zero deferred) ----
echo "== linking joint-e2e image (strict, expect zero deferred) =="
"$WORK/LINK.EXE" --transfer __main --use "$WORK/DECC\$SHR.EXE" \
    -o "$OUT/joint_e2e.exe" "$OUT/crt0.obj" "$OUT/joint_main.obj"

cp "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE" "$WORK/libots/LIBOTS_SHR.EXE" "$OUT/"
echo "== joint-e2e image built (genuine alpha path, vms-864) =="
ls -la "$OUT/"
readelf -h "$OUT/joint_e2e.exe" | grep -E "Type|Machine|Entry"
readelf -SW "$OUT/joint_e2e.exe" | grep -E "vms\\\$xfer|vms\\\$imp|CODE|DATA" || true
' 2>&1 | tee "$OUT/build.log"

echo
echo "== summary =="
echo "undefined-symbol errors (must be 0): $(grep -c 'LINK-F-UNDEF' "$OUT/build.log" || true)"
grep -E "LINK-I-GVALFOLD|LINK-I-IMPORT.*producer|LINK-S-CREATED: .*joint_e2e" "$OUT/build.log" || true
echo "artifacts in $OUT/"
