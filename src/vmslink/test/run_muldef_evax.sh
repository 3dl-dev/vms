#!/bin/sh
# run_muldef_evax.sh — LINK.EXE STRONG-vs-STRONG multiple-definition gate on the
# EVAX/Alpha path (vms-f1a). The Alpha mirror of the ELF-path gate proven by
# run_muldef_x86_64.sh (vms-d8d, #777).
#
# THE GAP this closes: LINK.EXE's EVAX/Alpha path resolves a symbol name with
# evax_find_sym() — a LINEAR FIRST-MATCH over every input's defined symbols with
# NO duplicate check. Two whole-archived objects that both STRONGLY define the
# same universal/global therefore bind SILENTLY to whichever appears first by
# link-input order, with no diagnostic anywhere. That is the exact latent
# correctness bug the ELF path's sym_insert() guard closed (#777): two addresses
# for one name in one link, discoverable only by symptom.
#
# THE FIX: emit_evax_common() now runs a ONE-TIME pre-pass (evax_check_muldef)
# over all inputs BEFORE layout/resolution that hard-errors %LINK-F-MULDEF
# (non-zero exit) the instant two DIFFERENT inputs both define a name and BOTH
# defs are STRONG (not EGSY__V_WEAK). WEAK-vs-STRONG / WEAK-vs-WEAK / a single
# def are UNCHANGED (strong/first still wins silently — correct VMS/ELF symbol
# resolution, load-bearing for musl-alpha's weak_alias overridable defs).
#
# WHY WHOLE-ARCHIVE-SAFE (the correctness property the joint-e2e job verifies):
# a well-formed library carries no strong-vs-strong dups — overridable defs
# (malloc, ...) are WEAK, and the one known alpha collision (decc$_malloc64) is
# #ifndef __VMS__-excluded on alpha. So the genuine alpha DECC$SHR whole-archive
# build sees ZERO spurious MULDEF (proven by joint-e2e-alpha-crt0, vms-864).
#
# This script proves BOTH halves with REAL alpha-dec-vms EVAX objects, compiled
# fresh by the cross cc1 (no object blobs checked in):
#   1. POSITIVE: two objects each STRONGLY define `dup_sym` -> LINK.EXE must fail
#      %LINK-F-MULDEF, naming the symbol and BOTH defining objects, exit non-zero.
#   2. CONTROL: one object WEAKLY defines `dup_sym`, the other STRONGLY (both link
#      orders) -> LINK.EXE must still SUCCEED (weak yields to strong, unchanged),
#      proving the new check is scoped to STRONG-vs-STRONG only.
#
# Runs inside the tools/cross-alpha-vms toolchain container (the alpha-dec-vms
# cross cc1 + a host gcc for LINK.EXE itself). Containerized, build-to-/tmp,
# Rule-9-clean build/oracle tooling — nothing runs inside an OVMX guest.
set -e
CC=${CC:-gcc}
ALPHA_CC=${ALPHA_CC:-alpha-dec-vms-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test-muldef-evax}
rm -rf "$WORK"; mkdir -p "$WORK"

# Sanity: the cross cc1 must be reachable (this test needs REAL EVAX objects).
command -v "$ALPHA_CC" >/dev/null 2>&1 || {
    echo "FAIL: $ALPHA_CC not found on PATH — run this inside the"
    echo "      tools/cross-alpha-vms toolchain container (the CI"
    echo "      joint-e2e-alpha-crt0 job builds/loads that image)."
    exit 1
}

echo "== build LINK.EXE (host tool) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

# Compile a C source into a REAL alpha-dec-vms EVAX object (64-bit pointer ABI,
# the port's identity; -g0 keeps the object lean).
evax_cc() { "$ALPHA_CC" -mpointer-size=64 -g0 -c "$1" -o "$2"; }

echo
echo "== POSITIVE: two EVAX objects both STRONGLY define the same symbol =="
cat > "$WORK/strong_a.c" <<'EOF'
int dup_sym(int x) { return x + 1; }
int caller_a(int x) { return dup_sym(x) + 100; }
EOF
cat > "$WORK/strong_b.c" <<'EOF'
int dup_sym(int x) { return x + 2; }   /* SAME name, DIFFERENT strong def */
int caller_b(int x) { return dup_sym(x) + 200; }
EOF
evax_cc "$WORK/strong_a.c" "$WORK/strong_a.obj"
evax_cc "$WORK/strong_b.c" "$WORK/strong_b.obj"

set +e
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "caller_a=PROCEDURE,caller_b=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/MULDEF\$SHR.EXE" \
    "$WORK/strong_a.obj" "$WORK/strong_b.obj" 2>"$WORK/muldef.err"
RC=$?
set -e
cat "$WORK/muldef.err"
[ "$RC" -ne 0 ] || { echo "FAIL: STRONG-vs-STRONG duplicate definition of dup_sym was NOT rejected (silent first-wins regression)"; exit 1; }
grep -q "%LINK-F-MULDEF" "$WORK/muldef.err" \
    || { echo "FAIL: rejection did not carry the %LINK-F-MULDEF diagnostic"; exit 1; }
grep -q "dup_sym" "$WORK/muldef.err" \
    || { echo "FAIL: %LINK-F-MULDEF diagnostic did not name the colliding symbol"; exit 1; }
grep -q "strong_a.obj" "$WORK/muldef.err" && grep -q "strong_b.obj" "$WORK/muldef.err" \
    || { echo "FAIL: %LINK-F-MULDEF diagnostic did not name BOTH defining objects"; exit 1; }
echo "MULDEF fired correctly (exit $RC): $(cat "$WORK/muldef.err")"

echo
echo "== CONTROL: WEAK def yields to STRONG def, either link order (unchanged) =="
cat > "$WORK/weak_def.c" <<'EOF'
__attribute__((weak)) int dup_sym(int x) { return x + 1000; }  /* WEAK */
int use_weak_side(int x) { return dup_sym(x); }
EOF
cat > "$WORK/strong_def.c" <<'EOF'
int dup_sym(int x) { return x + 2000; }                        /* STRONG */
int use_strong_side(int x) { return dup_sym(x); }
EOF
evax_cc "$WORK/weak_def.c" "$WORK/weak_def.obj"
evax_cc "$WORK/strong_def.c" "$WORK/strong_def.obj"

# weak object FIRST, strong object SECOND -> must SUCCEED (strong wins, no MULDEF)
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "use_weak_side=PROCEDURE,use_strong_side=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/WEAKFIRST\$SHR.EXE" \
    "$WORK/weak_def.obj" "$WORK/strong_def.obj" >/dev/null
echo "weak-then-strong EVAX link OK (unchanged weak-override behavior)"

# strong object FIRST, weak object SECOND -> must ALSO succeed (order-independent)
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "use_weak_side=PROCEDURE,use_strong_side=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/STRONGFIRST\$SHR.EXE" \
    "$WORK/strong_def.obj" "$WORK/weak_def.obj" >/dev/null
echo "strong-then-weak EVAX link OK (unchanged weak-override behavior, order-independent)"

echo
echo "== CONTROL 2: two STRONG defs in the SAME archive -> exempt (vms-f1a) =="
# VMS library search resolves a duplicate by first-MODULE-wins and never errors
# on a second definition; OVMX whole-archives as policy but the intent is
# identical (first-wins, the shipped behavior). Two members of ONE archive that
# both STRONGLY define a symbol must LINK, NOT %LINK-F-MULDEF. This is the
# whole-archive-safety property the genuine alpha DECC$SHR build also exercises:
# on alpha long double == double, so the cc1 decorates musl's *l math variants
# (cacoshl, ...) to their double counterpart's MATH$*_T symbol, both strong,
# same archive. (The GENUINE conflict — two EXPLICIT objects, or two DIFFERENT
# archives — still fires; that is the POSITIVE case above.)
ar rcs "$WORK/duplib.a" "$WORK/strong_a.obj" "$WORK/strong_b.obj"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "caller_a=PROCEDURE,caller_b=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/SAMEARCH\$SHR.EXE" \
    "$WORK/duplib.a" >/dev/null
echo "same-archive strong dup EVAX link OK (first-module-wins, no MULDEF — vms-f1a exemption)"

echo
echo "== POSITIVE 2: strong dup across TWO DIFFERENT archives -> MUST fire (vms-f1a) =="
# A same-symbol strong def in two DIFFERENT archives is a genuine conflict, NOT a
# single library's first-module-wins, so it must still fire — the exemption is
# scoped to same-archive-member ONLY, never "any strong dup".
ar rcs "$WORK/lib1.a" "$WORK/strong_a.obj"
ar rcs "$WORK/lib2.a" "$WORK/strong_b.obj"
set +e
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "caller_a=PROCEDURE,caller_b=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/XARCH\$SHR.EXE" \
    "$WORK/lib1.a" "$WORK/lib2.a" 2>"$WORK/xarch.err"
RC2=$?
set -e
cat "$WORK/xarch.err"
[ "$RC2" -ne 0 ] \
    || { echo "FAIL: cross-archive STRONG dup of dup_sym was NOT rejected (exemption too broad)"; exit 1; }
grep -q "%LINK-F-MULDEF" "$WORK/xarch.err" \
    || { echo "FAIL: cross-archive dup rejection did not carry %LINK-F-MULDEF"; exit 1; }
echo "cross-archive strong dup correctly REJECTED (exemption is same-archive-member-only)"

echo
echo "ALL LINK.EXE EVAX MULDEF CHECKS PASSED"
