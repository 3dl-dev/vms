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
# JOINT_MAIN (default joint_main.c) selects which self-contained main-source
# beside this script the cross cc1 compiles into joint_e2e.exe. The default
# builds the P1 milestone crt0-activation test byte-identically to before;
# JOINT_MAIN=crtl_rms_test.c builds the richer CRTL/RMS port variant (heap +
# RMS file I/O + stdio) against the SAME genuine alpha DECC$SHR:
#
#   JOINT_MAIN=crtl_rms_test.c IMG=ovmx-cross-alpha-vms \
#       tools/cross-alpha-vms/joint-e2e/build-joint-image.sh [OUTDIR]
#
# JOINT_CRTL_RMS_VENEER=1 (vms-2655, rung 3 of vms-b4f; default 0, so every
# existing caller/gate is byte-identical) opts DECC$SHR into the rung-2
# CRTL->RMS stdio veneer (two-pass bootstrap — see the JOINT_CRTL_RMS_VENEER
# block below) and adds --use LIBVMSRMS$SHR to the final link, so
# decc$fopen/fwrite/fread/fclose in whatever JOINT_MAIN builds bind to the
# veneer (ovmx_crtl_* -> real RMS system services) instead of musl-POSIX:
#
#   JOINT_MAIN=crtl_rms_test.c JOINT_CRTL_RMS_VENEER=1 IMG=ovmx-cross-alpha-vms \
#       tools/cross-alpha-vms/joint-e2e/build-joint-image.sh [OUTDIR]
#
# OUTDIR (default /tmp/joint-e2e-out) receives: LINK.EXE, LIBOTS_SHR.EXE,
# 'DECC$SHR.EXE', crt0.obj, joint_main.obj, joint_e2e.exe, build.log, and (with
# JOINT_CRTL_RMS_VENEER=1) 'LIBVMSRMS$SHR.EXE' alongside the other shareables.
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
# JOINT_MAIN (vms-crtl-rms-porttest): which main-source under this dir the cross
# cc1 compiles into joint_main.obj -> joint_e2e.exe. Defaults to joint_main.c so
# the P1 milestone crt0-activation test builds byte-identically to before; set
# JOINT_MAIN=crtl_rms_test.c to build the richer CRTL/RMS port variant instead.
# Every candidate is a self-contained .c compiled by the SAME real alpha-dec-vms
# cc1 and linked against the SAME genuine alpha DECC$SHR — only the main source
# changes. Basename only (must live beside this script; mounted read-only at
# /joint in the container).
JOINT_MAIN=${JOINT_MAIN:-joint_main.c}
case "$JOINT_MAIN" in
    */*) echo "FAIL: JOINT_MAIN must be a bare basename beside this script, got '$JOINT_MAIN'" >&2; exit 1;;
esac
test -f "$HERE/$JOINT_MAIN" || { echo "FAIL: JOINT_MAIN source '$JOINT_MAIN' not found in $HERE" >&2; exit 1; }

# JOINT_EXTRA (vms-bdd): space-separated bare basenames of ADDITIONAL .c sources
# beside this script, each compiled by the SAME real alpha-dec-vms cross cc1 into
# its OWN object and added to the STRICT link alongside crt0.obj + joint_main.obj.
# This is what makes the multi-.o rung real: mf_main.c (JOINT_MAIN) calls across
# a genuine .o boundary into mf_util.c (JOINT_EXTRA), and LINK.EXE must resolve
# both the intra-image cross-.o refs AND the decc$ imports each TU pulls in. Empty
# by default, so the N=3 (joint_main.c) and N=7 (crtl_rms_test.c) gates build
# byte-identically to before.
JOINT_EXTRA=${JOINT_EXTRA:-}
for _e in $JOINT_EXTRA; do
    case "$_e" in
        */*) echo "FAIL: JOINT_EXTRA entries must be bare basenames beside this script, got '$_e'" >&2; exit 1;;
    esac
    test -f "$HERE/$_e" || { echo "FAIL: JOINT_EXTRA source '$_e' not found in $HERE" >&2; exit 1; }
done

# JOINT_CRTL_RMS_VENEER (vms-2655, rung 3 of vms-b4f). Opt-in (default 0, so
# every existing gate/caller builds byte-identically to before). When set to
# 1, this recipe builds the port image's DECC$SHR via the SAME two-pass
# CRTL->RMS stdio-veneer bootstrap rung 2's build-decc-veneer.sh introduced
# (tools/cross-alpha-vms/decc-veneer/build-decc-veneer.sh -- read that script
# first, this composes it verbatim into this recipe's producer graph):
#   pass 1 (bootstrap): DECC$SHR WITHOUT the veneer, used only to build the
#     producer graph (LIBVMSSYS/PROCESS/LNM/FS/LIBVMS$SHR -> LIBVMSRMS$SHR).
#   pass 2 (final):     DECC$SHR WITH ALPHA_CRTL_RMS_USE=<pass-1 LIBVMSRMS$SHR>,
#     so decc$fopen/fwrite/fread/fclose alias to the crtl_rms_stdio.c veneer
#     (ovmx_crtl_*) instead of musl's own POSIX defs. The final joint_e2e.exe
#     link then ADDS --use LIBVMSRMS$SHR (the veneer's own cross-image sys$*
#     imports need a producer at THIS link too, exactly as build-decc-veneer.sh's
#     step-12 test image does), and LIBVMSRMS$SHR is staged into OUTDIR
#     alongside DECC$SHR/LIBOTS_SHR (the SYS$SHARE search-path set a bootable
#     runtime would load all three from). This is a build-ORCHESTRATION
#     opt-in confined to this recipe -- with the var unset/0, every existing
#     caller (joint-e2e-alpha-crt0, run-module-gp-activation-alpha.sh's gate/
#     crtl-rms-gate/mf-gate) gets the SAME single-pass DECC$SHR + --use
#     DECC$SHR/LIBOTS link as before this change, so those runtime-activation
#     gates (which need the LLP64 width fix, vms-1fc, before an RMS-routed
#     fopen is activation-safe on real /dev/vms -- rung 4, vms-f49) are
#     unaffected.
JOINT_CRTL_RMS_VENEER=${JOINT_CRTL_RMS_VENEER:-0}
mkdir -p "$OUT"

# vms-430: the PT_INTERP LINK.EXE bakes into every joint_e2e.exe this script
# produces. link.c's compiled-in DEFAULT is the native-toolchain path
# /vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE, which is NOT resolvable on the
# ACP-flipped bootable runtime (the /vms passthrough is retired, INV-6): DCL RUN
# forks and execv()s the staged image, the kernel opens PT_INTERP, and /vms is
# ENOENT -> the run fails. Every image the joint-e2e proof produces is meant to
# be activated under that runtime (BOOT A stages JOINT_E2E.EXE onto the ODS-2
# volume; the qemu-user LINK gate only readelf-checks EM_ALPHA/ET_DYN and never
# executes it, so it is interp-agnostic), so DEFAULT the interp here to the
# runtime stage dir where PID 1 stages IMGACT.EXE (ovmx_init.c) -- the SAME value
# stock bootable images bake via src/vmslink/CMakeLists.txt:40. A caller that
# needs a different interpreter (a bespoke initramfs proof staging IMGACT.EXE
# elsewhere) still overrides IMGACT_INTERP_PATH in the environment. The stale
# link.c DEFAULT itself is tracked separately as vms-06a (a broader design call).
IMGACT_INTERP_PATH="${IMGACT_INTERP_PATH:-/run/ovmx-boot/IMGACT.EXE}"
export IMGACT_INTERP_PATH

# vms-e7c5: if the toolchain image is already present (a CI gate PULLED the
# source-hash-keyed prebuilt image from ghcr, or a prior run built it), reuse
# it — do NOT rebuild, which would re-fetch gcc from ftp.gnu.org and reintroduce
# the outage flake this whole change exists to kill. A missing image still
# builds from source (local runs, or the pull-miss fallback in CI).
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/2] toolchain image $IMG already present — skipping build (prebuilt/pulled) =="
else
    # vms-495e: instead of a bare `docker build` (which eats ~90 min every run on a
    # rail worker with no ghcr access), go through ensure-toolchain-image.sh — it
    # pulls a content-hash-keyed copy from the LAN 30500 registry if present
    # (seconds), else builds once + pushes it so the NEXT run is cheap. Falls back
    # to a plain build if the registry is unreachable, so CI/local behaviour is
    # unchanged when the cache is absent.
    echo "== [1/2] toolchain image $IMG absent — ensure-toolchain-image.sh (pull-cached-or-build+push, vms-495e) =="
    IMG="$IMG" sh "$TC_DIR/ensure-toolchain-image.sh"
fi

echo "== [2/2] build the genuine alpha DECC\$SHR + link the joint-e2e proof (in-container) =="
docker run --rm \
    -v "$SRC_ROOT:/src:ro" \
    -v "$HERE:/joint:ro" \
    -v "$OUT:/out" \
    -e IMGACT_INTERP_PATH \
    -e JOINT_MAIN \
    -e JOINT_EXTRA \
    -e JOINT_CRTL_RMS_VENEER \
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
# vms-157/vms-430: IMGACT_INTERP_PATH is the PT_INTERP LINK.EXE bakes into the
# joint_e2e image. It is DEFAULTED to the ACP-flipped runtime stage dir
# /run/ovmx-boot/IMGACT.EXE just above (see the vms-430 note next to JOINT_MAIN)
# so the image is activatable under the bootable runtime out of the box; a caller
# can still override it in the environment for a different interpreter layout.
# (The link.c compiled-in default is itself the retired /vms path -- vms-06a.)
# Passed as a BARE token (not a quoted string), per link.c: link.c stringifies it.
echo "-- building LINK.EXE (IMGACT_INTERP_PATH=$IMGACT_INTERP_PATH) --"
gcc -std=gnu11 -O2 ${IMGACT_INTERP_PATH:+-DIMGACT_INTERP_PATH=$IMGACT_INTERP_PATH} \
    -I/src/src/vmslink/include -o "$WORK/LINK.EXE" /src/src/vmslink/link.c

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

RMS=""
JOINT_CRTL_RMS_VENEER=${JOINT_CRTL_RMS_VENEER:-0}
if [ "$JOINT_CRTL_RMS_VENEER" = 1 ]; then
    # ---- vms-2655 (rung 3): the two-pass CRTL->RMS veneer bootstrap, composed
    #      verbatim from tools/cross-alpha-vms/decc-veneer/build-decc-veneer.sh
    #      (the rung 2 template) into this recipe producer graph. ----
    MK=/src/src/vmslink
    OTS="$WORK/libots/LIBOTS_SHR.EXE"

    # vms-f49 (rung 4): build the pass-1 bootstrap DECC under its OWN directory
    # but with the BASENAME DECC$SHR.EXE (NOT DECC1$SHR.EXE). The producer graph +
    # LIBVMSRMS$SHR --use this file, and LINK.EXE records the producer by BASENAME
    # into their .vms$imp -- so with the basename DECC$SHR.EXE they record the name
    # DECC$SHR.EXE and, at ACTIVATION, the IMGACT name-keyed binding resolves those
    # imports against the SINGLE staged pass-2 (veneer) DECC$SHR.EXE (GSMATCH
    # LEQUAL: pass 2 only appends the veneer aliases, so every pass-1 universal the
    # graph bound is still present). Under the old DECC1$SHR.EXE basename the graph
    # recorded a producer name that does NOT exist on SYS$SHARE -> the rung-4
    # activation failed %IMGACT-F-IMGNOTFND. One DECC$SHR at runtime, not two -- no
    # duplicate musl C-RTL. (No apostrophes in this block -- docker bash -c quote.)
    echo "-- [vms-2655] DECC\$SHR pass 1 (bootstrap, no veneer; basename DECC\$SHR.EXE for runtime name-binding) --"
    mkdir -p "$WORK/p1"
    OVMX_DECC_ARCH=alpha NM="$PREFIX/bin/alpha-dec-vms-nm" AR_HOST=ar \
        ALPHA_CC="$ALPHA_CC" ALPHA_MUSL_SRC="$MUSL_SRC" DECC_USE="$OTS" \
        sh "$MK/mk_decc_shr.sh" "$WORK/LINK.EXE" "$WORK/p1/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
    DECC1="$WORK/p1/DECC\$SHR.EXE"

    echo "-- [vms-2655] the OVMX producer graph (rung 1, unchanged), using DECC1 --"
    export ALPHA_CC ALPHA_MUSL_SRC="$MUSL_SRC" OVMX_DECC_ARCH=alpha ALPHA_OTS_USE="$OTS"
    ALPHA_DECC_USE="$DECC1" sh "$MK/mk_vmssys_shr.sh" "$WORK/LINK.EXE" "$WORK/LIBVMSSYS\$SHR.EXE"
    SYS="$WORK/LIBVMSSYS\$SHR.EXE"
    sh "$MK/mk_vmsprocess_shr.sh" "$WORK/LINK.EXE" "$WORK/LIBVMSPROCESS\$SHR.EXE" "$DECC1" "$SYS"
    PROC="$WORK/LIBVMSPROCESS\$SHR.EXE"
    VMSSYS_SHR="$SYS" sh "$MK/mk_vmslnm_shr.sh" "$WORK/LINK.EXE" "$WORK/LIBVMSLNM\$SHR.EXE" "$DECC1"
    LNM="$WORK/LIBVMSLNM\$SHR.EXE"
    ALPHA_SYS_USE="$SYS" sh "$MK/mk_vmsfs_shr.sh" "$WORK/LINK.EXE" "$WORK/LIBVMSFS\$SHR.EXE" "$DECC1" "$LNM"
    FS="$WORK/LIBVMSFS\$SHR.EXE"
    sh "$MK/mk_libvms_shr.sh" "$WORK/LINK.EXE" "$WORK/LIBVMS\$SHR.EXE" "$DECC1" "$PROC" "$SYS" "$FS"
    VMS="$WORK/LIBVMS\$SHR.EXE"

    echo "-- [vms-2655] LIBVMSRMS\$SHR (rung 1, unchanged) --"
    sh "$MK/mk_vmsrms_shr.sh" "$WORK/LINK.EXE" "$OUT/LIBVMSRMS\$SHR.EXE" "$DECC1" "$VMS" "$FS" "$SYS"
    RMS="$OUT/LIBVMSRMS\$SHR.EXE"

    # vms-f49 (rung 4): LIBVMSRMS$SHR is NOT self-contained -- at activation it
    # (transitively) imports from the WHOLE executive producer graph
    # (LIBVMS$SHR/LIBVMSFS$SHR/LIBVMSLNM$SHR/LIBVMSPROCESS$SHR/LIBVMSSYS$SHR), so
    # every one of those shareables must be on SYS$SHARE for IMGACT to resolve the
    # veneer image. Emit them to OUTDIR alongside LIBVMSRMS$SHR (rung 3 only staged
    # LIBVMSRMS$SHR, which is why the rung-4 activation drew %IMGACT-F-IMGNOTFND on
    # the first unstaged producer). A non-veneer run never enters this block.
    cp "$SYS" "$PROC" "$LNM" "$FS" "$VMS" "$OUT/"

    echo "-- [vms-2655] DECC\$SHR pass 2 (final, CRTL->RMS stdio veneer wired, vms-ed1e) --"
    OVMX_DECC_ARCH=alpha NM="$PREFIX/bin/alpha-dec-vms-nm" AR_HOST=ar \
        ALPHA_CC="$ALPHA_CC" ALPHA_MUSL_SRC="$MUSL_SRC" DECC_USE="$OTS" \
        ALPHA_CRTL_RMS_USE="$RMS" \
        sh "$MK/mk_decc_shr.sh" "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
else
    echo "-- building the GENUINE alpha DECC\$SHR (OVMX_DECC_ARCH=alpha, forced) --"
    OVMX_DECC_ARCH=alpha \
        NM="$PREFIX/bin/alpha-dec-vms-nm" \
        AR_HOST=ar \
        ALPHA_CC="$ALPHA_CC" \
        ALPHA_MUSL_SRC="$MUSL_SRC" \
        DECC_USE="$WORK/libots/LIBOTS_SHR.EXE" \
        sh /src/src/vmslink/mk_decc_shr.sh "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
fi

# ---- 5. the real port crt0 + a hello main, both compiled/assembled fresh
#         by the REAL alpha-dec-vms cross toolchain (no object blobs checked
#         into the repo — only the .s/.c sources under tools/cross-alpha-vms/
#         joint-e2e/) ----
echo "-- assembling crt0.obj (real port vms-ucrt0.c -> crt0.s, cross as) --"
"$ALPHA_AS" -o "$OUT/crt0.obj" /joint/crt0.s

JOINT_MAIN=${JOINT_MAIN:-joint_main.c}
echo "-- compiling joint_main.obj from $JOINT_MAIN (cross cc1, -mpointer-size=64) --"
"$ALPHA_CC" -mpointer-size=64 -g0 -c "/joint/$JOINT_MAIN" -o "$OUT/joint_main.obj"

# vms-bdd: JOINT_EXTRA additional objects — each compiled by the SAME cross cc1
# into its OWN .obj, added to the STRICT link below. This is the multi-.o rung:
# mf_main.obj calls across the boundary into mf_util.obj, and both pull decc$
# imports from the genuine DECC$SHR. EXTRA_OBJS accumulates the /out paths.
EXTRA_OBJS=""
JOINT_EXTRA=${JOINT_EXTRA:-}
for _e in $JOINT_EXTRA; do
    _obj="$OUT/${_e%.c}.obj"
    echo "-- compiling extra $_obj from $_e (cross cc1, -mpointer-size=64) --"
    "$ALPHA_CC" -mpointer-size=64 -g0 -c "/joint/$_e" -o "$_obj"
    EXTRA_OBJS="$EXTRA_OBJS $_obj"
done

# ---- 6. the JOINT-E2E IMAGE: real crt0 + real main, --use the genuine
#         alpha DECC$SHR *and* LIBOTS$SHR, STRICT (no --allow-undefined;
#         expect zero deferred) ----
# LIBOTS$SHR.EXE is added to the canonical consumer link recipe alongside
# DECC$SHR.EXE (zlib-crtl-rungs). The alpha-dec-vms port compiler lowers every
# integer divide/remainder to an OTS$DIV_*/OTS$REM_* call (Alpha has no integer-
# divide instruction), and those universals live in the SEPARATE LIBOTS$
# shareable -- DECC$SHR imports OTS$ for its OWN use but does NOT transitively
# re-export it, so a consumer that divides (zlib, and most real C) would defer
# OTS$DIV_UL/OTS$REM_UI against DECC$SHR alone. LINK binds only REFERENCED
# imports, so adding --use LIBOTS$ is inert for programs (like this joint_main)
# that emit no OTS$ call, and closes the gap for those that do.
#
# vms-2655 (rung 3): when JOINT_CRTL_RMS_VENEER=1, DECC$SHR (above) is the
# pass-2 veneer-wired build, whose decc$fopen/fwrite/fread/fclose alias to the
# crtl_rms_stdio.c veneer (ovmx_crtl_*), which itself references
# sys$create/open/connect/put/get/close -- cross-image imports that need a
# producer at THIS link too, exactly like build-decc-veneer.sh step 12s test
# image. --use LIBVMSRMS$SHR supplies it (RMS is empty/unset otherwise, so
# this is inert -- no extra --use flag -- when the veneer is not opted in).
RMS_USE_FLAG=""
[ -n "$RMS" ] && RMS_USE_FLAG="--use $RMS"
"$WORK/LINK.EXE" --transfer __main \
    --use "$WORK/DECC\$SHR.EXE" $RMS_USE_FLAG --use "$WORK/libots/LIBOTS_SHR.EXE" \
    -o "$OUT/joint_e2e.exe" "$OUT/crt0.obj" "$OUT/joint_main.obj" $EXTRA_OBJS

cp "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE" "$WORK/libots/LIBOTS_SHR.EXE" "$OUT/"
# vms-2655: LIBVMSRMS$SHR.EXE was already built directly into $OUT (above), so
# it lands in OUTDIR alongside DECC$SHR.EXE/LIBOTS_SHR.EXE -- the same
# SYS$SHARE search-path set -- with no extra copy needed when the veneer path
# built it; a plain (non-veneer) run leaves $RMS empty and stages nothing new.
echo "== joint-e2e image built (genuine alpha path, vms-864) =="
# vms-3320: the FILE-OP veneer gate (JOINT_MAIN=crtl_rms3_test.c) drops a marker
# so build-alpha-bootimage.sh stages the FILE-OP independent-reader SYSTARTUP
# (DIRECTORY of the FOP*.DAT set) instead of the stdio VENEER one (which reads
# PORTTEST.DAT). Any other JOINT_MAIN leaves it absent -> unchanged behaviour.
[ "$JOINT_MAIN" = crtl_rms3_test.c ] && { : > "$OUT/FILEOP_PROOF"; echo "== FILEOP_PROOF marker staged (vms-3320 file-op veneer gate) =="; }
ls -la "$OUT/"
readelf -h "$OUT/joint_e2e.exe" | grep -E "Type|Machine|Entry"
readelf -SW "$OUT/joint_e2e.exe" | grep -E "vms\\\$xfer|vms\\\$imp|CODE|DATA" || true
' 2>&1 | tee "$OUT/build.log"

echo
echo "== summary =="
echo "undefined-symbol errors (must be 0): $(grep -c 'LINK-F-UNDEF' "$OUT/build.log" || true)"
grep -E "LINK-I-GVALFOLD|LINK-I-IMPORT.*producer|LINK-S-CREATED: .*joint_e2e" "$OUT/build.log" || true
echo "artifacts in $OUT/"
