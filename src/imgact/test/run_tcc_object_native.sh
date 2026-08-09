#!/bin/sh
# run_tcc_object_native.sh — the S2 CRUX proof (bead vms-4ba.3, epic vms-4ba
# "self-host S2: a C compiler as an OVMX image"). Proves the vms-4ba.2 reloc-gap
# RULING (docs/design-tcc-object-compat.md) holds end-to-end for a REAL run:
# stock, UNMODIFIED tinycc's arm64 backend emits an ELF object that OVMX's
# LINK.EXE (as-is, no code change) accepts into a VMS-native EXECUTABLE image,
# which IMGACT.EXE then activates and runs correctly.
#
# WHAT THIS SCRIPT PROVES (mirrors src/imgact/test/run_dcl_native.sh's pattern —
# build-to-/tmp, hard FAIL on any DONE-condition miss, no repo writes):
#   1. `tcc -c hello.c -o hello.o` (DEFAULT flags — no -gstabs, no __thread) ->
#      an ELF64/REL/AArch64 object using ONLY the reloc set the vms-4ba.2 ruling
#      confirmed LINK.EXE already supports (SHT_RELA; ABS64/ADR_GOT_PAGE/CALL26/
#      LD64_GOT_LO12_NC/PREL32 — never ABS32/TLSLE).
#   2. `LINK.EXE --executable --use DECC$SHR --use LIBVMS$SHR
#      --use LIBVMSPROCESS$SHR --use LIBVMSFS$SHR --use LIBVMSLNM$SHR
#      --use LIBVMSRMS$SHR -o HELLO.EXE hello.o` (mirrors mk_dcl.sh:112-115) ->
#      HELLO.EXE, a VMS-native ET_DYN executable. crt0 is synthesized by
#      LINK.EXE for main() programs (link.c:992-1004) — hello.c is a normal
#      `int main(void)`, nothing hand-rolled.
#   3. IMGACT.EXE activates HELLO.EXE (no ld / no ld.so): it prints "hello" via
#      printf (a real DECC$SHR CALL26 import, proving tcc left libc calls as
#      real imports rather than inlining them) and exits 0.
#
# RESIDUAL EMPIRICAL RISK the vms-4ba.2 ruling flagged (§5 point 3), NOW CONFIRMED
# TO MANIFEST (2026-07-27, this bead): tcc routes ALL symbol addressing through
# the GOT (ADR_GOT_PAGE/LD64_GOT_LO12_NC — arm64-gen.c:495-508 `arm64_sym()`,
# UNCONDITIONAL for the ELF target, no invocation flag selects direct addressing;
# the only branch is `#ifdef TCC_TARGET_PE`, a tcc-build-time, not run-time,
# switch), INCLUDING locally-defined statics and per-TU string-literal labels
# (`L.n`). Even in a SINGLE-TU program (hello.c, no cross-TU collision possible)
# this already breaks LINK.EXE: hello.c's "hello\n" literal becomes a LOCAL
# symbol `L.1` (readelf -s: `LOCAL OBJECT ... Ndx=3(.data.ro) L.1`) referenced via
# ADR_GOT_PAGE/LD64_GOT_LO12_NC, and LINK.EXE's GOT-slot resolver
# (`resolve_named()` -> `sym_lookup()`, link.c:1406) only searches the GLOBAL
# symbol hash built by `build_symhash()`, which explicitly SKIPS STB_LOCAL binds
# (link.c:755: `if (bind == STB_LOCAL || s->st_shndx == SHN_UNDEF) continue;`).
# The GOT slot therefore resolves to "undefined" and LINK.EXE dies with
# `%LINK-F-ERROR, GOT symbol undefined` (link.c:1420) even though L.1 IS defined,
# just not in a name-hash the GOT-collection loop (link.c:1099-1121) was built to
# consult. This is a LINK.EXE-side resolution gap, not a reloc-TYPE gap (the
# ADR_GOT_PAGE/LD64_GOT_LO12_NC pair is already supported) — closing it needs
# either a LINK.EXE change (out of Systems file-domain for this bead) or a tcc
# backend change to arm64_sym() (real codegen work, not invocation-shaping). See
# the vms-4ba.3 escalation. This script's diagnostics below make the finding
# self-documenting from a clean checkout, and the LINK.EXE step is intentionally
# left able to hard-FAIL (TCC_EXPECT_LINK=1) so the exact failure reproduces.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here (the vms-4ba.2 ruling: no LINK.EXE
# change is needed for this bootstrap).
#
# TCC_EXPECT_LINK (default 1, mirrors DCL_EXPECT_LINK in run_dcl_native.sh): the
# vms-4ba.2 ruling asserts the round-trip should just work, so a LINK.EXE failure
# is treated as a real regression (hard FAIL) by default. Set TCC_EXPECT_LINK=0
# to soften ONLY the LINK.EXE step (and downstream activation, which cannot run
# without it) to a SKIP (exit 2) — e.g. while iterating on a NEW finding filed
# against the residual risk above — without masking every other assertion in
# this script, which stay hard FAILs regardless.
#
# arm64 musl Alpine container only (CLAUDE.md test loop). Needs root to create
# /vms. Run natively on an aarch64 host, or under arm64 emulation (binfmt/QEMU)
# on x86_64 (see .github/workflows/ci.yml).
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
TCC_DIR="$REPO/third-party/tcc"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
WORK=${WORK:-/tmp/tcc-object-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ;;
    *) echo "SKIP-FAIL: run_tcc_object_native.sh needs a native aarch64 host (got $(uname -m)) — tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m, and LINK.EXE/IMGACT.EXE are aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md / epic vms-4ba."; exit 1 ;;
esac

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== DECC\$SHR.EXE (whole-archive musl) =="
sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR no symbol vector"; exit 1; }

echo "== LIBVMSSYS\$SHR.EXE =="
SYSCFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$LIBVMSSYS_DIR"
SYSOBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
    $CC $SYSCFLAGS -c -o "$WORK/sys_$c.o" "$LIBVMSSYS_DIR/$c.c"
    SYSOBJS="$SYSOBJS $WORK/sys_$c.o"
done
$CC -fPIC -mno-outline-atomics -c -o "$WORK/sys_syscall.o" "$LIBVMSSYS_DIR/arch/aarch64/syscall.S"
SYSOBJS="$SYSOBJS $WORK/sys_syscall.o"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_dclast=PROCEDURE,vms_kif_setast=PROCEDURE,vms_kif_deliverast=PROCEDURE,vms_kif_lnm_define=PROCEDURE,vms_kif_lnm_delete=PROCEDURE,vms_kif_lnm_translate=PROCEDURE,vms_kif_mbx_create=PROCEDURE,vms_kif_mbx_assign=PROCEDURE,vms_kif_mbx_delmbx=PROCEDURE,vms_kif_mbx_write=PROCEDURE,vms_kif_mbx_read=PROCEDURE,vms_kif_p0_map=PROCEDURE,vms_kif_p0_unmap=PROCEDURE,vms_kif_p1_map=PROCEDURE,vms_kif_enter_image=PROCEDURE,vms_kif_image_rundown=PROCEDURE,vms_kif_p1_protect=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $SYSOBJS

echo "== LIBVMSPROCESS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

echo "== LIBVMSLNM\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

echo "== LIBVMSFS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
    "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

echo "== LIBVMS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_libvms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$LIBVMS_DIR"

echo "== LIBVMSRMS\$SHR.EXE =="
CC="$CC" sh "$LINK_DIR/mk_vmsrms_shr.sh" \
    "$WORK/LINK.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$VMSRMS_DIR" "$LIBVMS_INC" "$VMSFS_INC"
echo "-- full six-library producer graph linked VMS-native (same graph run_dcl_native.sh proves) --"

echo
echo "== build stock, UNMODIFIED tcc from the vendored source (third-party/tcc/src) =="
"${MAKE:-make}" -C "$TCC_DIR" CC="$CC" BUILD="$WORK/tcc-build" clean >/dev/null 2>&1 || true
"${MAKE:-make}" -C "$TCC_DIR" CC="$CC" BUILD="$WORK/tcc-build"
TCC="$WORK/tcc-build/tcc"
[ -x "$TCC" ] || { echo "FAIL: tcc binary not produced at $TCC"; exit 1; }
"$TCC" -v

echo
echo "== tcc -c hello.c -o hello.o  (DEFAULT flags only — DONE condition 1) =="
cat > "$WORK/hello.c" <<'EOF'
#include <stdio.h>
int main(void)
{
    printf("hello\n");
    return 0;
}
EOF
# NO -gstabs (would emit ABS32 debug relocs LINK.EXE rejects — vms-4ba.2 §4.1),
# NO __thread anywhere (would emit TLSLE relocs LINK.EXE lacks — §4.3). Default
# `tcc -c` leaves mem*/str*/printf as real CALL26 imports (tcc does not inline
# libc builtins by default) — the compile-equivalent of gcc's -fno-builtin.
"$TCC" -c "$WORK/hello.c" -o "$WORK/hello.o"
[ -f "$WORK/hello.o" ] || { echo "FAIL: hello.o not produced"; exit 1; }

echo "-- hello.o: ELF64 / REL / AArch64 --"
readelf -h "$WORK/hello.o" | tee "$WORK/hello.readelf.h"
grep -q 'Class:.*ELF64' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not ELF64"; exit 1; }
grep -q 'Type:.*REL (Relocatable file)' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not ET_REL"; exit 1; }
grep -q 'Machine:.*AArch64' "$WORK/hello.readelf.h" || { echo "FAIL: hello.o is not EM_AARCH64"; exit 1; }

echo "-- hello.o section headers: confirm SHT_RELA (never SHT_REL) — vms-4ba.2 §1.1 --"
readelf -S "$WORK/hello.o" | tee "$WORK/hello.readelf.S"
grep -q 'RELA' "$WORK/hello.readelf.S" || { echo "FAIL: hello.o carries no RELA section (unexpected — ruling expects SHT_RELA on aarch64)"; exit 1; }
grep -qE '\bREL\b[^A]' "$WORK/hello.readelf.S" && { echo "FAIL: hello.o carries an SHT_REL section — LINK.EXE rejects SHT_REL (link.c:239-240,285)"; exit 1; }
echo "-- confirmed: RELA present, no bare REL section --"

echo "-- hello.o reloc histogram: confirm ONLY the ruling's supported set (no ABS32/TLSLE) --"
readelf -r "$WORK/hello.o" | tee "$WORK/hello.readelf.r"
readelf -r "$WORK/hello.o" | awk '/R_AARCH64/{print $3}' | sed 's/[[:space:]].*//' | sort | uniq -c | sed 's/^/   /'
grep -qE 'R_AARCH64_ABS32\b' "$WORK/hello.readelf.r" && { echo "FAIL: hello.o carries ABS32 (STABS leaked into the recipe — vms-4ba.2 §4.1)"; exit 1; }
grep -qE 'R_AARCH64_TLSLE' "$WORK/hello.readelf.r" && { echo "FAIL: hello.o carries TLSLE relocs (unexpected __thread use — vms-4ba.2 §4.3); LINK.EXE has no TLSLE support"; exit 1; }
echo "-- confirmed: no ABS32, no TLSLE --"

echo "-- confirm printf stayed a real CALL26 import (tcc did not inline it) --"
readelf -s "$WORK/hello.o" | grep -qE '\bUND\b.*\bprintf\b|\bprintf\b.*\bUND\b' \
  || { echo "FAIL: hello.o has no undefined printf symbol — tcc may have inlined/resolved it, breaking the DECC\$SHR import path"; exit 1; }
echo "-- confirmed: printf is UND (an unresolved import DECC\$SHR must satisfy) --"

echo "-- residual-risk check (vms-4ba.2 §5.3): does hello.o GOT-reference any LOCAL-bind symbol by name? --"
# readelf -r columns: Offset Info Type Sym.Value Sym.Name + Addend ($5 = name;
# the Type column ($3) is truncated by readelf's fixed-width print, e.g.
# "R_AARCH64_ADR_GOT" for ADR_GOT_PAGE and "R_AARCH64_LD64_GO" for
# LD64_GOT_LO12_NC — match the stable truncation-safe prefixes, not full names.
readelf -r "$WORK/hello.o" | awk '/R_AARCH64_ADR_GOT|R_AARCH64_LD64_GOT/{print $5}' | sort -u > "$WORK/got_syms.txt"
LOCAL_GOT=""
while read -r nm; do
    [ -z "$nm" ] && continue
    # readelf -s columns: Num Value Size Type Bind Vis Ndx Name ($5=Bind, $NF=Name)
    readelf -s "$WORK/hello.o" | awk -v n="$nm" '$5=="LOCAL" && $NF==n{f=1} END{exit !f}' && LOCAL_GOT="$LOCAL_GOT $nm"
done < "$WORK/got_syms.txt"
if [ -n "$LOCAL_GOT" ]; then
    echo "   PRESENT: hello.o GOT-references LOCAL-bind symbol(s):$LOCAL_GOT — the"
    echo "   vms-4ba.2 §5.3 residual risk. Pre-vms-9c1 this hard-failed: LINK.EXE's"
    echo "   build_symhash() skips STB_LOCAL (link.c:755) so resolve_named() could not"
    echo "   find these defined locals ('%LINK-F-ERROR, GOT symbol undefined'). vms-9c1"
    echo "   fixed it: LINK.EXE now gives each LOCAL-bind GOT reference a per-object"
    echo "   (obj,sym) slot resolved via placed_addr(), so the LINK.EXE step below"
    echo "   RESOLVES these locals and links cleanly (hard gate under TCC_EXPECT_LINK=1)."
else
    echo "   No LOCAL-bind GOT-referenced symbols in this particular hello.o build — the"
    echo "   residual risk did not manifest for this exact program (compiler-version /"
    echo "   optimization dependent). Proceeding to the real LINK.EXE step."
fi

echo
echo "== link HELLO.EXE VMS-native: LINK.EXE --executable --use {6 producers} (mirrors mk_dcl.sh:112-115) — DONE condition 2 =="
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$SYSEXE/HELLO.EXE" "$WORK/hello.o" 2>"$WORK/link.err"
LRC=$?
set -e
echo "-- LINK.EXE --executable exit=$LRC; message: --"
tail -5 "$WORK/link.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${TCC_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: HELLO.EXE link failed. CONFIRMED CAUSE (vms-4ba.3, 2026-07-27): tcc's"
        echo "  arm64 backend (arm64-gen.c:495-508 arm64_sym(), unconditional for ELF, no"
        echo "  invocation flag disables it) GOT-references EVERY symbol including locally-"
        echo "  defined statics/string-literal labels (here: 'hello\\n' -> LOCAL symbol L.1)."
        echo "  LINK.EXE's GOT-slot resolver (resolve_named()/sym_lookup(), link.c:1406)"
        echo "  only searches the GLOBAL symbol hash built by build_symhash(), which"
        echo "  deliberately skips STB_LOCAL binds (link.c:755) — so it reports the (very"
        echo "  much defined) L.1 as an undefined GOT import: '%LINK-F-ERROR, GOT symbol"
        echo "  undefined'. This is a LINK.EXE-side resolution gap (the ADR_GOT_PAGE/"
        echo "  LD64_GOT_LO12_NC reloc TYPES are already supported), not a reloc-type gap,"
        echo "  and it is NOT fixable by tcc invocation-shaping (no flag suppresses GOT-"
        echo "  addressing of locals; confirmed by reading arm64-gen.c). Fixing it needs"
        echo "  either a LINK.EXE change (out of Systems file-domain for this bead) or real"
        echo "  tcc backend codegen work (also out of scope per the escalation tripwire)."
        echo "  Do NOT patch link.c/imgact.c here — see the vms-4ba.3 escalation."
        exit 1
    fi
    echo "SKIP (TCC_EXPECT_LINK=0): HELLO.EXE link failed but the assertion is disabled --"
    echo "  known, escalated architecture gap (vms-4ba.3): LINK.EXE's GOT-slot resolver"
    echo "  cannot resolve tcc's GOT-referenced LOCAL symbols (build_symhash skips"
    echo "  STB_LOCAL). Everything up to this point (tcc build, hello.o codegen shape,"
    echo "  RELA-only / no-ABS32 / no-TLSLE / printf-stays-UND, the six-library producer"
    echo "  graph) is proven for real above. Flip TCC_EXPECT_LINK=1 to reproduce the hard"
    echo "  failure once link.c's GOT resolution is fixed to re-check this DONE condition."
    exit 2
fi
readelf -lW "$SYSEXE/HELLO.EXE" | grep -q 'INTERP' || { echo "FAIL: HELLO.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/HELLO.EXE"

echo
echo "== HELLO.EXE linked — activate through IMGACT.EXE — DONE condition 3 =="
set +e
"$SYSEXE/HELLO.EXE" > "$WORK/hello.out" 2>&1
RC=$?
set -e
echo "-- program output: --"; sed 's/^/   /' "$WORK/hello.out"
echo "exit code = $RC"
grep -q 'hello' "$WORK/hello.out" || { echo "FAIL: HELLO.EXE did not print 'hello'"; exit 1; }
[ "$RC" -eq 0 ] || { echo "FAIL: HELLO.EXE did not exit clean (got $RC)"; exit 1; }

echo
echo "================================================================================"
echo "== CROSS-TU LOCAL-COLLISION test (vms-9c1 DONE condition 2) =="
echo "================================================================================"
# Two translation units, compiled SEPARATELY by tcc. Each independently defines
# its own static object AND its own string literal — tcc's per-compilation label
# counter resets each `tcc -c`, so BOTH TUs emit a local `L.1`, `L.2`, ... . tcc
# GOT-references every one of them (arm64-gen.c:495-508). If LINK.EXE keyed local
# GOT slots by NAME (the pre-vms-9c1 bug would instead not resolve them at all;
# a naive name-keyed fix would COLLIDE), tu_a's `L.1` and tu_b's `L.1` would share
# a single GOT slot and one TU would read the OTHER's data. vms-9c1 gives every
# LOCAL-bind GOT reference a PER-OBJECT (oi, symidx) slot, so the two same-named
# locals resolve to DISTINCT addresses. The program below OBSERVES both correct,
# distinct values (its own tag string + its own static int); any collision would
# make a_report() print b_tag / b_val (or vice-versa) or mis-return, and the
# activated image would exit nonzero. This is a REAL activation, not a mock.
cat > "$WORK/tu_a.c" <<'EOF'
#include <stdio.h>
static const char a_tag[] = "AAA-local-a";   /* local object -> L.n, GOT-ref'd */
static int a_val = 111;                       /* local static  -> GOT-ref'd     */
int a_report(void)
{
    printf("A:%s:%d\n", a_tag, a_val);        /* format string is ALSO a local L.n */
    return a_val;
}
EOF
cat > "$WORK/tu_b.c" <<'EOF'
#include <stdio.h>
static const char b_tag[] = "BBB-local-b";   /* independent TU: its OWN L.1 too */
static int b_val = 222;
int b_report(void)
{
    printf("B:%s:%d\n", b_tag, b_val);
    return b_val;
}
EOF
cat > "$WORK/xtu_main.c" <<'EOF'
#include <stdio.h>
extern int a_report(void);
extern int b_report(void);
int main(void)
{
    int a = a_report();
    int b = b_report();
    /* If the two TUs' local L.n GOT slots collided, one of these is wrong. */
    if (a != 111) { printf("BAD: a_val=%d (expected 111)\n", a); return 3; }
    if (b != 222) { printf("BAD: b_val=%d (expected 222)\n", b); return 4; }
    printf("XTU-OK\n");
    return 0;
}
EOF
"$TCC" -c "$WORK/tu_a.c"     -o "$WORK/tu_a.o"
"$TCC" -c "$WORK/tu_b.c"     -o "$WORK/tu_b.o"
"$TCC" -c "$WORK/xtu_main.c" -o "$WORK/xtu_main.o"

echo "-- confirm BOTH tu_a.o and tu_b.o GOT-reference a LOCAL symbol named 'L.1' (the collision the fix must survive) --"
for o in tu_a tu_b; do
    readelf -s "$WORK/$o.o" | awk '$5=="LOCAL" && $NF=="L.1"{f=1} END{exit !f}' \
      || echo "   NOTE: $o.o has no LOCAL 'L.1' (tcc label naming differs in this build) — the test still exercises distinct per-TU locals"
done

echo "-- link XTU.EXE from THREE tcc objects (each with its own local L.n) — vms-9c1 --"
set +e
"$WORK/LINK.EXE" --executable \
    --use "$SYSLIB/DECC\$SHR.EXE" --use "$SYSLIB/LIBVMS\$SHR.EXE" --use "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    --use "$SYSLIB/LIBVMSFS\$SHR.EXE" --use "$SYSLIB/LIBVMSLNM\$SHR.EXE" --use "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    -o "$SYSEXE/XTU.EXE" "$WORK/xtu_main.o" "$WORK/tu_a.o" "$WORK/tu_b.o" 2>"$WORK/xtu_link.err"
XLRC=$?
set -e
echo "-- LINK.EXE --executable (cross-TU) exit=$XLRC; message: --"
tail -5 "$WORK/xtu_link.err" | sed 's/^/   /'
[ "$XLRC" -eq 0 ] || { echo "FAIL: XTU.EXE link failed — vms-9c1 local GOT resolution regression"; exit 1; }
chmod +x "$SYSEXE/XTU.EXE"

echo "-- activate XTU.EXE through IMGACT.EXE and observe BOTH distinct locals --"
set +e
"$SYSEXE/XTU.EXE" > "$WORK/xtu.out" 2>&1
XRC=$?
set -e
echo "-- program output: --"; sed 's/^/   /' "$WORK/xtu.out"
echo "exit code = $XRC"
grep -q '^A:AAA-local-a:111$' "$WORK/xtu.out" || { echo "FAIL: TU A did not observe its OWN local tag/value (cross-TU local GOT slots collided)"; exit 1; }
grep -q '^B:BBB-local-b:222$' "$WORK/xtu.out" || { echo "FAIL: TU B did not observe its OWN local tag/value (cross-TU local GOT slots collided)"; exit 1; }
grep -q '^XTU-OK$'            "$WORK/xtu.out" || { echo "FAIL: XTU.EXE did not reach XTU-OK (a local resolved to the wrong address)"; exit 1; }
[ "$XRC" -eq 0 ] || { echo "FAIL: XTU.EXE did not exit clean (got $XRC) — a cross-TU local resolved wrong"; exit 1; }
echo "-- confirmed: two separately-compiled TUs' same-named locals resolved to DISTINCT addresses --"

echo
echo "MILESTONE (vms-4ba.3 + vms-9c1, S2 CRUX): stock, UNMODIFIED tinycc ($(sed -n 's/^Pinned commit:\s*//p' "$TCC_DIR/VENDOR-REV")),"
echo "compiling with DEFAULT flags (no -gstabs, no __thread), emits aarch64 ELF"
echo "objects that OVMX's LINK.EXE links (single-TU HELLO.EXE and multi-TU XTU.EXE)"
echo "into VMS-native EXECUTABLE images, which IMGACT.EXE activates — printf runs"
echo "through DECC\$SHR, no ld / no ld.so. vms-9c1 closed the last gap: LINK.EXE now"
echo "resolves tcc's GOT references to DEFINED LOCAL symbols (statics, string-literal"
echo "labels 'L.n') via per-object (obj,sym) slots, so cross-TU 'L.1' collisions are"
echo "impossible. The vms-4ba.2 reloc-gap ruling holds end-to-end. Unblocks"
echo "vms-4ba.4 (tcc itself running AS an OVMX image)."
