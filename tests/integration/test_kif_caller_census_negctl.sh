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
# fired), so a mutation that goes red for the wrong reason fails this test.
# SOME CASES ARE GREEN CONTROLS -- they prove the census does not simply fail
# on any edit, and they bound the OVER-firing of each rule from the other
# side. Do not recite how many; count the expect_green calls, and add one
# whenever a new disqualifier lands. A gate that reds on correct code is the
# one the next person weakens.
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
# And the controls (42 onward) that pin what 36-41 all take for granted: that
# a call written inside a REACHED function is a call the product MAKES. Every
# red among them was a clean PASS until rd vms-c79, at a price of TWO EDITS in
# two files the build already compiles -- no new file, no dead function, no
# CMakeLists change. See their own definitions for the numbers.
#
#   42 an `if (0)` call inside a LIVE function                  -> RED
#   43 ... the same, one level up, through a helper             -> RED
#   44 ... an ADDRESS-TAKE under the dead branch                -> RED
#   45 ... the same buy in a translation unit built -O2         -> RED
#   46 GREEN: a call under a RUNTIME-false condition is credited
#   47 GREEN: a call through a helper the compiler INLINES      -> GREEN
#   48 27 COMPOSED WITH A RENAME out of the vms_kif_ namespace  -> RED
#
# And the control (49) that pins what root rule 2 -- "every product function
# PROTOTYPED IN A HEADER THE BUILD COMPILES" -- takes for granted: that a
# header declaration is the exported-API shape the rule exists for. It was
# not: a `static` declaration AND definition, both written inside a header
# the build compiles into MULTIPLE translation units, was credited exactly
# like an extern one, because the declaration reading (a "P" record) never
# carried the static/extern qualifier. See the gate's own root-rule bullet in
# section 0 (WHAT THIS GATE DOES NOT PROVE) for the measured recipe and the
# rd vms-41b fix. This was a clean PASS at rc=0 on every gate revision through
# rd vms-c79.
#
#   49 a dead helper declared AND defined `static` in a
#      multi-includer header does not buy a root                -> RED
#
# And control 50, which pins the OTHER half of rule 2 (36-41 pin the call-graph
# propagation the rule feeds; this pins the rule's OWN grant), closed by rd
# vms-d33 -- narrowing what counts as "the product emits a call" from "is
# there a product path" one step further, toward "is the path one that could
# actually be invoked from outside its own translation unit":
#
#   50 a `static` function's BODY, residing in a HEADER the build     -> RED
#      compiles, is not a root merely by that residency
#
# Every one of 50 was a clean PASS on the revision before vms-d33's fix. See
# its own definition below for the measured before/after.
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
cp -a "$SRC_ROOT/tracking" "$ROOT/tracking"

H="$ROOT/src/libvmssys/vms_kif.h"
C="$ROOT/src/libvmssys/vms_kif.c"
SHOW="$ROOT/src/vmsdcl/dcl_cmd_show.c"
QTEST="$ROOT/tests/qemu/test_kmod_devtab.c"
TOPCM="$ROOT/CMakeLists.txt"
SYSCM="$ROOT/src/libvmssys/CMakeLists.txt"
STRC="$ROOT/src/libvmssys/vms_string.c"
DCLCMDH="$ROOT/src/vmsdcl/include/dcl/dcl_cmd.h"
DCLCMH="$DCLCMDH"

MUTABLE="$H $C $SHOW $QTEST $TOPCM $SYSCM $STRC $DCLCMDH"

# THE ID EVERY FIXTURE DECLARATION CITES: a label only (rd vms-dc7), not
# verified against anything. Kept as real, currently-open tree ids purely by
# convention from before the citation checker was torn down; nothing here
# breaks if either closes.
FIX_ITEM="vms-a86"
FIX_ITEM2="vms-as1"

# Files a control CREATES rather than edits. restore() removes them, because
# a leftover fabricated caller would silently contaminate every later control.
CREATED="$ROOT/src/libvmssys/kif_negctl_orphan.c $ROOT/src/libvmssys/vms_kif_close.inc $ROOT/src/libvmssys/vms_kif_close_proto.h $ROOT/src/libvmssys/vms_kif_ttsetmode.inc $ROOT/src/libvmssys/vms_kif_ttsetmode_renamed.inc"

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
# The refusal the evidence compile added (rd vms-c79). It is NOT a property
# firing: it is the gate declining to measure a tree that does not build. It
# is a forbidden fragment on every control that asks about the emitted-call
# rule, because a fixture that stopped compiling would otherwise look like the
# evasion being caught -- which is exactly what happened to controls 21, 27
# and 38 when the compile step landed, and all three were broken FIXTURES,
# not a working gate.
F_NO_EVIDENCE="could not compile a product translation unit"

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
#     prototype and definition onto vms_kif_close -- an existing, genuinely
#     unwired entry point that already carries its own OVMX-UNWIRED
#     declaration (vms-a86). The universe shrinks by exactly one with no
#     orphan and no disagreement between the two readings -- the same
#     silent-shrink shape 13-20 all close by a different door. What catches
#     THIS one is not a universe check at all: the kerr_to_ss call sites now
#     name vms_kif_close, so vms_kif_close becomes REACHED while its own
#     OVMX-UNWIRED line is still sitting in the header -- an existing property
#     (control 9's, "stale declaration on a wired entry point"), reached by a
#     new route.
#
#     THE COLLISION TARGET'S OWN DEFINITION AND PROTOTYPE ARE DELETED, AND
#     THAT IS FORCED BY C, NOT A CHOICE (rd vms-c79). This fixture used to
#     rename onto vms_kif_setmode and leave the original in place -- which is
#     TWO DEFINITIONS OF ONE NAME IN ONE TRANSLATION UNIT. That does not
#     compile, in any spelling: extern collides ("redefinition"), and static
#     collides too ("static declaration follows non-static declaration"). It
#     PREPROCESSED, which is all the gate used to require, so the control ran
#     green for rounds against a tree that could never have shipped. Once the
#     census started compiling the build set the control met a refusal instead
#     of the property it tests. The route being controlled is the RENAME; the
#     shippable form of it repurposes the name, so the fixture deletes what it
#     is overwriting. vms_kif_close is the right target for it: it is
#     floor-exempt (it issues no opcode and names no selector, so deleting its
#     body strands nothing on the kernel side, which would have been a second
#     unrelated red), it carries a declaration (which is what makes the route
#     detectable at all -- see the gap below), and NO PRODUCT FILE calls it,
#     so the tree still builds.
#
#     A GAP THIS CONTROL EXPOSED, WHILE PROVING THE POINT, RECORDED HONESTLY
#     RATHER THAN QUIETLY ROUTED AROUND. The reason 21 is caught is that the
#     collision TARGET (vms_kif_close) happens to carry a declaration.
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
sed -i '/^void vms_kif_close(void);$/d' "$H"
sed -i '/^void vms_kif_close(void)$/,/^}$/d' "$C"
sed -i 's/vms_kif_kerr_to_ss/vms_kif_close/g' "$H"
sed -i 's/vms_kif_kerr_to_ss/vms_kif_close/g' "$C"
if grep -q '^void vms_kif_close(void)$' "$C"; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 21 did not delete"
    echo "        the collision target's own definition, so the tree it ran"
    echo "        against has two definitions of one name and cannot compile."
    echo "        Re-anchor the deletion; do NOT relax the gate."
    record_verdict "renamed onto a sibling's name, the sibling's own declaration goes stale" 0
    restore
else
expect_red "$H $C" \
    "renamed onto a sibling's name, the sibling's own declaration goes stale" \
    "$F_STALE" \
    "$F_UNDECL" "$F_MALFORMED" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL"
fi

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
#
#     THE .inc IS SYNTHESIZED, NOT EXTRACTED, AND THE REASON IS A REAL
#     FAILURE (rd vms-c79). This fixture used to `sed -n` the real
#     vms_kif_ttsetmode body out of vms_kif.c and #include it into
#     vms_string.c. That body uses vms_kif.c PRIVATES -- struct
#     vms_setmode_args, vms_memset, the KIF_CALL macro -- none of which
#     vms_string.c has, so the sandbox tree PREPROCESSED but did not COMPILE.
#     That was invisible while the gate only preprocessed; the moment it
#     started compiling (vms-c79) the control met a refusal instead of the
#     property it tests. The exfiltration this control is about is the
#     DEFINITION LEAVING vms_kif.c into a non-private .inc, and a
#     self-contained stub with the same name and signature exfiltrates it
#     exactly as well -- while being a tree that actually builds, which is
#     the only kind of evasion anyone could really ship.
TTINC="$ROOT/src/libvmssys/vms_kif_ttsetmode.inc"
cat > "$TTINC" <<'EOF'
#include "vms_kif.h"

static uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,
                                  uint64_t setchar, uint64_t clrchar,
                                  uint32_t width, uint32_t page)
{
    (void)chan; (void)flags; (void)setchar; (void)clrchar;
    (void)width; (void)page;
    return 0;
}
EOF
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/^}$/c\
#include "vms_kif_ttsetmode.inc"' "$C"
printf '\n#include "vms_kif_ttsetmode.inc"\n' >> "$STRC"
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/uint32_t width, uint32_t page);$/d' "$H"
sed -i 's|OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36)|(retired by negctl 27)|' "$H"
printf '\nenum { kif_negctl_floor_ref = (int)VMS_IOCTL_TTSETMODE };\n' >> "$C"
# THE FIXTURE CHECK THAT MATTERS IS THE REMOVAL, NOT THE CREATION. With the
# .inc synthesized, created_landed() on it is trivially true and would prove
# nothing; and injection_landed("$C") is satisfied by the `enum` line this
# control also appends, so it cannot tell a landed body-removal from a missed
# one. So the guard asks the question directly: the real body -- identified by
# the KIF_CALL it issues -- must no longer be in vms_kif.c.
if ! created_landed "$TTINC" || grep -q 'KIF_CALL(VMS_IOCTL_TTSETMODE' "$C"; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 27 did not move"
    echo "        vms_kif_ttsetmode's body out of vms_kif.c -- its anchor no"
    echo "        longer matches, so the tree it ran against was not the"
    echo "        evasion. Re-anchor it."
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
#
#     IT INJECTS INTO vms_string.c, NOT dcl_cmd_show.c, AND THAT IS FORCED
#     (rd vms-c79). dcl_cmd_show.c includes vms/logical.h, which DECLARES
#     lnm_init() -- so `static void lnm_init(void)` there is a hard C error
#     ("static declaration follows non-static declaration"), and once the gate
#     started COMPILING the build set the control met a refusal instead of the
#     property it tests. It preprocessed cleanly for as long as nobody
#     compiled it. vms_string.c is freestanding and sees no declaration of
#     lnm_init, so the collision is expressible there and the tree builds. The
#     property is unchanged: node identity is (origin file, name) for a
#     static, and WHICH file holds the dead body is incidental to it.
sed -i "$RETIRE_CHKPRIV" "$H"
printf '\n#include "vms_kif.h"\nstatic void %s(void) { (void)vms_kif_chkpriv(0); }\n' "$COLLIDE" >> "$STRC"
expect_red "$H $STRC" \
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

# ---------------------------------------------------------------------------
# 42-47. THE EMITTED-CALL PROPERTIES (rd vms-c79).
#
# WHAT 36-41 ALL TAKE FOR GRANTED, and it was false: that a call written
# inside a REACHED function is a call the product makes. `if (0) { ... }`
# survives the preprocessor -- only `#if 0` does not -- and the enclosing
# function is genuinely reached, so vms-e2b's build set and vms-c13's call
# graph were both satisfied while the code could never run. MEASURED on the
# revision before vms-c79, TWO edits in two files the build already compiles,
# no new file, no CMakeLists change, no dead function:
#
#     src/vmsdcl/dcl_cmd_show.c, inside the body of cmd_show():
#         if (0) { (void)vms_kif_chkpriv(0); }
#     src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#
#   rc=0, "44 entry points -- 32 reached from the product, 12 with no product
#   path", PASS. One level up -- `if (0) { ovmx_dead_helper(); }` in cmd_show()
#   with the helper holding the call -- was the identical rc=0 at 44/32/12.
#
# NONE OF THESE CONTROLS NAMES A SYNTACTIC FORM TO THE GATE, and that is the
# point. `if (0)` is what they WRITE, but what the gate checks is whether the
# compiler emitted a relocation, so `if (1 == 2)`, `while (0)` and a
# constant-folded flag all die at the same door without the gate learning any
# of them. What does NOT die is a condition the compiler cannot fold --
# control 46 is that boundary, made explicit and green.
#
#   42 an `if (0)` call in a LIVE function                      -> RED
#   43 ... the same, one level up: `if (0) { dead_helper(); }`  -> RED
#   44 ... an ADDRESS-TAKE under the dead branch instead        -> RED
#   45 ... the same buy in a translation unit built -O2         -> RED
#   46 GREEN: a call under a RUNTIME-false condition is credited
#   47 GREEN: a call through a helper the compiler INLINES is credited
#
# 42-45 are all clean PASSes on the revision before vms-c79; 46 and 47 bound
# the over-firing, and 47 was RED while the evidence compile still let the
# compiler inline (see the gate's EVIDENCE_FLAGS for the measurement).
# ---------------------------------------------------------------------------

# 42. THE BUY THE FINDING NAMES, exactly as measured. The subject is
#     vms_kif_chkpriv for the same reason 36-41 use it: it is genuinely
#     unwired and declared, so retiring its token is the whole second edit,
#     and its body issues VMS_IOCTL_CHKPRIV so nothing on the kernel side
#     moves.
sed -i "$RETIRE_CHKPRIV" "$H"
sed -i 's|^    const char \*subcmd = cmd->params\[0\];$|    if (0) { (void)vms_kif_chkpriv(0); }\n&|' "$SHOW"
expect_red "$H $SHOW" \
    "a call the compiler deletes is not a product path, even in a live function" \
    "vms_kif_chkpriv
$F_UNDECL
them were call(s) to an entry point: vms_kif_chkpriv" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"

# 43. ONE LEVEL UP, and it needs its own control because the obvious fix for
#     42 -- disqualify the CALL SITE -- does not touch it. Here the call to
#     the entry point is in a helper whose own body is perfectly real; what
#     the compiler deletes is the CALL TO THE HELPER. The disqualifier has to
#     apply to call EDGES, not just to the leaf sites, or this is a two-edit
#     buy again.
sed -i "$RETIRE_CHKPRIV" "$H"
sed -i 's|^static int cmd_show_time(struct dcl_command \*cmd)$|static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\n&|' "$SHOW"
sed -i 's|^    const char \*subcmd = cmd->params\[0\];$|    if (0) { ovmx_dead_helper(); }\n&|' "$SHOW"
expect_red "$H $SHOW" \
    "a helper reached only through a branch the compiler deletes is not reached" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"

# 44. THE ROUTE AROUND 42-43: take the helper's ADDRESS in the dead branch
#     instead of calling it. Control 39/40 made address-taking an EDGE rather
#     than a root, so an address taken in a live function makes its target
#     reachable -- which is correct, and is exactly why the same disqualifier
#     has to apply here. The compiler erases the address-take with the branch;
#     MEASURED on the probe used to design this, one function containing an
#     `if (0)` address-take and a live one emits exactly one relocation.
sed -i "$RETIRE_CHKPRIV" "$H"
sed -i 's|^static int cmd_show_time(struct dcl_command \*cmd)$|static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\n&|' "$SHOW"
sed -i 's|^    const char \*subcmd = cmd->params\[0\];$|    if (0) { void (*p)(void) = ovmx_dead_helper; (void)p; }\n&|' "$SHOW"
expect_red "$H $SHOW" \
    "an address taken in a branch the compiler deletes reaches nothing" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"

# 45. THE SAME BUY IN A TRANSLATION UNIT THE BUILD COMPILES -O2, and it is not
#     a duplicate of 43. The disqualifier's escape hatch is "no evidence" --
#     if the compiler emitted no section for a function, nothing is claimed
#     about it -- and at -O2 an unreferenced static is dropped WHOLE, section
#     and all, which puts the dead helper straight through that escape. It
#     was a clean PASS at 44/32/12 until the evidence compile added
#     -fkeep-static-functions. src/libvmssys/vms_string.c is one of the 17
#     translation units this build compiles -O2; vms_strlen() is reached (a
#     header the build compiles declares it).
sed -i "$RETIRE_CHKPRIV" "$H"
sed -i 's|^vms_size_t vms_strlen(const char \*s)$|#include "vms_kif.h"\nstatic void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }\n&|' "$STRC"
sed -i 's|^    while (\*p)$|    if (0) { ovmx_dead_helper(); }\n&|' "$STRC"
expect_red "$H $STRC" \
    "the -O2 escape hatch does not hand the buy back inside an optimised TU" \
    "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
    "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
    "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
    "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"

# 46. GREEN CONTROL, and it is the boundary of what this mechanism can claim.
#     The condition here is FALSE at runtime for every invocation the product
#     ever makes -- cmd->param_count is never negative -- but the compiler
#     cannot prove it, so it emits the call and the census credits it. That is
#     the correct behaviour: the gate answers "did the compiler emit a call",
#     not "does that call execute", and a gate that guessed at the second
#     would red on every defensive branch in the tree. It is ALSO the residual,
#     priced: this control IS the surviving evasion, at two edits in two files.
#     Closing it needs execution evidence (rd vms-d33), not a better reader.
add_probe_decl
add_probe_def
sed -i 's|^    const char \*subcmd = cmd->params\[0\];$|    if (cmd->param_count < 0) { (void)vms_kif_negctl_probe(1); }\n&|' "$SHOW"
expect_green "$H $C $SHOW" \
    "a call under a condition the compiler cannot fold is still credited"

# 47. GREEN CONTROL, the other side of the -fno-inline flag, and it went RED
#     without it. At -O2 the compiler INLINES a small static helper into its
#     caller, and the inlined call leaves no relocation attributable to the
#     helper -- which is indistinguishable, to a relocation reader, from a
#     call that was deleted. MEASURED on a pristine tree with inlining left
#     on: 62 edges between product functions vanished and 28 functions lost
#     reachability, including vms_fopen -> alloc_file and
#     __vms_runtime_init -> parse_auxv. That is reddening correct code, which
#     is the failure mode that gets a gate weakened by the next person. The
#     evidence compile therefore disables inlining -- it does not disable the
#     dead-branch elimination it is there to observe.
add_probe_decl
add_probe_def
sed -i 's|^vms_size_t vms_strlen(const char \*s)$|#include "vms_kif.h"\nstatic uint32_t ovmx_inlined_helper(void) { return vms_kif_negctl_probe(1); }\n&|' "$STRC"
sed -i 's|^    while (\*p)$|    (void)ovmx_inlined_helper();\n&|' "$STRC"
expect_green "$H $C $STRC" \
    "a call through a helper the compiler inlines is still credited"

# ---------------------------------------------------------------------------
# 48. EXFILTRATION COMPOSED WITH A RENAME (vms-05e7), THE RECIPE 27's OWN
#     COMMENT NAMED AS UNCLOSED. 27 shows the gate catches a body moved to a
#     non-private .inc AS LONG AS THE NAME STAYS vms_kif_ttsetmode -- the
#     third definition reading is namespaced. This composes that with a
#     rename: mark the moved body `static`, rename it out of the vms_kif_
#     namespace, #include it from vms_kif.c AND vms_string.c so it is neither
#     interface-private nor namespaced, delete the prototype, retire the
#     declaration, and add a floor-reference enum so the raw opcode floor
#     stays satisfied. Same seven edits as the gate's own "WHAT THIS GATE DOES
#     NOT SEE" writeup, reproduced here rather than merely cited.
#
#     THE .inc IS SYNTHESIZED, NOT EXTRACTED, for the same reason control 27's
#     is: the real vms_kif_ttsetmode body uses vms_kif.c privates (struct
#     vms_setmode_args, vms_memset, KIF_CALL) that vms_string.c does not have,
#     so an extracted body would preprocess but not compile -- a broken
#     fixture, not a working control. A self-contained stub with the same
#     signature, renamed, exfiltrates the DEFINITION exactly as well while
#     being a tree that actually builds.
# ---------------------------------------------------------------------------
TTINC48="$ROOT/src/libvmssys/vms_kif_ttsetmode_renamed.inc"
cat > "$TTINC48" <<'EOF'
#include "vms_kif.h"

static uint32_t kif_ttsetmode_apply(uint32_t chan, uint32_t flags,
                                     uint64_t setchar, uint64_t clrchar,
                                     uint32_t width, uint32_t page)
{
    (void)chan; (void)flags; (void)setchar; (void)clrchar;
    (void)width; (void)page;
    return 0;
}
EOF
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/^}$/c\
#include "vms_kif_ttsetmode_renamed.inc"' "$C"
printf '\n#include "vms_kif_ttsetmode_renamed.inc"\n' >> "$STRC"
sed -i '/^uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,$/,/uint32_t width, uint32_t page);$/d' "$H"
sed -i 's|OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36)|(retired by negctl 48)|' "$H"
printf '\nenum { kif_negctl_floor_ref48 = (int)VMS_IOCTL_TTSETMODE };\n' >> "$C"
# THE FIXTURE CHECK THAT MATTERS IS THE REMOVAL, same rationale as control 27:
# the real body -- identified by the KIF_CALL it issues -- must no longer be
# in vms_kif.c, or this control ran against an unmutated tree.
if ! created_landed "$TTINC48" || grep -q 'KIF_CALL(VMS_IOCTL_TTSETMODE' "$C"; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 48 did not move"
    echo "        vms_kif_ttsetmode's body out of vms_kif.c -- its anchor no"
    echo "        longer matches, so the tree it ran against was not the"
    echo "        evasion. Re-anchor it."
    record_verdict "the composed rename+shared-.inc exfiltration does not leave the census" 0
    restore
else
    expect_red "$H $C $STRC" \
        "the composed rename+shared-.inc exfiltration does not leave the census" \
        "kif_ttsetmode_apply
$F_UNDECL" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"
fi

# ---------------------------------------------------------------------------
# 49. THE ROOT-RULE'S HEADER CLAUSE, AND ITS PRICE BEFORE rd vms-41b. Root
#     rule 2 credits "every product function prototyped in a header the build
#     compiles" because that is what an exported library entry point looks
#     like. A `static` declaration in a header is never that: each includer
#     gets its OWN private symbol, not a shared entry point, and dcl_cmd.h is
#     included by MULTIPLE translation units (checked below, not assumed) --
#     so a non-static definition there would not even link.
#
# FIXTURE CONSTRAINT, CHECKED RATHER THAN ASSUMED: this control only proves
# the point if dcl_cmd.h really is included by more than one product .c file.
# If it is ever narrowed to a single includer, the recipe still goes red for
# the ordinary reason (a static in a header included once is no different
# from a static in the .c itself) and this control would silently stop
# testing the header case while still reporting PASS.
if [ "$(grep -rl '#include "dcl/dcl_cmd.h"' "$ROOT/src" --include='*.c' 2>/dev/null | wc -l)" -lt 2 ]; then
    echo "  FAIL: BROKEN FIXTURE (not a broken gate): control 49 needs a header"
    echo "        included by more than one product .c file to prove a static"
    echo "        header declaration is not credited as exported. dcl_cmd.h no"
    echo "        longer qualifies; pick another multi-includer header."
    record_verdict "a static declaration AND definition in a multi-includer header does not buy a root" 0
else
    sed -i "$RETIRE_CHKPRIV" "$H"
    sed -i 's|^#include <stdint.h>$|#include <stdint.h>\n#include "vms_kif.h"\nstatic void ovmx_dead_helper(void);\nstatic void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }|' "$DCLCMDH"
    expect_red "$H $DCLCMDH" \
        "a static declaration AND definition in a multi-includer header does not buy a root" \
        "vms_kif_chkpriv
$F_UNDECL
credit NOTHING: ovmx_dead_helper" \
        "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP" \
        "$F_ORPHAN_DEF" "$F_ORPHAN_PROTO" "$F_ORPHAN_OPCODE" "$F_ORPHAN_SEL" \
        "$F_NO_BUILD" "$F_NO_IFACE" "$F_NO_EVIDENCE"
fi

# ---------------------------------------------------------------------------
# 50. THE ROOT RULE GRANTED BY HEADER RESIDENCY RATHER THAN LINKAGE
#     (rd vms-d33).
#
# Rule 2 grants a root to "every product function PROTOTYPED IN A HEADER THE
# BUILD COMPILES" -- ON PURPOSE, because an exported symbol is reachable by
# anything that links the library whether or not anything IN THIS TREE calls
# it. A `static` function is never exported, no matter which file its
# forward declaration sits in -- but the reader's (origin file, name) tagging
# that tells a `.c` TU's private static from an extern definition keyed
# statfn[] only for origins that are themselves one of the compiled
# TRANSLATION UNITS. A header is never itself a TU, so a `static` function
# whose declaration AND body both live in one fell through untagged, landed
# on the exact bare-name node an exported symbol gets, and rule 2 handed it a
# root.
#
# MEASURED before the fix, TWO EDITS in two files the build already compiles
# -- no new file, no CMakeLists change, no `#if 0`:
#
#     src/vmsdcl/include/dcl/dcl_cmd.h (included by every dcl_cmd_*.c):
#         static void ovmx_dead_helper(void);
#         static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }
#     src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#
#   rc=0, "44 entry points -- 32 the product emits a call to, 12 with no
#   product path", PASS, root count 731 -> 732 -- one more than main() and
#   every genuinely exported header prototype in the tree.
#
# THE SUBJECT IS AGAIN vms_kif_chkpriv, for the same reason 36-41 use it: it
# is genuinely unwired and declared, and its body issues VMS_IOCTL_CHKPRIV,
# so nothing on the kernel side moves either.
sed -i "$RETIRE_CHKPRIV" "$H"
sed -i 's|^#include <stdint.h>$|#include <stdint.h>\n#include "vms_kif.h"\nstatic void ovmx_dead_helper(void);\nstatic void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }|' "$DCLCMH"
expect_red "$H $DCLCMH" \
    "a static function's body residing in a compiled HEADER is not a root by residency alone" \
    "vms_kif_chkpriv
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
