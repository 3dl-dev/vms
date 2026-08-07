#!/bin/sh
#
# facility_attribution_negctl.sh - THE MEASURED NEGATIVE CONTROL for
#                                  tests/qemu/facility_attribution.sh
#                                  (rd vms-38c, vms-d33, vms-2b2)
#
# ===========================================================================
# WHY THIS FILE EXISTS AT ALL
# ===========================================================================
#
# This program's own history is the argument. Across the vms-d89 / vms-ecf /
# vms-e2b / vms-c13 rounds, EVERY strengthening of the register and the census
# was shipped with a claim about its price, and every one of those claims was
# broken by execution within days -- usually by a CHEAPER buy using the
# OPPOSITE mechanism to the one the implementer had defended against:
#
#   round 1  "the proof mentions the service"    bought by a COMMENT
#   round 2  "the assertion names the service"   bought by RE-WORDING the
#                                                assertion in both files
#   round 3  "three edits, two ignored calls"    measured at ONE edit
#   round 4  "four edits, by DELETION"           measured at TWO, by ADDITION
#   run-6    "check 4 stops the flips"           the paying call was placed
#                                                AFTER `return` -- statically
#                                                unreachable dead code
#
# So a new instrument that arrives with a prose claim and no executed control
# is, on this tree's own evidence, worth nothing. This file therefore does not
# argue that the attribution instrument is unbuyable. It CONSTRUCTS THE EXACT
# BUY THE RECORD DESCRIBES, runs both gates against it, and prints what each
# one answered.
#
# ===========================================================================
# THE BUY, VERBATIM FROM rd vms-38c (2026-08-03 run-6 adversary)
# ===========================================================================
#
#   "STILL EXACTLY 2 EDITS ON MAIN f5a321e -- re-measured, not assumed.
#    Edit 1 alone (flip sys$wflor's PARTIAL+LOCAL block to EXECUTIVE) reds
#    with 'EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE'.
#    Edit 2 (one ignored call in the proof) -> rc=0, EXECUTIVE 7->8 [...]
#    SHARPENING WORTH KEEPING: the paying call was placed AFTER return --
#    statically unreachable dead code. Check 4 is satisfied by code the
#    compiler is free to delete."
#
# Reproduced here as two `sed` edits against a throwaway copy of the tree.
#
# ===========================================================================
# WHAT EACH CONTROL BELOW CATCHES
# ===========================================================================
#
#   A. THE BUY STILL WORKS ON THE OLD GATE. If this stops being true the
#      control has gone stale and must be re-measured, not deleted -- a
#      negative control whose defect no longer applies proves nothing about
#      the new instrument, and reporting it as a pass would be the exact
#      "certified for the wrong reason" failure vms-c9c records.
#
#   B. THE NEW INSTRUMENT REFUSES THE SAME BUY. The measured check must find
#      NO assertion whose verdict was observed to change when a function in
#      sys$wflor's own answer path was mutated.
#
#      UPDATE (rd vms-38c, re-measured after vms-2b2 closed): vms_ioctl_wflor
#      is NOT a permanently safe target for this. vms-2b2's own follow-up work
#      (vms-2ed) added a REAL, oracle-pinned defect (eflag-wflor-status-wrong)
#      that genuinely mutates vms_ioctl_wflor and reddens an assertion in
#      test_syssvc_ef_mproc -- so sys$wflor's handler now has HONEST measured
#      dependence in exactly the suite this control cites, independent of any
#      buy. That is a coverage GAIN, not a broken control, and treating it as
#      a bare FAIL would be exactly the stale-anchor mistake this file exists
#      to catch elsewhere (rd vms-38c's own selftest checks 3/5 hardcoded the
#      same name and needed the identical fix). Control B therefore checks
#      FIRST whether the target has organically graduated, and if so falls
#      through to a still-live equivalent: sys$readef, which rd vms-38c's
#      re-measurement (post vms-2b2) found is declared OVMX-EXECUTIVE with
#      proof=test_syssvc_ef_mproc.c yet UNMEASURED in that exact suite -- a
#      real, current gap, no declaration flip required to construct it.
#
#   C. THE NEW INSTRUMENT IS NOT VACUOUS. It must PAY the claims that are
#      honestly measured. A check that refuses everything passes B trivially
#      and gates nothing -- that is a blunderbuss, and this program has
#      already paid to remove one.
#
#   D. EDIT 2 IS INERT, MEASURED SEPARATELY. Adding the ignored call must not
#      change a single attribution row. This is the trap the items recorded in
#      terms -- "a wrapper that merely records 'was called' is bought by one
#      more ignored call inside the assertion" -- asserted as an executed
#      before/after diff rather than as a property of the design.
#
#   E. DEAD CODE AFTER `return` IS INERT TOO, and separately from D, because
#      the two are different claims: D says an executed-but-ignored call buys
#      nothing, E says unexecuted code buys nothing. The run-6 adversary's
#      buy was E; a fix that only closed D would be bought by it again.
#
# Usage: sh tests/qemu/facility_attribution_negctl.sh
# Env:   FAN_KEEP=1   keep the scratch trees for inspection

set -u

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# The instrument, read from THIS checkout. The register, by contrast, is always
# invoked from the SANDBOX copy under test -- a control that ran the pristine
# register against a bought tree would be measuring the wrong pair.
ATTR="$REPO_ROOT/tests/qemu/facility_attribution.sh"

pass_n=0; fail_n=0
ok()  { echo "  ok: $*"; pass_n=$((pass_n + 1)); }
bad() { echo "  FAIL: $*"; fail_n=$((fail_n + 1)); }

WORK=$(mktemp -d) || { echo "FATAL: mktemp"; exit 2; }
if [ "${FAN_KEEP:-0}" = "1" ]; then
    echo "(scratch kept at $WORK)"
else
    trap 'rm -rf "$WORK"' EXIT INT TERM
fi

echo "=========================================================="
echo " Negative controls for per-assertion runtime attribution"
echo "=========================================================="
echo "repo: $REPO_ROOT"
echo ""

# ---------------------------------------------------------------------------
# A throwaway copy of the tree. git archive rather than cp -r so the scratch
# is exactly the tracked content -- a stray build directory in the copy would
# make the register read a different compile set than the one under test.
# ---------------------------------------------------------------------------
mk_tree() {
    _dst="$1"
    mkdir -p "$_dst"
    ( cd "$REPO_ROOT" && git archive --format=tar HEAD ) 2>/dev/null | tar -x -C "$_dst" \
        || { echo "FATAL: could not export the tree with git archive"; exit 2; }
    # Uncommitted work must be under test too, or this control measures the
    # last commit instead of the change being reviewed.
    ( cd "$REPO_ROOT" && git diff HEAD -- src tests ) 2>/dev/null > "$WORK/wip.patch"
    if [ -s "$WORK/wip.patch" ]; then
        ( cd "$_dst" && git apply "$WORK/wip.patch" ) 2>/dev/null \
            || echo "  (note: uncommitted src/tests changes could not be applied to the scratch tree)"
    fi
}

# EDIT 1 -- flip sys$wflor's OVMX-PARTIAL + OVMX-LOCAL pair to a full
# OVMX-EXECUTIVE exemption, citing the event-flag proof. This is the edit that
# DELETES the sentence saying full residency is UNPROVEN.
edit1_flip_wflor() {
    _t="$1"
    _f="$_t/src/libvms/syssvc/sys_event.c"
    [ -f "$_f" ] || { echo "FATAL: $_f missing in the scratch tree"; exit 2; }
    cp "$_f" "$_f.pristine"
    sed -i \
      -e 's| \* OVMX-PARTIAL: sys\$wflor (vms-afc) -- exec: the wait and every flag it tests are| * OVMX-EXECUTIVE: sys$wflor (vms-afc) proof=tests/qemu/test_syssvc_ef_mproc.c -- the wait|' \
      -e 's| \*     the executive.s; this is a one-line pass-through to vms_kif_wflor.| *     and every flag it tests are the executive'"'"'s; one-line pass-through.|' \
      -e '/^ \* OVMX-LOCAL: sys\$wflor -- nothing of the answer is computed here, but no$/,+1d' \
      "$_f"
    if cmp -s "$_f" "$_f.pristine"; then
        echo "FATAL: BROKEN CONTROL -- edit 1 did not land. The declaration block moved;"
        echo "       re-anchor it. Reporting a pass from an unmutated tree is the failure"
        echo "       this whole file exists to prevent."
        exit 2
    fi
    rm -f "$_f.pristine"
}

# EDIT 2 -- ONE IGNORED CALL, PLACED AFTER `return`. Not a typo: the run-6
# adversary's sharpening is that the paying call is statically unreachable dead
# code the compiler is free to delete, and check 4 accepts it anyway.
edit2_dead_call() {
    _t="$1"
    _f="$_t/tests/qemu/test_syssvc_ef_mproc.c"
    [ -f "$_f" ] || { echo "FATAL: $_f missing in the scratch tree"; exit 2; }
    cp "$_f" "$_f.pristine"
    # The final `return fail > 0 ? 1 : 0;` of main(). The call goes AFTER it.
    awk '
        /^    return fail > 0 \? 1 : 0;$/ && !done {
            print
            print "    (void)sys$wflor(0u, 0u);   /* NEGCTL: dead code after return */"
            done = 1
            next
        }
        { print }' "$_f" > "$_f.new" && mv "$_f.new" "$_f"
    if cmp -s "$_f" "$_f.pristine"; then
        echo "FATAL: BROKEN CONTROL -- edit 2 did not land (the return statement moved)."
        exit 2
    fi
    rm -f "$_f.pristine"
}

# EDIT 2' -- the OTHER shape, for control D: an ignored call on a LIVE path.
# It really executes; its result is discarded. If the instrument were a call
# tracer this would buy the claim.
edit2b_live_ignored_call() {
    _t="$1"
    _f="$_t/tests/qemu/test_syssvc_ef_mproc.c"
    cp "$_f" "$_f.pristine"
    awk '
        /^    printf\("=== test_syssvc_ef_mproc: %d passed, %d failed ===\\n", pass, fail\);$/ && !done {
            print "    (void)sys$wflor(0u, 0u);   /* NEGCTL: live path, result ignored */"
            done = 1
        }
        { print }' "$_f" > "$_f.new" && mv "$_f.new" "$_f"
    if cmp -s "$_f" "$_f.pristine"; then
        echo "FATAL: BROKEN CONTROL -- edit 2b did not land."
        exit 2
    fi
    rm -f "$_f.pristine"
}

# EDIT 3 -- the LIVE equivalent of EDIT 2, aimed at sys$readef instead of
# sys$wflor. No declaration flip is needed: sys$readef is ALREADY declared
# OVMX-EXECUTIVE proof=tests/qemu/test_syssvc_ef_mproc.c on an untouched tree,
# and rd vms-38c's re-measurement (post vms-2b2) found it UNMEASURED there --
# a real, standing gap, not a constructed one. This is the sharpest live
# question left: can ONE MORE ignored call, in the exact suite already cited
# as proof, flip that UNMEASURED to MEASURED.
edit3_readef_dead_call() {
    _t="$1"
    _f="$_t/tests/qemu/test_syssvc_ef_mproc.c"
    [ -f "$_f" ] || { echo "FATAL: $_f missing in the scratch tree"; exit 2; }
    cp "$_f" "$_f.pristine"
    awk '
        /^    return fail > 0 \? 1 : 0;$/ && !done {
            print
            print "    { uint32_t _negctl_st; (void)sys$readef(0u, &_negctl_st); }   /* NEGCTL: dead code after return, targets sys$readef (vms-38c live gap) */"
            done = 1
            next
        }
        { print }' "$_f" > "$_f.new" && mv "$_f.new" "$_f"
    if cmp -s "$_f" "$_f.pristine"; then
        echo "FATAL: BROKEN CONTROL -- edit 3 did not land (the return statement moved)."
        exit 2
    fi
    rm -f "$_f.pristine"
}

# The measured check, run against a tree: does ANY assertion in the cited proof
# have an OBSERVED dependence on a function in the service's own answer path?
#
# The answer path used here is the HANDLER the service's ioctl dispatches to,
# read out of the executive's own switch -- the same bridge the register walks,
# kept at FUNCTION granularity instead of being collapsed to a filename.
measured_dependence() {
    _t="$1"; _service="$2"; _proof="$3"
    _ioctl=$(printf '%s' "$_service" | sed 's/^sys\$//' | tr 'a-z' 'A-Z')
    _handler=$(awk -v c="VMS_IOCTL_$_ioctl" '
        $0 ~ ("case[ \t]+" c "[ \t]*:") { armed = 1 }
        armed && match($0, /vms_ioctl_[A-Za-z0-9_]+/) {
            print substr($0, RSTART, RLENGTH); exit
        }' "$_t/src/kernel/vms_module.c")
    if [ -z "$_handler" ]; then
        echo "      (no dispatch arm found for VMS_IOCTL_$_ioctl)"
        return 1
    fi
    echo "      answer path (handler granularity): $_handler"
    FA_REPO_ROOT="$_t" FA_RECORD="$_t/tests/qemu/facility_negctl_observed.tsv" \
        sh "$_t/tests/qemu/facility_attribution.sh" depends "$_proof" "$_handler" >/dev/null 2>&1
}

# ---------------------------------------------------------------------------
echo "--- 0. baseline: the register passes on the pristine tree ---"
mk_tree "$WORK/pristine"
if timeout 900 sh "$WORK/pristine/tests/integration/test_userspace_service_register.sh" >"$WORK/reg_pristine.txt" 2>&1; then
    _n=$(grep -c '^      sys\$' "$WORK/reg_pristine.txt" || true)
    ok "the register passes on the pristine scratch tree ($_n OVMX-EXECUTIVE claim(s))"
else
    bad "the register ALREADY fails on the pristine scratch tree -- every control below"
    echo "        would be measuring the wrong thing. Last 20 lines:"
    tail -20 "$WORK/reg_pristine.txt" | sed 's/^/        | /'
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- A. the recorded 2-edit buy STILL WORKS against the register ---"
mk_tree "$WORK/bought"
edit1_flip_wflor "$WORK/bought"
echo "  edit 1: sys\$wflor OVMX-PARTIAL + OVMX-LOCAL -> OVMX-EXECUTIVE proof=tests/qemu/test_syssvc_ef_mproc.c"
if timeout 900 sh "$WORK/bought/tests/integration/test_userspace_service_register.sh" >"$WORK/reg_e1.txt" 2>&1; then
    echo "  (edit 1 alone already passes -- the record says it should red with"
    echo "   'PROOF NEVER CALLS THE SERVICE'. Noted, not fatal.)"
else
    if grep -q 'NEVER CALLS THE SERVICE' "$WORK/reg_e1.txt"; then
        ok "edit 1 alone reds with 'EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE' (as recorded)"
    else
        echo "  (edit 1 alone reds, but not with the recorded message:)"
        grep -E '^  (FAIL|EXECUTIVE)' "$WORK/reg_e1.txt" | head -5 | sed 's/^/        | /'
    fi
fi
edit2_dead_call "$WORK/bought"
echo "  edit 2: ONE ignored call, placed AFTER \`return\` -- statically unreachable dead code"
if timeout 900 sh "$WORK/bought/tests/integration/test_userspace_service_register.sh" >"$WORK/reg_e2.txt" 2>&1; then
    _n=$(grep -c '^      sys\$' "$WORK/reg_e2.txt" || true)
    ok "THE BUY LANDS: the register exits 0 with sys\$wflor now a full exemption ($_n claim(s))"
    grep -q 'sys\$wflor' "$WORK/reg_e2.txt" \
        && echo "        the register prints it as a paid claim:" \
        && grep -A2 '^      sys\$wflor' "$WORK/reg_e2.txt" | sed 's/^/        | /'
else
    bad "the buy no longer lands -- this control is STALE. Re-measure the current cheapest"
    echo "        buy and re-anchor this file. Do NOT delete the control."
    tail -20 "$WORK/reg_e2.txt" | sed 's/^/        | /'
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- B. the MEASURED instrument refuses the same buy ---"
echo "    question: was any assertion in tests/qemu/test_syssvc_ef_mproc.c OBSERVED"
echo "              to change verdict when sys\$wflor's own handler was mutated?"
if measured_dependence "$WORK/pristine" 'sys$wflor' test_syssvc_ef_mproc; then
    echo "  NOTE: vms_ioctl_wflor is measured-dependent in this suite on the PRISTINE"
    echo "        tree already -- vms-2ed gave it real coverage since this control was"
    echo "        written. It is no longer a valid UNPAID target; this is a coverage"
    echo "        GAIN, not a defeat of the buy, so it is not scored bad() here."
    echo "    falling through to the live equivalent: sys\$readef, no flip needed --"
    echo "    question: does ONE MORE ignored call in the exact cited proof flip its"
    echo "              standing UNMEASURED to MEASURED?"
    mk_tree "$WORK/bought2"
    edit3_readef_dead_call "$WORK/bought2"
    if measured_dependence "$WORK/pristine" 'sys$readef' test_syssvc_ef_mproc; then
        bad "BASELINE BROKEN: sys\$readef already shows measured dependence on the"
        echo "        pristine tree -- rd vms-38c's recorded gap has closed by some OTHER"
        echo "        change; re-derive this control's target rather than trust this note."
    elif measured_dependence "$WORK/bought2" 'sys$readef' test_syssvc_ef_mproc; then
        bad "the measured check PAYS sys\$readef after ONE ignored call -- buyable"
    else
        ok "NO measured dependence, before or after the ignored call. sys\$readef's"
        echo "        standing UNMEASURED gap (rd vms-38c) is not moved by adding one more"
        echo "        call to the exact suite already cited as its proof."
    fi
else
    if measured_dependence "$WORK/bought" 'sys$wflor' test_syssvc_ef_mproc; then
        bad "the measured check PAYS the bought claim -- it is buyable by the same 2 edits"
    else
        ok "NO measured dependence. The bought OVMX-EXECUTIVE claim is UNPAID by execution,"
        echo "        where the register's check 4 accepted it from dead code."
    fi
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- C. the measured instrument is NOT vacuous ---"
echo "    a check that refuses everything passes B for free and gates nothing."
_paid=""; _unpaid=""
for _pair in 'sys$clref:test_syssvc_ef_mproc' 'sys$waitfr:test_syssvc_ef_mproc' \
             'sys$setef:test_syssvc_ef_mproc' 'sys$readef:test_syssvc_ef_mproc' \
             'sys$ascefc:test_syssvc_ef_mproc'; do
    _svc=${_pair%%:*}; _prf=${_pair##*:}
    if measured_dependence "$WORK/pristine" "$_svc" "$_prf" >/dev/null 2>&1; then
        _paid="$_paid $_svc"
    else
        _unpaid="$_unpaid $_svc"
    fi
done
echo "      MEASURED-PAID  :${_paid:- (none)}"
echo "      UNMEASURED     :${_unpaid:- (none)}"
if [ -n "$_paid" ]; then
    ok "the instrument pays real claims:$_paid -- so its refusal in B is discriminating,"
    echo "        not a blanket no"
else
    bad "the instrument pays NOTHING -- it is a blunderbuss and control B proves nothing"
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- D. an IGNORED CALL ON A LIVE PATH changes zero attribution rows ---"
echo "    'a wrapper that merely records was-called is bought by one more ignored"
echo "     call inside the assertion' (rd vms-38c). Measured as a before/after diff."
mk_tree "$WORK/livecall"
edit2b_live_ignored_call "$WORK/livecall"
FA_REPO_ROOT="$WORK/pristine" FA_RECORD="$WORK/pristine/tests/qemu/facility_negctl_observed.tsv" \
    sh "$WORK/pristine/tests/qemu/facility_attribution.sh" attribute >"$WORK/attr_pristine.txt" 2>&1
FA_REPO_ROOT="$WORK/livecall" FA_RECORD="$WORK/livecall/tests/qemu/facility_negctl_observed.tsv" \
    sh "$WORK/livecall/tests/qemu/facility_attribution.sh" attribute >"$WORK/attr_livecall.txt" 2>&1
_a=$(wc -l < "$WORK/attr_pristine.txt"); _b=$(wc -l < "$WORK/attr_livecall.txt")
echo "      attribution rows: pristine=$_a  with-ignored-call=$_b"
if diff -q "$WORK/attr_pristine.txt" "$WORK/attr_livecall.txt" >/dev/null 2>&1; then
    ok "byte-identical: an executed-but-ignored call bought exactly nothing"
else
    bad "the ignored call CHANGED the attribution:"
    diff "$WORK/attr_pristine.txt" "$WORK/attr_livecall.txt" | head -10 | sed 's/^/        | /'
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- E. DEAD CODE AFTER \`return\` changes zero attribution rows ---"
echo "    a separate claim from D: D is 'ran but ignored', E is 'never ran'."
FA_REPO_ROOT="$WORK/bought" FA_RECORD="$WORK/bought/tests/qemu/facility_negctl_observed.tsv" \
    sh "$WORK/bought/tests/qemu/facility_attribution.sh" attribute >"$WORK/attr_bought.txt" 2>&1
_c=$(wc -l < "$WORK/attr_bought.txt")
echo "      attribution rows: pristine=$_a  with-dead-code-and-flipped-declaration=$_c"
if diff -q "$WORK/attr_pristine.txt" "$WORK/attr_bought.txt" >/dev/null 2>&1; then
    ok "byte-identical: unreachable code, and the declaration flip beside it, bought nothing"
else
    bad "the dead call or the declaration flip CHANGED the attribution:"
    diff "$WORK/attr_pristine.txt" "$WORK/attr_bought.txt" | head -10 | sed 's/^/        | /'
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- F. the instrument's own selftest ---"
if FA_REPO_ROOT="$WORK/pristine" FA_RECORD="$WORK/pristine/tests/qemu/facility_negctl_observed.tsv" \
        sh "$WORK/pristine/tests/qemu/facility_attribution.sh" selftest >"$WORK/self.txt" 2>&1; then
    ok "facility_attribution.sh selftest passed"
    sed 's/^/        | /' "$WORK/self.txt"
else
    bad "facility_attribution.sh selftest FAILED"
    sed 's/^/        | /' "$WORK/self.txt"
fi
echo ""

# ---------------------------------------------------------------------------
echo "--- G. the OFFLINE site derivation equals what the REAL container injection does ---"
echo "    the SITE half of the attribution is only execution-sourced if the apply this"
echo "    runs on the host is the apply inject_and_run.sh runs in the container. That is"
echo "    an ARGUMENT until it is measured, and an argument is what this program keeps"
echo "    proving is worth nothing. So: run the injection in BOTH places, diff the"
echo "    changed line numbers."
_engine=""
if [ -n "${CONTAINER_ENGINE:-}" ]; then _engine="$CONTAINER_ENGINE"
elif command -v docker >/dev/null 2>&1; then _engine=docker
elif command -v podman >/dev/null 2>&1; then _engine=podman
fi
_image="${FAN_IMAGE:-ovmx-ktest-negctl-base:latest}"
if [ -z "$_engine" ] || ! "$_engine" image inspect "$_image" >/dev/null 2>&1; then
    echo "  NOT RUN: no container engine, or the harness image '$_image' is not built."
    echo "           Build it with:  $_engine build -f tests/qemu/Dockerfile -t $_image ."
    echo "           This is a DEGRADATION, not a pass: the site half is unverified against"
    echo "           the container on this host. Set FAN_REQUIRE_CONTAINER=1 to make it red."
    if [ "${FAN_REQUIRE_CONTAINER:-0}" = "1" ]; then
        bad "FAN_REQUIRE_CONTAINER=1 and the container check could not run"
    fi
else
    cat > "$WORK/ics.sh" <<'ICSEOF'
#!/bin/sh
set -u
for d in $(sh /src/tests/qemu/facility_defects.sh list); do
    tg=$(sh /src/tests/qemu/facility_defects.sh field "$d" targets)
    rm -rf /tmp/w
    mkdir -p /tmp/w/src
    for t in $tg; do
        [ -f "/src/repo/src/$t" ] || continue
        mkdir -p "/tmp/w/src/$(dirname "$t")"
        cp "/src/repo/src/$t" "/tmp/w/src/$t"
        cp "/src/repo/src/$t" "/tmp/w/$(echo "$t" | tr / _)"
    done
    if ! sh /src/tests/qemu/facility_defects.sh apply "$d" /tmp/w/src >/dev/null 2>&1; then
        printf 'CSITE\t%s\t(APPLY-FAILED)\t0\n' "$d"
        continue
    fi
    for t in $tg; do
        [ -f "/tmp/w/src/$t" ] || continue
        old="/tmp/w/$(echo "$t" | tr / _)"
        diff "$old" "/tmp/w/src/$t" \
          | sed -n 's/^\([0-9]*\)\(,[0-9]*\)\{0,1\}[acd].*$/\1/p' | sort -un \
          | while read -r l; do printf 'CSITE\t%s\t%s\t%s\n' "$d" "$t" "$l"; done
    done
done
ICSEOF
    if "$_engine" run --rm -v "$WORK/ics.sh:/tmp/ics.sh:ro" "$_image" sh /tmp/ics.sh \
            > "$WORK/csites.raw" 2>"$WORK/csites.err"; then
        cut -f2,3,4 "$WORK/csites.raw" | sort > "$WORK/csites"
        sh "$ATTR" sites | cut -f2,3,4 | sort > "$WORK/hsites"
        _cn=$(wc -l < "$WORK/csites"); _hn=$(wc -l < "$WORK/hsites")
        echo "      host: $_hn site row(s)   container: $_cn site row(s)"
        if [ "$_cn" -eq 0 ]; then
            bad "the container produced NO sites -- the check judged nothing"
        elif diff "$WORK/hsites" "$WORK/csites" >/dev/null 2>&1; then
            ok "IDENTICAL, all $_cn rows: the host derivation IS the container injection,"
            echo "        not a model of it"
        else
            bad "the host and container injections disagree:"
            diff "$WORK/hsites" "$WORK/csites" | head -20 | sed 's/^/        | /'
        fi
    else
        bad "the container run failed:"
        tail -10 "$WORK/csites.err" | sed 's/^/        | /'
    fi
fi
echo ""

echo "=========================================================="
echo " Attribution negative controls: $pass_n passed, $fail_n failed"
echo "=========================================================="
[ "$fail_n" -eq 0 ] || exit 1
exit 0
