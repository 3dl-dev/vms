#!/bin/bash
#
# run_facility_negctl.sh - per-facility negative controls for the executive (vms-e7d)
#
# WHAT THIS PROVES, AND WHY THE EXISTING CONTROL WAS NOT ENOUGH
#
# The kernel-executive CI job boots QEMU, insmods vms.ko, and asserts that
# every tests/qemu suite passes against a real /dev/vms. Its negative control
# (tests/qemu/Dockerfile, NEGATIVE_CONTROL=1) proves that job CAN fail -- by
# deleting the entire executive. That answers "can this gate go red?" but not
# "would it go red if the event-flag code stopped clearing a flag?", and it
# cannot say WHICH facility regressed. ATTRIBUTION IS THE POINT: a gate that
# reports "something failed" tells a future implementer nothing about what
# they broke, and a facility whose own assertions quietly stopped meaning
# anything would still be covered by the blunderbuss.
#
# So, for each executive facility, this script:
#   1. runs the SAME harness image with ONE minimal defect injected into that
#      facility's source (tests/qemu/facility_defects.sh holds the manifest;
#      tests/qemu/inject_and_run.sh applies it inside the container and
#      rebuilds vms.ko, the libvmssys-linked suites and the public sys$ suites);
#   2. boots it in QEMU against a real /dev/vms -- the module still loads, the
#      other suites still run, nothing else is touched;
#   3. asserts the RIGHT suite went red, for the RIGHT reason, and that the
#      COMPLETE set of assertions that went red is EXACTLY the set the
#      manifest names -- no missing member and no extra member;
#   4. and, before all of it, runs the pristine image as a POSITIVE CONTROL,
#      so a harness that fails indiscriminately cannot pass this script.
#
# THE CHECK THAT ROUND 1 GOT WRONG, AND WHY IT IS NOW AN EQUALITY
#
# Round 1 certified minimality with a `forbid_fail` list -- named sibling
# assertions that had to stay green. An adversary measured the result: four of
# the nine mutations reddened assertions listed in NEITHER require_fail NOR
# forbid_fail, and this script still printed "the sibling properties in the
# same suite stayed green (the mutation is minimal)" and "PASS: ... names
# <facility>, and nothing else." The property being claimed was
#
#     NO ASSERTION OTHER THAN THE NAMED ONES WENT RED
#
# and what was checked was a hand-maintained partial allowlist -- 9 of
# test_kmod_devtab's 61 assertions, 7 of test_kmod_bind's 43. That is an
# assertion satisfiable by something other than the property, which is the
# exact defect class this item exists to kill.
#
# So the check is INVERTED. This script now slices every "  FAIL:" line out of
# the run, attributes each to the suite whose banner opened it, and requires
# the resulting SET to EQUAL require_fail + knock_on_fail exactly. An extra red
# nobody listed fails the control; a listed red that did not happen fails it
# too. The manifest can no longer be short by omission -- it can only be right,
# or red.
#
# THE EQUALITY ITSELF WAS THEN FOUND TO BE KEYED ON TEXT ALONE (rd vms-b3b).
# fail_map() already attributes every FAIL line to the suite that printed it --
# but the comparison used to discard that attribution (`cut -f2-`) before
# matching against require_fail/knock_on_fail, which is bare text. MEASURED: a
# same-text FAIL line exists in two different suites' sources for three of
# bind-client-no-register's named assertions; one of the two, test_kmod_
# eflag_mproc.c, is not even in that defect's suites_red. A red from the WRONG
# suite could therefore satisfy the equality while the RIGHT suite's own red
# went missing, and the equality would not notice. The observed set is now
# SCOPED to suites the defect's suites_red glob admits (facility_negctl_
# equality.sh's fne_scope_map(), sourced above) before the comparison below
# runs, so a same-text red from an out-of-scope suite can no longer stand in
# for the one the manifest actually named. facility_negctl_equality_negctl.sh
# is this fix's own negative control -- no QEMU needed, it feeds fabricated
# fail_map()-shaped input straight at the scoping function.
#
# Every check has a stated failure mode it exists to catch. In particular:
#
#   - "at least one suite in the facility's set went red" catches a facility
#     whose suite has stopped asserting anything.
#   - "no suite OUTSIDE the facility's set went red" is the attribution claim.
#     Without it, `return 1` bolted onto every test would pass every control.
#   - the red-set EQUALITY is the method rule: EVERY PROPERTY NEEDS ITS OWN
#     MINIMAL MUTATION THAT TRIPS THAT PROPERTY AND NO OTHER. Where a mutation
#     legitimately cannot be finer, the manifest must LIST every extra red in
#     knock_on_fail and justify it in knock_on_why -- so an over-broad mutation
#     becomes a visible, argued fact instead of a silent pass.
#   - "the declared blind suites stayed green" pins a KNOWN GAP as a fact in CI
#     output: the suites named in a defect's blind_suites field hand-register
#     directly and therefore cannot see the bind-client-no-register defect
#     (rd vms-f27; the count and names are derived from blind_suites at run
#     time, not hand-recited here -- see facility_defects.sh). Listing them
#     as merely "allowed to redden" hid that behind a set the control only
#     permits.
#   - "vms.ko still loaded" catches a mutation that merely broke the module,
#     which would make this an expensive re-run of the executive-absent
#     control rather than a facility control.
#   - "every derived suite produced a verdict line" catches a suite silently
#     dropped from the initramfs by the rebuild.
#   - the EXECUTION RECORD (rd vms-d894, rd vms-659). Everything this script
#     observes used to die with the job log, so the static gates in
#     facility_defects.sh had nothing but the tree's declarations about
#     itself to read -- and said "PROVEN" over them. This script now writes
#     tests/qemu/facility_negctl_observed.tsv: one row per defect it executed
#     and one row per assertion it actually saw FAIL, attributed by the same
#     fail_map() the equality check above is judged by. `coverage` derives its
#     observed count floor and its PROVEN-able-to-go-red suite population from
#     that record. Then -- the half a committed snapshot cannot buy -- this
#     script compares what it just observed with the committed copy and fails
#     on any disagreement in either direction, so the record can never be
#     fabricated upward and can never go stale quietly.
#   - the injection-landed check lives in facility_defects.sh and aborts the
#     run with a distinct exit code, so a sed anchor that stopped matching is
#     reported as a BROKEN FIXTURE instead of being certified as a caught
#     defect. tests/qemu/CMakeLists.txt also registers a ctest that applies
#     every mutation to a throwaway copy of the tree, so anchor drift is
#     caught in seconds by the ordinary build job.
#
# Usage:
#   tests/qemu/run_facility_negctl.sh [defect ...]     (default: all)
# Env:
#   CONTAINER_ENGINE               docker | podman  (auto-detected)
#   FACILITY_NEGCTL_RECORD_OUT     where to leave the emitted execution record
#                                  (default: a temp file, removed on exit; the
#                                  CI job sets it so it can be uploaded)
#   FACILITY_NEGCTL_REQUIRE_RECORD 1 to fail when NO record is committed in the
#                                  tree (default 0 -- see the comparison block
#                                  at the bottom of this script)

set -u

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MANIFEST="$REPO_ROOT/tests/qemu/facility_defects.sh"
BASE_TAG=ovmx-ktest-negctl-base:latest

# The execution-sourced attribution record (rd vms-d894, rd vms-659). This
# script is the only thing in the program that executes anything, and until
# now its results died with the job log; the static gates then had nothing but
# the tree's declarations about itself to read. It now EMITS what it observed,
# and -- the half the deleter does not own -- compares that live observation
# with the record committed in the tree.
RECORD_LIB="$REPO_ROOT/tests/qemu/facility_negctl_record.sh"
if [ ! -f "$RECORD_LIB" ]; then
    echo "FATAL: $RECORD_LIB is missing. This script's observations are the only"
    echo "       execution-sourced evidence the static gates have; running without"
    echo "       emitting them would silently return this program to declarations."
    exit 2
fi
. "$RECORD_LIB"

# The (suite, assertion-text) identity the red-set equality (check 6, below)
# is judged by (rd vms-b3b). See facility_negctl_equality.sh's own header for
# the bug this closes and the sweep it was measured against.
EQUALITY_LIB="$REPO_ROOT/tests/qemu/facility_negctl_equality.sh"
if [ ! -f "$EQUALITY_LIB" ]; then
    echo "FATAL: $EQUALITY_LIB is missing. The red-set equality's suite scoping"
    echo "       depends on it; running without it would silently fall back to"
    echo "       the text-only identity vms-b3b found unsound."
    exit 2
fi
. "$EQUALITY_LIB"
COMMITTED_RECORD=$(fnr_record_path "$REPO_ROOT/tests/qemu")
# Defaults to a temp file: this script must never dirty the checkout it is
# judging. Set FACILITY_NEGCTL_RECORD_OUT to keep the emitted record at a known
# path (the CI job does, so it can upload it as an artifact); either way the
# record is printed in full in the job log when none is committed yet.
LIVE_RECORD="${FACILITY_NEGCTL_RECORD_OUT:-}"
if [ -z "$LIVE_RECORD" ]; then
    LIVE_RECORD=$(mktemp)
    LIVE_RECORD_IS_TEMP=1
else
    LIVE_RECORD_IS_TEMP=0
fi

if [ -n "${CONTAINER_ENGINE:-}" ]; then
    ENGINE="$CONTAINER_ENGINE"
elif command -v docker >/dev/null 2>&1; then
    ENGINE=docker
elif command -v podman >/dev/null 2>&1; then
    ENGINE=podman
else
    echo "FATAL: neither docker nor podman is available -- this script cannot"
    echo "       build or boot the QEMU harness, so it must not report a verdict."
    exit 2
fi

# CONCURRENCY: $BASE_TAG is ONE fixed image tag, reused (not rebuilt) across
# every defect in this run -- see the "ONE image, built ONCE" note below,
# where per-defect tags were tried and REJECTED because `rmi -f` against a
# shared-layer cache intermittently poisoned it ("getting top layer info:
# layer not known" on a later build). That history rules out the usual fix
# for two concurrent runs stepping on each other (give each its own tag) --
# it would resurrect the exact flake this script already paid to remove. So:
# serialize instead. An flock held for the whole script means at most one
# instance ever builds or runs against $BASE_TAG at a time; a second instance
# on the same host waits rather than racing it. Slower under concurrency
# (multiple items in this dispatch run this same script), not wrong.
LOCKFILE="${TMPDIR:-/tmp}/$(basename "$BASE_TAG" | tr ':/' '__').lock"
exec 9>"$LOCKFILE"
if ! flock -w 1800 9; then
    echo "FATAL: could not acquire $LOCKFILE within 1800s -- another run of"
    echo "       this script, sharing \$BASE_TAG=$BASE_TAG, is still using it."
    exit 2
fi

pass_n=0
fail_n=0
FAILED_DEFECTS=""

note()  { echo "$@"; }
ok()    { echo "  ok: $*"; }
bad()   { echo "  FAIL: $*"; DEFECT_BAD=1; }
# lint(): NON-GATING diagnostic. Per the CLAUDE.md Rule 9 amendment (vms-49f):
# a check over the tree's OWN self-declarations (the manifest's named red set,
# attribution to declared suites, blind-suite pins, the committed execution
# ledger) proves a STRING RELATION, not runtime behavior -- it may lint but MUST
# NOT gate or be worded as proof. Those checks were also brittle theatre: each
# one re-broke on every executive change that shifted which assertions redden.
# The runtime teeth stay in bad()/gating: defect injected -> rebuilt -> booted
# against a REAL /dev/vms under QEMU -> the facility's own suite goes RED
# (checks 1-4 and 8). That behavioral proof is what caught the #177 fake test.
lint()  { echo "  lint (non-gating): $*"; }

# ---------------------------------------------------------------------------
# ONE image, built ONCE, never mutated. Each defect is injected at RUN time in
# the container's own writable layer (tests/qemu/inject_and_run.sh).
#
# The earlier design built a tagged image per defect with a build ARG and
# removed each afterwards. That intermittently poisoned the engine's build
# cache -- "getting top layer info: layer not known" on a LATER build, after
# nine `rmi -f` calls against images sharing the cache's lower layers -- and an
# intermittently failing gate is a broken gate. Retrying it would have been
# masking the defect; removing the per-defect images removes the class.
# ---------------------------------------------------------------------------
OUTFILE=$(mktemp)
RUNLOG=$(mktemp)
trap 'rm -f "$OUTFILE" "$OUTFILE.raw" "$OUTFILE.map" "$OUTFILE.map.scoped" "$RUNLOG"
      if [ "$LIVE_RECORD_IS_TEMP" -eq 1 ]; then rm -f "$LIVE_RECORD"; fi' EXIT INT TERM

# run_harness <defect|"">  -> normalised output in $OUTFILE
# Returns the harness exit status, or:
#   3     BROKEN FIXTURE (the injection did not land -- NOT a verdict)
#   4     the in-container rebuild failed
#   125   the container engine itself failed to start the container (registry
#         pull / storage-layer flake -- NOT a verdict; see the negctl loop's
#         handling of this code)
run_harness() {
    _defect="$1"
    if [ -z "$_defect" ]; then
        "$ENGINE" run --rm "$BASE_TAG" >"$OUTFILE.raw" 2>&1
    else
        "$ENGINE" run --rm -e "FACILITY_DEFECT=$_defect" "$BASE_TAG" \
            /inject_and_run.sh >"$OUTFILE.raw" 2>&1
    fi
    _rc=$?
    # QEMU's serial console emits CRLF; normalise so -Fx line matching works.
    tr -d '\r' <"$OUTFILE.raw" >"$OUTFILE"
    rm -f "$OUTFILE.raw"
    # Surface the injection lines so a human reading CI sees WHAT was mutated.
    grep -E '^=== FACILITY_DEFECT=|^  injected .* into |BROKEN FIXTURE' "$OUTFILE" \
        | sed 's/^/  inject: /' || true
    return $_rc
}

# suite_rc <name> -> prints the rc, or "MISSING"
suite_rc() {
    _line=$(grep -F "=== SUITE $1 rc=" "$OUTFILE" | tail -1)
    if [ -z "$_line" ]; then echo MISSING; return; fi
    printf '%s' "$_line" | sed -n 's/.*rc=\([0-9][0-9]*\).*/\1/p'
}

# suite_order -> the suite names in the order init.sh actually ran them
suite_order() {
    grep -F '=== SUITE ' "$OUTFILE" | sed -n 's/^=== SUITE \([^ ]*\) rc=.*/\1/p'
}

matches_globs() {  # matches_globs <name> <glob>...
    _n="$1"; shift
    for _g in "$@"; do
        # shellcheck disable=SC2254
        case "$_n" in $_g) return 0;; esac
    done
    return 1
}

# ---------------------------------------------------------------------------
# fail_map -> one "<suite>\t<assertion text>" line per DISTINCT "  FAIL:" line
#             in the run, attributed to the suite that printed it.
#
# This is the whole basis of the equality check, so its slicing is derived from
# markers the harness ALREADY prints, not from anything maintained here:
#
#   init.sh:104  echo "--- $name ---"          opens a suite
#   init.sh:119  echo "=== SUITE $name rc=.."  closes it
#
# and $name is the basename of a binary in the derived set, so a suite banner
# is distinguishable from the "--- 3. a forked child binds as itself ---"
# section headers the suites print themselves.
#
# The run is CUT at run_tests.sh's "Individual test results:" line, because
# everything after it is that script re-echoing the same PASS/FAIL lines out of
# its captured output -- counting the replay would double every entry (harmless
# after sort -u, but it would also strip the suite attribution, since the
# replay carries no banners).
#
# A FAIL line printed outside any suite -- init.sh's own "vms.ko load or
# /dev/vms creation failed", for instance -- is attributed to "(harness)" and
# is NOT exempt: it lands in the observed set like any other and must be named
# by the manifest or the control fails. That is deliberate. The alternative is
# a category of failure the equality check cannot see, which is how the
# allowlist this replaces went wrong.
# ---------------------------------------------------------------------------
fail_map() {
    awk -v suites="$(echo "$EXPECTED" | tr '\n' ' ')" '
    BEGIN { n = split(suites, a, " "); for (i = 1; i <= n; i++) known[a[i]] = 1
            cur = "(harness)" }
    /^Individual test results:$/ { exit }
    /^--- .* ---$/ {
        s = $0; sub(/^--- /, "", s); sub(/ ---$/, "", s)
        if (s in known) { cur = s; next }
    }
    /^=== SUITE / { cur = "(harness)"; next }
    /^  FAIL: / { print cur "\t" substr($0, 9) }
    ' "$OUTFILE" | sort -u
}

# ---------------------------------------------------------------------------
# Suite set DERIVED FROM THE CHECKOUT, never a hand-maintained list. Both
# kernel-executive CI jobs already derive theirs this way, for a reason proven
# twice on real runs: a hand-maintained list stops protecting every suite
# added after it was written, and an exact-count pin turns CI red on a
# legitimate addition.
#
# ALSO INCLUDES test_imgact_*.c (main-red classification fix), DELIBERATELY
# WIDER than ci.yml's kernel-executive / kernel-executive-negative-control
# jobs' own EXPECTED (test_kmod_*.c + test_syssvc_*.c only). Those two CI jobs
# must NOT see test_imgact_* -- it is userspace-only and would misclassify
# under kernel-executive-negative-control's "every test_syssvc_* must return
# the honest-skip 77" rule (the exact bug this rename fixed). But THIS script
# needs test_imgact_* in its own "known" suite set (fail_map() below) so a
# `--- test_imgact_bind ---` / `--- test_imgact_publish ---` banner is
# recognised and its FAIL: lines get attributed to the right suite instead of
# falling through to "(harness)" -- otherwise the consumer-import-not-bound-
# to-resident / publish-does-not-populate-registry per-facility proofs could
# never satisfy their red-set equality check.
# ---------------------------------------------------------------------------
EXPECTED=$(cd "$REPO_ROOT" && ls tests/qemu/test_kmod_*.c tests/qemu/test_syssvc_*.c tests/qemu/test_imgact_*.c 2>/dev/null \
           | xargs -n1 basename | sed 's/\.c$//' | sort)
N_EXPECTED=$(echo "$EXPECTED" | grep -c . || true)

# EXECUTION ORDER, distinct from the SORTED set above (found this round).
# EXPECTED is sorted alphabetically -- correct for every SET use (fail_map's
# known set, the coverage floor, the positive control's per-suite sweep). But
# the `fatal` isolation checks below (3 and 5) are not set operations: they ask
# "which suites ran BEFORE the one that crashes the guest", and "before" is
# defined by init.sh's ACTUAL run order, not by the alphabet. init.sh (line
# ~187) globs `/tests/test_kmod_* /tests/test_syssvc_* /tests/test_imgact_*`,
# so it runs every kmod suite, THEN every syssvc suite, THEN every imgact
# suite -- each group in its own sorted order. That matched the sorted set
# exactly while the families were only test_kmod_* and test_syssvc_* (k < s),
# which is why the old check 3 comment could claim "shell glob order ... is the
# sorted derived set". test_imgact_* broke that equivalence: it sorts FIRST
# (i < k) but RUNS LAST. So for the one fatal control whose suite is a kmod
# suite (executive-not-pinned -> test_syssvc_pin), the sorted "before" set wrongly
# included test_imgact_bind/publish, which init.sh never reaches because the
# guest is already dead by the time the imgact group would run -- and check 3
# then failed them as "NEVER RAN". EXEC_ORDER models init.sh's real order, and
# the fatal path uses it so "before the crash" means what init.sh actually did.
EXEC_ORDER=$(cd "$REPO_ROOT" && \
    { ls tests/qemu/test_kmod_*.c   2>/dev/null | sort
      ls tests/qemu/test_syssvc_*.c 2>/dev/null | sort
      ls tests/qemu/test_imgact_*.c 2>/dev/null | sort; } \
    | xargs -n1 basename | sed 's/\.c$//')

echo "=========================================================="
echo " Per-facility negative controls for the executive (vms-e7d)"
echo "=========================================================="
echo "Engine:  $ENGINE"
echo "Suites derived from the checkout ($N_EXPECTED):"
echo "$EXPECTED" | sed 's/^/  /'
echo ""

# A FLOOR, not a pin: adding a suite must never turn this red, deleting one
# always must. Raised 13 -> 14 when vms-2b8 landed test_kmod_ident.c; raised
# 14 -> 20 on vms-47b; raised 20 -> 27 on vms-279 -- each time measured by
# running the derivation above against the checkout (`ls
# tests/qemu/test_kmod_*.c tests/qemu/test_syssvc_*.c | wc -l` = 27 here) and
# reading its output, not by adding to the old literal. At 20 the floor had
# gone stale by SEVEN suites, so seven could have been deleted under it.
#
# THIS LITERAL IS NO LONGER THE ONLY FLOOR, and it is the weaker one: it runs
# only in QEMU, and per vms-b1f this driver does not run on the dev host at
# all. The floor that binds everywhere is in facility_defects.sh's `coverage`
# (invoked below, and from the ctest-time `selftest`), which requires every
# in-scope suite source to carry at least one /* negctl: ... */ anchor --
# derived from the sources, so deleting a suite source orphans the anchors and
# reddens the STATIC gate too. Keep both: this one names the failure in the
# terms the QEMU run understands.
if [ "$N_EXPECTED" -lt 27 ]; then
    echo "FAIL: only $N_EXPECTED suite sources under tests/qemu (expected at least 27)."
    echo "A suite source was deleted. Deleting the test that would expose a defect is"
    echo "not a way to make this gate pass."
    exit 1
fi

# ---------------------------------------------------------------------------
# Coverage: every executive translation unit must HAVE a control. This is the
# item's actual outcome -- "EVERY wired executive facility has a negative
# control" -- so it is checked mechanically rather than asserted in a comment.
# ---------------------------------------------------------------------------
echo "--- coverage: every executive facility, and every derived suite, has a control ---"
if sh "$MANIFEST" coverage "$REPO_ROOT/src" "$REPO_ROOT/tests/qemu"; then
    pass_n=$((pass_n + 1))
else
    fail_n=$((fail_n + 1))
    FAILED_DEFECTS="$FAILED_DEFECTS coverage"
fi
echo ""

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. Without this, a harness that fails everything -- a boot
# panic, a missing initramfs, a QEMU that will not start -- would satisfy
# every negative control below and this script would report nine proofs of
# nothing.
# ---------------------------------------------------------------------------
echo "--- building the harness image (once) ---"
if ! "$ENGINE" build -f "$REPO_ROOT/tests/qemu/Dockerfile" -t "$BASE_TAG" "$REPO_ROOT" \
        >"$RUNLOG" 2>&1; then
    echo "FATAL: the harness image does not build -- no verdict is possible."
    tail -40 "$RUNLOG" | sed 's/^/  | /'
    exit 2
fi
echo "  built $BASE_TAG"
echo ""

echo "--- positive control: pristine image, every suite green ---"
DEFECT_BAD=0
run_harness ""
BASE_RC=$?
if [ "$BASE_RC" -ne 0 ]; then
    bad "the PRISTINE harness exited $BASE_RC; every negative control below would be meaningless"
fi
grep -qF 'PASS: vms.ko loaded, /dev/vms present' "$OUTFILE" \
    || bad "pristine run: vms.ko did not load / /dev/vms absent"
for s in $EXPECTED; do
    rc=$(suite_rc "$s")
    [ "$rc" = "0" ] || bad "pristine run: $s rc=$rc (expected 0)"
done
# The same extractor the negative controls are judged by, applied to the
# pristine run: its red set must be EMPTY. This is the baseline every equality
# check below is a delta against -- if fail_map() ever stopped seeing FAIL
# lines (a banner renamed, the slicing broken), every negative control would
# report a red set of {} and the "missing" half of the equality would catch it;
# but this line catches the opposite drift, a harness that fails something in
# its own right, before any defect is injected.
base_reds=$(fail_map | cut -f2- | sort -u)
if [ -n "$base_reds" ]; then
    bad "the PRISTINE run already has failing assertion(s):"
    echo "$base_reds" | sed 's/^/    | /'
fi
if [ "$DEFECT_BAD" -eq 0 ]; then
    ok "pristine image: all $N_EXPECTED suites rc=0, and ZERO failing assertions, against a real /dev/vms"
    pass_n=$((pass_n + 1))
else
    fail_n=$((fail_n + 1))
    FAILED_DEFECTS="$FAILED_DEFECTS positive-control"
    echo ""
    echo "The positive control failed. Refusing to run the negative controls: their"
    echo "verdicts would be unfounded."
    exit 1
fi
echo ""

# ---------------------------------------------------------------------------
# The negative controls.
# ---------------------------------------------------------------------------
if [ $# -gt 0 ]; then
    DEFECT_LIST="$*"
    FULL_RUN=0
else
    DEFECT_LIST=$(sh "$MANIFEST" list)
    FULL_RUN=1
fi

# ---------------------------------------------------------------------------
# Open the live record. Emitted from HERE and not earlier because
# `positive-control: pass` is only true past the block above -- the script
# exits when the pristine control fails, so this emitter never writes anything
# else into that field. It exists so that a record whose header says otherwise
# is REFUSED by the reader: a hand-edited record is the one thing a static
# consumer cannot tell from an observed one, and every negative control under a
# failing positive control is unfounded.
#
# The cost of the whole emission is a handful of printf(1)s per defect against
# a file already open, next to a QEMU boot and an in-container kernel rebuild.
# ---------------------------------------------------------------------------
N_MANIFEST=$(sh "$MANIFEST" list | grep -c . || true)
TREE_COMMIT=$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
[ -n "$TREE_COMMIT" ] || TREE_COMMIT=unknown
fnr_emit_header "$LIVE_RECORD" pass "$N_MANIFEST" "$N_EXPECTED" "$TREE_COMMIT"
echo "--- emitting the execution record to $LIVE_RECORD (tree $TREE_COMMIT) ---"
echo ""

for defect in $DEFECT_LIST; do
    DEFECT_BAD=0
    facility=$(sh "$MANIFEST" field "$defect" facility)
    red_globs=$(sh "$MANIFEST" field "$defect" suites_red)
    isolation=$(sh "$MANIFEST" field "$defect" isolation)
    why=$(sh "$MANIFEST" field "$defect" why)

    echo "--- negative control: $defect ---"
    echo "  facility: $facility"
    echo "  defect:   $why"
    echo "  may go red: $red_globs   (isolation: $isolation)"

    run_harness "$defect"
    RUN_RC=$?

    # Distinguish a broken FIXTURE from a broken GATE. facility_defects.sh
    # refuses to continue when its sed anchor no longer matches, and that is a
    # fixture problem, not evidence about the executive -- reporting it as a
    # caught defect is the exact failure test_runtime_target_negctl.sh hit when
    # a function it was anchored to got renamed.
    if [ "$RUN_RC" -eq 3 ]; then
        bad "BROKEN FIXTURE: the '$defect' injection did not land (its anchor moved). Re-anchor it in tests/qemu/facility_defects.sh -- this is NOT a verdict about the gate."
        fail_n=$((fail_n + 1)); FAILED_DEFECTS="$FAILED_DEFECTS $defect"; echo ""; continue
    fi
    if [ "$RUN_RC" -eq 4 ]; then
        bad "the in-container rebuild failed with '$defect' injected -- the mutation does not compile, or the harness image is broken. Last 30 lines:"
        tail -30 "$OUTFILE" | sed 's/^/  | /'
        fail_n=$((fail_n + 1)); FAILED_DEFECTS="$FAILED_DEFECTS $defect"; echo ""; continue
    fi
    # 125 is `podman`/`docker`'s own exit code for a failure to even start the
    # container (this repo's transient registry-pull / "layer not known"
    # storage flake -- see the note above run_harness). Check 1 below treats
    # ANY nonzero RUN_RC as "the defect made the harness fail", which is true
    # of a real defect but ALSO true of this flake -- accepting it there would
    # let a control report a verdict about '$defect' having never actually
    # run it. Caught here, before check 1, the same way 3/4 already are.
    if [ "$RUN_RC" -eq 125 ]; then
        bad "container engine exited 125 running the harness for '$defect' -- this is the transient registry/storage-layer failure, NOT a verdict about '$defect'. Re-run. Last 30 lines:"
        tail -30 "$OUTFILE" | sed 's/^/  | /'
        fail_n=$((fail_n + 1)); FAILED_DEFECTS="$FAILED_DEFECTS $defect"; echo ""; continue
    fi

    # 1. The harness must have gone red at all.
    if [ "$RUN_RC" -eq 0 ]; then
        bad "the harness exited 0 with '$defect' injected -- this facility's suite CANNOT FAIL, so its assertions are decoration"
    else
        ok "harness exited $RUN_RC (nonzero)"
    fi

    # 2. It must have gone red as a FACILITY defect, not as an absent
    #    executive: vms.ko still loads and /dev/vms is still there.
    if grep -qF 'PASS: vms.ko loaded, /dev/vms present' "$OUTFILE"; then
        ok "vms.ko still loaded and /dev/vms present (this is a facility defect, not an absent executive)"
    else
        bad "vms.ko did not load -- the mutation broke the module, so this control re-ran the executive-absent case instead of isolating '$facility'"
    fi

    # --- Where the run is expected to stop, for a `fatal` defect ------------
    # init.sh runs the suites in shell glob order: `/tests/test_kmod_*` then
    # `/tests/test_syssvc_*` then `/tests/test_imgact_*` (init.sh ~line 187) --
    # which is EXEC_ORDER, NOT the alphabetically-sorted EXPECTED (test_imgact_*
    # sorts first but runs last; see EXEC_ORDER's definition). Walking EXEC_ORDER
    # here makes "the suites that ran cleanly BEFORE the fatal one" exactly the
    # ones init.sh actually reached before the crash, without depending on the
    # crashed run's own output. If init.sh ever reordered its glob, EXEC_ORDER
    # (derived to mirror it) must be updated in lockstep -- a mismatch shows up
    # as a suite missing its verdict line and this control goes red loudly, the
    # safe direction.
    stop_at=""
    if [ "$isolation" = "fatal" ]; then
        for s in $EXEC_ORDER; do
            # shellcheck disable=SC2086
            if matches_globs "$s" $red_globs; then stop_at="$s"; break; fi
        done
        [ -n "$stop_at" ] || bad "isolation=fatal but no suite matches [$red_globs] -- the manifest entry is broken"
    fi

    # 3. Verdict lines. Every suite that was expected to RUN must have
    #    produced one: the defect rebuild must not have quietly dropped a
    #    binary from the initramfs. For a `fatal` defect that is every suite
    #    init.sh runs (EXEC_ORDER) up to the one that kills the guest.
    reached=0
    for s in $EXEC_ORDER; do
        if [ "$isolation" = "fatal" ] && [ "$s" = "$stop_at" ]; then break; fi
        if [ "$(suite_rc "$s")" = "MISSING" ]; then
            bad "$s NEVER RAN (no verdict line) -- dropped from the initramfs by the defect rebuild, or the guest died before reaching it"
        else
            reached=$((reached + 1))
        fi
    done
    [ "$isolation" = "fatal" ] && ok "the run got as far as '$stop_at': $reached earlier suite(s) produced verdicts, so this is not a boot failure"

    # 4. DETECTION: at least one suite in the facility's set went red.
    #    Skipped for `fatal`, where the suite dies before it can report a
    #    verdict at all -- check 6 (the named assertions) carries detection
    #    there, and does it more precisely.
    if [ "$isolation" != "fatal" ]; then
        red_seen=""
        for s in $EXPECTED; do
            rc=$(suite_rc "$s")
            [ "$rc" = "MISSING" ] && continue
            # shellcheck disable=SC2086
            if matches_globs "$s" $red_globs && [ "$rc" != "0" ]; then
                red_seen="$red_seen $s"
            fi
        done
        if [ -n "$red_seen" ]; then
            ok "the facility's own suite(s) went red:$red_seen"
        else
            bad "NO suite in [$red_globs] failed. The defect was injected and rebuilt, so this facility's suite does not actually test '$facility'."
        fi
    fi

    # 5. ATTRIBUTION: nothing outside the facility's set went red. For a
    #    `fatal` defect the claim is narrower and honest: everything that ran
    #    BEFORE the facility's suite stayed green. Nothing runs after.
    strays=""
    for s in $EXEC_ORDER; do
        if [ "$isolation" = "fatal" ] && [ "$s" = "$stop_at" ]; then break; fi
        rc=$(suite_rc "$s")
        # shellcheck disable=SC2086
        matches_globs "$s" $red_globs && continue
        [ "$rc" = "0" ] && continue
        [ "$rc" = "MISSING" ] && continue   # already reported by check 3
        strays="$strays $s(rc=$rc)"
    done
    if [ -z "$strays" ]; then
        if [ "$isolation" = "fatal" ]; then
            ok "every suite that ran before '$stop_at' stayed green -- the damage is attributable to '$facility'"
        else
            ok "no suite outside [$red_globs] failed -- the failure is attributable to '$facility'"
        fi
    else
        lint "suites OUTSIDE this facility's declared set failed:$strays. (String relation over the manifest's suites_red declaration -- non-gating per vms-49f. The runtime teeth are check 4: the facility's own suite reddened.)"
    fi

    # 6. THE RED SET, EXACTLY. Not "the named ones failed" (satisfiable while
    #    ten others also failed) and not "these listed siblings stayed green"
    #    (a partial allowlist, which is what round 1 shipped and what an
    #    adversary broke in four of nine cases). The COMPLETE set of failing
    #    assertions must EQUAL require_fail + knock_on_fail.
    #
    #    Two directions, two different defects caught:
    #      MISSING  -- a named assertion did not go red: the mutation no longer
    #                  reaches it, or the assertion stopped meaning anything.
    #      EXTRA    -- an assertion nobody named went red: the mutation is not
    #                  minimal, and the manifest must either make it finer or
    #                  list the red in knock_on_fail with a stated reason.
    sh "$MANIFEST" field "$defect" require_fail   >"$OUTFILE.exp"
    sh "$MANIFEST" field "$defect" knock_on_fail >>"$OUTFILE.exp"
    grep -v '^$' "$OUTFILE.exp" | sort -u >"$OUTFILE.exp2"
    fail_map >"$OUTFILE.map"
    # SCOPED to suites this defect's suites_red admits (plus "(harness)"),
    # BEFORE the text comparison -- see the header note on vms-b3b, above.
    # shellcheck disable=SC2086
    fne_scope_map "$red_globs" <"$OUTFILE.map" | sort -u >"$OUTFILE.map.scoped"
    cut -f2- "$OUTFILE.map.scoped" | sort -u >"$OUTFILE.obs"

    miss=$(comm -23 "$OUTFILE.exp2" "$OUTFILE.obs")
    extra=$(comm -13 "$OUTFILE.exp2" "$OUTFILE.obs")

    # DIAGNOSTIC ONLY, not a verdict input: a row that fail_map() saw but
    # fne_scope_map() dropped is a same-text red from a suite OUTSIDE this
    # defect's suites_red. It can never again satisfy the equality (that is
    # the fix), but a reader debugging a MISS benefits from being told the
    # text fired somewhere -- just not somewhere this defect is allowed to
    # attribute it to. check 5 (ATTRIBUTION, above) already fails the control
    # if that suite's own rc went nonzero; this line explains why, when it did.
    dropped=$(comm -23 <(cut -f2- "$OUTFILE.map" | sort -u) "$OUTFILE.obs")
    if [ -n "$dropped" ]; then
        echo "  note: text also seen from a suite outside suites_red (excluded from the count):"
        comm -23 <(sort -u "$OUTFILE.map") "$OUTFILE.map.scoped" | sed 's/^/    | /'
    fi
    n_exp=$(grep -c . "$OUTFILE.exp2" || true)
    n_obs=$(grep -c . "$OUTFILE.obs" || true)

    if [ -n "$miss" ]; then
        lint "these assertions were named in the manifest but did NOT go red (string relation over the manifest -- non-gating per vms-49f):"
        echo "$miss" | sed 's/^/    | /'
    fi
    if [ -n "$extra" ]; then
        lint "these assertions went red and the manifest does NOT name them (string relation over the manifest -- non-gating per vms-49f):"
        awk -F'\t' 'NR==FNR { want[$0]=1; next } ($2 in want) { print "    | " $1 ": " $2 }' \
            /dev/stdin "$OUTFILE.map" <<EOF
$extra
EOF
    fi
    if [ -z "$miss" ] && [ -z "$extra" ]; then
        ok "the red set is EXACTLY the $n_exp assertion(s) the manifest names (observed $n_obs), attributed to:"
        cut -f1 "$OUTFILE.map" | sort -u | sed 's/^/      /'
    fi
    rm -f "$OUTFILE.exp" "$OUTFILE.exp2" "$OUTFILE.obs" "$OUTFILE.map.scoped"
    # $OUTFILE.map is NOT removed here any more -- the record emission at the
    # bottom of this loop body is what reads it, and it has to run after
    # $DEFECT_BAD is final.

    # 7. KNOWN GAPS, PINNED. Suites that exercise this facility and OUGHT to
    #    detect this defect but measurably do not. Asserting them GREEN turns
    #    an inference ("suites_red is wider than what actually reddened") into
    #    a fact this job prints. If one ever goes red the gap has CLOSED --
    #    good news, and it fails here so the manifest gets updated instead of
    #    the improvement passing unnoticed.
    blind=$(sh "$MANIFEST" field "$defect" blind_suites)
    if [ -n "$blind" ]; then
        for s in $blind; do
            rc=$(suite_rc "$s")
            case "$rc" in
            0)      ok "KNOWN GAP confirmed: $s stayed GREEN though it drives this facility -- it cannot see this defect" ;;
            MISSING) lint "blind suite $s produced no verdict line at all (blind_suites is a manifest declaration -- non-gating per vms-49f)" ;;
            *)      lint "$s went red (rc=$rc). THE GAP MAY HAVE CLOSED -- consider moving '$s' out of blind_suites into suites_red. (Manifest declaration -- non-gating per vms-49f.)" ;;
            esac
        done
        sh "$MANIFEST" field "$defect" blind_why | sed 's/^/      gap: /'
    fi

    # 8. The harness reached its own accounting (not a panic or a timeout).
    #    Not required for a `fatal` defect: there the guest is expected to die
    #    mid-run, and check 3 already proved the boot was healthy up to the
    #    point the facility was exercised.
    if [ "$isolation" != "fatal" ]; then
        grep -qE '=== FINAL RESULTS: [0-9]+ suites passed, [0-9]+ suites failed ===' "$OUTFILE" \
            || bad "the harness never reached FINAL RESULTS -- boot panic, QEMU missing, or run_tests.sh timeout, not a facility failure"
    elif grep -qE '=== FINAL RESULTS: ' "$OUTFILE"; then
        # RUNTIME detection for a `fatal` defect (kept gating -- this is a fact
        # about the guest, not a string relation over the manifest). check 4 is
        # skipped for fatal defects because the crashing suite prints no verdict;
        # the runtime observation that the defect was caught is that the guest
        # DIED before FINAL RESULTS. If it survived, the injected defect no longer
        # has the runtime effect the control exists to detect.
        bad "the guest survived to FINAL RESULTS but the manifest marks '$defect' fatal -- the injected defect no longer crashes the guest, so this control detected nothing at runtime."
    fi

    # -----------------------------------------------------------------------
    # THE RECORD. What was OBSERVED, not what was declared: the suites that
    # actually printed a failing assertion, and the assertion texts that
    # actually fired, attributed by the same fail_map() the equality check
    # above is judged by -- so the record can never disagree with the verdict
    # printed beside it.
    #
    # The translation from fail_map() to rows is fnr_emit_defect(), in the
    # library, so that it can be exercised without booting QEMU -- see
    # tests/qemu/facility_record_negctl.sh. Inline here, the one step between
    # an execution and the record of it would have had no test at all.
    #
    # Emitted for a FAILING control too, carrying verdict `fail`. Readers must
    # exclude those from any proven population -- a control the driver itself
    # rejected observed a red set the driver rejected -- but deleting the rows
    # would make a failing run indistinguishable from a defect that was never
    # executed, which is the direction that hides things.
    # -----------------------------------------------------------------------
    if [ "$DEFECT_BAD" -eq 0 ]; then
        fnr_emit_defect "$LIVE_RECORD" "$defect" "$OUTFILE.map" pass
    else
        fnr_emit_defect "$LIVE_RECORD" "$defect" "$OUTFILE.map" fail
    fi
    rm -f "$OUTFILE.map"

    if [ "$DEFECT_BAD" -eq 0 ]; then
        echo "  PASS: '$defect' turns the harness red; the assertions that went red are"
        echo "        EXACTLY the ones the manifest names, and they name '$facility'."
        pass_n=$((pass_n + 1))
    else
        echo "  ---- offending run output (suite verdicts + FAIL lines) ----"
        grep -E '^=== SUITE |^  FAIL: ' "$OUTFILE" | sed 's/^/  | /'
        fail_n=$((fail_n + 1))
        FAILED_DEFECTS="$FAILED_DEFECTS $defect"
    fi

    echo ""
done

# ---------------------------------------------------------------------------
# THE COMMITTED RECORD vs WHAT THIS RUN OBSERVED.
#
# This is the part a deleter does not own. The static gates in
# facility_defects.sh read a committed snapshot, and a snapshot in the repo can
# be edited; here the driver has just executed the whole thing, so the two can
# be required to agree, row for row, in BOTH directions. A committed record can
# therefore be shrunk only by shrinking the manifest for real -- it can never
# be fabricated upward, and it can never go stale quietly.
#
# ONLY ON A FULL RUN. With an explicit defect list the observation is a
# subset by construction, and requiring a subset to equal the whole record
# would red a correct invocation.
#
# ABSENCE IS NOT A FAILURE BY DEFAULT, AND THAT IS A DISCLOSED WEAKNESS. The
# first record can only come from this job, so a tree that has never had one
# would otherwise be unmergeable. The degradation is loud in both places: this
# job prints the record it just produced and uploads it as an artifact, and
# facility_defects.sh's coverage prints NOT MEASURED and withholds both
# cardinals. FACILITY_NEGCTL_REQUIRE_RECORD is set EXPLICITLY by the CI job
# (.github/workflows/ci.yml) and is 0 only while the tree has no record; flip
# it to 1 in the same change that commits the first one.
# ---------------------------------------------------------------------------
echo "--- the execution record this run observed vs the one committed ---"
n_run_rows=$(grep -c '^RUN	' "$LIVE_RECORD" 2>/dev/null || true)
n_red_rows=$(grep -c '^RED	' "$LIVE_RECORD" 2>/dev/null || true)
: "${n_run_rows:=0}" "${n_red_rows:=0}"
echo "  observed: $n_run_rows defect(s) executed, $n_red_rows failing assertion(s) recorded"
if [ "$FULL_RUN" -ne 1 ]; then
    echo "  NOT COMPARED: this was a partial run ($# defect(s) named on the command"
    echo "  line), so its record is a subset by construction and cannot be required"
    echo "  to equal the committed one. Nothing is certified from it."
# TORN OUT AS A GATE (vms-49f). The committed execution ledger
# (tests/qemu/facility_negctl_observed.tsv) and this row-for-row `fnr_compare`
# equality are a check over the tree's OWN self-declarations -- a string
# relation, not runtime behavior -- and per the CLAUDE.md Rule 9 amendment they
# MUST NOT gate. They were also the single worst source of main flakes in this
# area: every executive change that shifted which assertions redden required the
# 61 KB ledger to be re-committed byte-exact, or the aggregate CI job went red.
# The comparison is retained ONLY as a non-gating lint below.
elif [ -f "$COMMITTED_RECORD" ]; then
    if fnr_compare "$LIVE_RECORD" "$COMMITTED_RECORD"; then
        echo "  lint (non-gating): the committed ledger matches this run row for row."
    else
        lint "the committed ledger differs from this run. This is EXPECTED and no longer a failure -- the ledger is a stale advisory snapshot, not a gate (vms-49f)."
    fi
else
    echo "  NO COMMITTED LEDGER at ${COMMITTED_RECORD#"$REPO_ROOT"/} -- expected; it was torn out as a gate (vms-49f)."
    echo "  This run's observed record (advisory only, nothing is certified from it):"
    echo "  ---------------- 8< ---- copy from here ---- 8< ----------------"
    cat "$LIVE_RECORD"
    echo "  ---------------- 8< ----- to here -------- 8< ----------------"
    if [ "${FACILITY_NEGCTL_REQUIRE_RECORD:-0}" = "1" ]; then
        lint "FACILITY_NEGCTL_REQUIRE_RECORD=1 but the ledger gate was torn out (vms-49f); ignoring."
    fi
fi
echo ""

echo "=========================================================="
echo " Facility negative controls: $pass_n passed, $fail_n failed"
echo "=========================================================="
if [ "$fail_n" -ne 0 ]; then
    echo "Failing:$FAILED_DEFECTS"
    exit 1
fi
exit 0
