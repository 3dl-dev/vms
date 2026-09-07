#!/bin/bash
# build-decc-veneer.sh (vms-ed1e, rung 2 of the vms-b4f ladder) — build the
# alpha-dec-vms LP64 DECC$SHR with the CRTL->RMS stdio veneer wired in
# (decc$fopen/fwrite/fread/fclose -> src/vmsrms/crtl_rms_stdio.c's
# ovmx_crtl_fopen/fwrite/fread/fclose, which drive real RMS system services
# against the alpha LIBVMSRMS$SHR that rung 1 landed on main), then
# STRICT-link a tiny alpha image against the result (zero deferred). Mirrors
# rung 1's tools/cross-alpha-vms/rms-substrate/build-rms-substrate.sh
# container/host-tool discipline exactly.
#
#   IMG=ovmx-cross-alpha-vms tools/cross-alpha-vms/decc-veneer/build-decc-veneer.sh [OUTDIR]
#
# THE TWO-PASS DECC$SHR BOOTSTRAP (the shape this recipe's alpha branch
# already supports via mk_decc_shr.sh's generic DECC_USE knob — see that
# script's "CRTL->RMS STDIO VENEER" comment block for the full rationale):
# src/vmsrms/crtl_rms_stdio.c's ovmx_crtl_fopen/fwrite/fread/fclose call
# sys$create/$open/$connect/$put/$get/$close, defined by LIBVMSRMS$SHR — a
# cross-image import that only resolves against an ALREADY-BUILT producer.
# But LIBVMSRMS$SHR itself --use's DECC$SHR (for malloc/free/decc$fprintf/...,
# rung 1). This is a genuine mutual dependency between the two shareables, so
# DECC$SHR is built TWICE:
#   pass 1 (bootstrap): DECC$SHR WITHOUT the veneer (mk_decc_shr.sh's existing,
#     unmodified-by-default alpha branch) -- exactly rung 1's DECC$SHR, used
#     only to build the producer graph through LIBVMSRMS$SHR.
#   pass 2 (final):     DECC$SHR WITH the veneer, ALPHA_CRTL_RMS_USE=<the
#     pass-1-built LIBVMSRMS$SHR.EXE>. LIBVMSRMS$SHR itself is NOT rebuilt --
#     GSMATCH LEQUAL + NAME-keyed activation binding (IMGACT sv_find_named)
#     keeps it valid against pass 2's DECC$SHR (every pass-1 universal it
#     bound stays present, unmoved; pass 2 only appends/aliases-in-place).
# This is a build-ORCHESTRATION decision confined to this NEW rung-2
# acceptance script -- it does not change the shape of any existing,
# CI-wired build (rung 1's build-rms-substrate.sh, joint-e2e, ...), all of
# which call mk_decc_shr.sh WITHOUT ALPHA_CRTL_RMS_USE and get a
# byte-identical DECC$SHR to before this change.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TC_DIR=$(cd "$HERE/.." && pwd)              # tools/cross-alpha-vms
SRC_ROOT=$(cd "$TC_DIR/../.." && pwd)       # repo root
IMG=${IMG:-ovmx-cross-alpha-vms}
OUT=${1:-/tmp/decc-veneer-out}
mkdir -p "$OUT"

if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== toolchain image $IMG present — reusing (prebuilt/pulled) =="
else
    echo "== building the alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$TC_DIR"
fi

docker run --rm \
    -v "$SRC_ROOT:/src:ro" \
    -v "$OUT:/out" \
    "$IMG" bash -c '
set -euxo pipefail
OUT=/out
PREFIX=/opt/cross-alpha-vms
export PATH="$PREFIX/bin:$PATH"
ALPHA_CC="$PREFIX/bin/alpha-dec-vms-gcc"
ALPHA_AS="$PREFIX/bin/alpha-dec-vms-as"
ALPHA_NM="$PREFIX/bin/alpha-dec-vms-nm"
WORK=/tmp/work; mkdir -p "$WORK"
S=/src/src
MK=/src/src/vmslink

# ---- 1. musl-alpha libc.a + source tree (public headers) ----
OVERLAY=/src/tools/cross-alpha-vms/musl-arch MUSL_EXTRA_CFLAGS=-g0 WORK="$WORK/musl-build" \
    bash /src/tools/cross-alpha-vms/musl-arch/build-musl.sh
LIBC="$WORK/musl-build/musl-1.2.5/lib/libc.a"
MUSL_SRC="$WORK/musl-build/musl-1.2.5"
LIBGCC="$PREFIX/lib/libgcc.a"
test -f "$LIBC"

# ---- 2. LINK.EXE (host tool) ----
gcc -std=gnu11 -O2 -I/src/src/vmslink/include -o "$WORK/LINK.EXE" /src/src/vmslink/link.c
LINK="$WORK/LINK.EXE"

# ---- 3. LIBOTS$SHR.EXE ----
LINK_EXE="$LINK" OUT="$WORK/libots" bash /src/tools/cross-alpha-vms/ots/build-libots.sh
OTS="$WORK/libots/LIBOTS_SHR.EXE"

# ---- 4. DECC$SHR PASS 1 (bootstrap — no veneer, byte-identical to rung 1) ----
echo "== DECC\$SHR pass 1 (bootstrap, no veneer) =="
OVMX_DECC_ARCH=alpha NM="$ALPHA_NM" AR_HOST=ar ALPHA_CC="$ALPHA_CC" \
    ALPHA_MUSL_SRC="$MUSL_SRC" DECC_USE="$OTS" \
    sh "$MK/mk_decc_shr.sh" "$LINK" "$WORK/DECC1\$SHR.EXE" "$LIBC" "$LIBGCC"
DECC1="$WORK/DECC1\$SHR.EXE"

# ---- 5..10. the OVMX producer graph (unchanged from rung 1), using DECC1 ----
export ALPHA_CC ALPHA_MUSL_SRC="$MUSL_SRC"
export OVMX_DECC_ARCH=alpha ALPHA_OTS_USE="$OTS"

echo "== LIBVMSSYS\$SHR =="
ALPHA_DECC_USE="$DECC1" sh "$MK/mk_vmssys_shr.sh" "$LINK" "$WORK/LIBVMSSYS\$SHR.EXE"
SYS="$WORK/LIBVMSSYS\$SHR.EXE"

echo "== LIBVMSPROCESS\$SHR =="
sh "$MK/mk_vmsprocess_shr.sh" "$LINK" "$WORK/LIBVMSPROCESS\$SHR.EXE" "$DECC1" "$SYS"
PROC="$WORK/LIBVMSPROCESS\$SHR.EXE"

echo "== LIBVMSLNM\$SHR =="
VMSSYS_SHR="$SYS" sh "$MK/mk_vmslnm_shr.sh" "$LINK" "$WORK/LIBVMSLNM\$SHR.EXE" "$DECC1"
LNM="$WORK/LIBVMSLNM\$SHR.EXE"

echo "== LIBVMSFS\$SHR =="
ALPHA_SYS_USE="$SYS" sh "$MK/mk_vmsfs_shr.sh" "$LINK" "$WORK/LIBVMSFS\$SHR.EXE" "$DECC1" "$LNM"
FS="$WORK/LIBVMSFS\$SHR.EXE"

echo "== LIBVMS\$SHR =="
sh "$MK/mk_libvms_shr.sh" "$LINK" "$WORK/LIBVMS\$SHR.EXE" "$DECC1" "$PROC" "$SYS" "$FS"
VMS="$WORK/LIBVMS\$SHR.EXE"

echo "== LIBVMSRMS\$SHR (the RMS substrate, rung 1 — unchanged) =="
sh "$MK/mk_vmsrms_shr.sh" "$LINK" "$OUT/LIBVMSRMS\$SHR.EXE" "$DECC1" "$VMS" "$FS" "$SYS"
RMS="$OUT/LIBVMSRMS\$SHR.EXE"

# ---- 11. DECC$SHR PASS 2 (final — CRTL->RMS stdio veneer wired) ----
echo "== DECC\$SHR pass 2 (CRTL->RMS stdio veneer, vms-ed1e) =="
OVMX_DECC_ARCH=alpha NM="$ALPHA_NM" AR_HOST=ar ALPHA_CC="$ALPHA_CC" \
    ALPHA_MUSL_SRC="$MUSL_SRC" DECC_USE="$OTS" ALPHA_CRTL_RMS_USE="$RMS" \
    sh "$MK/mk_decc_shr.sh" "$LINK" "$OUT/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
DECC="$OUT/DECC\$SHR.EXE"

# ---- 12. STRICT-link the tiny stdio-veneer test image ----
echo "== STRICT-link the decc-veneer test image (--use DECC\$SHR LIBVMSRMS\$SHR LIBOTS) =="
"$ALPHA_AS" -o "$OUT/crt0.obj" /src/tools/cross-alpha-vms/joint-e2e/crt0.s
"$ALPHA_CC" -mpointer-size=64 -g0 -c /src/tools/cross-alpha-vms/decc-veneer/test_veneer_main.c -o "$OUT/veneer_main.obj"
"$LINK" --transfer __main \
    --use "$DECC" --use "$RMS" --use "$OTS" \
    -o "$OUT/decc_veneer_test.exe" "$OUT/crt0.obj" "$OUT/veneer_main.obj"

# ---- 13. copy artifacts + dump the DECC$SHR universals for the gate ----
cp "$OTS" "$SYS" "$PROC" "$LNM" "$FS" "$VMS" "$OUT/" 2>/dev/null || true
readelf -h "$OUT/decc_veneer_test.exe" | grep -E "Type|Machine"
' 2>&1 | tee "$OUT/build.log"

echo
echo "======================= vms-ed1e rung-2 GATE ======================="
LOG="$OUT/build.log"
fail=0
# (1) DECC$SHR pass 2 (the veneer-wired final image) created
grep -qE "LINK-S-CREATED, .*DECC\\\$SHR\.EXE:" "$LOG" || { echo "GATE FAIL: DECC\$SHR.EXE (pass 2) not created"; fail=1; }
# (2) the veneer wiring log line fired (the alias VEC + --use RMS)
grep -qE "mk_decc_shr: CRTL->RMS stdio veneer wired: decc\\\$fopen/fwrite/fread/fclose -> ovmx_crtl_\* \(--use .*LIBVMSRMS\\\$SHR\.EXE\)" "$LOG" \
    || { echo "GATE FAIL: veneer-wired log line not seen"; fail=1; }
# (3) DECC$SHR's OWN pass-2 build resolved sys$create/open/connect/put/get/close
#     against --use LIBVMSRMS$SHR (the un-fakeable proof the veneer's sys$*
#     calls resolve against the real RMS substrate, not a stub/deferred zero)
for s in create open connect put get close; do
    grep -qE "cross-image import 'sys\\\$${s}' bound to --use producer LIBVMSRMS\\\$SHR.EXE" "$LOG" \
        || { echo "GATE FAIL: sys\$${s} not bound to LIBVMSRMS\$SHR inside DECC\$SHR pass 2"; fail=1; }
done
# (4) the test image links: EVAX/Alpha ET_DYN, %LINK-S-CREATED, zero UNDEF
grep -qE "LINK-S-CREATED, .*decc_veneer_test.exe: EVAX/Alpha" "$LOG" || { echo "GATE FAIL: decc_veneer_test.exe not created"; fail=1; }
grep -q "Machine:.*Alpha" "$LOG" || { echo "GATE FAIL: test image not EM_ALPHA"; fail=1; }
grep -qE "Type:.*DYN" "$LOG" || { echo "GATE FAIL: test image not ET_DYN"; fail=1; }
[ "$(grep -c 'LINK-F-UNDEF' "$LOG")" = 0 ] || { echo "GATE FAIL: LINK-F-UNDEF present"; fail=1; }
[ "$(grep -c 'LINK-F-MULDEF' "$LOG")" = 0 ] || { echo "GATE FAIL: LINK-F-MULDEF present (suppression did not prevent a collision)"; fail=1; }
# LINK-W-DEFERRED is EXPECTED earlier in the pipeline (e.g. LIBVMS$SHR's own
# first-light residual, pre-existing/unrelated to this rung) -- scope the
# zero-deferred requirement to the FINAL test-image link only (from the
# "STRICT-link the decc-veneer test image" marker to end of log), mirroring
# rung 1's gate scoping its LIBVMSRMS$SHR-specific deferred check.
awk '/STRICT-link the decc-veneer test image/{f=1} f&&/LINK-W-DEFERRED/{print "GATE FAIL: test-image link deferred:", $0; c=1} END{exit c}' "$LOG" || fail=1
if [ "$fail" = 0 ]; then
    echo "GATE PASS (vms-ed1e rung-2): alpha DECC\$SHR exports decc\$fopen/fwrite/fread/fclose"
    echo "  aliased to the crtl_rms_stdio.c veneer (ovmx_crtl_*), the musl-POSIX decc\$ defs"
    echo "  suppressed (no MULDEF); the veneer's sys\$create/open/connect/put/get/close bind"
    echo "  as real cross-image imports to the on-main alpha LIBVMSRMS\$SHR; the stdio-veneer"
    echo "  test image STRICT-links (--use DECC\$SHR LIBVMSRMS\$SHR LIBOTS) zero-deferred,"
    echo "  EM_ALPHA/ET_DYN."
else
    echo "GATE FAIL (vms-ed1e rung-2) — see lines above"; exit 1
fi
echo "artifacts in $OUT/"
