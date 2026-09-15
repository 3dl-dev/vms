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

# vms-8e8c (CHF rung-5): the port's libgcc EH source. Unlike the shell handler
# above (fetched from the GCC mirror), this one is EXTRACTED from the GCC 14.2.0
# tarball VENDORED in-repo (tools/cross-alpha-vms/gcc-14.2.0.tar.xz, #1125) and
# SHA256-pinned to the extracted file, so the proof stays hermetic/offline.
GCC_TARBALL="$CROSS/gcc-${GCC_VER}.tar.xz"
UNWIND_MEMBER="gcc-${GCC_VER}/libgcc/config/alpha/vms-unwind.h"
UNWIND_SHA256="77bd5b7bef426a09abd0cbb22560b72dd9b520d5ea76fb57fd5be446847a1617"

# vms-e7c5 pattern: reuse an already-present toolchain image (a CI gate PULLED
# the source-hash-keyed prebuilt image from ghcr, or a prior job in this run
# built it) — do NOT rebuild, which would re-fetch gcc from ftp.gnu.org. A
# missing image still builds from source.
if docker image inspect "$IMG" >/dev/null 2>&1; then
    echo "== [1/5] toolchain image $IMG already present — skipping build (prebuilt/pulled) =="
else
    echo "== [1/5] build alpha-dec-vms cross toolchain image ($IMG) =="
    docker build -t "$IMG" "$CROSS"
fi

echo "== [2/5] fetch the genuine upstream port source (GCC ${GCC_VER}, SHA256-pinned) =="
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
curl -fsSL --retry 3 --max-time 30 -o "$WORK/vms-gcc_shell_handler.c" "$SRC_URL"
echo "${SRC_SHA256}  $WORK/vms-gcc_shell_handler.c" | sha256sum -c -

echo "== [3/5] compile it (shell handler), UNPATCHED, with the real alpha-dec-vms cross cc1 =="
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

echo "== shell-handler PROOF PASSED: the unpatched alpha-dec-vms port source (libgcc/config/alpha/vms-gcc_shell_handler.c, GCC ${GCC_VER}) compiles to real VMS/Alpha assembly against the OVMX include surface =="

# ============================================================================
# vms-8e8c (CHF rung-5): the SAME include-surface bar, one rung deeper — the
# port's libgcc EH source libgcc/config/alpha/vms-unwind.h. It #includes all
# four canonicalized names <vms/pdscdef.h>, <vms/libicb.h>, <vms/chfctxdef.h>,
# <vms/chfdef.h> and dereferences the CHFCTX / PDSC / mechanism-array / ICB
# fields the fallback frame-state routine needs. vms-unwind.h is not a
# standalone TU (in a real build it is textually included into
# libgcc/unwind-dw2.c as md-unwind-support.h); compile_vms_unwind.c supplies
# exactly that unwinder-ABI scaffolding (libgcc_unwind_harness.h — NOT the OVMX
# shim, NOT the port's VMS types) and then #includes the VERBATIM vms-unwind.h.
# ============================================================================
echo "== [4/5] extract vms-unwind.h VERBATIM from the vendored GCC ${GCC_VER} tarball (SHA256-pinned) =="
tar -xJf "$GCC_TARBALL" -C "$WORK" --strip-components=4 "$UNWIND_MEMBER"
echo "${UNWIND_SHA256}  $WORK/vms-unwind.h" | sha256sum -c -

echo "== [5/5] compile vms-unwind.h, UNPATCHED, with the real alpha-dec-vms cross cc1 =="
docker run --rm \
    -v "$REPO/src/libvms/include:/incsurf:ro" \
    -v "$WORK:/src:ro" \
    -v "$HERE:/surf:ro" \
    "$IMG" bash -euxo pipefail -c '
        export PATH=/opt/cross-alpha-vms/bin:$PATH
        W=/tmp/unwind-surface-proof; mkdir -p "$W"

        # Assemble the compile dir: the harness prologue, the wrapper, and the
        # verbatim port header side-by-side so the wrap:s #include "vms-unwind.h"
        # / #include "libgcc_unwind_harness.h" resolve.
        P=/tmp/proof; mkdir -p "$P"
        cp /surf/libgcc_unwind_harness.h /surf/compile_vms_unwind.c /src/vms-unwind.h "$P"/
        # Ambient-CRTL stubs (<stdlib.h>/<stdio.h>): the bare cross image has no
        # target libc headers, whereas a real alpha-vms libgcc build compiles
        # against the VMS DEC C RTL. These stand in for that ambient libc so the
        # proof isolates the vms/ surface; present in BOTH baseline and success.
        cp /surf/libc-stub/stdlib.h /surf/libc-stub/stdio.h "$P"/

        # Baseline control: with the NEW shim file vms/chfctxdef.h removed (the
        # single biggest hole this increment closes), the SAME verbatim
        # vms-unwind.h must NOT resolve — proving the additions are load-bearing,
        # not a pre-existing capability. (The port header includes vms/pdscdef.h
        # and vms/libicb.h first, then vms/chfctxdef.h; the missing chfctxdef.h
        # is the failure the port source hits at this increment.)
        cp -r /incsurf /tmp/base_inc
        rm -f /tmp/base_inc/vms/chfctxdef.h
        if alpha-dec-vms-gcc -S -mpointer-size=64 -I"$P" -I/tmp/base_inc \
             "$P/compile_vms_unwind.c" -o "$W/baseline.s" 2>"$W/baseline.err"; then
            echo "PROOF FAIL: vms-unwind.h compiled WITHOUT the vms-8e8c additions —" >&2
            echo "the gap this proof exists to demonstrate is not real." >&2
            exit 6
        fi
        grep -q "vms/chfctxdef.h: No such file or directory" "$W/baseline.err" \
            || { echo "PROOF FAIL: baseline failed for an unexpected reason:" >&2; cat "$W/baseline.err" >&2; exit 7; }
        echo "== baseline confirmed: vms/chfctxdef.h unresolved without the vms-8e8c additions =="

        # THE PROOF: with -I at the full include surface, the VERBATIM port
        # header compiles to genuine VMS/Alpha assembly. Because the success
        # compile dereferences every field the increment adds (chf$l_sig_name,
        # chf$q_mch_savr16..28, pdsc$l_ireg_mask, pdsc$b_save_ra/fp,
        # PDSC$M_BASE_REG_IS_FP, libicb$ph_chfctx_addr, chfctx$q_sigarglst/
        # mcharglst/expt_fp), it passing IS the field-level proof.
        alpha-dec-vms-gcc -S -mpointer-size=64 -I"$P" -I/incsurf \
            "$P/compile_vms_unwind.c" -o "$W/out.s"

        grep -q "\.ent alpha_vms_fallback_frame_state" "$W/out.s" \
            && grep -q "\.pdesc alpha_vms_fallback_frame_state" "$W/out.s" \
            || { echo "PROOF FAIL: no VMS procedure descriptor emitted for alpha_vms_fallback_frame_state" >&2; exit 8; }
    '

echo "== unwind PROOF PASSED: the unpatched alpha-dec-vms port source (libgcc/config/alpha/vms-unwind.h, GCC ${GCC_VER}) compiles to real VMS/Alpha assembly against the OVMX include surface =="
echo "== ALL INCLUDE-SURFACE PROOFS PASSED =="
