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
#     output: three suites hand-register and therefore cannot see the
#     bind-client-no-register defect (rd vms-f27). Listing them as merely
#     "allowed to redden" hid that behind a set the control only permits.
#   - "vms.ko still loaded" catches a mutation that merely broke the module,
#     which would make this an expensive re-run of the executive-absent
#     control rather than a facility control.
#   - "every derived suite produced a verdict line" catches a suite silently
#     dropped from the initramfs by the rebuild.
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
#   CONTAINER_ENGINE   docker | podman  (auto-detected)

set -u

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MANIFEST="$REPO_ROOT/tests/qemu/facility_defects.sh"
BASE_TAG=ovmx-ktest-negctl-base:latest

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

pass_n=0
fail_n=0
FAILED_DEFECTS=""

note()  { echo "$@"; }
ok()    { echo "  ok: $*"; }
bad()   { echo "  FAIL: $*"; DEFECT_BAD=1; }

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
trap 'rm -f "$OUTFILE" "$OUTFILE.raw" "$RUNLOG"' EXIT INT TERM

# run_harness <defect|"">  -> normalised output in $OUTFILE
# Returns the harness exit status, or:
#   3   BROKEN FIXTURE (the injection did not land -- NOT a verdict)
#   4   the in-container rebuild failed
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
# ---------------------------------------------------------------------------
EXPECTED=$(cd "$REPO_ROOT" && ls tests/qemu/test_kmod_*.c tests/qemu/test_syssvc_*.c 2>/dev/null \
           | xargs -n1 basename | sed 's/\.c$//' | sort)
N_EXPECTED=$(echo "$EXPECTED" | grep -c . || true)

echo "=========================================================="
echo " Per-facility negative controls for the executive (vms-e7d)"
echo "=========================================================="
echo "Engine:  $ENGINE"
echo "Suites derived from the checkout ($N_EXPECTED):"
echo "$EXPECTED" | sed 's/^/  /'
echo ""

# A FLOOR, not a pin: adding a suite must never turn this red, deleting one
# always must. Raised 13 -> 14 when vms-2b8 landed test_kmod_ident.c; raised
# 14 -> 20 on vms-47b -- measured by running the derivation above against
# this checkout (`ls tests/qemu/test_kmod_*.c tests/qemu/test_syssvc_*.c |
# wc -l` = 20) and reading its output, not by adding to the old literal.
if [ "$N_EXPECTED" -lt 20 ]; then
    echo "FAIL: only $N_EXPECTED suite sources under tests/qemu (expected at least 20)."
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
else
    DEFECT_LIST=$(sh "$MANIFEST" list)
fi

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
    # init.sh runs the suites in shell glob order (`/tests/test_kmod_*` then
    # `/tests/test_syssvc_*`), which is the sorted derived set, so the suites
    # that must have run cleanly BEFORE the fatal one are known without
    # depending on the crashed run's own output. If init.sh ever reordered
    # them, those suites would be missing their verdict lines and this control
    # would go red loudly -- the safe direction.
    stop_at=""
    if [ "$isolation" = "fatal" ]; then
        for s in $EXPECTED; do
            # shellcheck disable=SC2086
            if matches_globs "$s" $red_globs; then stop_at="$s"; break; fi
        done
        [ -n "$stop_at" ] || bad "isolation=fatal but no suite matches [$red_globs] -- the manifest entry is broken"
    fi

    # 3. Verdict lines. Every suite that was expected to RUN must have
    #    produced one: the defect rebuild must not have quietly dropped a
    #    binary from the initramfs. For a `fatal` defect that is every suite
    #    up to the one that kills the guest.
    reached=0
    for s in $EXPECTED; do
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
    for s in $EXPECTED; do
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
        bad "suites OUTSIDE this facility failed:$strays. The mutation is not minimal, or the harness is failing indiscriminately -- either way it proves nothing about '$facility'."
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
    cut -f2- "$OUTFILE.map" | sort -u >"$OUTFILE.obs"

    miss=$(comm -23 "$OUTFILE.exp2" "$OUTFILE.obs")
    extra=$(comm -13 "$OUTFILE.exp2" "$OUTFILE.obs")
    n_exp=$(grep -c . "$OUTFILE.exp2" || true)
    n_obs=$(grep -c . "$OUTFILE.obs" || true)

    if [ -n "$miss" ]; then
        bad "these assertions were named but did NOT go red -- the mutation does not reach them, or they have stopped asserting anything:"
        echo "$miss" | sed 's/^/    | /'
    fi
    if [ -n "$extra" ]; then
        bad "these assertions went red and the manifest does NOT name them. Either make the mutation finer so it trips only the property it claims, or add each to knock_on_fail with a knock_on_why saying why it is the SAME defect observed again:"
        awk -F'\t' 'NR==FNR { want[$0]=1; next } ($2 in want) { print "    | " $1 ": " $2 }' \
            /dev/stdin "$OUTFILE.map" <<EOF
$extra
EOF
    fi
    if [ -z "$miss" ] && [ -z "$extra" ]; then
        ok "the red set is EXACTLY the $n_exp assertion(s) the manifest names (observed $n_obs), attributed to:"
        cut -f1 "$OUTFILE.map" | sort -u | sed 's/^/      /'
    fi
    rm -f "$OUTFILE.exp" "$OUTFILE.exp2" "$OUTFILE.obs" "$OUTFILE.map"

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
            MISSING) bad "blind suite $s produced no verdict line at all" ;;
            *)      bad "$s went red (rc=$rc). THE GAP HAS CLOSED -- this is an improvement, not a regression. Move '$s' out of blind_suites and into suites_red, and record its assertions in require_fail." ;;
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
        note "  (note: the guest survived to FINAL RESULTS; the manifest expects it not to. Re-read the 'why' -- if the unload is now survivable, this control's reasoning needs revisiting, not its threshold.)"
    fi

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

echo "=========================================================="
echo " Facility negative controls: $pass_n passed, $fail_n failed"
echo "=========================================================="
if [ "$fail_n" -ne 0 ]; then
    echo "Failing:$FAILED_DEFECTS"
    exit 1
fi
exit 0
