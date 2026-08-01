#!/bin/sh
#
# test_kif_caller_census_negctl.sh - negative controls for the caller census.
#
# WHY THIS EXISTS. test_kif_caller_census.sh is a lint, and this dispatch has
# now caught three separate interfaces that shipped with an assertion nobody had
# ever seen fail: an #ifdef nothing compiled (so an #error inside it never
# fired), a wrapper that bypassed KIF_CALL (so it never bound), and a system
# service whose only passing assertions were its two fabricated successes. A
# gate whose negative control has not been run is exactly that shape. So the
# evasions are checked in, and they run in CI.
#
# THE RULE THIS FILE ENFORCES ON ITSELF, inherited from
# test_runtime_target_negctl.sh: every property gets its OWN MINIMAL mutation
# that trips THAT property AND NO OTHER. Each case asserts a required substring
# (the right property fired) and a set of forbidden substrings (nothing else
# fired), so a mutation that goes red for the wrong reason fails this test. Two
# cases are GREEN controls: they prove the census does not simply fail on any
# edit, and they pin the reachability rule from the other side.
#
# The properties under control:
#   1  a new entry point with no caller and no declaration          -> RED
#   2  ... the same entry point, properly declared                  -> GREEN
#   3  a declaration with no item id (a free-text excuse)           -> RED
#   4  a caller that exists only inside a COMMENT                   -> RED
#   5  a caller that exists only in tests/                          -> RED
#   6  a caller inside vms_kif.c that nothing reaches               -> RED
#   7  ... a caller inside vms_kif.c that IS reached (kif_bind)     -> GREEN
#   8  a prototype at file scope, which is not a call               -> RED
#   9  a declaration left behind on an entry point that IS wired    -> RED
#   10 a declaration naming a function that does not exist          -> RED
#   11 the same entry point declared twice                          -> RED
#   12 a REGRESSION: an existing wired facility loses its caller    -> RED
#
# And the controls below (13 onward) that pin the UNIVERSE itself, because a
# gate that can be disarmed by removing the thing it counts is worth nothing.
# Every one of these was a PASS at some earlier revision of the gate.
#
# DELIBERATELY NO CARDINAL HERE, AND NOT BECAUSE THE NUMBER IS COMPUTED
# INSTEAD -- round 6 tried exactly that and it was the same defect wearing a
# disguise. Round 6 replaced the prose word "seven" with a shell variable,
# $pin_total, incremented by a SEPARATE "pin_total=$((pin_total + 1))" line
# next to each of controls 13-20. That is not tied to the call; it is a
# second hand-maintained count with the identical human obligation the word
# "seven" carried, and it drifted the same way: appending a genuine 21st
# universe-pin control without also touching that separate line left the
# printed total at 8 while 9 controls actually ran, and the run still
# reported "PASS" throughout -- worse than the prose word it replaced,
# because a printed, computed-looking number is trusted more than a comment,
# and this one silently agreed with the drift. Demonstrated by running this
# file after adding a control with no companion increment; see the item's
# audit trail for the exact transcript.
#
# THE FIX (round 7): there is no separate increment left to forget FOR ANY
# CONTROL THAT USES THE DISPATCHER. CATEGORY is now a required argument to
# expect_red()/expect_green() -- "core" for controls 1-12, "pin" for 13
# onward -- not a second statement written near it. The dispatcher tallies
# $pin_total itself, once, as a byproduct of the one call that also runs the
# mutation and prints its PASS/FAIL line, so a control that goes through it
# cannot be silently uncounted the way $pin_total's old companion line could
# be skipped. ROUND 7 CLAIMED (falsely, see below) this made every control's
# outcome tamper-evident; it only closed the $pin_total hole, and the file
# still hand-rolled $passed in exactly two places -- the positive control and
# the meta-control below -- which were the in-file TEMPLATE for bypassing the
# dispatcher altogether. Fixed in round 8: record_verdict() (defined above,
# next to tally_category()) is now the only place in this file where a
# CONTROL'S OUTCOME touches $passed/$failed/$status or prints a "  PASS:"
# line. The positive control is now a plain expect_green() call with an
# empty file list (its "for _f in $files" injection-landed loop iterates
# zero times, which is exactly "run the gate, expect PASS, no mutation to
# verify"). The meta-control cannot be expressed as an expect_red()/
# expect_green() call -- it tests injection_landed() directly and never runs
# the gate -- so it calls tally_category()/record_verdict() itself; it is
# the one control in this file that reaches those two functions without
# going through the dispatcher built on top of them. A THIRD, PRE-EXISTING
# EXCEPTION WAS FOUND AND FIXED WHILE AUDITING THIS VERY PARAGRAPH (Part 3 of
# this round's dispatch, Method 6: grep the file for the pattern before
# claiming it can't occur): control 20's own fixture-integrity guard (below,
# "control 20 needs a product function...") hand-rolled
# "failed=$((failed + 1)); status=1" directly, skipping tally_category() too,
# so a broken fixture on that one path would have both mis-recorded the
# verdict and undercounted $pin_total. It now calls tally_category()/
# record_verdict() the same as everything else. Grepped, not asserted, AFTER
# that fix: `grep -c '^\s*\(expect_red\|expect_green\) ' <this file>` is 22
# (the positive control plus the 21 numbered controls). THE FOLLOWING TWO
# GREPS ARE ANCHORED TO A LEADING TAB/SPACE ON PURPOSE -- this file's own
# prose repeatedly quotes "passed=$((passed" and "failed=$((failed" (as it
# does two paragraphs up and does again below), so the unanchored patterns
# match this comment text too and would UNDERSTATE nothing but overstate the
# count; anchoring to code indentation is what excludes prose. Anchored:
# `grep -c '^\s*passed=\$((passed' <this file>` is 1, inside
# record_verdict()'s own definition; `grep -c '^\s*failed=\$((failed' <this
# file>` is 2, one inside tally_category() (its own malformed-category path,
# a fixture defect, not a control's outcome -- deliberately not folded into
# record_verdict, see that function's comment) and one inside
# record_verdict() itself. So: every one
# of the 23 controls in this file funnels through tally_category() and, for
# its own outcome, record_verdict(). ON A CLEAN RUN, where every fixture
# precondition holds -- confirmed for THIS tree by "controls: 23 passed" with
# no "BROKEN FIXTURE" line printed anywhere in a run's output -- 22 of the 23
# also go through expect_red()/expect_green() on top of that, and the
# meta-control is the one control whose SOURCE never calls expect_red/
# expect_green on any path, because it never runs the gate. Control 20 has a
# SECOND such bypass in its source (its own comment, below), but only on a
# defensive fallback path that runs when its fixture precondition fails --
# the same shape as the "BROKEN FIXTURE" branches already inside expect_red/
# expect_green itself, which also skip $GATE -- and that path is not
# exercised in this tree: control 20's collision target ($COLLIDE=lnm_init)
# is a real, grepped-for product function today. An unrecognized category
# passed to expect_red/expect_green/the meta-control/control 20's guard IS a
# hard, immediate FAIL naming the control, not a default of zero -- that
# part is enforced in code, see tally_category() below. See "universe-pin
# controls run" in the summary this script prints when it
# runs.
#
# TESTED, NOT ASSUMED, AND CORRECTED (round 8): round 7 tried exactly one
# bypass shape -- a rogue block that printed its own "PASS:" line but touched
# NEITHER $passed NOR $pin_total -- found a visible mismatch ("controls: 23
# passed" under 24 "PASS:" lines), and generalized that to "what it CANNOT do
# is hide". FALSE: this file's OWN pre-round-8 template (the positive and
# meta-controls, before the fix above) was a hand-rolled if/then that DID
# increment $passed by hand right next to its own "  PASS:" echo -- and a
# rogue control copying THAT shape produces NO mismatch at all, because
# $passed is a plain shell variable and the printed count is computed from it
# after the fact, not compared against anything independent. Tried it (round
# 8, after the fix above landed): a rogue pin control appended before the
# summary, with `echo "  PASS: rogue pin control bypassing the dispatcher"`
# followed by `passed=$((passed + 1))` on the next line and no call to
# tally_category or record_verdict, produces 24 "PASS:" lines under "controls:
# 24 passed" -- consistent, not mismatched -- and "universe-pin controls run:
# 9", with no reciprocal check anywhere in the output for a reader to compare
# that "9" against. rc=0, suite green, nothing caught it. THIS SHAPE IS NOT
# DETECTED, by inspection or otherwise, and removing the two in-file
# templates does not change that -- sh has no construct that stops a future
# author writing raw `if`/`echo`/`passed=$((passed+1))` instead of calling a
# function, template or no template. What round 8 actually fixes is narrower
# and is stated precisely above: the file no longer DEMONSTRATES that shape
# as its own established style. Do not read this paragraph as a stronger
# claim than that.
#
#   13 a PROTOTYPE is deleted (definition stays)                    -> RED
#   14 a DEFINITION is deleted (prototype stays)                    -> RED
#   15 prototype + definition + declaration deleted outright        -> RED
#      ... caught on the kernel side: the opcode it issued is stranded.
#   16 prototype deleted and the definition marked static           -> RED
#      ... the route around 13; the union counts static definitions too.
#   17 the entry point is RENAMED out of the vms_kif_ namespace     -> RED
#      ... the route around 13-16: nothing is deleted, so nothing dangles and
#          no opcode is stranded. It was a PASS while BOTH readings filtered on
#          the prefix, and the wrapper stayed in the tree, unwired and compiled.
#   18 renamed out of the namespace AND marked static               -> RED
#      ... 17 composed with 16.
#   19 a SHARED-OPCODE wrapper deleted outright                     -> RED
#      ... the route around 15: the opcode survives in its sibling wrapper, so
#          an opcode-only floor certified it. Caught at selector grain.
#   20 renamed onto a PRODUCT FUNCTION'S NAME, and marked static     -> RED
#      ... the route the fix for 17 opened. Not a shrink: the census CERTIFIED
#          the unwired wrapper as REACHED, moving the count UP, on the strength
#          of an unrelated same-named function in src/vmslnm/. A gate that
#          vouches for the thing it exists to catch is worse than one that
#          loses it, so this control asserts the reason as well as the red.
#   21 renamed onto a SIBLING vms_kif_* NAME that is declared unwired -> RED
#      ... a route 13-20 never tried: collide two entry points WITHIN the
#          namespace instead of leaving it. Added last, mechanically the way
#          20 was (one control appended, nothing else edited) as the required
#          proof that the dispatcher tallies a new control without help --
#          see the comment at control 21's own definition below for the
#          measured before/after and for a genuine gap this control exposed
#          while proving the point.
#
# Usage: test_kif_caller_census_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_kif_caller_census.sh"
status=0
passed=0
failed=0
# Tallied by expect_red()/expect_green() themselves, once per call, keyed by
# the "category" argument every call now carries ("core" for 1-12, "pin" for
# 13 onward). There is no separate statement anywhere that increments this --
# see the dispatcher below and the PROOF paragraph above the usage line.
pin_total=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms_kif census negative controls: every property must have an evasion that trips it"

if ! command -v cmp >/dev/null 2>&1; then
    echo "  FAIL: cmp(1) unavailable -- this file cannot verify that its own"
    echo "        injections landed, so its verdicts would be unfounded"
    exit 1
fi

# ---------------------------------------------------------------------------
# A sandbox copy of exactly what the gate reads, plus tests/ -- which the gate
# deliberately does NOT read, and case 5 exists to prove it.
# ---------------------------------------------------------------------------
ROOT="$WORK/tree"
mkdir -p "$ROOT" "$WORK/orig"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp -a "$SRC_ROOT/tools" "$ROOT/tools"
cp -a "$SRC_ROOT/tests" "$ROOT/tests"

H="$ROOT/src/libvmssys/vms_kif.h"
C="$ROOT/src/libvmssys/vms_kif.c"
SHOW="$ROOT/src/vmsdcl/dcl_cmd_show.c"
QTEST="$ROOT/tests/qemu/test_kmod_devtab.c"

MUTABLE="$H $C $SHOW $QTEST"

key_of() { printf '%s' "${1#$ROOT/}" | tr '/.' '__'; }

for f in $MUTABLE; do
    if [ ! -f "$f" ]; then
        echo "  FAIL: BROKEN FIXTURE: $f does not exist, so the controls that"
        echo "        mutate it would run against an unmutated tree"
        exit 1
    fi
    cp "$f" "$WORK/orig/$(key_of "$f")"
done

restore() {
    for _f in $MUTABLE; do
        cp "$WORK/orig/$(key_of "$_f")" "$_f"
    done
}

# injection_landed <file>: 0 if it really differs from its pristine copy.
#
# An injection whose anchor no longer matches is a SILENT NO-OP: the sandbox
# stays clean, the gate correctly stays green, and the control then reports
# "the evasion was CERTIFIED, not caught" -- blaming the gate for a broken
# fixture. test_runtime_target_negctl.sh went 13/20 that way after a rename.
injection_landed() {
    cmp -s "$1" "$WORK/orig/$(key_of "$1")" && return 1
    return 0
}

# ---------------------------------------------------------------------------
# The distinctive fragment of each failure the gate can print. A control names
# the one it requires and forbids the rest.
# ---------------------------------------------------------------------------
F_UNDECL="NO product caller and NO unwired declaration"
F_MALFORMED="malformed unwired declaration"
F_STALE="is declared unwired but has a product caller"
F_UNKNOWN="which is not an entry"
F_DUP="declared unwired more than once"
F_ORPHAN_DEF="with NO prototype in"
F_ORPHAN_PROTO="with NO definition in"
F_ORPHAN_OPCODE="kernel opcode(s) no wrapper in"
F_ORPHAN_SEL="kernel selector(s) no wrapper in"

# Every control forbids all three universe-pin fragments except the one it is
# testing. They are spelled out at each call site rather than collected in a
# variable: the fragments contain spaces, so an unquoted expansion would split
# them into words that match nothing, and every "forbidden" check would pass
# vacuously -- a control that cannot fail, which is this file's whole subject.

# tally_category <name> <category>: the ONLY place $pin_total is touched.
# Called from inside the dispatcher, once per invocation, so counting a
# control is not a second action a caller can forget -- it is a side effect
# of the one call that runs the mutation. An unrecognized category is a hard
# FAIL naming the control, not a silent no-op that would undercount the same
# way $pin_total's old companion line could be skipped.
tally_category() {
    case "$2" in
        pin)  pin_total=$((pin_total + 1)) ;;
        core) : ;;
        *)
            echo "  FAIL: BROKEN FIXTURE: control \"$1\" passed unknown category"
            echo "        \"$2\" -- every call must say \"core\" or \"pin\" so the"
            echo "        tally has somewhere to go; there is no default."
            failed=$((failed + 1)); status=1
            return 1
            ;;
    esac
    return 0
}

# record_verdict <name> <ok>: the only place in this file where a CONTROL'S
# OUTCOME touches $passed/$failed/$status or prints a "  PASS:" line (round
# 8's fix; grepped, not assumed -- ANCHORED to a leading tab/space, because
# this file's own prose quotes both patterns verbatim in several places,
# including this sentence, so an unanchored grep would count its own
# commentary: `grep -c '^\s*passed=\$((passed'` and
# `grep -c '^\s*echo "  PASS:'` each return 1, both inside this
# function's own body). expect_red() and expect_green() call this instead of
# incrementing $passed inline; so does the meta-control below, the one
# control in the file that cannot use expect_red/expect_green because it
# never runs the gate at all. The positive control goes through
# expect_green() itself (see its own site) rather than being a third
# exception. tally_category() (above) is a SEPARATE call, made once per
# control regardless of outcome, that folds $pin_total in and has its OWN
# distinct failure path for a malformed category (a fixture defect, not a
# control's outcome) -- that path is the one place besides this function
# that touches $failed/$status, and it is not folded in here because it is
# not a verdict on what the control tests. Any FAIL diagnostic text for a
# control's own outcome is the caller's job, printed before this is called;
# this only prints the PASS line and does the counting.
record_verdict() {
    name="$1"; ok="$2"
    if [ "$ok" -eq 1 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        failed=$((failed + 1)); status=1
    fi
}

# expect_red <files> <name> <category> <required> [forbidden ...]
expect_red() {
    files="$1"; name="$2"; category="$3"; need="$4"; shift 4
    if ! tally_category "$name" "$category"; then
        restore
        return
    fi
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#$ROOT/} at all -- its"
            echo "        anchor no longer matches the source, so this control ran"
            echo "        the gate against an UNMUTATED tree and proved nothing."
            echo "        Re-anchor the mutation; do NOT relax the gate."
            record_verdict "$name" 0; restore; return
        fi
    done

    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    if [ "$rc" -eq 0 ]; then
        echo "  FAIL: the census CERTIFIED the evasion: $name"
        ok=0
    else
        # $need may carry MORE THAN ONE required fragment, one per line. Almost
        # every control names exactly one -- the rule of this file is that a
        # mutation trips one property and no other. Control 17 is the documented
        # exception: a rename trips two BY CONSTRUCTION and cannot be narrowed,
        # so it asserts both rather than forbidding one it knows will fire.
        # Split on newline ONLY: the fragments contain spaces.
        _oldifs=$IFS
        IFS='
'
        for req in $need; do
            IFS=$_oldifs
            if ! printf '%s\n' "$out" | grep -qF "$req"; then
                echo "  FAIL: the census went red for the WRONG reason: $name"
                echo "        expected to see: $req"
                ok=0
            fi
            IFS='
'
        done
        IFS=$_oldifs

        for bad in "$@"; do
            if printf '%s\n' "$out" | grep -qF "$bad"; then
                echo "  FAIL: mutation is not minimal -- another property also fired: $name"
                echo "        unexpected: $bad"
                ok=0
            fi
        done
    fi

    if [ "$ok" -eq 1 ]; then
        record_verdict "$name" 1
    else
        printf '%s\n' "$out" | sed 's/^/          /'
        record_verdict "$name" 0
    fi
    restore
}

# expect_green <files> <name> <category>
expect_green() {
    files="$1"; name="$2"; category="$3"
    if ! tally_category "$name" "$category"; then
        restore
        return
    fi
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#$ROOT/} at all."
            record_verdict "$name" 0; restore; return
        fi
    done

    out=$(sh "$GATE" "$ROOT" 2>&1)
    if [ $? -eq 0 ]; then
        record_verdict "$name" 1
    else
        echo "  FAIL: the census rejected a legitimate tree: $name"
        printf '%s\n' "$out" | sed 's/^/          /'
        record_verdict "$name" 0
    fi
    restore
}

# ---------------------------------------------------------------------------
# The injections.
#
# add_probe_decl anchors on the include guard, add_probe_def appends a
# definition shaped like every other wrapper -- note that the DEFINITION must
# not count as a caller of itself, which case 1 proves by going red anyway.
# ---------------------------------------------------------------------------
GUARD_RE='^#endif /\* _VMS_KIF_H \*/$'

add_probe_decl() {
    sed -i "s|$GUARD_RE|uint32_t vms_kif_negctl_probe(uint32_t x);\n\n#endif /* _VMS_KIF_H */|" "$H"
}

add_decl_comment() {   # $1 = comment body, verbatim
    sed -i "s|$GUARD_RE|/* $1 */\n\n#endif /* _VMS_KIF_H */|" "$H"
}

add_probe_def() {
    printf '\nuint32_t vms_kif_negctl_probe(uint32_t x)\n{\n    struct vms_mode_args args;\n\n    vms_memset(&args, 0, sizeof(args));\n    args.mode = (uint8_t)x;\n\n    KIF_CALL(VMS_IOCTL_SETMODE, &args);\n\n    return args.status;\n}\n' >> "$C"
}

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. Without it, every red below could be the sandbox itself.
#
# Routed through expect_green() itself (round 8) with an EMPTY file list: the
# "for _f in $files" injection-landed loop iterates zero times over an empty
# $files, so this is exactly "run the gate, expect PASS" with no mutation to
# verify -- expect_green's own shape, not a lookalike. Before round 8 this
# hand-rolled its own "PASS:"/passed++ instead of calling the dispatcher; that
# was the in-file template a rogue bypass could copy undetected. See the
# header's TESTED, NOT ASSUMED paragraph for what routing this control (and
# the meta-control below) through the dispatcher does and does not fix.
# ---------------------------------------------------------------------------
expect_green "" \
    "positive control - unmutated sandbox tree passes the census" \
    "core"

# ---------------------------------------------------------------------------
# META-CONTROL. The no-op detector must go red on a dead anchor. The anchor
# used here is a real one that broke before: sys_lock.c's bind_to_executive(),
# which vms-9fc deleted.
#
# This does not run the gate at all -- it tests injection_landed() directly --
# so it cannot be expressed as an expect_red()/expect_green() call the way the
# positive control above was. It still calls tally_category() and
# record_verdict() itself (round 8), the same two functions the dispatcher
# calls, so this is an exception to "every control is invoked through
# expect_red()/expect_green()" -- it shares the dispatcher's
# verdict-recording, not its gate-running. This control's SOURCE is the only
# one in the file with no expect_red/expect_green call on ANY path.
# (Control 20's fixture-integrity guard, below, has a second bypass site in
# its source, but only on a defensive fallback path that is not exercised in
# this tree -- see the header paragraph this control's category call feeds,
# which states the two precisely rather than as one count.)
# ---------------------------------------------------------------------------
meta_name="meta-control - an injection whose anchor no longer matches is caught as a no-op"
if tally_category "$meta_name" "core"; then
    sed -i 's|^static void bind_to_executive(void)$|static void bind_to_executive(void) /* evasion */|' "$C"
    if injection_landed "$C"; then
        echo "  FAIL: meta-control - an injection anchored to a function that does not"
        echo "        exist was reported as having landed, so a future rename would"
        echo "        silently disarm every control below"
        record_verdict "$meta_name" 0
    else
        record_verdict "$meta_name" 1
    fi
fi
restore

# ---------------------------------------------------------------------------
# 1. THE CONTROL THE RULING NAMES: a new entry point, no caller, no
#    declaration. This is what shipped four times.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
expect_red "$H $C" \
    "a new entry point with no caller and no declaration is caught by name" \
    "core" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 2. GREEN CONTROL: the same entry point, declared. The escape hatch works,
#    and nothing else in the tree trips.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe (vms-7fb) -- negative control fixture"
expect_green "$H $C" \
    "a declared unwired entry point passes" \
    "core"

# ---------------------------------------------------------------------------
# 3. A declaration with no item id. Declared against vms_kif_enq -- an entry
#    point that IS wired -- so that ONLY the malformed property can fire: an
#    unparseable line registers no declaration at all, so pinning it to the
#    probe would trip the undeclared property too and the mutation would not
#    be minimal.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_enq -- will get to it later, honest"
expect_red "$H" \
    "an unwired declaration with no item id is rejected" \
    "core" \
    "$F_MALFORMED" "$F_UNDECL" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 4. A caller that exists only inside a COMMENT. This is not hypothetical: the
#    only occurrences of vms_kif_devscan and vms_kif_setident outside vms_kif.c
#    were comments saying the work was still to come, and the interfaces read
#    as wired to anyone grepping.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
sed -i 's|^        (void)uname;$|        (void)uname;\n        /* vms_kif_negctl_probe(1); -- conversion is future work */|' "$SHOW"
expect_red "$H $C $SHOW" \
    "a caller that exists only in a comment does not count" \
    "core" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 5. A caller that exists only in tests/. "Kernel facility + wrapper + test
#    suite, and no product path" is the exact defect; a test caller must not
#    satisfy the census. (tests/qemu/test_kmod_devtab.c really is the only
#    caller of the whole device family today.)
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
sed -i 's|^    status = vms_kif_getdvi_devnam(ABSENT_DEV, \&info);$|    status = vms_kif_getdvi_devnam(ABSENT_DEV, \&info);\n    (void)vms_kif_negctl_probe(1);|' "$QTEST"
expect_red "$H $C $QTEST" \
    "a caller that exists only in tests/ is not a product path" \
    "core" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 6. A caller inside vms_kif.c that nothing reaches. Presence of a call inside
#    the interface's own translation unit proves nothing -- a family that only
#    calls itself is still dead.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
printf '\nstatic void kif_negctl_dead(void)\n{\n    (void)vms_kif_negctl_probe(1);\n}\n' >> "$C"
expect_red "$H $C" \
    "a caller inside vms_kif.c that nothing reaches does not count" \
    "core" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 7. GREEN CONTROL, the other side of 6: a call from kif_bind(), which every
#    wired wrapper reaches through KIF_CALL -> kif_call. This is exactly how
#    vms_kif_open/register/kerr_to_ss are wired without any caller outside the
#    file, and a census that could not see it would demand a false declaration.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
sed -i 's|^    (void)vms_kif_register(NULL);$|    (void)vms_kif_register(NULL);\n    (void)vms_kif_negctl_probe(1);|' "$C"
expect_green "$H $C" \
    "a call from kif_bind() counts: the bind path is reachable from every wired wrapper" \
    "core"

# ---------------------------------------------------------------------------
# 8. A prototype at file scope is not a call. Re-declaring the entry point in
#    another translation unit must not wire it.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
sed -i 's|^#include "prvdef.h"|uint32_t vms_kif_negctl_probe(uint32_t x);\n#include "prvdef.h"|' "$SHOW"
expect_red "$H $C $SHOW" \
    "a prototype at file scope is not a caller" \
    "core" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 9. A declaration left behind on an entry point that IS wired. This is how a
#    census rots into an allowlist: wire the facility, keep the excuse.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_enq (vms-7fb) -- stale, it has been wired since"
expect_red "$H" \
    "a stale declaration on a wired entry point is rejected" \
    "core" \
    "$F_STALE" "$F_UNDECL" "$F_MALFORMED" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 10. A declaration naming a function that does not exist protects nothing --
#     and reads as coverage.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_no_such_entry (vms-7fb) -- typo or ghost"
expect_red "$H" \
    "a declaration naming a non-existent entry point is rejected" \
    "core" \
    "$F_UNKNOWN" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 11. The same entry point declared twice: two items each believing the other
#     owns it is how an unwired facility goes unclaimed.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe (vms-7fb) -- fixture"
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe (vms-2a8) -- fixture, second owner"
expect_red "$H $C" \
    "the same entry point declared twice is rejected" \
    "core" \
    "$F_DUP" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 12. THE REGRESSION CASE, and the one with day-to-day value: an existing wired
#     facility silently loses its only product caller. SHOW SYSTEM's
#     vms_kif_procscan() call is the reader that vms-8019 landed; delete it and
#     the executive process table is a facade again.
# ---------------------------------------------------------------------------
sed -i 's|^    while (vms_kif_procscan(&index, &info) & 1) {|    while (dcl_local_procscan(\&index, \&info) \& 1) {|' "$SHOW"
expect_red "$SHOW" \
    "an existing wired facility that loses its product caller is caught" \
    "core" \
    "vms_kif_procscan" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# THE UNIVERSE PINS (13-19). Everything above asks "is this entry point wired?".
# These ask the question that has to be settled first: CAN AN ENTRY POINT LEAVE
# THE CENSUS AT ALL? An earlier revision derived the universe from vms_kif.h
# alone, so deleting a prototype produced a smaller PASS -- the gate could be
# disarmed by removing the thing it counts, which is the same family as the
# guard compiled nowhere and the fixture that silently no-ops. 13-16 were all
# GREEN then; 17 and 18 were GREEN one revision later, when BOTH readings were
# still filtered on the vms_kif_ prefix so a rename left the census through both
# doors at once; 19 was GREEN until the floor grew its selector grain.
#
# CHOOSING A SUBJECT IS ITSELF CONSTRAINED, and the constraint is the point:
#   - vms_kif_getdvi_chan (13, 16, 17, 19) shares VMS_IOCTL_GETDVI with
#     vms_kif_getdvi_devnam, so mutating it cannot strand the OPCODE -- which is
#     exactly the property 19 needs. FIVE wrappers share an opcode this way, all
#     confirmed by the same brute force: getdvi_chan and getdvi_devnam over
#     VMS_IOCTL_GETDVI, and getjpi_self, getjpi_pid and getjpi_prcnam over
#     VMS_IOCTL_GETJPI. Deleting ANY of the five goes red at SELECTOR grain and
#     never at opcode grain, so any of them would serve as 19's subject; chan is
#     used because 13/16/17 already anchor on it. (An earlier revision of this
#     line called it "the only wrapper for which that is true". Also false, and
#     from the same habit as the two claims corrected below.)
#   - vms_kif_devscan (15, 18) is the sole issuer of VMS_IOCTL_DEVSCAN.
#   - vms_kif_close (14) is ONE OF THREE prototyped entry points whose body
#     issues no ioctl and names no selector -- the others are vms_kif_open and
#     vms_kif_kerr_to_ss -- so deleting its definition strands nothing on the
#     kernel side and the orphaned-prototype property can fire ALONE. All three
#     were checked: each yields the orphaned-prototype FAIL by itself. Any of
#     them would serve; close is used. (A fourth definition issues no opcode and
#     names no selector, the static vms_kif_alloc_op, but it has no prototype to
#     strand, so it cannot be this control's subject.)
#     AN EARLIER REVISION OF THIS COMMENT SAID "the only entry point", and the
#     gate header said no wrapper at all could be deleted without stranding
#     something. Brute force over all file-scope definitions in vms_kif.c
#     contradicted both, and THIS COUNT MOVES WHEN THE FILE DOES -- at 43
#     definitions (vms-7fb, after PR #22 / e5cf411 wired the event-flag
#     family and added the kif_wait_call static), 37 go RED and SIX are a
#     silent PASS: the three above plus the static vms_kif_alloc_op, plus the
#     statics kif_call and kif_wait_call -- both take their opcode as a
#     parameter the same way alloc_op does, so their removal strands nothing
#     on the kernel side either; kif_call was NOT in this set before PR #22,
#     because it was then the sole caller of kif_bind() and deleting it broke
#     the bind chain (a different property, not this one) -- kif_wait_call now
#     calls kif_bind() too, so that chain survives kif_call's deletion and it
#     joined the silent-PASS set. The floor's real claim is narrower -- no
#     wrapper THAT ISSUES AN OPCODE OR NAMES A SELECTOR can be deleted -- and
#     it is stated that way in the gate now. In a pair of files whose whole
#     subject is assertions nobody ever saw fail, an emphatic claim that has
#     not been run is the defect, spelled.
# ---------------------------------------------------------------------------

PROTO_CHAN='^uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo \*info);$'
DEF_CHAN='^uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo \*info)$'
PROTO_DEVSCAN='^uint32_t vms_kif_devscan(uint32_t \*index, struct vms_devinfo \*info);$'
DEF_DEVSCAN='^uint32_t vms_kif_devscan(uint32_t \*index, struct vms_devinfo \*info)$'
DEF_CLOSE='^void vms_kif_close(void)$'

# 13. The prototype is deleted; the definition stays. Under a header-only
#     census this dropped the entry point out of the universe silently.
sed -i "/$PROTO_CHAN/d" "$H"
expect_red "$H" \
    "deleting a prototype is a RED naming what vanished, not a smaller pass" \
    "pin" \
    "$F_ORPHAN_DEF" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 14. The definition is deleted; the prototype stays. The dangling half of the
#     same shrink, and the reason the two readings are compared BOTH ways.
sed -i "/$DEF_CLOSE/,/^}$/d" "$C"
expect_red "$C" \
    "deleting a definition is a RED naming what vanished" \
    "pin" \
    "$F_ORPHAN_PROTO" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 15. THE HONEST-LOOKING DISARM: delete the prototype, the definition AND the
#     declaration together. Nothing disagrees -- the union genuinely shrinks by
#     one and every remaining entry point is still wired or declared. What it
#     cannot do is take the kernel handler with it, so the floor catches it and
#     names the stranded opcode. This is why the floor exists.
sed -i "/$PROTO_DEVSCAN/d" "$H"
sed -i '/OVMX-UNWIRED: vms_kif_devscan/d' "$H"
sed -i "/$DEF_DEVSCAN/,/^}$/d" "$C"
expect_red "$H $C" \
    "deleting a wrapper outright strands its kernel opcode and is caught" \
    "pin" \
    "VMS_IOCTL_DEVSCAN" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_SEL"

# 16. THE ROUTE AROUND 13: delete the prototype and mark the definition static,
#     so the header/definition comparison has nothing to disagree about, then
#     drop the declaration. The union counts static definitions precisely so
#     this cannot work: the entry point is still in the census, now undeclared.
sed -i "/$PROTO_CHAN/d" "$H"
sed -i '/OVMX-UNWIRED: vms_kif_getdvi_chan/d' "$H"
sed -i "s|$DEF_CHAN|static &|" "$C"
expect_red "$H $C" \
    "an entry point cannot leave the census by going static" \
    "pin" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 17. THE ROUTE AROUND ALL OF 13-16: do not delete anything, RENAME. When both
#     readings were filtered on the vms_kif_ prefix, one name filter gated BOTH
#     halves of the union, so a single sed across the header and the source --
#     plus dropping the now-stale declaration, which the gate would otherwise
#     reject as naming a function that does not exist -- took the census from 38
#     entry points to 37 and PASSED. Nothing disagreed, no opcode was stranded
#     (the renamed body still issues it), and the wrapper was STILL THERE, still
#     unwired, still compiled. The definition reading is unfiltered precisely so
#     that this is a RED naming the new name.
#
#     THIS IS THE ONE CONTROL THAT ASSERTS TWO PROPERTIES, and it is not a
#     relaxation: a rename trips both BY CONSTRUCTION and cannot be narrowed to
#     one. The definition loses its vms_kif_ prototype (orphaned definition) AND
#     the renamed entry point is undeclarable -- the declaration grammar is
#     namespaced to vms_kif_, so "OVMX-UNWIRED: kif_getdvi_chan_impl (vms-fb9)"
#     is rejected as malformed rather than quieting it. There is no silent exit
#     from the census by rename, which is the property under test; the control
#     requires both halves instead of forbidding one it knows will fire.
sed -i '/OVMX-UNWIRED: vms_kif_getdvi_chan/d' "$H"
sed -i 's/vms_kif_getdvi_chan/kif_getdvi_chan_impl/g' "$H"
sed -i 's/vms_kif_getdvi_chan/kif_getdvi_chan_impl/g' "$C"
expect_red "$H $C" \
    "renaming an entry point out of the vms_kif_ namespace does not remove it" \
    "pin" \
    "kif_getdvi_chan_impl
$F_ORPHAN_DEF
$F_UNDECL" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" \
    "$F_DUP" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 18. THE ROUTE AROUND 17, composing it with 16: rename out of the namespace in
#     vms_kif.c ONLY, mark the definition static and delete both the prototype
#     and the declaration, so there is nothing left for the two readings to
#     disagree about. The unfiltered definition reading still holds the entry
#     point in the universe, where it is now unreachable and undeclared.
#     vms_kif_devscan is the subject because its body names no selector, so the
#     undeclared property fires alone.
sed -i "/$PROTO_DEVSCAN/d" "$H"
sed -i '/OVMX-UNWIRED: vms_kif_devscan/d' "$H"
sed -i "s|$DEF_DEVSCAN|static &|" "$C"
sed -i 's/vms_kif_devscan/kif_devscan_impl/g' "$C"
expect_red "$H $C" \
    "renaming out of the namespace AND going static does not remove it either" \
    "pin" \
    "kif_devscan_impl" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 19. THE SHARED-OPCODE DELETION, and the reason the floor has a second grain.
#     Delete vms_kif_getdvi_chan outright -- prototype, definition, declaration.
#     Nothing dangles, the universe shrinks honestly, and VMS_IOCTL_GETDVI is
#     STILL ISSUED by vms_kif_getdvi_devnam, so the opcode floor is satisfied:
#     under an opcode-only floor this was a clean, silent PASS. The harm is this
#     gate's own subject -- src/kernel/vms_ioctl.h dispatches vms_ioctl_getdvi()
#     on args.select, and after this deletion no userspace wrapper can reach the
#     VMS_DVI_SEL_CHAN path at all. The selector grain names it.
sed -i "/$PROTO_CHAN/d" "$H"
sed -i '/OVMX-UNWIRED: vms_kif_getdvi_chan/d' "$H"
sed -i "/$DEF_CHAN/,/^}$/d" "$C"
expect_red "$H $C" \
    "deleting a shared-opcode wrapper strands its kernel selector and is caught" \
    "pin" \
    "VMS_DVI_SEL_CHAN" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE"

# 20. THE NAME COLLISION, and it is the worst of the routes in this file --
#     because it does not make the census LOSE the unwired wrapper, it makes the
#     census VOUCH FOR it. Rename the definition to a name a product file already
#     calls AND mark it static, then drop the prototype and the declaration.
#     Nothing dangles (a static needs no prototype), no opcode or selector is
#     stranded (the renamed body still names VMS_IOCTL_DEVSCAN), and the entry
#     point is now credited to an entirely unrelated function of the same name in
#     src/vmslnm/. This was a clean PASS with the reached count moving UP -- 41
#     entry points, 15 reached, rc=0 -- for exactly one revision of the gate: the
#     revision that widened root seeding from the vms_kif_ prefix to the whole
#     universe in order to close control 17. The fix does not undo that widening,
#     which is right for externally-linked names; it removes from the SEEDABLE
#     set the names for which an outside caller is semantically impossible.
#
#     The extern half of this evasion needs no control of its own: without the
#     `static`, the definition has no vms_kif_ prototype and 1a fires first
#     (F_ORPHAN_DEF), which control 17 already covers.
COLLIDE=lnm_init

# FIXTURE CONSTRAINT, CHECKED RATHER THAN ASSUMED. This control only tests the
# seeding restriction if $COLLIDE is genuinely called by a product file the gate
# scans. If src/vmslnm/ ever renames it, the mutation still goes RED -- for the
# ordinary reason control 18 already covers -- and this control would silently
# degrade into a duplicate while still reporting PASS. That is the same
# silently-no-op shape injection_landed() exists to catch, one level up.
if ! grep -rl "[^A-Za-z0-9_]${COLLIDE}[ 	]*(" "$ROOT/src" --include='*.c' 2>/dev/null \
     | grep -qv 'libvmssys/vms_kif'; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 20 needs a product"
    echo "        function named ${COLLIDE}() outside vms_kif.c to collide with,"
    echo "        and there is none. Pick another real product function name;"
    echo "        do NOT drop the control -- without a real collision it tests"
    echo "        nothing and would report PASS anyway."
    # Round 8: was a hand-rolled "failed=$((failed + 1)); status=1" here, found
    # by grepping this file for exactly that pattern while auditing the claim
    # below that record_verdict() is the only place it happens -- it was not,
    # this predates round 8, and it also skipped tally_category(), so a broken
    # fixture on this path undercounted $pin_total too. Routed through both.
    tally_category "an unwired wrapper renamed onto a product function's name is NOT credited to it" "pin"
    record_verdict "an unwired wrapper renamed onto a product function's name is NOT credited to it" 0
else
    sed -i "/$PROTO_DEVSCAN/d" "$H"
    sed -i '/OVMX-UNWIRED: vms_kif_devscan/d' "$H"
    sed -i "s|^uint32_t vms_kif_devscan(|static uint32_t ${COLLIDE}(|" "$C"
    expect_red "$H $C" \
        "an unwired wrapper renamed onto a product function's name is NOT credited to it" \
        "pin" \
        "$COLLIDE
$F_UNDECL" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"
fi

# ---------------------------------------------------------------------------
# RUN, NOT DESCRIBED. Immediately before control 21 was appended above (only
# the "expect_red ... category pin" call block, nothing else in this file
# touched): "controls: 22 passed, 0 failed" / "universe-pin controls run: 8".
# Immediately after, same command, same file otherwise unchanged:
# "controls: 23 passed, 0 failed" / "universe-pin controls run: 9". The
# summary line below is what printed both times; it was not hand-edited
# between them.
# ---------------------------------------------------------------------------
echo "  controls: $passed passed, $failed failed"
echo "  universe-pin controls run: $pin_total (13 onward -- see the header for what"
echo "          each proves; tallied by expect_red()/expect_green() themselves as a"
echo "          byproduct of running each control, not by a second statement here)"
if [ "$status" -eq 0 ]; then
    echo "vms_kif census negative controls: PASS"
else
    echo "vms_kif census negative controls: FAIL"
fi
exit "$status"
