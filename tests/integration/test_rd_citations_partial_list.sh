#!/bin/bash
#
# test_rd_citations_partial_list.sh -- absence from `rd list` must not mean
# CLOSED (rd vms-10c).
#
# ---------------------------------------------------------------------------
# THE DEFECT THIS EXISTS FOR
# ---------------------------------------------------------------------------
#
# tools/gen_rd_citations.py records an id `open` if and only if it appears in
# `rd list`, and everything else it can still `rd show` it wrote as `closed`.
# That is sound only if `rd list` is COMPLETE. It is not: rd resolves which
# board it is talking to from the WORKING DIRECTORY, and the same board
# answered 289 items from the repo root and 159 from a clone of the same repo
# at another path -- same machine, same second, stable across repeated runs
# from each. This is deterministic cwd-dependence, not flakiness.
#
# Run against a sandbox copy -- which is exactly what the freshness suite does
# -- genuinely-active items fell into the `closed` branch. OBSERVED on vms-as1
# and vms-pv1, both cited by OVMX-UNWIRED declarations: had that ledger been
# committed it would have redded every gate that reads it, and the obvious way
# to "fix" that red is to hand-edit rows, which is the one thing the mechanism
# cannot detect.
#
# THE ROW IS SELF-CONTRADICTORY AND THAT IS THE TELL: `closed  active`. A
# closed item does not have status `active`.
#
# ---------------------------------------------------------------------------
# WHY THIS TEST STUBS rd INSTEAD OF USING IT
# ---------------------------------------------------------------------------
#
# The defect is a disagreement BETWEEN two rd answers -- what `rd list`
# returns and what `rd show` returns. Reproducing it against the real rd means
# reproducing a partial list, which means depending on cwd-dependence: the very
# ambient condition the fix exists to stop trusting. A test whose setup is the
# bug is not a test.
#
# So `rd` is stubbed: a script on PATH that answers `list` and `show` from
# fixture files this test writes. That makes every case exact and, unlike every
# other member of this family, makes it RUNNABLE WITHOUT rd -- so it runs in
# CI, where test_rd_citations_fresh.sh skips on every single run (project rule
# 7: a skipped test is a failing test, and that skip is a standing residual).
# This does not replace that test. It covers one property that test cannot
# reach, on hosts that test cannot run on.
#
# Usage: test_rd_citations_partial_list.sh [repo-root]
set -u

REPO_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GEN="$REPO_ROOT/tools/gen_rd_citations.py"
[ -f "$GEN" ] || { echo "FATAL: $GEN not found"; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/rdpartial.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; [ -n "${2:-}" ] && echo "        $2"; }

# --------------------------------------------------------------------------
# The stub rd.
#
# RD_LIST_IDS   space-separated ids `rd list --json` reports (each as status
#               `inbox`), or empty for an empty list.
# RD_SHOW_MAP   "id:status" pairs `rd show <id> --json` resolves. An id absent
#               from the map makes `rd show` exit 1, which is rd's own answer
#               for an item it does not have.
# --------------------------------------------------------------------------
mkdir -p "$WORK/bin"
cat > "$WORK/bin/rd" <<'STUB'
#!/bin/bash
# stub rd -- answers from RD_LIST_IDS / RD_SHOW_MAP (test_rd_citations_partial_list.sh)
cmd="${1:-}"
case "$cmd" in
  list)
    printf '['
    first=1
    for id in ${RD_LIST_IDS:-}; do
      [ $first -eq 1 ] || printf ','
      first=0
      printf '{"id":"%s","status":"inbox","title":"stub open item %s"}' "$id" "$id"
    done
    printf ']\n'
    exit 0 ;;
  show)
    want="${2:-}"
    for pair in ${RD_SHOW_MAP:-}; do
      pid="${pair%%:*}"; pst="${pair##*:}"
      if [ "$pid" = "$want" ]; then
        printf '{"id":"%s","status":"%s","title":"stub item %s"}\n' "$pid" "$pst" "$pid"
        exit 0
      fi
    done
    exit 1 ;;
esac
exit 1
STUB
chmod +x "$WORK/bin/rd"

# --------------------------------------------------------------------------
# A minimal tree carrying exactly one declaration, citing one id.
# --------------------------------------------------------------------------
make_tree() {   # make_tree <dir> <cited-id>
    mkdir -p "$1/src" "$1/tools" "$1/tracking"
    cat > "$1/src/decl.c" <<EOF
/* OVMX-EXECUTIVE: sys\$stub ($2) proof=tests/qemu/test_stub.c -- fixture */
EOF
}

# run_gen <tree> <list-ids> <show-map> -> sets RC, OUT (stdout+stderr), LEDGER
run_gen() {
    LEDGER="$1/tracking/rd-citations.tsv"
    rm -f "$LEDGER"
    OUT=$(PATH="$WORK/bin:$PATH" RD_LIST_IDS="$2" RD_SHOW_MAP="$3" \
          python3 "$GEN" --root "$1" 2>&1)
    RC=$?
}

# ==========================================================================
echo "test_rd_citations_partial_list: absence from \`rd list\` is not CLOSED (vms-10c)"
echo

# --- A. CONTROL: the cited id IS in the list -------------------------------
# Without this, every refusal below could be a generator that refuses always.
T="$WORK/a"; make_tree "$T" vms-aaa
run_gen "$T" "vms-aaa vms-bbb" "vms-aaa:inbox"
if [ "$RC" -eq 0 ] && grep -q "^vms-aaa	open	inbox" "$LEDGER" 2>/dev/null; then
    ok "A control: an id present in \`rd list\` is written open (rc=0)"
else
    bad "A control: a complete list must still produce an open row (rc=$RC)" "$OUT"
fi

# --- B. THE DEFECT: absent from the list, but rd says it is ACTIVE ---------
# This is vms-10c exactly. Before the fix this wrote `closed  active`.
T="$WORK/b"; make_tree "$T" vms-aaa
run_gen "$T" "vms-bbb" "vms-aaa:active"
if [ "$RC" -ne 0 ]; then
    ok "B: a live item missing from a PARTIAL list is REFUSED, not written closed (rc=$RC)"
else
    bad "B: the generator accepted a live item as closed (rc=0)" "$OUT"
fi
if [ ! -s "$LEDGER" ]; then
    ok "B: and no ledger was written -- a refusal must not leave a corrupt one"
else
    bad "B: a ledger was written despite the contradiction" "$(cat "$LEDGER")"
fi
if printf '%s' "$OUT" | grep -q "active"; then
    ok "B: the refusal names the contradicting status, so the cause is readable"
else
    bad "B: the refusal did not name the status it refused on" "$OUT"
fi

# --- B2. every open status rd actually uses takes the same path ------------
# `active` alone would let a fix that special-cases one status pass.
for st in inbox blocked waiting active; do
    T="$WORK/b2-$st"; make_tree "$T" vms-aaa
    run_gen "$T" "vms-bbb" "vms-aaa:$st"
    if [ "$RC" -ne 0 ]; then
        ok "B2/$st: an item whose status is \`$st\` is refused, not written closed"
    else
        bad "B2/$st: status \`$st\` was accepted as closed" "$OUT"
    fi
done

# --- C. THE LEGITIMATE CASE still works ------------------------------------
# The fix must not turn every genuinely-closed citation into a refusal --
# that would make the ledger unwritable and the "fix" would be reverted.
for st in done cancelled failed; do
    T="$WORK/c-$st"; make_tree "$T" vms-aaa
    run_gen "$T" "vms-bbb" "vms-aaa:$st"
    if [ "$RC" -eq 0 ] && grep -q "^vms-aaa	closed	$st" "$LEDGER" 2>/dev/null; then
        ok "C/$st: a genuinely closed item is still written \`closed $st\` (rc=0)"
    else
        bad "C/$st: a real closing status must still produce a closed row (rc=$RC)" "$OUT"
    fi
done

# --- D. AN UNKNOWN STATUS REFUSES -----------------------------------------
# The safe direction for a state this script has never seen is to stop, not to
# guess `closed`. If rd grows a status, this fires instead of corrupting.
T="$WORK/d"; make_tree "$T" vms-aaa
run_gen "$T" "vms-bbb" "vms-aaa:frobnicated"
if [ "$RC" -ne 0 ]; then
    ok "D: an UNRECOGNISED status refuses rather than defaulting to closed"
else
    bad "D: an unknown status was silently accepted as closed" "$OUT"
fi

# --- E. ABSENT is unchanged ------------------------------------------------
# An id rd genuinely does not have must still be `absent`, not a refusal:
# that row is data the gates deliberately red on, and turning it into a
# refusal would hide it.
T="$WORK/e"; make_tree "$T" vms-zzz
run_gen "$T" "vms-bbb" ""
if [ "$RC" -eq 0 ] && grep -q "^vms-zzz	absent" "$LEDGER" 2>/dev/null; then
    ok "E: an id rd has never heard of is still \`absent\`, not a refusal"
else
    bad "E: the absent path changed (rc=$RC)" "$OUT"
fi

# --- F. the empty-list floor still fires ----------------------------------
# It predates this fix and is the case B's refusal must not have replaced.
T="$WORK/f"; make_tree "$T" vms-aaa
run_gen "$T" "" "vms-aaa:active"
if [ "$RC" -ne 0 ] && printf '%s' "$OUT" | grep -qi "NO open items"; then
    ok "F: the pre-existing empty-list floor still fires on its own message"
else
    bad "F: the empty-list floor stopped firing (rc=$RC)" "$OUT"
fi

# --------------------------------------------------------------------------
# G. NON-VACUITY. Remove the fix and case B must go red.
#
# Without this the whole file could be asserting properties the generator has
# always had. The mutation is the smallest one that restores the defect:
# make CLOSING_STATUSES accept everything, which is what "absence means
# closed" amounts to.
# --------------------------------------------------------------------------
MUT="$WORK/gen_mutated.py"
sed 's/^CLOSING_STATUSES = frozenset((.*$/CLOSING_STATUSES = frozenset(("done", "cancelled", "failed", "active", "inbox", "blocked", "waiting", "frobnicated"))/' \
    "$GEN" > "$MUT"
if cmp -s "$GEN" "$MUT"; then
    bad "G: the non-vacuity mutation did not change the generator -- broken fixture" \
        "the CLOSING_STATUSES anchor no longer matches; re-anchor it"
else
    T="$WORK/g"; make_tree "$T" vms-aaa
    LEDGER="$T/tracking/rd-citations.tsv"; rm -f "$LEDGER"
    G_OUT=$(PATH="$WORK/bin:$PATH" RD_LIST_IDS="vms-bbb" RD_SHOW_MAP="vms-aaa:active" \
            python3 "$MUT" --root "$T" 2>&1)
    G_RC=$?
    if [ "$G_RC" -eq 0 ] && grep -q "^vms-aaa	closed	active" "$LEDGER" 2>/dev/null; then
        ok "G non-vacuity: with the fix removed the defect returns -- \`closed active\` is written and rc=0"
    else
        bad "G non-vacuity: could not reproduce the defect, so case B proves nothing (rc=$G_RC)" "$G_OUT"
    fi
fi

echo
echo "test_rd_citations_partial_list: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
