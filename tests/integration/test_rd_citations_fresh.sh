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
# ---------------------------------------------------------------------------
# THREE THINGS RUN HERE, AND THEY ARE NOT THE SAME CHECK
# ---------------------------------------------------------------------------
#
#   1. THE COMPARISON. Regenerate with tools/gen_rd_citations.py and require
#      the committed rows to match. Catches staleness and hand-edited rows.
#
#   2. THE INDEPENDENT READ (this file's own code, NOT the generator's). Ask rd
#      directly about every id in the ledger and require the ledger's
#      open/closed/absent column to match the answer.
#
#      WHY 2 EXISTS. Until it did, both sides of this mechanism reduced to one
#      file: the CI-side gate reads a ledger written by the generator, and this
#      test regenerated with THE SAME generator and compared. A generator that
#      lied agreed with itself. MEASURED, running the revision this replaces:
#      ONE edit to tools/gen_rd_citations.py -- the `absent` branch writing
#      ("open", "active") instead of ("absent", "-") -- plus
#      `sed s/(vms-a86)/(vms-q9z9)/` on one declaration and a regenerate, gave
#      census rc=0 PASS ("13 sites cite 5 ids -- 5 open, 0 closed, 0 unknown",
#      44/31/13 unchanged) and the old freshness test rc=0 PASS. It read like a
#      false-absent bugfix. The same tree run against THIS file is rc=1.
#
#   3. READING THE GENERATOR'S OWN REPORT INSTEAD OF ECHOING IT. The generator
#      prints "CLOSED: <id> ..." and "ABSENT: rd has no item <id> ..." on
#      stderr. The revision this replaces piped that stderr into the report
#      with `sed 's/^/    /'` and never looked at it -- so in the attack above
#      the line "ABSENT: rd has no item vms-q9z9, 2 citation(s): ..." printed
#      TWO LINES ABOVE the word PASS. The evidence was produced, displayed, and
#      ignored while the row said `open`. Those lines are now PARSED and
#      cross-checked against the rows, and against the generator's own summary
#      counts.
#
# ---------------------------------------------------------------------------
# WHAT IS COMPARED, AND THE ONE COLUMN THAT IS NOT
# ---------------------------------------------------------------------------
#
# A ledger row is  <id> TAB <open|closed|absent> TAB <rd status> TAB <title>.
# Columns 1-3 are load-bearing: they are what every gate reads. They are
# compared unconditionally and any difference is a red.
#
# COLUMN 4, THE TITLE, IS COMPARED ONLY WHEN BOTH SIDES CARRY A READABLE ONE.
# This is a deliberate narrowing of what this test checks, and it is argued
# here rather than slipped in:
#
#   rd is nostr-backed, and rd's answer for an item whose event this host
#   cannot decrypt is the literal string `[encrypted]` in the title field.
#   MEASURED on the dev host: `rd list --json` returned "[encrypted]" for all
#   12 rows -- identically from the repo and from a clean `git archive`, so it
#   is a KEYS condition and not the working-directory trap the generator
#   guards. In that state this test compared 12 committed titles against 12
#   `[encrypted]` ones and went rc=1 on pristine main. The prescribed remedy
#   made it worse: regenerating writes `[encrypted]` into all 12 committed
#   titles, this test goes green, the census still passes -- and the ledger's
#   own disclosed ceiling ("a hand-forged row is caught on a host with rd") is
#   then closed nowhere, because every title on both sides is the same
#   constant. The same test also went red for any routine retitle in rd.
#
#   So: an unreadable title is TOLERATED EXPLICITLY, per row, and every row
#   whose title could not be compared is NAMED in the output with a count. It
#   is not tolerated by accident, it is not tolerated silently, and it is not
#   fixed by writing `[encrypted]` into the committed ledger.
#
#   WHAT THAT COSTS: while a title is unreadable, a hand-edited title on that
#   row is not caught. A hand-edited id, state or status still is -- by columns
#   1-3, and independently by the live read in (2). Self-controls E, F, I and J
#   below pin that the tolerance did not widen past column 4.
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
# The LEDGER-FLOOR controls (K-N) likewise run with no rd: they build fixture
# trees and require tests/integration/lib/rd_citations.sh to refuse.
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

# rd's own answer when it holds an item it cannot decrypt. Observed from rd,
# not chosen here; self-control E is what keeps it honest -- if rd ever stops
# emitting this exact string, E stops proving anything and says so, because it
# asserts the suppression actually happened rather than only that the pair
# agreed.
UNREADABLE_TITLE='[encrypted]'

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
# The comparator. Data rows only; see the header for why the stamp is out and
# why column 4 is conditional.
# ---------------------------------------------------------------------------
ledger_rows() {
    grep -v '^#' "$1" 2>/dev/null | grep -v '^[ 	]*$' | sort
}

# ledgers_agree <a> <b>: 0 when columns 1-3 are identical AND every row whose
# title is readable on BOTH sides has the same title. Rows where either title
# is unreadable are recorded in $WORK/title_skipped and not compared.
ledgers_agree() {
    ledger_rows "$1" > "$WORK/cmp_a"
    ledger_rows "$2" > "$WORK/cmp_b"
    : > "$WORK/title_skipped"

    cut -f1-3 "$WORK/cmp_a" > "$WORK/key_a"
    cut -f1-3 "$WORK/cmp_b" > "$WORK/key_b"
    cmp -s "$WORK/key_a" "$WORK/key_b" || return 1

    awk -F'\t' -v u="$UNREADABLE_TITLE" -v skipf="$WORK/title_skipped" '
        NR == FNR { t[$1] = $4; seen[$1] = 1; next }
        {
            if (!($1 in seen)) next
            if ($4 == u || t[$1] == u) {
                printf "%s\t%s\t%s\n", $1, t[$1], $4 >> skipf
                next
            }
            if ($4 != t[$1]) bad = 1
        }
        END { exit bad ? 1 : 0 }
    ' "$WORK/cmp_a" "$WORK/cmp_b"
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

# sc_ok <name> <message-if-false> <cmd...>: a control on something other than
# the comparator's verdict, folded into the same counters.
sc_ok() {
    _name="$1"; _why="$2"; shift 2
    if "$@"; then
        echo "  PASS: self-control $_name"
        sc_pass=$((sc_pass + 1))
    else
        echo "  FAIL: self-control $_name -- $_why"
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

# D: a title changed, both sides readable. Cosmetic-looking, still a divergence
#    from derived truth, and still caught -- the column-4 tolerance below is
#    conditional on UNREADABILITY, not on the column being ignored.
sed '$s/[^	]*$/retitled by hand/' "$B" > "$WORK/sc_d.tsv"
sc "D: a title edited by hand (both titles readable)" differ "$B" "$WORK/sc_d.tsv"

# E (GREEN): the keys condition. Every title on one side is rd's `[encrypted]`.
#    Columns 1-3 are untouched, so the ledger is as true as it was; the titles
#    simply cannot be compared on this host today. This is the exact shape that
#    made this test rc=1 on pristine main.
awk -F'\t' -v u="$UNREADABLE_TITLE" 'BEGIN{OFS="\t"}
     /^#/ || /^[ \t]*$/ { print; next } { $4 = u; print }' "$B" > "$WORK/sc_e.tsv"
if cmp -s "$B" "$WORK/sc_e.tsv"; then
    echo "  FAIL: BROKEN FIXTURE: self-control E did not change any title"
    status=1
else
    sc "E: every title unreadable, rows otherwise identical" agree "$B" "$WORK/sc_e.tsv"
    # ... and the suppression must be REPORTED, not silent. Without this, a
    # comparator that had simply dropped column 4 would pass E identically.
    _e_rows=$(ledger_rows "$B" | grep -c . || true)
    _e_skipped=$(grep -c . "$WORK/title_skipped" 2>/dev/null || true)
    sc_ok "E2: the unreadable titles were named, not silently dropped ($_e_skipped of $_e_rows)" \
        "the comparator agreed without recording which titles it could not compare -- that is a silent skip, not a tolerated one" \
        [ "$_e_skipped" -eq "$_e_rows" ]
fi

# F: unreadable titles must NOT buy a state change. Same `[encrypted]` titles,
#    plus one open row flipped to closed.
sed 's/^\([a-z0-9.-]*\)	open	/\1	closed	/' "$WORK/sc_e.tsv" > "$WORK/sc_f.tsv"
sc "F: unreadable titles AND an open row flipped to closed" differ "$B" "$WORK/sc_f.tsv"

# I: unreadable titles must NOT buy a forged row.
cp "$WORK/sc_e.tsv" "$WORK/sc_i.tsv"
printf 'vms-q9z9\topen\tactive\t%s\n' "$UNREADABLE_TITLE" >> "$WORK/sc_i.tsv"
sc "I: unreadable titles AND a hand-forged row appended" differ "$B" "$WORK/sc_i.tsv"

# J: unreadable titles must NOT buy an rd-status edit (column 3).
sed 's/	inbox	/	active	/' "$WORK/sc_e.tsv" > "$WORK/sc_j.tsv"
if cmp -s "$WORK/sc_e.tsv" "$WORK/sc_j.tsv"; then
    echo "  FAIL: BROKEN FIXTURE: self-control J changed no rd status"
    status=1
else
    sc "J: unreadable titles AND an rd status edited" differ "$B" "$WORK/sc_j.tsv"
fi

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

# ---------------------------------------------------------------------------
# LEDGER-FLOOR CONTROLS (K-O). These do not exercise the comparator; they
# exercise tests/integration/lib/rd_citations.sh, the reader the census uses,
# on fixture trees built here. They need no rd.
#
# WHY THEY ARE HERE. rd_cite_check treated "0 declaration site(s) cite 0
# distinct rd item(s)" as a PASS. MEASURED, calling the old rd_cite_check on a
# header-only ledger with an empty id list: rc=0, and that line as its summary.
# The only thing in the repo that noticed an emptied ledger was self-control A
# above -- and A notices it WITHOUT rd, so it says nothing about the floor
# holding wherever this test skips, which is every CI run. The floor now lives
# in the checker, where the census reaches it too; these are its controls.
# MEASURED after: stripping every data row from the committed ledger takes the
# census from rc=0 PASS to rc=1 "REFUSING ... ZERO data rows".
# ---------------------------------------------------------------------------
FT="$WORK/floortree"
build_floor_tree() {
    rm -rf "$FT"
    mkdir -p "$FT/src/libvms" "$FT/tools" "$FT/tracking" "$WORK/fw"
    cat > "$FT/src/libvms/fixture.c" <<'EOF'
/* OVMX-UNWIRED: vms_kif_fixture (vms-a86) -- fixture declaration */
EOF
    {
        echo "# fixture ledger"
        echo "# generated-at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'vms-a86\topen\tinbox\tfixture row\n'
    } > "$FT/tracking/rd-citations.tsv"
    rm -rf "$WORK/fw"; mkdir -p "$WORK/fw"
    printf 'vms-a86\n' > "$WORK/fw/ids"
}

# floor_rc <expected-rc> <expected-substring>: run the checker on the fixture
# tree, require the rc AND the diagnostic.
floor_rc() {
    _f_name="$1"; _f_want="$2"; _f_msg="$3"
    rd_cite_check "$FT" "$WORK/fw/ids" "$WORK/fw" 1 "the floor fixture" \
        > "$WORK/fw/out" 2>&1
    _f_got=$?
    if [ "$_f_got" -ne "$_f_want" ]; then
        echo "  FAIL: floor control $_f_name -- checker returned $_f_got, wanted $_f_want"
        sed 's/^/        /' "$WORK/fw/out"
        sc_fail=$((sc_fail + 1)); status=1
        return
    fi
    if [ -n "$_f_msg" ] && ! grep -qF "$_f_msg" "$WORK/fw/out"; then
        echo "  FAIL: floor control $_f_name -- rc was $_f_got but the diagnostic"
        echo "        never said '$_f_msg'. A refusal nobody can read is a silent one."
        sed 's/^/        /' "$WORK/fw/out"
        sc_fail=$((sc_fail + 1)); status=1
        return
    fi
    echo "  PASS: floor control $_f_name (rc=$_f_got)"
    sc_pass=$((sc_pass + 1))
}

# K (GREEN): the unmutated fixture passes. Without this the four reds below
#    could all be firing for a reason that has nothing to do with the mutation.
build_floor_tree
floor_rc "K: an intact fixture tree passes" 0 ""

# L: the ledger keeps its header and loses every data row, declarations intact.
build_floor_tree
grep '^#' "$FT/tracking/rd-citations.tsv" > "$FT/tracking/rd-citations.tsv.new"
mv "$FT/tracking/rd-citations.tsv.new" "$FT/tracking/rd-citations.tsv"
floor_rc "L: a ledger with zero data rows is a REFUSAL" 2 "ZERO data rows"

# M: the attack in full -- every data row AND every declaration deleted. This
#    is the one that used to read as a PASS.
build_floor_tree
grep '^#' "$FT/tracking/rd-citations.tsv" > "$FT/tracking/rd-citations.tsv.new"
mv "$FT/tracking/rd-citations.tsv.new" "$FT/tracking/rd-citations.tsv"
rm -f "$FT/src/libvms/fixture.c"
: > "$WORK/fw/ids"
floor_rc "M: an empty ledger AND no declarations is a REFUSAL, not a pass" 2 \
    "found NO rd id cited"

# N: a stamp in the future. The rows are real; the impossible number is the
#    defect. Used to print "(-355531 day(s) old)" and return 0.
build_floor_tree
sed -i 's/^# generated-at:.*/# generated-at: 2999-12-31T00:00:00Z/' \
    "$FT/tracking/rd-citations.tsv"
floor_rc "N: a ledger stamped in the future is a REFUSAL" 2 "in the FUTURE"

# O: a citation the ledger does not carry, found by the checker's OWN rescan
#    rather than by the caller's id list -- the ids file below names nothing.
build_floor_tree
cat >> "$FT/src/libvms/fixture.c" <<'EOF'
/* OVMX-UNWIRED: vms_kif_fixture2 (vms-q9z9) -- cites an unresolved id */
EOF
: > "$WORK/fw/ids"
floor_rc "O: an id the caller never passed in is still resolved (or red)" 1 \
    "has NO ROW for it"

rm -rf "$FT"
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

# ---------------------------------------------------------------------------
# (3) READ that report. Every CLOSED:/ABSENT: line the generator printed names
# an id it says it could not resolve as open; the row it wrote for that same id
# must say so too. This is what turns "the evidence was produced and ignored"
# into "the evidence is checked".
# ---------------------------------------------------------------------------
ledger_rows "$WORK/fresh.tsv" > "$WORK/fresh_rows"
row_state() { awk -F'\t' -v id="$1" '$1 == id { print $2; exit }' "$WORK/fresh_rows"; }

sed -n 's/^ *CLOSED: \([a-z0-9.-]*\) .*/\1/p' "$WORK/gen.err" | sort -u > "$WORK/said_closed"
sed -n 's/^ *ABSENT: rd has no item \([a-z0-9.-]*\),.*/\1/p' "$WORK/gen.err" | sort -u > "$WORK/said_absent"

while IFS= read -r gid; do
    [ -n "$gid" ] || continue
    gst=$(row_state "$gid")
    if [ "$gst" != "closed" ]; then
        echo "FAIL: the generator REPORTED $gid as CLOSED and wrote its row as '${gst:-(no row)}'."
        echo "  -> the report and the rows come from the same resolution, so they"
        echo "     cannot honestly disagree. This is the shape of the measured"
        echo "     one-edit attack: the row is fabricated, the report is not, and"
        echo "     the report used to be printed and never read."
        status=1
    fi
done < "$WORK/said_closed"

while IFS= read -r gid; do
    [ -n "$gid" ] || continue
    gst=$(row_state "$gid")
    if [ "$gst" != "absent" ]; then
        echo "FAIL: the generator REPORTED $gid as ABSENT (rd has no such item) and"
        echo "      wrote its row as '${gst:-(no row)}'."
        echo "  -> an id rd does not know cannot have a row saying otherwise."
        status=1
    fi
done < "$WORK/said_absent"

# The generator's summary counts must also match the rows it wrote.
gsum=$(sed -n 's/^gen_rd_citations: .* -- \([0-9]*\) open, \([0-9]*\) closed, \([0-9]*\) absent$/\1 \2 \3/p' \
    "$WORK/gen.err" | head -1)
if [ -z "$gsum" ]; then
    echo "FAIL: the generator printed no summary line this test could parse."
    echo "  -> that line is one of the cross-checks; without it, the report is"
    echo "     decoration again."
    status=1
else
    g_open=$(printf '%s' "$gsum" | cut -d' ' -f1)
    g_closed=$(printf '%s' "$gsum" | cut -d' ' -f2)
    g_absent=$(printf '%s' "$gsum" | cut -d' ' -f3)
    r_open=$(awk -F'\t' '$2 == "open"' "$WORK/fresh_rows" | grep -c . || true)
    r_closed=$(awk -F'\t' '$2 == "closed"' "$WORK/fresh_rows" | grep -c . || true)
    r_absent=$(awk -F'\t' '$2 == "absent"' "$WORK/fresh_rows" | grep -c . || true)
    if [ "$g_open" != "$r_open" ] || [ "$g_closed" != "$r_closed" ] || [ "$g_absent" != "$r_absent" ]; then
        echo "FAIL: the generator's summary and its own rows disagree."
        echo "      summary: $g_open open, $g_closed closed, $g_absent absent"
        echo "      rows:    $r_open open, $r_closed closed, $r_absent absent"
        status=1
    else
        echo "    (report cross-check: $g_open open / $g_closed closed / $g_absent absent, matching the rows written)"
    fi
fi

# ---------------------------------------------------------------------------
# (2) THE INDEPENDENT READ. Ask rd about every id in the COMMITTED ledger using
# this file's own code. Nothing below goes through tools/gen_rd_citations.py.
#
# rd resolves which board it is talking to from the working directory and
# answers rc=0 with an empty list when it finds none -- so every call is pinned
# to $SRC_ROOT, and an empty open set is a FAILURE here rather than a verdict
# that every citation is closed.
# ---------------------------------------------------------------------------
echo "  asking rd directly about every ledger row (not via the generator)"
ledger_rows "$LEDGER" > "$WORK/committed_rows"

if ! (cd "$SRC_ROOT" && rd list --json) > "$WORK/rd_list.json" 2>"$WORK/rd_list.err"; then
    echo "FAIL: \`rd list --json\` failed in $SRC_ROOT:"
    sed 's/^/    /' "$WORK/rd_list.err"
    status=1
elif ! python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
if not isinstance(d, list):
    sys.exit("rd list --json did not return a list")
ids = [x.get("id") for x in d if isinstance(x, dict) and x.get("id")]
if not ids:
    sys.exit("rd list --json returned NO open items")
print("\n".join(sorted(set(ids))))
' "$WORK/rd_list.json" > "$WORK/rd_open_ids" 2>"$WORK/rd_open.err"; then
    echo "FAIL: could not read rd's open set independently:"
    sed 's/^/    /' "$WORK/rd_open.err"
    echo "  -> an empty or unreadable open set is NOT evidence that every cited"
    echo "     item is closed; it is overwhelmingly likely to be rd resolving a"
    echo "     different board, or none. Refusing to certify from it."
    status=1
else
    ind_bad=0
    ind_n=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        lid=$(printf '%s\n' "$line" | cut -f1)
        lstate=$(printf '%s\n' "$line" | cut -f2)
        ind_n=$((ind_n + 1))
        if grep -qx "$lid" "$WORK/rd_open_ids"; then
            want=open
        elif (cd "$SRC_ROOT" && rd show "$lid" --json) >/dev/null 2>&1; then
            want=closed
        else
            want=absent
        fi
        if [ "$lstate" != "$want" ]; then
            echo "FAIL: the committed ledger says $lid is '$lstate'; rd, asked directly"
            echo "      by this test, says '$want'."
            echo "  -> this check does not run the generator, so a generator that"
            echo "     writes a row rd does not support is caught here even when it"
            echo "     agrees with itself."
            ind_bad=$((ind_bad + 1))
            status=1
        fi
    done < "$WORK/committed_rows"
    if [ "$ind_bad" -eq 0 ]; then
        echo "    independent read: rd's own answer matches all $ind_n committed row(s)."
    fi
fi

# ---------------------------------------------------------------------------
# (1) THE COMPARISON.
# ---------------------------------------------------------------------------
if ledgers_agree "$LEDGER" "$WORK/fresh.tsv"; then
    n=$(ledger_rows "$LEDGER" | grep -c . || true)
    nskip=$(grep -c . "$WORK/title_skipped" 2>/dev/null || true)
    echo "  the committed ledger and live rd agree on all $n row(s)"
    echo "  (id, open/closed/absent and rd status compared on every row)."
    if [ "$nskip" -gt 0 ]; then
        echo "  TITLE NOT COMPARED on $nskip of $n row(s) -- rd returned"
        echo "  '$UNREADABLE_TITLE' for them on this host, so there is no title to"
        echo "  compare against. Columns 1-3 were still compared, and the live read"
        echo "  above still confirmed each row against rd. The residual is narrow and"
        echo "  it is this: a hand-edited TITLE on these rows would not be caught"
        echo "  until this host can decrypt them."
        cut -f1 "$WORK/title_skipped" | sort -u | sed 's/^/      /'
    fi
else
    echo "FAIL: the committed ledger DISAGREES with live rd."
    echo "      -committed / +live (columns 1-3):"
    diff "$WORK/key_a" "$WORK/key_b" | sed 's/^/    /' || true
    echo "      -committed / +live (full rows):"
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
