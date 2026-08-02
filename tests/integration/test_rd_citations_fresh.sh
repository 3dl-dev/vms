#!/bin/sh
#
# test_rd_citations_fresh.sh - the committed citation ledger still matches live
# rd (rd vms-8cc).
#
# WHAT THIS IS FOR. tracking/rd-citations.tsv is what lets the standing gates
# check that an exemption's cited rd item EXISTS and is OPEN without reaching
# rd -- rd is nostr-backed and unavailable in CI, which is why those gates
# shape-checked the id and nothing more until vms-8cc. The ledger is derived by
# tools/gen_rd_citations.py and committed. Committed derived state has exactly
# one failure mode, and it is the failure mode one level up from the defect
# being fixed:
#
#   THE LEDGER GOES STALE, AND THE GATE THEN CERTIFIES A CLOSED ITEM AS OPEN.
#
# Nothing inside CI can see that, and nothing else in this repo re-derives the
# ledger. This test does: it regenerates from live rd and reds if the committed
# copy disagrees.
# It needs rd, so where rd is absent it SKIPS -- loudly, with ctest exit code
# 77, reported as "Skipped" and never as "Passed". A skip here is a run in
# which the ledger was NOT verified, and the census says so in its own output
# at every run by printing the ledger's stamp and age.
#
# WHAT A GREEN HERE DOES AND DOES NOT BUY:
#   BUYS - the committed ledger's data rows are byte-identical to what live rd
#          produces right now. That catches staleness, and it catches a row
#          edited by hand -- the attack the CI-side check does not see, MEASURED:
#          a fabricated citation plus one appended `open` row left the census at
#          rc=0/PASS and this test at rc=1, naming the row.
#   NOT  - it says nothing about any moment other than this one. An item closed
#          five minutes after a green run leaves the ledger stale until the
#          next run. The window is real and is disclosed rather than closed.
#
# THE STAMP IS EXCLUDED FROM THE COMPARISON, deliberately: `# generated-at:`
# changes on every regeneration, so comparing whole files would red on every
# run and the test would be turned off within a week. Only the DATA ROWS are
# compared. Self-control G below pins that this exclusion has not been widened
# into "compare nothing".
#
# THE SELF-CONTROLS RUN EVEN WHEN rd DOES NOT. They need no rd -- they feed
# constructed pairs to the comparator and require it to fire. So a CI run that
# skips the live comparison still proves the comparator is not stuck at green.
# A failure found before the skip point is a FAIL, not a skip: MEASURED by
# appending an unparseable row to the ledger and running with rd off PATH --
# rc=1, "FAIL (self-controls, before any skip)", not 77.
#
# Usage: test_rd_citations_fresh.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
LEDGER="$SRC_ROOT/tracking/rd-citations.tsv"
GEN="$SRC_ROOT/tools/gen_rd_citations.py"
CITE_LIB="$(cd "$(dirname "$0")" && pwd)/lib/rd_citations.sh"

status=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "rd citation ledger freshness: the committed ledger still matches live rd"

if [ ! -f "$CITE_LIB" ]; then
    echo "FAIL: cannot find the citation checker at $CITE_LIB"
    exit 1
fi
. "$CITE_LIB"

# ---------------------------------------------------------------------------
# The comparator. Data rows only; see the header for why the stamp is out.
# ---------------------------------------------------------------------------
ledger_rows() {
    grep -v '^#' "$1" 2>/dev/null | grep -v '^[ 	]*$' | sort
}

# ledgers_agree <a> <b>: 0 when the DATA ROWS are identical.
ledgers_agree() {
    ledger_rows "$1" > "$WORK/cmp_a"
    ledger_rows "$2" > "$WORK/cmp_b"
    cmp -s "$WORK/cmp_a" "$WORK/cmp_b"
}

# ---------------------------------------------------------------------------
# Self-controls. Each constructs a pair and requires a specific verdict.
# ---------------------------------------------------------------------------
sc_pass=0
sc_fail=0
sc() {
    _name="$1"; _want="$2"; _a="$3"; _b="$4"
    if ledgers_agree "$_a" "$_b"; then _got=agree; else _got=differ; fi
    if [ "$_got" = "$_want" ]; then
        echo "  PASS: self-control $_name (comparator said $_got)"
        sc_pass=$((sc_pass + 1))
    else
        echo "  FAIL: self-control $_name -- comparator said $_got, wanted $_want"
        echo "        The freshness comparison is broken, so a green from this"
        echo "        test would mean nothing. Fix the comparator."
        sc_fail=$((sc_fail + 1)); status=1
    fi
}

if [ ! -f "$LEDGER" ]; then
    echo "FAIL: there is no citation ledger at tracking/rd-citations.tsv"
    echo "  -> regenerate it on a host that has rd:  tools/gen_rd_citations.py"
    exit 1
fi

B="$WORK/base.tsv"
cp "$LEDGER" "$B"

# A: a status flip -- exactly what staleness looks like when an item is closed.
sed 's/^\([a-z0-9.-]*\)	open	/\1	closed	/' "$B" > "$WORK/sc_a.tsv"
if cmp -s "$B" "$WORK/sc_a.tsv"; then
    echo "  FAIL: BROKEN FIXTURE: self-control A did not change anything --"
    echo "        the ledger has no 'open' row to flip, so this control ran"
    echo "        against an unmutated pair and proved nothing."
    status=1
else
    sc "A: an open row flipped to closed" differ "$B" "$WORK/sc_a.tsv"
fi

# B: a row dropped -- what a citation resolved before and unresolvable now
#    looks like, and what deleting an inconvenient row looks like.
sed '$d' "$B" > "$WORK/sc_b.tsv"
sc "B: a row deleted" differ "$B" "$WORK/sc_b.tsv"

# C: a row ADDED -- the hand-forged row. This is the attack the CI-side check
#    does not see: a fabricated id given a hand-written `open` row reads exactly
#    like a derived one there (measured -- census rc=0, PASS). It is caught here.
cp "$B" "$WORK/sc_c.tsv"
printf 'vms-q9z9\topen\tactive\thand-forged row, never derived from rd\n' >> "$WORK/sc_c.tsv"
sc "C: a hand-forged row appended" differ "$B" "$WORK/sc_c.tsv"

# D: a title changed. Cosmetic-looking, still a divergence from derived truth.
sed '$s/[^	]*$/retitled by hand/' "$B" > "$WORK/sc_d.tsv"
sc "D: a title edited by hand" differ "$B" "$WORK/sc_d.tsv"

# G (GREEN): the same rows with a DIFFERENT generated-at stamp must AGREE.
#    This is the control on the exclusion itself. Without it, "ignore the
#    stamp" could silently widen into "ignore the file" and every control
#    above would still pass -- while the live comparison below could never
#    fail. A comparator that cannot say `agree` is as useless as one that
#    cannot say `differ`.
sed 's/^# generated-at:.*/# generated-at: 1999-01-01T00:00:00Z/' "$B" > "$WORK/sc_g.tsv"
if cmp -s "$B" "$WORK/sc_g.tsv"; then
    echo "  FAIL: BROKEN FIXTURE: self-control G did not change the stamp"
    status=1
else
    sc "G: only the generated-at stamp differs" agree "$B" "$WORK/sc_g.tsv"
fi

# H (GREEN): reordered rows agree. The comparator sorts, so a generator whose
#    row order changed is not a false red.
sort -r "$B" > "$WORK/sc_h.tsv"
sc "H: the same rows in a different order" agree "$B" "$WORK/sc_h.tsv"

echo "  self-controls: $sc_pass passed, $sc_fail failed"

# ---------------------------------------------------------------------------
# The committed ledger must parse under the SAME reader the gates use. If it
# does not, every gate is refusing right now and this test should say so
# rather than let the live comparison be the first to notice.
# ---------------------------------------------------------------------------
: > "$WORK/no_ids"
if ! rd_cite_check "$SRC_ROOT" "$WORK/no_ids" "$WORK" 0 "the ledger self-check"; then
    echo "FAIL: the committed ledger does not parse under the gates' own reader."
    status=1
fi

# ---------------------------------------------------------------------------
# The live comparison, or an honest skip.
# ---------------------------------------------------------------------------
missing=""
command -v rd >/dev/null 2>&1 || missing="$missing rd"
command -v python3 >/dev/null 2>&1 || missing="$missing python3"
[ -f "$GEN" ] || missing="$missing tools/gen_rd_citations.py"

if [ -n "$missing" ]; then
    if [ "$status" -ne 0 ]; then
        echo "rd citation ledger freshness: FAIL (self-controls, before any skip)"
        exit 1
    fi
    echo
    echo "  SKIP: not run --$missing unavailable here."
    echo "        THE COMMITTED LEDGER WAS NOT VERIFIED IN THIS RUN. rd is"
    echo "        nostr-backed and is not reachable from CI, which is the whole"
    echo "        reason the ledger is committed in the first place; this test is"
    echo "        the only thing that can tell a fresh ledger from a stale one or"
    echo "        from a hand-forged row, and it cannot run here."
    echo "        Run it on the dev host:  tests/integration/test_rd_citations_fresh.sh"
    echo "        Exiting 77 so ctest reports SKIPPED, not PASSED -- a run that"
    echo "        checked nothing must not read as a run that checked something."
    exit 77
fi

echo
echo "  rd is available: re-deriving the ledger from live rd"
if ! python3 "$GEN" --root "$SRC_ROOT" --out "$WORK/fresh.tsv" 2>"$WORK/gen.err"; then
    echo "FAIL: tools/gen_rd_citations.py could not produce a ledger:"
    sed 's/^/    /' "$WORK/gen.err"
    echo "  -> this is NOT a skip. rd is on PATH and the generator still failed,"
    echo "     so the committed ledger is unverifiable for a reason worth fixing."
    exit 1
fi
sed 's/^/    /' "$WORK/gen.err"

if ledgers_agree "$LEDGER" "$WORK/fresh.tsv"; then
    n=$(ledger_rows "$LEDGER" | grep -c . || true)
    echo "  the committed ledger and live rd agree on all $n row(s)."
else
    echo "FAIL: the committed ledger DISAGREES with live rd."
    echo "      -committed / +live:"
    diff "$WORK/cmp_a" "$WORK/cmp_b" | sed 's/^/    /' || true
    echo "  -> the ledger is stale, or a row was written by hand. Either way the"
    echo "     gates that read it are currently certifying something rd does not"
    echo "     say. Regenerate and commit:  tools/gen_rd_citations.py"
    echo "     If a row moved from open to closed, the citation that depends on"
    echo "     it is the real problem -- fix the declaration, not the ledger."
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "rd citation ledger freshness: PASS"
else
    echo "rd citation ledger freshness: FAIL"
fi
exit "$status"
