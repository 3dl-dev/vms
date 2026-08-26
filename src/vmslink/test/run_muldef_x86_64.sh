#!/bin/sh
# run_muldef_x86_64.sh — LINK.EXE STRONG-vs-STRONG multiple-definition gate
# (vms-d8d).
#
# THE GAP this closes: LINK.EXE's ELF path (x86_64/aarch64) previously picked
# a winner SILENTLY when two whole-archived objects both STRONGLY (STB_GLOBAL)
# defined the same universal/global symbol — whichever def sym_insert() saw
# FIRST (incidental link-input order), with no diagnostic anywhere. That is a
# latent correctness bug, not just a missing nicety: a strong SELF-reference
# inside one of the two objects (resolve_ref, link.c ~1531) never consults the
# global symbol hash for STRONG symbols — it binds directly to that object's
# own placed_addr — while every cross-object reference and the exported
# .vms$sv universal bind whichever def sym_insert() inserted first. Two
# different addresses for one name in one link, discoverable only by symptom.
#
# THE FIX: sym_insert() now hard-errors (%LINK-F-MULDEF, non-zero exit) the
# instant it sees a second STRONG def of a name already STRONG-defined by a
# prior object — matching real `ld`'s "multiple definition of `sym'" behavior.
# The pre-existing WEAK-def-yields-to-STRONG-def override is UNCHANGED (a
# strong def still silently wins over a weak one of the same name — that is
# correct ELF symbol resolution and is load-bearing for musl's
# __libc_malloc_impl weak/strong alias split, vms-36a).
#
# This script proves BOTH halves:
#   1. POSITIVE: two objects that each STRONGLY define `dup_sym` -> LINK.EXE
#      must fail with %LINK-F-MULDEF naming the symbol and BOTH defining
#      objects, and exit non-zero.
#   2. REGRESSION: one object WEAKLY defines `dup_sym`, the other STRONGLY
#      defines it (order: weak object first, strong object second, and then
#      the reverse order too) -> LINK.EXE must still SUCCEED, with the strong
#      def winning (unchanged pre-existing behavior) — proving the new check
#      is scoped to STRONG-vs-STRONG only, not misfiring on the weak-override
#      case it must not touch.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test-muldef}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo
echo "== POSITIVE: two objects both STRONGLY define the same symbol =="
cat > "$WORK/strong_a.c" <<'EOF'
int dup_sym(int x) { return x + 1; }
int caller_a(int x) { return dup_sym(x) + 100; }
EOF
cat > "$WORK/strong_b.c" <<'EOF'
int dup_sym(int x) { return x + 2; }   /* SAME name, DIFFERENT strong def */
int caller_b(int x) { return dup_sym(x) + 200; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/strong_a.o" "$WORK/strong_a.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/strong_b.o" "$WORK/strong_b.c"
readelf -sW "$WORK/strong_a.o" | grep -w dup_sym | grep -qi "GLOBAL" \
    || { echo "FAIL: dup_sym in strong_a.o is not STB_GLOBAL (test setup broken)"; exit 1; }
readelf -sW "$WORK/strong_b.o" | grep -w dup_sym | grep -qi "GLOBAL" \
    || { echo "FAIL: dup_sym in strong_b.o is not STB_GLOBAL (test setup broken)"; exit 1; }

set +e
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "caller_a=PROCEDURE,caller_b=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/MULDEF\$SHR.EXE" \
    "$WORK/strong_a.o" "$WORK/strong_b.o" 2>"$WORK/muldef.err"
RC=$?
set -e
cat "$WORK/muldef.err"
[ "$RC" -ne 0 ] || { echo "FAIL: STRONG-vs-STRONG duplicate definition of dup_sym was NOT rejected (silent first-wins regression)"; exit 1; }
grep -q "%LINK-F-MULDEF" "$WORK/muldef.err" \
    || { echo "FAIL: rejection did not carry the %LINK-F-MULDEF diagnostic"; exit 1; }
grep -q "dup_sym" "$WORK/muldef.err" \
    || { echo "FAIL: %LINK-F-MULDEF diagnostic did not name the colliding symbol"; exit 1; }
grep -q "strong_a.o" "$WORK/muldef.err" && grep -q "strong_b.o" "$WORK/muldef.err" \
    || { echo "FAIL: %LINK-F-MULDEF diagnostic did not name BOTH defining objects"; exit 1; }
echo "MULDEF fired correctly (exit $RC): $(cat "$WORK/muldef.err")"

echo
echo "== REGRESSION: WEAK def yields to STRONG def, either link order (unchanged) =="
cat > "$WORK/weak_def.c" <<'EOF'
__attribute__((weak)) int shared_pick(int x) { return x + 1000; }  /* WEAK */
int use_weak_side(int x) { return shared_pick(x); }
EOF
cat > "$WORK/strong_def.c" <<'EOF'
int shared_pick(int x) { return x + 2000; }                        /* STRONG */
int use_strong_side(int x) { return shared_pick(x); }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/weak_def.o" "$WORK/weak_def.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/strong_def.o" "$WORK/strong_def.c"
readelf -sW "$WORK/weak_def.o" | grep -w shared_pick | grep -qi "WEAK" \
    || { echo "FAIL: shared_pick in weak_def.o is not STB_WEAK (test setup broken)"; exit 1; }
readelf -sW "$WORK/strong_def.o" | grep -w shared_pick | grep -qi "GLOBAL" \
    || { echo "FAIL: shared_pick in strong_def.o is not STB_GLOBAL (test setup broken)"; exit 1; }

# weak object FIRST, strong object SECOND
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "use_weak_side=PROCEDURE,use_strong_side=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/WEAKFIRST\$SHR.EXE" \
    "$WORK/weak_def.o" "$WORK/strong_def.o"
echo "weak-then-strong link OK (unchanged weak-override behavior)"

# strong object FIRST, weak object SECOND — must ALSO succeed (order-independent)
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "use_weak_side=PROCEDURE,use_strong_side=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/STRONGFIRST\$SHR.EXE" \
    "$WORK/strong_def.o" "$WORK/weak_def.o"
echo "strong-then-weak link OK (unchanged weak-override behavior, order-independent)"

echo
echo "ALL LINK.EXE MULDEF CHECKS PASSED"
