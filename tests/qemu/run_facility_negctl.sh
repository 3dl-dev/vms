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
#   3. asserts the RIGHT suite went red, for the RIGHT reason (the specific
#      assertion text), while the facility's OWN sibling assertions stayed
#      green and NO OTHER suite went red;
#   4. and, before all of it, runs the pristine image as a POSITIVE CONTROL,
#      so a harness that fails indiscriminately cannot pass this script.
#
# Every check has a stated failure mode it exists to catch. In particular:
#
#   - "at least one suite in the facility's set went red" catches a facility
#     whose suite has stopped asserting anything.
#   - "no suite OUTSIDE the facility's set went red" is the attribution claim.
#     Without it, `return 1` bolted onto every test would pass every control.
#   - "these specific assertion texts went red AND these specific sibling
#      texts did not" is the method rule: EVERY PROPERTY NEEDS ITS OWN MINIMAL
#     MUTATION THAT TRIPS THAT PROPERTY AND NO OTHER. A mutation that reddens
#     a whole suite proves nothing about any single property in it, which is
#     how five earlier "proofs" in this epic passed while testing nothing.
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

if [ "$N_EXPECTED" -lt 13 ]; then
    echo "FAIL: only $N_EXPECTED suite sources under tests/qemu (expected at least 13)."
    echo "A suite source was deleted. Deleting the test that would expose a defect is"
    echo "not a way to make this gate pass."
    exit 1
fi

# ---------------------------------------------------------------------------
# Coverage: every executive translation unit must HAVE a control. This is the
# item's actual outcome -- "EVERY wired executive facility has a negative
# control" -- so it is checked mechanically rather than asserted in a comment.
# ---------------------------------------------------------------------------
echo "--- coverage: every src/kernel/*.c has a negative control ---"
if sh "$MANIFEST" coverage "$REPO_ROOT/src"; then
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
if [ "$DEFECT_BAD" -eq 0 ]; then
    ok "pristine image: all $N_EXPECTED suites rc=0 against a real /dev/vms"
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

    # 6. THE REASON, not merely the failure. The named assertions must be the
    #    ones that went red.
    miss=""
    sh "$MANIFEST" field "$defect" require_fail | while IFS= read -r txt; do
        [ -n "$txt" ] || continue
        grep -qFx "  FAIL: $txt" "$OUTFILE" || echo "$txt"
    done >"$OUTFILE.req"
    miss=$(cat "$OUTFILE.req"); rm -f "$OUTFILE.req"
    if [ -z "$miss" ]; then
        ok "the expected assertion(s) reported FAIL by name"
    else
        bad "expected these assertions to fail and they did not:$(echo "$miss" | sed 's/^/ [/;s/$/]/' | tr '\n' ' ')"
    fi

    # 7. THE METHOD RULE: the sibling properties must NOT have gone red. A
    #    mutation that trips its neighbours is a blunderbuss inside one suite
    #    and proves nothing about the property it claims to isolate.
    sh "$MANIFEST" field "$defect" forbid_fail | while IFS= read -r txt; do
        [ -n "$txt" ] || continue
        grep -qFx "  FAIL: $txt" "$OUTFILE" && echo "$txt"
    done >"$OUTFILE.forb"
    stray_fail=$(cat "$OUTFILE.forb"); rm -f "$OUTFILE.forb"
    if [ -z "$stray_fail" ]; then
        ok "the sibling properties in the same suite stayed green (the mutation is minimal)"
    else
        bad "these assertions were NOT supposed to fail:$(echo "$stray_fail" | sed 's/^/ [/;s/$/]/' | tr '\n' ' '). The mutation trips more than one property, so it cannot attribute any of them."
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
        echo "  PASS: '$defect' turns the harness red, names '$facility', and nothing else."
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
