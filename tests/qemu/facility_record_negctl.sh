#!/bin/sh
#
# facility_record_negctl.sh - negative controls ON the execution-sourced
# attribution record (rd vms-d894, rd vms-659).
#
# WHAT IS UNDER TEST HERE, AND WHAT IS NOT
#
# tests/qemu/facility_negctl_record.sh is a reader of a committed record of a
# PAST run. It executes nothing, it boots nothing, and it can never prove
# anything about the executive. What it CAN be wrong about is whether it
# notices that the record and the tree disagree -- and a reader that certifies
# a stale or contradicted record is strictly worse than no reader, because it
# turns "declared" back into "PROVEN" on no evidence. That is the whole subject
# of this file.
#
# EVERY FIXTURE HERE IS SYNTHETIC, AND SYNTHETIC IN A SPECIFIC WAY. The record
# these controls mutate is BUILT, in a sandbox, from the manifest's own
# declarations -- it is what a record WOULD look like if a run had observed
# exactly what the manifest names. It is NOT evidence: no QEMU boot produced
# it, and nothing in this file may be read as saying an assertion fires. It
# exists so the reader can be shown to refuse the things it claims to refuse.
# Anything this file writes lives and dies in a temp dir; nothing is committed.
#
# THE SANDBOX IS A COPY OF THE REAL TREE, and the mutations are aimed at the
# RECORD and at the reader, never at a literal that is supposed to change. No
# control here keys on a defect name, an rd id, or an assertion's wording:
# five register controls and two census controls went vacuous in an earlier
# round by being anchored to exactly that kind of fact (rd vms-fab). The
# defect names used below are read out of the manifest at run time, and the
# synthetic ones are strings no product file contains.
#
# Usage: facility_record_negctl.sh <repo-root>

set -u

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
SB=$(mktemp -d) || exit 2
trap 'rm -rf "$SB"' EXIT INT TERM

passed=0
failed=0
status=0

echo "=========================================================="
echo " Negative controls on the execution record (vms-d894/659)"
echo "=========================================================="
echo "Sandbox: $SB"
echo ""

# ------------------------------------------------------------------ sandbox --
mkdir -p "$SB/tests/qemu" "$SB/src/kernel" "$SB/tracking"
cp "$ROOT"/tests/qemu/*.c        "$SB/tests/qemu/"  2>/dev/null
cp "$ROOT"/tests/qemu/*.sh       "$SB/tests/qemu/"  2>/dev/null
cp "$ROOT"/tests/qemu/facility_defects_floor.txt "$SB/tests/qemu/"
cp "$ROOT"/src/kernel/*.c        "$SB/src/kernel/"  2>/dev/null
# vms-165: src/kernel/vmsfs (the retired vmsfs VFS driver) no longer exists.

MF="$SB/tests/qemu/facility_defects.sh"
LIB="$SB/tests/qemu/facility_negctl_record.sh"
FLOOR="$SB/tests/qemu/facility_defects_floor.txt"
REC="$SB/tests/qemu/facility_negctl_observed.tsv"

for f in "$MF" "$LIB" "$FLOOR"; do
    [ -f "$f" ] || { echo "  FAIL: BROKEN FIXTURE: $f did not make it into the sandbox"; exit 1; }
done

# The reader's own functions, used below to BUILD fixtures -- the same writer
# the driver uses, so a fixture can never be in a shape the writer cannot emit.
. "$LIB"

# ------------------------------------------------------- the synthetic record --
# Built from the manifest: one RUN row per defect, one RED row per assertion
# text the manifest names, attributed to the suite source that literally
# contains that text (facility_defects.sh selftest already proves every named
# text appears in exactly that way, so this attribution is derived, not
# invented). The tree-commit field says out loud what this is.
SYNTH_COMMIT="synthetic-fixture-no-run-produced-this"

# Built through fnr_emit_defect -- the SAME translation the driver applies to
# its own fail_map() -- so the fixture can never be in a shape the writer could
# not produce, and so the writer is exercised on every run of this file.
synth_record() {   # synth_record <out>
    _sr_out="$1"
    _sr_n=$(sh "$MF" list | grep -c .)
    _sr_ns=$(ls "$SB"/tests/qemu/test_kmod_*.c "$SB"/tests/qemu/test_syssvc_*.c 2>/dev/null | wc -l)
    fnr_emit_header "$_sr_out" pass "$_sr_n" "$_sr_ns" "$SYNTH_COMMIT"
    for _sr_d in $(sh "$MF" list); do
        { sh "$MF" field "$_sr_d" require_fail; sh "$MF" field "$_sr_d" knock_on_fail; } \
            | grep -v '^[ 	]*$' >"$SB/texts.tmp"
        : >"$SB/map.tmp"
        while IFS= read -r _sr_t; do
            [ -n "$_sr_t" ] || continue
            _sr_s=$(grep -lF "$_sr_t" "$SB"/tests/qemu/test_kmod_*.c \
                                      "$SB"/tests/qemu/test_syssvc_*.c 2>/dev/null | head -1)
            if [ -n "$_sr_s" ]; then _sr_s=$(basename "$_sr_s" .c); else _sr_s="(harness)"; fi
            printf '%s\t%s\n' "$_sr_s" "$_sr_t" >>"$SB/map.tmp"
        done <"$SB/texts.tmp"
        fnr_emit_defect "$_sr_out" "$_sr_d" "$SB/map.tmp" pass
    done
    rm -f "$SB/texts.tmp" "$SB/map.tmp"
}

synth_record "$SB/pristine.tsv"
N_RUN=$(grep -c '^RUN	' "$SB/pristine.tsv")
N_RED=$(grep -c '^RED	' "$SB/pristine.tsv")
echo "Synthetic record built: $N_RUN RUN row(s), $N_RED RED row(s)."
echo "  (SYNTHETIC. No QEMU boot produced it. It exists to exercise the reader.)"
echo ""

# The fixture has to be non-trivial or every control below is vacuous.
if [ "$N_RUN" -lt 2 ] || [ "$N_RED" -lt 2 ]; then
    echo "  FAIL: BROKEN FIXTURE: the synthetic record has $N_RUN RUN and $N_RED RED"
    echo "        row(s). Controls that delete a row from it would be deleting nothing."
    exit 1
fi

# A defect name and an assertion text that exist, read out of the manifest --
# never written down here, because both are facts that are supposed to change.
LIVE_DEFECT=$(sh "$MF" list | head -1)
LIVE_TEXT=$(awk -F'\t' -v d="$LIVE_DEFECT" '$1=="RED" && $2==d { print $4; exit }' "$SB/pristine.tsv")
[ -n "$LIVE_DEFECT" ] && [ -n "$LIVE_TEXT" ] || {
    echo "  FAIL: BROKEN FIXTURE: could not read a live defect/text pair out of the manifest"
    exit 1; }

# Synthetic names, checked to be absent from the real tree so no control can be
# measuring product state instead of its own fixture.
SYNTH_DEFECT="negctl-fixture-defect-zzq"
SYNTH_TEXT="negctl fixture assertion zzq that no suite prints"
# Scanned against the populations the controls actually reason over -- the
# product tree, the suite sources and the manifest -- and NOT against this file,
# which necessarily contains them.
for _n in "$SYNTH_DEFECT" "$SYNTH_TEXT"; do
    if grep -rqF "$_n" "$ROOT/src" 2>/dev/null \
       || grep -qF "$_n" "$ROOT"/tests/qemu/test_*.c 2>/dev/null \
       || grep -qF "$_n" "$ROOT/tests/qemu/facility_defects.sh" 2>/dev/null; then
        echo "  FAIL: BROKEN FIXTURE: '$_n' already occurs in the tree, so a control"
        echo "        using it would be measuring real state. Rename it; do NOT relax"
        echo "        the gate."
        exit 1
    fi
done

# --------------------------------------------------------------- the harness --
restore() {
    cp "$ROOT/tests/qemu/facility_defects.sh" "$MF"
    cp "$ROOT/tests/qemu/facility_negctl_record.sh" "$LIB"
    cp "$ROOT/tests/qemu/facility_defects_floor.txt" "$FLOOR"
    cp "$ROOT"/tests/qemu/test_kmod_*.c "$ROOT"/tests/qemu/test_syssvc_*.c "$SB/tests/qemu/"
    cp "$SB/pristine.tsv" "$REC"
}

# del_defect <name> -- the complete deletion, as an adversary would do it:
# the DEFECTS entry, BOTH case arms, every anchor in every suite source, and
# the declared count floor lowered to match. Nothing here special-cases a
# name: the arms are found by the same `^    <name>)$` shape section 5 of
# coverage already scans for, and the anchors by the same comment shape
# section 4 already parses.
del_defect() {
    _dd="$1"
    awk -v d="$_dd" '$0 == d { next } { print }' "$MF" >"$SB/t" && mv "$SB/t" "$MF"
    awk -v d="$_dd" '
        $0 ~ ("^    " d "\\)$") { skip = 1; next }
        skip && /^    [a-z0-9][a-z0-9-]*\)$/ { skip = 0 }
        !skip { print }' "$MF" >"$SB/t" && mv "$SB/t" "$MF"
    sed -i "/negctl\(-knockon\)\{0,1\}: $_dd \*\//d" \
        "$SB"/tests/qemu/test_kmod_*.c "$SB"/tests/qemu/test_syssvc_*.c
    printf '%s\n' "$((N_RUN - 1))" >"$FLOOR"
}

gate() { sh "$MF" coverage "$SB/src" "$SB/tests/qemu" 2>&1; }

verdict() {
    if [ "$2" -eq 1 ]; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        failed=$((failed + 1)); status=1
    fi
}

# expect_red <name> <required fragment> [forbidden fragments, one per line]
#
# The forbidden list is not decoration. Eleven controls below mutate the same
# artifact, and every refusal in the reader is reachable from it: a control
# that only requires its OWN message passes when the mutation trips a second,
# unrelated refusal, and is then one edit away from passing on the wrong one.
expect_red() {
    _er_name="$1"; _er_need="$2"; _er_forbid="${3:-}"
    _er_out=$(gate); _er_rc=$?
    _er_ok=1
    if [ "$_er_rc" -eq 0 ]; then
        echo "  FAIL: the gate CERTIFIED it: $_er_name"
        _er_ok=0
    elif ! printf '%s\n' "$_er_out" | grep -qF "$_er_need"; then
        echo "  FAIL: went red for the WRONG reason: $_er_name"
        echo "        expected output to contain: $_er_need"
        printf '%s\n' "$_er_out" | grep -E '^FAIL|REFUSING|NOT MEASURED' | sed 's/^/          /'
        _er_ok=0
    elif [ -n "$_er_forbid" ]; then
        printf '%s\n' "$_er_out" >"$SB/red_out"
        : >"$SB/red_bad"
        printf '%s\n' "$_er_forbid" | while IFS= read -r _bad; do
            [ -n "$_bad" ] || continue
            grep -qF "$_bad" "$SB/red_out" || continue
            echo "  FAIL: went red for its own reason AND for a forbidden one: $_er_name"
            echo "        output must NOT contain: $_bad"
            printf 'x' >>"$SB/red_bad"
        done
        [ -s "$SB/red_bad" ] && _er_ok=0
        rm -f "$SB/red_bad" "$SB/red_out"
    fi
    verdict "$_er_name" "$_er_ok"
    restore
}

# expect_green <name> <required fragment> [forbidden fragment block]
expect_green() {
    _eg_name="$1"; _eg_need="$2"; _eg_forbid="${3:-}"
    _eg_out=$(gate); _eg_rc=$?
    _eg_ok=1
    if [ "$_eg_rc" -ne 0 ]; then
        echo "  FAIL: the gate went RED on something it must tolerate: $_eg_name"
        printf '%s\n' "$_eg_out" | grep -E '^FAIL|REFUSING' | sed 's/^/          /'
        _eg_ok=0
    elif ! printf '%s\n' "$_eg_out" | grep -qF "$_eg_need"; then
        echo "  FAIL: green, but it did not print what it must: $_eg_name"
        echo "        expected output to contain: $_eg_need"
        _eg_ok=0
    elif [ -n "$_eg_forbid" ]; then
        printf '%s\n' "$_eg_out" >"$SB/g_out"
        : >"$SB/g_bad"
        printf '%s\n' "$_eg_forbid" | while IFS= read -r _bad; do
            [ -n "$_bad" ] || continue
            grep -qF "$_bad" "$SB/g_out" || continue
            echo "  FAIL: green, but it printed something forbidden: $_eg_name"
            echo "        output must NOT contain: $_bad"
            printf 'x' >>"$SB/g_bad"
        done
        [ -s "$SB/g_bad" ] && _eg_ok=0
        rm -f "$SB/g_bad" "$SB/g_out"
    fi
    verdict "$_eg_name" "$_eg_ok"
    restore
}

F_REFUSE="FAIL: REFUSING"
F_NOTMEAS="NOT MEASURED"

# ---------------------------------------------------------- positive control --
# Without this, every control below is satisfied by a sandbox that is broken in
# some way none of them names.
restore
out=$(gate)
if [ $? -ne 0 ]; then
    echo "  FAIL: BROKEN BASELINE: the pristine sandbox + synthetic record is already RED."
    printf '%s\n' "$out" | grep -E '^FAIL|REFUSING' | sed 's/^/    /'
    exit 1
fi
echo "  PASS: positive control -- pristine sandbox with a well-formed record is green"
passed=$((passed + 1))

# ============================================================ GREEN CONTROLS ==
# Bounding the over-firing. A reader that refuses a record which merely
# PREDATES a correct edit reds on correct code, and that is the reader the next
# person weakens.

# The number of suites the synthetic record puts into the PROVEN population,
# DERIVED from the record itself -- never a literal. A control asserting only
# that the phrase "PROVEN ABLE TO GO RED" appears would pass on the sentence
# "0 are PROVEN ABLE TO GO RED", which is the exact opposite of what it means
# to check.
#
# THE FORBIDDEN FRAGMENT CARRIES A ", " PREFIX (vms-e5c/vms-221, MEASURED),
# not just "0 are PROVEN ABLE TO GO RED": the real sentence this guards
# against is "of those N, 0 are PROVEN ABLE TO GO RED", and grep -qF is a
# plain substring search -- so an UNPREFIXED needle also matches "of those
# N, 30 are PROVEN ABLE TO GO RED" (the tens digit's own trailing "0" reads
# as the needle). Hit for real once the manifest's total named-suite count
# happened to land on a multiple of ten (61 defects, 30 named suites,
# vms-e5c's and vms-221's anchors merging in the same window) -- a false
# "forbidden fragment" failure on an otherwise-correct PASS, not a defect
# in the gate being tested. The comma-space anchors on the actual sentence
# shape without switching this shared helper's matching mode off -F.
EXP_PROVEN=$(awk -F'\t' '$1=="RED" { print $3 }' "$SB/pristine.tsv" \
             | grep -vFx '(harness)' | sort -u | wc -l)
[ "$EXP_PROVEN" -ge 2 ] || { echo "  FAIL: BROKEN FIXTURE: only $EXP_PROVEN suite(s) in"
                             echo "        the synthetic record's RED rows"; exit 1; }
expect_green "a well-formed record moves suites into the PROVEN population, by count" \
    "$EXP_PROVEN are PROVEN ABLE TO GO RED" \
    "$F_REFUSE
$F_NOTMEAS
, 0 are PROVEN ABLE TO GO RED"

expect_green "the observed-executed count is derived from the record's RUN rows" \
    "$N_RUN of this manifest's" \
    "$F_REFUSE"

# GROWTH IS INCOMPLETENESS, NOT CONTRADICTION -- both directions, both green.
sed -i "/^RUN	$LIVE_DEFECT	/d;/^RED	$LIVE_DEFECT	/d" "$REC"
expect_green "a record that does not yet cover a defect the manifest has (the manifest grew)" \
    "1 manifest defect(s) are in no run this record covers" \
    "$F_REFUSE"

awk -F'\t' -v d="$LIVE_DEFECT" -v t="$LIVE_TEXT" \
    '!($1=="RED" && $2==d && $4==t)' "$REC" >"$SB/t" && mv "$SB/t" "$REC"
expect_green "a record missing one assertion text the manifest has since added" \
    "them, 1 carrying fewer" \
    "$F_REFUSE"

# The required fragment names the ABSENT-record message specifically, not the
# bare words "NOT MEASURED": section 2b prints its own NOT MEASURED line for
# every non-usable record state, so the generic phrase is satisfied by four
# different situations. MEASURED: with "NOT MEASURED" alone, deleting section
# 6b's absent-record disclosure outright left this suite at 24/0.
rm -f "$REC"
expect_green "no record committed at all -- degraded loudly, not silently certified" \
    "$F_NOTMEAS: no execution record at" \
    "$F_REFUSE
$EXP_PROVEN are PROVEN ABLE TO GO RED"

# ============================================================== RED CONTROLS ==
# The record contradicts the tree. Each is a REFUSAL: the gate certifies
# nothing from it, rather than certifying less.

sed -i "s/^RUN	$LIVE_DEFECT	/RUN	$SYNTH_DEFECT	/;s/^RED	$LIVE_DEFECT	/RED	$SYNTH_DEFECT	/" "$REC"
expect_red "the record names a defect the manifest no longer has (THE DELETION CASE)" \
    "it records defect(s) the manifest" \
    "assertion text(s) that
generated-at
positive-control"

awk -F'\t' -v d="$LIVE_DEFECT" -v t="$LIVE_TEXT" -v n="$SYNTH_TEXT" \
    'BEGIN{OFS="\t"} ($1=="RED" && $2==d && $4==t){ $4=n } {print}' "$REC" >"$SB/t" \
    && mv "$SB/t" "$REC"
expect_red "the record observes an assertion text the manifest does not name" \
    "assertion text(s) that" \
    "it records defect(s) the manifest
generated-at"

sed -i 's/^# positive-control: pass/# positive-control: fail/' "$REC"
expect_red "a record from a run whose pristine positive control did not pass" \
    "positive-control is" \
    "records defect(s)
assertion text(s) that"

sed -i '/^# generated-at:/d' "$REC"
expect_red "a record with no generated-at stamp" \
    "no generated-at stamp" \
    "records defect(s)
positive-control is"

sed -i 's/^# generated-at: .*/# generated-at: 2999-12-31T00:00:00Z/' "$REC"
expect_red "a record stamped in the future" \
    "generated-at is in the FUTURE" \
    "records defect(s)
no generated-at stamp"

sed -i '/^# tree-commit:/d' "$REC"
expect_red "a record that does not say which tree produced it" \
    "no tree-commit header" \
    "records defect(s)
generated-at"

printf 'RUN\tthree\tfields\n' >>"$REC"
expect_red "a row with too FEW fields" \
    "malformed row(s)" \
    "records defect(s)
positive-control is"

# A row with too MANY fields -- what an assertion text containing a TAB would
# produce. MEASURED why this is a separate control: with only the short-row
# case above, deleting the reader's `NF != 4` clause outright left the suite at
# 24/0, because a 3-field row also trips the "empty column 4" clause. The long
# row is the only case where NF != 4 is the sole mechanism, so it is the only
# case that proves that clause is load-bearing.
printf 'RED\t%s\ta_suite\tsplit\tby a tab\n' "$LIVE_DEFECT" >>"$REC"
expect_red "a row with too MANY fields (an assertion text carrying a tab)" \
    "want 4" \
    "records defect(s)
positive-control is"

printf 'RUN\t%s\t-\tmaybe\n' "$LIVE_DEFECT" >>"$REC"
expect_red "a RUN row whose verdict is neither pass nor fail" \
    "verdict must be pass|fail" \
    "records defect(s)"

grep "^RUN	$LIVE_DEFECT	" "$REC" >>"$REC.dup" && cat "$REC.dup" >>"$REC" && rm -f "$REC.dup"
expect_red "one defect with two RUN rows, so its verdict depends on read order" \
    "defect(s) with two RUN rows" \
    "malformed row(s)
records defect(s)"

sed -i "/^RUN	$LIVE_DEFECT	/d" "$REC"
expect_red "RED rows for a defect the record does not say was run" \
    "RED row(s) for defect(s) with no" \
    "records defect(s)
malformed row(s)"

grep '^#' "$SB/pristine.tsv" >"$REC"
expect_red "the record emptied of every data row, header left in place" \
    "ZERO data rows" \
    "malformed row(s)
records defect(s)"

rm -f "$LIB"
expect_red "the reader itself deleted, to make the gate print fewer claims" \
    "the execution-record reader" \
    "no execution record at"

# ============================================== THE PRICED ATTACK, MEASURED ==
# PR #65's disclosed residual, executed rather than assumed: a defect removed
# from DEFECTS with BOTH its case arms, ALL its anchors, and the declared floor
# lowered to match. That deletion satisfies every other check in `coverage`.
#
# WHICH DEFECT IS NOT WRITTEN DOWN. It is SEARCHED FOR, by executing the
# deletion against a record-free sandbox and keeping the first one that leaves
# `coverage` at rc=0 -- so the control's premise ("everything else tolerates
# this") is a measurement taken on the tree in front of it, not a fact about a
# name that some later item is free to change. A tree in which NO deletion
# passes would make this control vacuous, so that case FAILS here rather than
# passing quietly.
D_DEL=""
for _cand in $(sh "$MF" list); do
    rm -f "$REC"
    del_defect "$_cand"
    if gate >/dev/null 2>&1; then D_DEL="$_cand"; restore; break; fi
    restore
done
if [ -z "$D_DEL" ]; then
    echo "  FAIL: BROKEN FIXTURE: no single-defect deletion leaves coverage green with"
    echo "        the record removed, so the attack this control exists to price"
    echo "        cannot be staged and the control would prove nothing."
    failed=$((failed + 1)); status=1
else
    del_defect "$D_DEL"
    _price_files=0
    for _pf in "$MF" "$FLOOR" "$SB"/tests/qemu/test_kmod_*.c "$SB"/tests/qemu/test_syssvc_*.c; do
        _o="$ROOT/tests/qemu/$(basename "$_pf")"
        cmp -s "$_pf" "$_o" || _price_files=$((_price_files + 1))
    done
    _price_lines=$(
        for _pf in "$MF" "$FLOOR" "$SB"/tests/qemu/test_kmod_*.c "$SB"/tests/qemu/test_syssvc_*.c; do
            diff "$ROOT/tests/qemu/$(basename "$_pf")" "$_pf" 2>/dev/null
        done | grep -c '^[<>]' || true)
    echo "  MEASURED: deleting '$D_DEL' costs $_price_lines changed line(s) across"
    echo "            $_price_files file(s) and leaves every OTHER coverage check green."
    expect_red "that deletion, with the execution record still in the tree" \
        "it records defect(s) the manifest" \
        "malformed row(s)
positive-control is
generated-at"
fi

# ============================== the writer's own translation, directly ==
# fnr_emit_defect is the ONE step between what a QEMU run printed and what the
# record says. It cannot be reached through `coverage`, and it cannot be
# reached at all on a host that cannot boot the driver, so it is exercised
# here against a synthesized fail_map -- the exact
# "<suite-or-(harness)>\t<assertion text>" shape run_facility_negctl.sh's
# fail_map() produces.
emit_case() {  # emit_case <name> <expected-RUN-suites-field> <expected-RED-count>
    _ec_run=$(awk -F'\t' '$1 == "RUN" { print $3 }' "$SB/emit.tsv")
    _ec_nred=$(awk -F'\t' '$1 == "RED"' "$SB/emit.tsv" | grep -c . || true)
    _ec_ok=1
    if [ "$_ec_run" != "$2" ]; then
        echo "  FAIL: $1: RUN row's suite field is [$_ec_run], wanted [$2]"
        _ec_ok=0
    elif [ "$_ec_nred" -ne "$3" ]; then
        echo "  FAIL: $1: $_ec_nred RED row(s), wanted $3"
        _ec_ok=0
    fi
    verdict "$1" "$_ec_ok"
}

: >"$SB/emit.tsv"
printf 'test_kmod_lock\tan assertion in the lock suite\n'   >"$SB/map"
printf 'test_kmod_ast\tan assertion in the ast suite\n'    >>"$SB/map"
printf 'test_kmod_lock\tan assertion in the lock suite\n'  >>"$SB/map"
printf '(harness)\tvms.ko load or /dev/vms creation failed\n' >>"$SB/map"
fnr_emit_defect "$SB/emit.tsv" "$SYNTH_DEFECT" "$SB/map" pass
emit_case "the writer: (harness) is excluded from the RUN row's suite list and kept as a RED row" \
    "test_kmod_ast test_kmod_lock" 4

: >"$SB/emit.tsv"; : >"$SB/map"
fnr_emit_defect "$SB/emit.tsv" "$SYNTH_DEFECT" "$SB/map" fail
emit_case "the writer: a defect that reddened nothing gets a '-' suite field and no RED rows" \
    "-" 0

# ================================================= the comparator, directly ==
# fnr_compare is what runs in CI beside a live QEMU run, and it is the only
# thing in this mechanism that a committed record cannot lie to. It is not
# reachable through `coverage`, so it is exercised here directly.
cmp_case() {  # cmp_case <name> <expect-rc> <live> <committed> <required fragment>
    _cc_out=$(fnr_compare "$3" "$4" 2>&1); _cc_rc=$?
    _cc_ok=1
    if [ "$_cc_rc" -ne "$2" ]; then
        echo "  FAIL: fnr_compare returned $_cc_rc, wanted $2: $1"
        _cc_ok=0
    elif [ -n "$5" ] && ! printf '%s\n' "$_cc_out" | grep -qF "$5"; then
        echo "  FAIL: fnr_compare said the wrong thing: $1"
        echo "        expected: $5"
        printf '%s\n' "$_cc_out" | sed 's/^/          /'
        _cc_ok=0
    fi
    verdict "$1" "$_cc_ok"
}

cp "$SB/pristine.tsv" "$SB/live.tsv"
cp "$SB/pristine.tsv" "$SB/com.tsv"
sed -i 's/^# generated-at: .*/# generated-at: 1999-01-01T00:00:00Z/' "$SB/com.tsv"
cmp_case "identical rows compare equal even when the headers differ (they always do)" \
    0 "$SB/live.tsv" "$SB/com.tsv" ""

sed -i "0,/^RUN	/{/^RUN	/d}" "$SB/com.tsv"
cmp_case "a committed record BEHIND what the run observed" \
    1 "$SB/live.tsv" "$SB/com.tsv" "it is BEHIND the tree"

cp "$SB/pristine.tsv" "$SB/com.tsv"
sed -i "0,/^RUN	/{/^RUN	/d}" "$SB/live.tsv"
cmp_case "a committed record claiming an observation the run did NOT make" \
    1 "$SB/live.tsv" "$SB/com.tsv" "this run did not make"

echo ""
echo "=========================================================="
echo " Execution-record controls: $passed passed, $failed failed"
echo "=========================================================="
exit $status
