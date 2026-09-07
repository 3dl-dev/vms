#!/bin/bash
# build-rms-substrate.sh (vms-a7a, rung 1) — build the alpha-dec-vms LP64 RMS
# substrate LIBVMSRMS$SHR and its faithful producer graph, then STRICT-link a
# tiny alpha image against it (zero deferred). Mirrors joint-e2e/build-joint-
# image.sh's container/host-tool discipline: everything runs inside the
# tools/cross-alpha-vms toolchain image, builds to /tmp, Rule-9-clean (nothing
# runs inside an OVMX guest or touches the repo tree).
#
#   IMG=ovmx-cross-alpha-vms tools/cross-alpha-vms/rms-substrate/build-rms-substrate.sh [OUTDIR]
#
# THE FAITHFUL PRODUCER DAG (mirrors the x86_64/aarch64 mk_vmsrms_shr.sh
# composition — symbol vectors resolve each producer's imports at link, which is
# what keeps the graph bounded rather than one fat bundle):
#   DECC$SHR         (musl-alpha libc, mk_decc_shr.sh ALPHA branch)  [pre-existing]
#   LIBOTS$SHR       (OTS$ integer-divide/block runtime)             [pre-existing]
#   LIBVMSSYS$SHR    vms_kif_* + the /dev/vms ioctl transport + arch/alpha/syscall.S
#   LIBVMSPROCESS$SHR vms_pcb_* (--use DECC$SHR LIBVMSSYS$SHR)
#   LIBVMSLNM$SHR    lnm_*      (--use DECC$SHR LIBVMSSYS$SHR)
#   LIBVMSFS$SHR     vmsfs_*    (--use DECC$SHR LIBVMSLNM$SHR)
#   LIBVMS$SHR       lib$/sys$ RTL + system services (--use DECC PROCESS SYS FS)
#   LIBVMSRMS$SHR    the RMS engine (--use DECC LIBVMS FS SYS LIBOTS)
#
# NOTE (vms-a7a, documented composition adjustment): the alpha LIBVMS$SHR OMITS
# syssvc/sys_imgact.c + syssvc/imgact_prodreg.c. Those are the host-side IMAGE
# ACTIVATOR; sys_imgact.c emits an ELF `.hidden` directive and a TLSDESC static
# resolver (ovmx_tlsdesc_static) the EVAX assembler cannot assemble on alpha
# (the alpha TLS/visibility model is unimplemented — its own rung of work). They
# are referenced by NOTHING else in the producer graph and are irrelevant to the
# RMS substrate and to the alpha port image (which carries its own crt0), so
# omitting them is the "alpha needs its own composition" case, not a libvms
# compile failure (57/58 libvms TUs build clean on alpha). Tracked for the alpha
# image-activation rung.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TC_DIR=$(cd "$HERE/.." && pwd)              # tools/cross-alpha-vms
SRC_ROOT=$(cd "$TC_DIR/../.." && pwd)       # repo root
IMG=${IMG:-ovmx-cross-alpha-vms}
OUT=${1:-/tmp/rms-substrate-out}
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

# ---- 4. DECC$SHR (mk_decc_shr ALPHA branch, --use LIBOTS) ----
OVMX_DECC_ARCH=alpha NM="$ALPHA_NM" AR_HOST=ar ALPHA_CC="$ALPHA_CC" \
    ALPHA_MUSL_SRC="$MUSL_SRC" DECC_USE="$OTS" \
    sh "$MK/mk_decc_shr.sh" "$LINK" "$WORK/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
DECC="$WORK/DECC\$SHR.EXE"

# ---- 5..10. the OVMX producer graph, each built by its OWN recipe'"'"'s
#            OVMX_DECC_ARCH=alpha branch (which delegates to mk_alpha_shr.sh).
#            Building through the recipes (not mk_alpha_shr directly) keeps each
#            shareable'"'"'s TU LIST single-sourced in its recipe. ----
export ALPHA_CC ALPHA_MUSL_SRC="$MUSL_SRC"
export OVMX_DECC_ARCH=alpha ALPHA_OTS_USE="$OTS"

echo "== LIBVMSSYS\$SHR =="
ALPHA_DECC_USE="$DECC" sh "$MK/mk_vmssys_shr.sh" "$LINK" "$WORK/LIBVMSSYS\$SHR.EXE"
SYS="$WORK/LIBVMSSYS\$SHR.EXE"

echo "== LIBVMSPROCESS\$SHR =="
sh "$MK/mk_vmsprocess_shr.sh" "$LINK" "$WORK/LIBVMSPROCESS\$SHR.EXE" "$DECC" "$SYS"
PROC="$WORK/LIBVMSPROCESS\$SHR.EXE"

echo "== LIBVMSLNM\$SHR =="
VMSSYS_SHR="$SYS" sh "$MK/mk_vmslnm_shr.sh" "$LINK" "$WORK/LIBVMSLNM\$SHR.EXE" "$DECC"
LNM="$WORK/LIBVMSLNM\$SHR.EXE"

echo "== LIBVMSFS\$SHR =="
ALPHA_SYS_USE="$SYS" sh "$MK/mk_vmsfs_shr.sh" "$LINK" "$WORK/LIBVMSFS\$SHR.EXE" "$DECC" "$LNM"
FS="$WORK/LIBVMSFS\$SHR.EXE"

echo "== LIBVMS\$SHR =="
sh "$MK/mk_libvms_shr.sh" "$LINK" "$WORK/LIBVMS\$SHR.EXE" "$DECC" "$PROC" "$SYS" "$FS"
VMS="$WORK/LIBVMS\$SHR.EXE"

echo "== LIBVMSRMS\$SHR (the RMS substrate) =="
sh "$MK/mk_vmsrms_shr.sh" "$LINK" "$OUT/LIBVMSRMS\$SHR.EXE" "$DECC" "$VMS" "$FS" "$SYS"
RMS="$OUT/LIBVMSRMS\$SHR.EXE"

# ---- 11. STRICT-link the tiny test image against the substrate ----
echo "== STRICT-link the rms-substrate test image (--use LIBVMSRMS DECC LIBOTS) =="
"$ALPHA_AS" -o "$OUT/crt0.obj" /src/tools/cross-alpha-vms/joint-e2e/crt0.s
"$ALPHA_CC" -mpointer-size=64 -g0 -c /src/tools/cross-alpha-vms/rms-substrate/rms_substrate_main.c -o "$OUT/rms_main.obj"
"$LINK" --transfer __main \
    --use "$RMS" --use "$DECC" --use "$OTS" \
    -o "$OUT/rms_substrate_test.exe" "$OUT/crt0.obj" "$OUT/rms_main.obj"

# ---- 12. copy artifacts + dump the LIBVMSRMS$SHR universals for the gate ----
cp "$DECC" "$OTS" "$SYS" "$PROC" "$LNM" "$FS" "$VMS" "$OUT/" 2>/dev/null || true
echo "== LIBVMSRMS\$SHR universals (the six sys\$ entries must appear) =="
# Enumerate from the LINKER'"'"'s own view of the RMS OBJECTS (a linked shareable is
# not re-enumerable by nm); the objects were compiled by mk_alpha_shr into a
# temp dir already cleaned, so re-dump via the vector the link recorded: grep the
# LIBVMSRMS build'"'"'s SYMVEC + the six sys$ from the vector count is proven by the
# test-link IMPORT bindings below (the un-fakeable proof: the test image bound
# each sys$ to LIBVMSRMS$SHR).
readelf -h "$OUT/rms_substrate_test.exe" | grep -E "Type|Machine"
' 2>&1 | tee "$OUT/build.log"

echo
echo "======================= vms-a7a rung-1 GATE ======================="
LOG="$OUT/build.log"
fail=0
# (1) all six producer shareables + LIBVMSRMS built ([$] = literal $, not the ERE anchor)
for img in "LIBVMSSYS[$]SHR" "LIBVMSPROCESS[$]SHR" "LIBVMSLNM[$]SHR" "LIBVMSFS[$]SHR" "LIBVMS[$]SHR" "LIBVMSRMS[$]SHR"; do
    grep -qE "LINK-S-CREATED, .*${img}.EXE:" "$LOG" || { echo "GATE FAIL: ${img}.EXE not created"; fail=1; }
done
# (2) LIBVMSRMS$SHR strict-links zero-deferred (built with no --allow-undefined)
awk '/building LIBVMSRMS\$SHR/{f=1} f&&/LINK-W-DEFERRED/{print "GATE FAIL: LIBVMSRMS deferred:", $0; c=1} /rms_substrate_test/{f=0} END{exit c}' "$LOG" || fail=1
# (3) the six sys$ entries bound to LIBVMSRMS$SHR in the STRICT test link
for s in create open connect put get close; do
    grep -qE "cross-image import 'sys\\\$${s}' bound to --use producer LIBVMSRMS\\\$SHR.EXE" "$LOG" \
        || { echo "GATE FAIL: sys\$${s} not bound to LIBVMSRMS\$SHR in the test link"; fail=1; }
done
# (4) the test image links: EVAX/Alpha ET_DYN, %LINK-S-CREATED, zero UNDEF
grep -qE "LINK-S-CREATED, .*rms_substrate_test.exe: EVAX/Alpha" "$LOG" || { echo "GATE FAIL: rms_substrate_test.exe not created"; fail=1; }
grep -q "Machine:.*Alpha" "$LOG" || { echo "GATE FAIL: test image not EM_ALPHA"; fail=1; }
grep -qE "Type:.*DYN" "$LOG" || { echo "GATE FAIL: test image not ET_DYN"; fail=1; }
# the test link (last link) must have no LINK-F-UNDEF at all
[ "$(grep -c 'LINK-F-UNDEF' "$LOG")" = 0 ] || { echo "GATE FAIL: LINK-F-UNDEF present"; fail=1; }
if [ "$fail" = 0 ]; then
    echo "GATE PASS (vms-a7a rung-1): 6 alpha producer shareables + LIBVMSRMS\$SHR build;"
    echo "  LIBVMSRMS\$SHR strict-links zero-deferred and exports sys\$create/open/connect/put/get/close;"
    echo "  rms_substrate_test.exe STRICT-links (--use LIBVMSRMS\$SHR DECC\$SHR LIBOTS) with all six bound, EM_ALPHA/ET_DYN."
else
    echo "GATE FAIL (vms-a7a rung-1) — see lines above"; exit 1
fi
echo "artifacts in $OUT/"
