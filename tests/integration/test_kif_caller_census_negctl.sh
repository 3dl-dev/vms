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
# round 9: this file used to carry a $pin_total tally over controls 13-21,
# and a "category" argument to expect_red()/expect_green() to feed it.
# $pin_total was only ever incremented or printed, never read in a
# condition -- it gated nothing, and three rounds (6, 7, 8) each shipped a
# false claim about how faithfully it was kept. The suite's real output is
# which controls passed and which failed, printed by name below. Removed
# rather than corrected a fourth time; see the item's audit trail for what
# each round's false claim was.
#
# NOT DETECTED, BY CONSTRUCTION (round 8, reconfirmed round 9). A control
# that hand-rolls `echo "  PASS: ..."` and `passed=$((passed + 1))` instead
# of calling record_verdict() produces an internally consistent summary --
# $passed is a plain shell variable, and the printed count is computed from
# it after the fact, not compared against anything independent. Tried
# (round 8): a rogue control appended before the summary, printing its own
# "  PASS:" line and incrementing $passed by hand with no call to
# record_verdict, produced "controls: 24 passed" under 24 "PASS:" lines --
# consistent, not mismatched. rc=0, suite green, nothing caught it. THIS
# SHAPE IS NOT DETECTED, by inspection or otherwise: sh has no construct
# that stops a future author writing raw `if`/`echo`/`passed=$((passed+1))`
# instead of calling a function. Do not read this paragraph as a stronger
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
# And the controls (22 onward) that pin what 1-21 all take for granted: that
# the text being read is PART OF THE PRODUCT. Every one of these was a clean
# PASS on the revision of the gate that scanned raw source under src/ and
# tools/ -- see their own definitions below for the measured numbers.
#
#   22 a caller in a file that is in NO CMakeLists                  -> RED
#   23 a caller inside `#if 0`, in a file that IS built             -> RED
#   24 a body and prototype exfiltrated into #included files        -> RED
#   25 a tree that does not configure                               -> REFUSE
#   26 an interface translation unit the build does not compile     -> REFUSE
#   27 an exfiltrated body in a .inc that is NOT interface-private   -> RED
#
# And the controls (28 onward) that pin what 1-27 all take for granted: that
# the ITEM a declaration cites is a real, open item. Every one of these was a
# clean PASS until vms-8cc -- the id was shape-checked, and the gate's header
# said so. See control 28's own definition for the two measured attacks.
#
#   28 a fabricated id the ledger does not resolve               -> RED
#   29 ... the same id after the ledger is regenerated           -> RED
#   30 a citation of a CLOSED rd item                            -> RED
#   31 the citation ledger is deleted                            -> REFUSE
#   32 the ledger has a malformed row                            -> REFUSE
#   33 the ledger has no generated-at stamp                      -> REFUSE
#   34 the ledger lists one id twice                             -> REFUSE
#   35 ... a citation repointed to a DIFFERENT open item         -> GREEN
#
# And the controls (36 onward) that pin what 1-35 all take for granted: that
# the function HOLDING a call is reachable from anything at all. Every red
# among them was a clean PASS until rd vms-c13, at a price of TWO EDITS in two
# files the build already compiles. See their own definitions for the numbers.
#
#   36 a call inside a dead STATIC                              -> RED
#   37 ... the same function EXTERNALLY LINKED                  -> RED
#   38 ... a dead static NAMED AFTER a reached product function -> RED
#   39 ... reached only from a DEAD callback table              -> RED
#   40 GREEN: reached from a LIVE callback table                -> GREEN
#   41 the SCALED form: one dead static, all 12 wrappers        -> RED
#
# Usage: test_kif_caller_census_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_kif_caller_census.sh"
status=0
passed=0
failed=0

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
# deliberately does NOT credit, and case 5 exists to prove it.
#
# THE TOP-LEVEL CMakeLists.txt IS PART OF WHAT THE GATE READS NOW (rd vms-e2b).
# The census no longer globs source text: it configures cmake and reads
# compile_commands.json, so the sandbox has to be a configurable tree or every
# control below would meet a refusal instead of the property it is testing.
# Controls 25 and 26 are the ones that deliberately break that and require the
# refusal.
# ---------------------------------------------------------------------------
ROOT="$WORK/tree"
mkdir -p "$ROOT" "$WORK/orig"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp -a "$SRC_ROOT/tools" "$ROOT/tools"
cp -a "$SRC_ROOT/tests" "$ROOT/tests"
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"
# tracking/ carries the rd citation ledger the gate now resolves cited item ids
# against (rd vms-8cc). It is tree DATA, so it lives in the sandbox where
# controls 28-34 can break it. The CHECKER is not: the gate loads
# tests/integration/lib/rd_citations.sh from its own directory, so no control
# here can disarm the check by editing the copy.
cp -a "$SRC_ROOT/tracking" "$ROOT/tracking"

H="$ROOT/src/libvmssys/vms_kif.h"
C="$ROOT/src/libvmssys/vms_kif.c"
SHOW="$ROOT/src/vmsdcl/dcl_cmd_show.c"
QTEST="$ROOT/tests/qemu/test_kmod_devtab.c"
TOPCM="$ROOT/CMakeLists.txt"
SYSCM="$ROOT/src/libvmssys/CMakeLists.txt"
STRC="$ROOT/src/libvmssys/vms_string.c"
LEDGER="$ROOT/tracking/rd-citations.tsv"

MUTABLE="$H $C $SHOW $QTEST $TOPCM $SYSCM $STRC $LEDGER"

# THE ID EVERY FIXTURE DECLARATION CITES, and why it is not this gate's own
# item any more. The fixtures used to cite vms-7fb (the census's item) and
# vms-2a8; BOTH ARE NOW `done`. Since vms-8cc the gate resolves cited ids
# against the ledger, so a fixture citing a closed item reds for the CITATION
# property instead of for the property under test -- and the two GREEN controls
# would red outright, which is how a suite starts reporting failures that say
# nothing about the gate. These two are open at the time of writing AND carry a
# ledger row, because they are cited from src/. If either closes, controls 2, 9,
# 10, 11 and 20 go red naming the citation, which is the correct and legible
# failure: repoint them, or add the fixture ids to the ledger.
FIX_ITEM="vms-a86"
FIX_ITEM2="vms-as1"

# Files a control CREATES rather than edits. restore() removes them, because
# a leftover fabricated caller would silently contaminate every later control.
CREATED="$ROOT/src/libvmssys/kif_negctl_orphan.c $ROOT/src/libvmssys/vms_kif_close.inc $ROOT/src/libvmssys/vms_kif_close_proto.h $ROOT/src/libvmssys/vms_kif_ttsetmode.inc"

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
    for _f in $CREATED; do
        rm -f "$_f"
    done
}

# created_landed <file>: 0 if a control that CREATES a file really created it.
# The counterpart of injection_landed() for the controls whose mutation is a
# new file rather than an edit -- without it, a control whose creation silently
# failed would run against an unmutated tree and blame the gate.
created_landed() {
    [ -s "$1" ]
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
# The two refusals the build-set reading added (rd vms-e2b). They are not
# "properties fired"; they are the gate declining to measure at all.
F_NO_BUILD="so there is no build set"
F_NO_IFACE="is not in the product build set"
# The citation properties (rd vms-8cc). Three reds -- the cited item is
# unresolved, does not exist, or is closed -- and four refusals, because a
# ledger that cannot be read is not a reason to certify anything.
F_CITE_UNLISTED="NOT IN the citation ledger"
F_CITE_ABSENT="cites an rd item that DOES NOT EXIST"
F_CITE_CLOSED="cites a CLOSED rd item"
F_CITE_NO_LEDGER="no citation ledger at"
F_CITE_MALFORMED="malformed citation ledger"
F_CITE_NO_STAMP="citation ledger has no generated-at stamp"
F_CITE_LEDGER_DUP="listed twice in the citation ledger"

# Every control forbids all three universe-pin fragments except the one it is
# testing. They are spelled out at each call site rather than collected in a
# variable: the fragments contain spaces, so an unquoted expansion would split
# them into words that match nothing, and every "forbidden" check would pass
# vacuously -- a control that cannot fail, which is this file's whole subject.

# record_verdict <name> <ok>: the only place in this file where a CONTROL'S
# OUTCOME touches $passed/$failed/$status or prints a "  PASS:" line
# (grepped, not assumed -- ANCHORED to a leading tab/space, because this
# file's own prose quotes both patterns verbatim in several places,
# including this sentence, so an unanchored grep would count its own
# commentary: `grep -c '^\s*passed=\$((passed'` and
# `grep -c '^\s*echo "  PASS:'` each return 1, both inside this
# function's own body). expect_red() and expect_green() call this instead of
# incrementing $passed inline; so do the positive control and the
# meta-control below, neither of which runs through expect_red/expect_green
# because neither exercises a mutation. Any FAIL diagnostic text for a
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

# expect_red <files> <name> <required> [forbidden ...]
expect_red() {
    files="$1"; name="$2"; need="$3"; shift 3
    if [ -z "$files" ]; then
        echo "  FAIL: BROKEN FIXTURE: $name"
        echo "        called with no files to verify a mutation landed in --"
        echo "        a control with nothing to check injection_landed() against"
        echo "        cannot prove its mutation ran; it is not a dispatcher call."
        record_verdict "$name" 0
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

# expect_green <files> <name>
expect_green() {
    files="$1"; name="$2"
    if [ -z "$files" ]; then
        echo "  FAIL: BROKEN FIXTURE: $name"
        echo "        called with no files to verify a mutation landed in --"
        echo "        a control with nothing to check injection_landed() against"
        echo "        cannot prove its mutation ran; it is not a dispatcher call."
        record_verdict "$name" 0
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
# There is no mutation for it to verify landed, so it does not go through
# expect_green() (round 9: an empty file list through the dispatcher was a
# fixture-integrity hole -- see expect_red/expect_green's own BROKEN FIXTURE
# guard above) -- it runs the gate directly and records the verdict itself,
# the same as the meta-control below.
# ---------------------------------------------------------------------------
positive_name="positive control - unmutated sandbox tree passes the census"
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    record_verdict "$positive_name" 1
else
    echo "  FAIL: the census rejected a legitimate, unmutated tree: $positive_name"
    printf '%s\n' "$out" | sed 's/^/          /'
    record_verdict "$positive_name" 0
fi

# ---------------------------------------------------------------------------
# META-CONTROL. The no-op detector must go red on a dead anchor. The anchor
# used here is a real one that broke before: sys_lock.c's bind_to_executive(),
# which vms-9fc deleted.
#
# This does not run the gate at all -- it tests injection_landed() directly --
# so it cannot be expressed as an expect_red()/expect_green() call the way the
# other controls are. It calls record_verdict() itself, the same function the
# dispatcher calls, so this and the positive control above are the two
# controls in the file with no expect_red/expect_green call on any path.
# ---------------------------------------------------------------------------
meta_name="meta-control - an injection whose anchor no longer matches is caught as a no-op"
sed -i 's|^static void bind_to_executive(void)$|static void bind_to_executive(void) /* evasion */|' "$C"
if injection_landed "$C"; then
    echo "  FAIL: meta-control - an injection anchored to a function that does not"
    echo "        exist was reported as having landed, so a future rename would"
    echo "        silently disarm every control below"
    record_verdict "$meta_name" 0
else
    record_verdict "$meta_name" 1
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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 2. GREEN CONTROL: the same entry point, declared. The escape hatch works,
#    and nothing else in the tree trips.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe ($FIX_ITEM) -- negative control fixture"
expect_green "$H $C" \
    "a declared unwired entry point passes"

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
    "$F_MALFORMED" "$F_UNDECL" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 4. A caller that exists only inside a COMMENT. This is not hypothetical: the
#    only occurrences of vms_kif_devscan and vms_kif_setident outside vms_kif.c
#    were comments saying the work was still to come, and the interfaces read
#    as wired to anyone grepping.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
# Re-anchored (vms-2b8, this round): the previous anchor was
# `(void)uname;`, a line inside cmd_show_process()'s /ALL branch that no
# longer exists -- vms-6a7 rewrote cmd_show_process() to read a target's
# identity through sys$getjpi instead of the caller's own ctx->username,
# and vms-2b8 (this round) applied the same executive-read fix to the /ALL
# row. Re-anchored to a line that still exists and is unique in this file,
# not relaxed: the property under test (a call inside a comment is not a
# caller) is unchanged.
sed -i 's|^        return cmd_show_process_quotas(ctx);$|        return cmd_show_process_quotas(ctx);\n        /* vms_kif_negctl_probe(1); -- conversion is future work */|' "$SHOW"
expect_red "$H $C $SHOW" \
    "a caller that exists only in a comment does not count" \
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
    "a call from kif_bind() counts: the bind path is reachable from every wired wrapper"

# ---------------------------------------------------------------------------
# 8. A prototype at file scope is not a call. Re-declaring the entry point in
#    another translation unit must not wire it.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
sed -i 's|^#include "prvdef.h"|uint32_t vms_kif_negctl_probe(uint32_t x);\n#include "prvdef.h"|' "$SHOW"
expect_red "$H $C $SHOW" \
    "a prototype at file scope is not a caller" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 9. A declaration left behind on an entry point that IS wired. This is how a
#    census rots into an allowlist: wire the facility, keep the excuse.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_enq ($FIX_ITEM) -- stale, it has been wired since"
expect_red "$H" \
    "a stale declaration on a wired entry point is rejected" \
    "$F_STALE" "$F_UNDECL" "$F_MALFORMED" "$F_UNKNOWN" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 10. A declaration naming a function that does not exist protects nothing --
#     and reads as coverage.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_no_such_entry ($FIX_ITEM) -- typo or ghost"
expect_red "$H" \
    "a declaration naming a non-existent entry point is rejected" \
    "$F_UNKNOWN" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_DUP" "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 11. The same entry point declared twice: two items each believing the other
#     owns it is how an unwired facility goes unclaimed.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe ($FIX_ITEM) -- fixture"
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe ($FIX_ITEM2) -- fixture, second owner"
expect_red "$H $C" \
    "the same entry point declared twice is rejected" \
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
    "$F_ORPHAN_DEF" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# 14. The definition is deleted; the prototype stays. The dangling half of the
#     same shrink, and the reason the two readings are compared BOTH ways.
sed -i "/$DEF_CLOSE/,/^}$/d" "$C"
expect_red "$C" \
    "deleting a definition is a RED naming what vanished" \
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
    record_verdict "an unwired wrapper renamed onto a product function's name is NOT credited to it" 0
else
    sed -i "/$PROTO_DEVSCAN/d" "$H"
    sed -i '/OVMX-UNWIRED: vms_kif_devscan/d' "$H"
    sed -i "s|^uint32_t vms_kif_devscan(|static uint32_t ${COLLIDE}(|" "$C"
    expect_red "$H $C" \
        "an unwired wrapper renamed onto a product function's name is NOT credited to it" \
        "$COLLIDE
$F_UNDECL" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"
fi

# ---------------------------------------------------------------------------
# 21. THE WITHIN-NAMESPACE COLLISION -- a route 13-20 never tried: instead of
#     renaming a wrapper OUT of the vms_kif_ namespace (17-20), rename it onto
#     a SIBLING that is still IN it. Take vms_kif_kerr_to_ss (reached today
#     with no declaration, through kif_call/kif_wait_call) and rename its
#     prototype and definition to vms_kif_setmode -- an existing, genuinely
#     unwired entry point that already carries its own OVMX-UNWIRED
#     declaration (vms-pv1). sort -u then collapses the two names into one
#     line in both $WORK/protos and $WORK/defs_extern, so nothing orphans and
#     the universe shrinks by exactly one with no disagreement between the two
#     readings -- the same silent-shrink shape 13-20 all close by a different
#     door. What catches THIS one is not a universe check at all: the
#     kerr_to_ss call sites now name vms_kif_setmode, so vms_kif_setmode
#     becomes REACHED while its own OVMX-UNWIRED line is still sitting in the
#     header -- an existing property (control 9's, "stale declaration on a
#     wired entry point"), reached by a new route.
#
#     A GAP THIS CONTROL EXPOSED, WHILE PROVING THE POINT, RECORDED HONESTLY
#     RATHER THAN QUIETLY ROUTED AROUND. The reason 21 is caught is that the
#     collision TARGET (vms_kif_setmode) happens to carry a declaration.
#     Colliding the identical way onto a sibling that has NO declaration is a
#     silent PASS this file cannot turn into a passing control without
#     changing the gate's pass/fail behaviour, which this round is not
#     authorized to do. Measured directly (vms-7fb r7, not asserted):
#     renaming vms_kif_kerr_to_ss onto vms_kif_open -- both reached today,
#     neither declared -- drops the union from 43 entries / 24 reached to 42
#     entries / 23 reached with rc=0, the census reporting PASS having
#     silently lost an entry point. See test_kif_caller_census.sh's "WHAT
#     THIS GATE DOES NOT SEE" section for the same finding recorded there,
#     and this round's report to the operator for the follow-up.
# ---------------------------------------------------------------------------
sed -i 's/vms_kif_kerr_to_ss/vms_kif_setmode/g' "$H"
sed -i 's/vms_kif_kerr_to_ss/vms_kif_setmode/g' "$C"
expect_red "$H $C" \
    "renamed onto a sibling's name, the sibling's own declaration goes stale" \
    "$F_STALE" \
    "$F_UNDECL" "$F_MALFORMED" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"

# ---------------------------------------------------------------------------
# 22-26. THE BUILD-SET AND PREPROCESSOR PROPERTIES (rd vms-e2b).
#
# 1-21 all ask a question about the SOURCE TEXT. 22-24 ask the question that
# has to be settled before any of them mean anything: IS THIS TEXT PART OF THE
# PRODUCT AT ALL? Each of the three was bought against the previous revision of
# this gate, cheaply, and MEASURED -- the numbers below are what the gate
# printed, not what it was expected to print:
#
#   22 a caller in a file that is in NO CMakeLists          -> RED
#      ... 13 additive edits, one new file under src/libvmssys/ in no build
#          and #included by nothing, plus retiring the declarations it
#          "wires": rc=0, and the reached count went 31/44 -> 44/44. The
#          number this gate hands the Phase 2 verdict, bought outright.
#   23 a caller inside `#if 0` in a file that IS built     -> RED
#      ... two lines appended to src/vmsdcl/dcl_cmd_show.c: rc=0, 44/32/12.
#          The old reader was a text scan and was not preprocessor-aware.
#   24 an entry point whose body and prototype are moved
#      into #included files, and its declaration retired   -> RED
#      ... EXFILTRATION: the entry point left the UNIVERSE rather than
#          gaining a caller. rc=0 and the census printed 43 entry points
#          instead of 44, with `cc -E -P` output byte-identical to pristine.
#          Doing it to all 13 unwired wrappers at once printed "31 entry
#          points -- 31 reached, 0 unwired" with all 13 still compiled.
#   25 the tree cannot be configured at all                 -> REFUSE
#   26 the interface TU is in no CMakeLists                 -> REFUSE
#      ... 25 and 26 are not evasions, they are the NO-SILENT-FALLBACK rule.
#          A census that quietly reverted to the old glob when the build set
#          was unavailable would report the same PASS from a strictly worse
#          measurement. Both assert the refusal AND forbid every property
#          fragment, so a refusal that also claimed to have measured
#          something would fail these.
# ---------------------------------------------------------------------------

ORPHAN="$ROOT/src/libvmssys/kif_negctl_orphan.c"

# 22. FIXTURE CONSTRAINT, CHECKED RATHER THAN ASSUMED: the file must be in no
#     CMakeLists. If some CMakeLists ever globs src/libvmssys/*.c, this file
#     WOULD be compiled, it WOULD be a real product caller, and the control
#     would be testing nothing while still reporting PASS.
if grep -rq 'kif_negctl_orphan' "$ROOT" --include=CMakeLists.txt 2>/dev/null \
   || grep -rq 'file(GLOB' "$ROOT/src/libvmssys" --include=CMakeLists.txt 2>/dev/null; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 22 needs a source"
    echo "        file that the build does NOT compile, and src/libvmssys is"
    echo "        globbed or already names it. Pick a directory that lists its"
    echo "        sources; do NOT drop the control."
    record_verdict "a caller in a file that is in no CMakeLists is not a product path" 0
else
    add_probe_decl
    add_probe_def
    printf '#include "vms_kif.h"\nvoid kif_negctl_orphan(void);\nvoid kif_negctl_orphan(void)\n{\n    (void)vms_kif_negctl_probe(1);\n}\n' > "$ORPHAN"
    if ! created_landed "$ORPHAN"; then
        echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 22 could not"
        echo "        create ${ORPHAN#$ROOT/}, so it ran against a tree with no"
        echo "        fabricated caller in it and proved nothing."
        record_verdict "a caller in a file that is in no CMakeLists is not a product path" 0
        restore
    else
        expect_red "$H $C" \
            "a caller in a file that is in no CMakeLists is not a product path" \
            "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
            "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
            "$F_NO_BUILD" "$F_NO_IFACE"
    fi
fi

# 23. The same call, in a file the build DOES compile, inside `#if 0`. The
#     subject is src/vmsdcl/dcl_cmd_show.c because it is a real product
#     translation unit with real vms_kif callers in it -- so the only thing
#     distinguishing this call from its neighbours is the dead preprocessor
#     block around it.
add_probe_decl
add_probe_def
printf '\n#if 0\nvoid kif_negctl_dead_block(void);\nvoid kif_negctl_dead_block(void)\n{\n    (void)vms_kif_negctl_probe(1);\n}\n#endif\n' >> "$SHOW"
expect_red "$H $C $SHOW" \
    "a caller inside #if 0 does not count, even in a file that is built" \
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

# 24. EXFILTRATION. vms_kif_close is the subject for the same reason control 14
#     uses it: its body issues no opcode and names no selector, so nothing on
#     the kernel side fires and the undeclared property can be asserted alone.
#     Body -> vms_kif_close.inc, prototype -> vms_kif_close_proto.h, both
#     #included straight back, declaration retired. Nothing is deleted and
#     nothing dangles; the entry point simply stops being visible to a reader
#     that looks at two files instead of at the translation unit.
sed -n '/^void vms_kif_close(void)$/,/^}$/p' "$C" > "$ROOT/src/libvmssys/vms_kif_close.inc"
sed -i '/^void vms_kif_close(void)$/,/^}$/c\
#include "vms_kif_close.inc"' "$C"
echo 'void vms_kif_close(void);' > "$ROOT/src/libvmssys/vms_kif_close_proto.h"
sed -i 's|^void vms_kif_close(void);$|#include "vms_kif_close_proto.h"|' "$H"
# Retire the declaration by replacing the token, so the enclosing comment stays
# terminated -- deleting the line would swallow the prototype below it.
sed -i 's|OVMX-UNWIRED: vms_kif_close (vms-a86)|(retired by negctl 24)|' "$H"
if ! created_landed "$ROOT/src/libvmssys/vms_kif_close.inc" \
   || ! created_landed "$ROOT/src/libvmssys/vms_kif_close_proto.h"; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 24 extracted an"
    echo "        empty body or prototype -- its anchor no longer matches, so"
    echo "        the tree it ran against was not the evasion. Re-anchor it."
    record_verdict "a body moved into an #included .inc does not leave the census" 0
    restore
else
    expect_red "$H $C" \
        "a body moved into an #included .inc does not leave the census" \
        "vms_kif_close
$F_UNDECL" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE"
fi

# 25. NO SILENT FALLBACK, part one: the tree does not configure. The gate must
#     REFUSE, not quietly read source text instead.
# A parse error rather than an empty file: MEASURED, an EMPTY CMakeLists.txt
# still configures successfully (cmake rc=0) and the gate then refuses one step
# later, at "configured but produced no compile_commands.json" -- a different
# refusal, and this control is about the configure failing.
printf 'this_is_not_a_cmake_command(\n' > "$TOPCM"
expect_red "$TOPCM" \
    "a tree that does not configure is a REFUSAL, not a fallback to a glob" \
    "$F_NO_BUILD" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_IFACE"

# 26. NO SILENT FALLBACK, part two, and the sharper half: the tree configures
#     fine, but the INTERFACE ITSELF is compiled by nothing. Every reading in
#     the census would then be describing dead source, and a PASS would mean
#     the opposite of what it says.
sed -i '/^    vms_kif\.c$/d' "$SYSCM"
expect_red "$SYSCM" \
    "an interface translation unit the build does not compile is a REFUSAL" \
    "$F_NO_IFACE" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD"

# ---------------------------------------------------------------------------
# 27. THE ROUTE AROUND 24, and the one that made section 1 grow a third
#     definition reading. 24 relies on the exfiltrated .inc being PRIVATE to
#     the interface translation unit -- so make it not private: mark the
#     function `static`, so two translation units can each hold a copy and the
#     product still LINKS, and #include the .inc from vms_kif.c AND from one
#     other product file. Then delete the prototype, retire the declaration,
#     and add a floor reference so the raw opcode floor stays satisfied.
#     Nothing dangles, nothing is stranded, and the entry point is gone.
#
#     MEASURED (vms-e2b): this took the census from 44 entry points to 43 with
#     rc=0 -- on the previous gate AND on this one, until the namespaced third
#     reading of the interface TU landed. It is caught now because the function
#     KEEPS ITS NAME. The same evasion composed with a rename out of the
#     vms_kif_ namespace is a residual this suite does NOT close; it is written
#     up, with its recipe and its measured numbers, in the gate's own
#     "WHAT THIS GATE DOES NOT SEE" section, and it is not silently absent.
# ---------------------------------------------------------------------------
TTINC="$ROOT/src/libvmssys/vms_kif_ttsetmode.inc"
sed -n '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/^}$/p' "$C" > "$TTINC"
sed -i 's|^uint32_t vms_kif_ttsetmode(|static uint32_t vms_kif_ttsetmode(|' "$TTINC"
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/^}$/c\
#include "vms_kif_ttsetmode.inc"' "$C"
printf '\n#include "vms_kif_ttsetmode.inc"\n' >> "$STRC"
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/uint32_t width, uint32_t page);$/d' "$H"
sed -i 's|OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36)|(retired by negctl 27)|' "$H"
printf '\nenum { kif_negctl_floor_ref = (int)VMS_IOCTL_TTSETMODE };\n' >> "$C"
if ! created_landed "$TTINC"; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 27 extracted an"
    echo "        empty body -- its anchor no longer matches vms_kif.c, so the"
    echo "        tree it ran against was not the evasion. Re-anchor it."
    record_verdict "an exfiltrated body in a NON-private .inc does not leave the census" 0
    restore
else
    expect_red "$H $C $STRC" \
        "an exfiltrated body in a NON-private .inc does not leave the census" \
        "vms_kif_ttsetmode
$F_UNDECL" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE"
fi

# ---------------------------------------------------------------------------
# 28-35. THE CITATION PROPERTIES (rd vms-8cc).
#
# WHAT THESE PIN, and it was measured on the revision immediately before them:
# the item id in an OVMX-UNWIRED line was SHAPE-checked and nothing else, and
# the gate said so in its own header. `sed -i s/(vms-a86)/(vms-q9z9)/` on one
# declaration -- an id that has never existed -- left the census rc=0 at
# 44 entry points / 31 reached / 13 unwired, unchanged. Repointing another at
# vms-fb9, whose rd status is `done`, did the same. ONE TOKEN bought the
# exemption, which made "13 unwired" a floor on how many were genuinely tracked
# rather than a bound on it.
#
# The gate still cannot ask rd -- rd is nostr-backed and unreachable from CI --
# so it reads tracking/rd-citations.tsv, derived by tools/gen_rd_citations.py
# on a host that has rd. That moves the trust from "a human wrote an id down"
# to "the ledger says so", and these controls are what stop THAT from being
# another unfired assertion. Four of the eight break the ledger rather than the
# citation, because a ledger that cannot be read must be a REFUSAL: if deleting
# the evidence were cheaper than fixing the citation, the whole mechanism would
# be one `rm` from vacuous.
#
#   28 a citation the ledger does not resolve at all            -> RED
#   29 a citation the ledger records as unknown to rd           -> RED
#   30 a citation of a CLOSED rd item                           -> RED
#   31 the ledger is deleted                                    -> REFUSE
#   32 the ledger has a malformed row                           -> REFUSE
#   33 the ledger has no generated-at stamp                     -> REFUSE
#   34 the ledger lists one id twice                            -> REFUSE
#   35 GREEN: a citation repointed to a DIFFERENT open item     -> GREEN
#
# WHY 30 AND 34 BUILD THEIR OWN CLOSED EXEMPLAR (rd vms-a85). Both used to name
# the literal `vms-fb9`, an id that was `done` in rd AND carried a ledger row
# because src/libvms/syssvc/sys_device.c cited it. 30 repointed a declaration
# at it; 34 appended a second row for it. vms-fab then repointed that citation
# -- it was the last closed id cited anywhere under src/ -- and the regenerated
# ledger became 22 rows, every one open. MEASURED on that tree: control 30 went
# red for the WRONG reason (F_CITE_UNLISTED: the id is in no row at all), and
# control 34's appended row became the FIRST vms-fb9 row rather than a second
# one, so no refusal fired and it reported "the census CERTIFIED the evasion".
# A control written to prove the duplicate-row property was proving nothing.
#
# Both had been keyed on a property of the product tree that another item
# existed to REMOVE, which is the same defect this suite exists to catch, one
# level up -- and the same one vms-fab found in the sibling register negctl.
# Re-aiming them at a different real closed id is not available: after vms-fab
# no closed id is cited under src/ or tools/ at all, and keeping it that way is
# what vms-fab is for. Deriving the exemplar from whatever the ledger happens
# to carry was the third option; it resolves to nothing on this tree, so the
# control would disable itself, which is the shape these gates reject.
#
# So each control SYNTHESIZES its fixture in the sandbox -- the citing
# declaration AND the ledger row that resolves it -- and then CHECKS that what
# it built has the property it is about to test the gate against.
# injection_landed() proves a file changed; it does not prove the change means
# anything, and control 34 is what that distinction costs when it is skipped.
# What is under test here is the gate's reading of a `closed` row and of two
# rows for one id, not rd's opinion of any particular item, so a row in the
# shape tools/gen_rd_citations.py writes is the faithful fixture.
# ---------------------------------------------------------------------------

# The synthetic id controls 30 and 34 own. Nothing in the product tree cites
# it and no committed ledger row names it; cite_id_free below is the check
# that this is still true, rather than an assumption.
CITE_CLOSED_ID="vms-negctl30"

# cite_row <verdict> <rd status> <title>: append one row for $CITE_CLOSED_ID in
# the ledger's own four-field tab-separated shape.
cite_row() {
    printf '%s\t%s\t%s\t%s\n' "$CITE_CLOSED_ID" "$1" "$2" "$3" >> "$LEDGER"
}

# The verdict column of every row now carrying $CITE_CLOSED_ID, IN FILE ORDER,
# space-joined: "" before a fixture is built, "closed" after 30's, and
# "open closed" after 34's -- which is the order that matters, since the reader
# takes the first match.
cite_verdicts() {
    awk -F'\t' -v id="$CITE_CLOSED_ID" '
        $1 == id { out = (out == "" ? $2 : out " " $2) }
        END { print out }' "$LEDGER"
}

# How many declarations cite it. 1 once a fixture is built.
cite_decl_count() {
    grep -cF "OVMX-UNWIRED: vms_kif_ttsetmode ($CITE_CLOSED_ID)" "$H"
}

# How many rows the PRISTINE sandbox ledger carries for it -- 0, or these two
# controls are measuring a real citation instead of the fixture they build.
cite_pristine_rows() {
    awk -F'\t' -v id="$CITE_CLOSED_ID" '
        $1 == id { n++ } END { print n + 0 }' "$WORK/orig/$(key_of "$LEDGER")"
}

# cite_fixture_broken <control> <want-verdicts>: 0 when the fixture just built
# does NOT have the property the control is about to test the gate against,
# after printing why. This is the check control 34 did not have: it appended a
# row and asked the gate a question, and when the row stopped being a SECOND
# row there was nothing between that and a green suite.
cite_fixture_broken() {
    if [ "$(cite_pristine_rows)" -eq 0 ] && \
       [ "$(cite_verdicts)" = "$2" ] && [ "$(cite_decl_count)" -eq 1 ]; then
        return 1
    fi
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): $1"
    echo "        the fixture does not have the property this control tests: the"
    echo "        ledger carries [$(cite_verdicts)] for $CITE_CLOSED_ID, of which"
    echo "        $(cite_pristine_rows) row(s) were there before this control ran,"
    echo "        and $(cite_decl_count) declaration(s) cite it -- where [$2], none"
    echo "        pre-existing, and one citing declaration are wanted. The gate"
    echo "        would be asked about a tree that is not the evasion. Re-anchor"
    echo "        it, or rename the synthetic id; do NOT relax the gate."
    return 0
}

# 28. The fabricated id, ledger untouched. This is the attack exactly as it was
#     run against the previous revision: one token, nothing else edited.
sed -i 's|OVMX-UNWIRED: vms_kif_getlki (vms-a86)|OVMX-UNWIRED: vms_kif_getlki (vms-q9z9)|' "$H"
expect_red "$H" \
    "a fabricated item id is not resolved by the ledger" \
    "$F_CITE_UNLISTED" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" \
    "$F_CITE_ABSENT" "$F_CITE_CLOSED" "$F_CITE_NO_LEDGER" \
    "$F_CITE_MALFORMED" "$F_CITE_NO_STAMP" "$F_CITE_LEDGER_DUP"

# 29. The same fabricated id AFTER the ledger is regenerated -- which is what an
#     author who ran tools/gen_rd_citations.py would actually commit. The
#     generator does not invent a row; it records that rd has no such item, and
#     the gate reds on that rather than on the id being unlisted. Without this
#     control, "regenerate it and the red goes away" would be true.
sed -i 's|OVMX-UNWIRED: vms_kif_getlki (vms-a86)|OVMX-UNWIRED: vms_kif_getlki (vms-q9z9)|' "$H"
printf 'vms-q9z9\tabsent\t-\t(rd has no such item)\n' >> "$LEDGER"
expect_red "$H $LEDGER" \
    "a fabricated item id stays red after the ledger is regenerated" \
    "$F_CITE_ABSENT" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" \
    "$F_CITE_UNLISTED" "$F_CITE_CLOSED" "$F_CITE_NO_LEDGER" \
    "$F_CITE_MALFORMED" "$F_CITE_NO_STAMP" "$F_CITE_LEDGER_DUP"

# 30. A CLOSED item, the fixture built in the sandbox (see the note above).
#     Two edits: a declaration repointed at $CITE_CLOSED_ID, and the row the
#     generator writes for a cited id that rd reports done. The gate is then
#     asked the question it is for -- does a `closed` row refuse the exemption
#     -- with nothing else in the tree changed. The required fragments include
#     the id, so the red has to be about THIS citation and not some other.
name30="a citation of a CLOSED rd item is rejected"
sed -i "s|OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36)|OVMX-UNWIRED: vms_kif_ttsetmode ($CITE_CLOSED_ID)|" "$H"
cite_row closed done "negctl 30 -- the row the generator writes for a closed item"
if cite_fixture_broken "$name30" "closed"; then
    record_verdict "$name30" 0
    restore
else
    expect_red "$H $LEDGER" \
        "$name30" \
        "$F_CITE_CLOSED
$CITE_CLOSED_ID" \
        "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE" \
        "$F_CITE_UNLISTED" "$F_CITE_ABSENT" "$F_CITE_NO_LEDGER" \
        "$F_CITE_MALFORMED" "$F_CITE_NO_STAMP" "$F_CITE_LEDGER_DUP"
fi

# 31. Delete the ledger. The declarations are untouched and all five cited
#     items are open in rd -- so the ONLY honest answer is a refusal to
#     measure. A gate that reported "0 citations checked, PASS" here would be
#     the silent-fallback shape Rule 9 forbids one layer down.
rm -f "$LEDGER"
expect_red "$LEDGER" \
    "a deleted citation ledger is a REFUSAL, not a skip" \
    "$F_CITE_NO_LEDGER" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" \
    "$F_CITE_UNLISTED" "$F_CITE_ABSENT" "$F_CITE_CLOSED" \
    "$F_CITE_MALFORMED" "$F_CITE_NO_STAMP" "$F_CITE_LEDGER_DUP"

# 32. A row this reader cannot parse. Skipping it is how a derived ledger rots
#     into an allowlist with a typo in it, so an unparseable row refuses the
#     whole file rather than dropping one line.
printf 'vms-a86 open inbox not tab separated at all\n' >> "$LEDGER"
expect_red "$LEDGER" \
    "a malformed ledger row is a REFUSAL" \
    "$F_CITE_MALFORMED" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" \
    "$F_CITE_UNLISTED" "$F_CITE_ABSENT" "$F_CITE_CLOSED" \
    "$F_CITE_NO_LEDGER" "$F_CITE_NO_STAMP" "$F_CITE_LEDGER_DUP"

# 33. Strip the stamp. The ledger is a SNAPSHOT of rd; the one residual this
#     whole design carries is that the snapshot can be older than the truth.
#     A ledger whose age cannot be printed cannot have that residual judged, so
#     losing the stamp is a refusal rather than a cosmetic loss.
sed -i '/^# generated-at:/d' "$LEDGER"
expect_red "$LEDGER" \
    "a ledger with no generated-at stamp is a REFUSAL" \
    "$F_CITE_NO_STAMP" \
    "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" \
    "$F_CITE_UNLISTED" "$F_CITE_ABSENT" "$F_CITE_CLOSED" \
    "$F_CITE_NO_LEDGER" "$F_CITE_MALFORMED" "$F_CITE_LEDGER_DUP"

# 34. Two rows for one id -- the override, written in the order that would buy
#     something. The reader takes the FIRST match, so the forged `open` row
#     goes ABOVE the true `closed` one, and the id is CITED by a declaration so
#     that there is a verdict to steal: this is 30's tree with one row pasted
#     in front of the row that reds it. Forbidding F_CITE_CLOSED is the load-
#     bearing half of the assertion -- it is what proves the refusal beat the
#     forgery rather than the forgery being harmless.
#
#     MEASURED, so that this control's non-vacuity is a run and not a claim:
#     with the duplicate-row refusal deleted from
#     tests/integration/lib/rd_citations.sh (the `_cs_dups` block, 10 lines)
#     and nothing else changed, the gate reads the forged row first, prints
#     "13 declaration site(s) cite 5 distinct rd item(s) -- 5 open, 0 closed"
#     and exits rc=0 on this control's tree: the census CERTIFIES the override
#     for an item its own ledger also records as closed. The full suite on
#     that mutilated tree was 42 passed / 1 FAILED, the single failure being
#     this control, reporting "the census CERTIFIED the evasion".
name34="an id listed twice in the ledger is a REFUSAL"
sed -i "s|OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36)|OVMX-UNWIRED: vms_kif_ttsetmode ($CITE_CLOSED_ID)|" "$H"
cite_row open active "negctl 34 -- forged override row, written ABOVE the true one"
cite_row closed done "negctl 34 -- the row the generator writes for a closed item"
if cite_fixture_broken "$name34" "open closed"; then
    record_verdict "$name34" 0
    restore
else
    expect_red "$H $LEDGER" \
        "$name34" \
        "$F_CITE_LEDGER_DUP
$CITE_CLOSED_ID" \
        "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE" \
        "$F_CITE_UNLISTED" "$F_CITE_ABSENT" "$F_CITE_CLOSED" \
        "$F_CITE_NO_LEDGER" "$F_CITE_MALFORMED" "$F_CITE_NO_STAMP"
fi

# 35. GREEN. Repoint a declaration from one OPEN ledgered item to a DIFFERENT
#     one. Every red above edits a citation, so without this the suite would be
#     equally consistent with a check that reds on ANY change to a cited id --
#     which would make the escape hatch unusable and would be discovered by
#     whoever next needed to move a declaration to its real owner, not here.
sed -i 's|OVMX-UNWIRED: vms_kif_getlki (vms-a86)|OVMX-UNWIRED: vms_kif_getlki (vms-dv1)|' "$H"
expect_green "$H" \
    "a citation repointed to a different OPEN item still passes"

# ---------------------------------------------------------------------------
# 36-41. THE REACHABILITY PROPERTIES (rd vms-c13).
#
# 22-27 settled "is this text part of the product". These settle the question
# that survived it: IS THE FUNCTION HOLDING THE CALL REACHABLE FROM ANYTHING?
# Until section 2' of the gate existed, a call anywhere in any compiled product
# translation unit counted, with no requirement on its enclosing function. The
# buy was TWO EDITS in two files the build already compiles -- no new file, no
# CMakeLists change, no `#if 0`:
#
#     src/vmsdcl/dcl_cmd_show.c:
#         static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }
#     src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#
#   rc=0, "44 entry points -- 32 reached, 12 with no product path", PASS, and
#   `cmake --build --target vmsdcl` clean. Scaled to one 16-line dead static
#   calling all 12 externally-linked unwired wrappers, with `sed
#   s/OVMX-UNWIRED:/NOTE:/` over the header, the same gate printed "44 entry
#   points -- 44 reached, 0 with no product path" and PASSED.
#
#   36 a call inside a dead STATIC                              -> RED
#   37 ... the same function EXTERNALLY LINKED                  -> RED
#   38 ... a dead static NAMED AFTER a reached product function -> RED
#   39 ... reached only from a DEAD callback table              -> RED
#   40 GREEN: reached from a LIVE callback table                -> GREEN
#   41 the SCALED form: one dead static, all 12 wrappers        -> RED
#
# EVERY ONE OF 36-39 AND 41 IS A CLEAN PASS ON THE REVISION BEFORE vms-c13,
# and all five retire a REAL declaration rather than using the probe: the
# probe has no declaration, so a dead caller for it would go red whether or
# not the dead call counted, and the control would prove nothing. The subject
# is vms_kif_chkpriv -- genuinely unwired, declared against vms-pv1, and its
# body issues VMS_IOCTL_CHKPRIV, so nothing on the kernel side moves either.
# ---------------------------------------------------------------------------

RETIRE_CHKPRIV='s|OVMX-UNWIRED: vms_kif_chkpriv (vms-pv1)|(retired by the reachability controls)|'

# 36. THE BUY THE RULING NAMES, exactly as measured.
sed -i "$RETIRE_CHKPRIV" "$H"
printf '\nstatic void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\n' >> "$SHOW"
expect_red "$H $SHOW" \
    "a call inside a function nothing calls is not a product path" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

# 37. THE SAME FUNCTION, EXTERNALLY LINKED. It needs its own control because
#     the obvious fix for 36 -- "a static nothing calls is dead" -- would not
#     touch this one, and an exported symbol IS a legitimate root when a
#     header declares it. What makes this dead is that nothing declares it.
sed -i "$RETIRE_CHKPRIV" "$H"
printf '\nvoid ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\n' >> "$SHOW"
expect_red "$H $SHOW" \
    "external linkage alone does not make a dead function a root" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

# 38. THE NODE COLLISION, and it is control 20's shape one level up: instead of
#     colliding an unwired WRAPPER with a product function's name, collide the
#     DEAD CALLER with a reached one, so the dead body inherits the live
#     function's reachability. It is defeated by node identity being (origin
#     file, name) for a static in a translation unit -- which is what `static`
#     means -- rather than the bare name. $COLLIDE is checked to be a real
#     product function by control 20 above, so if src/vmslnm/ renames it, that
#     control reports the broken fixture before this one runs.
sed -i "$RETIRE_CHKPRIV" "$H"
printf '\nstatic void %s(void) { (void)vms_kif_chkpriv(0); }\n' "$COLLIDE" >> "$SHOW"
expect_red "$H $SHOW" \
    "a dead static named after a reached product function does not inherit its reachability" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: $COLLIDE" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

# 39. THE ROUTE AROUND 36-38, and it was open for one revision of the fix.
#     Indirect calls have to be followed or the gate reds on correct code, so
#     the first version of section 2' made ANY function whose address is taken
#     a root. That is bought by writing a table: two lines in ONE file, and
#     rc=0 at 44 / 32 / 12 again. The table is DEAD -- nothing reads it -- so
#     the fix is that address-taking is an EDGE from whatever context takes the
#     address, not a root, and a file-scope table is reachable only when
#     something reachable names it. Control 40 is the other side.
sed -i "$RETIRE_CHKPRIV" "$H"
printf '\nstatic void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\nstatic void (*const ovmx_dead_tab[1])(void) = { ovmx_dead_helper };\n' >> "$SHOW"
expect_red "$H $SHOW" \
    "a DEAD callback table does not make the functions in it reachable" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

# 40. GREEN CONTROL, the other side of 39, and the one that stops the fix for
#     39 from being a gate that simply cannot see indirect calls. DCL dispatches
#     its verbs through a table, RMS takes completion routines, $ENQ and $QIO
#     take AST handlers: a census blind to those would report every handler
#     unreachable and demand false declarations for whatever they call. Here the
#     probe is called ONLY from a function whose address is taken in a table,
#     and that table is named by cmd_show_process() -- which is reached. The
#     census must be green.
#
#     THE TABLE IS DECLARED IN THE `(*const tab[1])(void)` FORM ON PURPOSE:
#     the declared name sits INSIDE the declarator parentheses, and while the
#     reader tracked only names outside them, this exact control was RED --
#     the table had no node, so nothing could reference it and its handler
#     read as dead. That is an under-count on correct code, and it is why the
#     reader tracks the last name followed by "[" as well.
add_probe_decl
add_probe_def
sed -i 's|^extern int vms_status_string(uint32_t status, char \*buf, size_t bufsize);$|static uint32_t kif_negctl_cb(void) { return vms_kif_negctl_probe(1); }\nstatic uint32_t (*const kif_negctl_tab[1])(void) = { kif_negctl_cb };\n&|' "$SHOW"
sed -i 's|^        return cmd_show_process_quotas(ctx);$|        (void)kif_negctl_tab[0]();\n        return cmd_show_process_quotas(ctx);|' "$SHOW"
expect_green "$H $C $SHOW" \
    "a function reached only through a LIVE callback table counts as a caller"

# 41. THE SCALED FORM, which is what the buy looks like when it is used rather
#     than demonstrated: ONE dead static calling every externally-linked
#     unwired wrapper, and one sed retiring every declaration in the header.
#     On the revision before vms-c13 this printed "44 entry points -- 44
#     reached, 0 with no product path" and PASSED, handing the Phase 2 verdict
#     a fully-wired executive interface with nothing wired.
sed -i 's/OVMX-UNWIRED:/NOTE:/' "$H"
printf '\nstatic void ovmx_dead_helper(void)\n{\n    uint8_t m = 0; uint64_t a = 0, b = 0, p = 0; uint32_t g = 0, r = 0;\n    char nm[64]; uint8_t vb[16];\n    vms_kif_close();\n    (void)vms_kif_setmode(0);\n    (void)vms_kif_getmode(&m, &a, &b);\n    (void)vms_kif_setprv(0, 0, 0, &p);\n    (void)vms_kif_chkpriv(0);\n    (void)vms_kif_dclast(0, 0, 0);\n    (void)vms_kif_setast(0);\n    (void)vms_kif_deliverast(&a, &b, &m);\n    (void)vms_kif_getlki(0, &g, &r, nm, vb);\n    (void)vms_kif_alloc("X");\n    (void)vms_kif_dalloc("X");\n    (void)vms_kif_ttsetmode(0, 0, 0, 0, 0, 0);\n}\n' >> "$SHOW"
expect_red "$H $SHOW" \
    "one dead static calling every unwired wrapper wires nothing" \
    "vms_kif_chkpriv
vms_kif_ttsetmode
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE"

echo "  controls: $passed passed, $failed failed"
if [ "$status" -eq 0 ]; then
    echo "vms_kif census negative controls: PASS"
else
    echo "vms_kif census negative controls: FAIL"
fi
exit "$status"
