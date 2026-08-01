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

# expect_red <files> <name> <required> [forbidden ...]
expect_red() {
    files="$1"; name="$2"; need="$3"; shift 3
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#$ROOT/} at all -- its"
            echo "        anchor no longer matches the source, so this control ran"
            echo "        the gate against an UNMUTATED tree and proved nothing."
            echo "        Re-anchor the mutation; do NOT relax the gate."
            failed=$((failed + 1)); status=1; restore; return
        fi
    done

    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    if [ "$rc" -eq 0 ]; then
        echo "  FAIL: the census CERTIFIED the evasion: $name"
        ok=0
    elif ! printf '%s\n' "$out" | grep -qF "$need"; then
        echo "  FAIL: the census went red for the WRONG reason: $name"
        echo "        expected to see: $need"
        ok=0
    else
        for bad in "$@"; do
            if printf '%s\n' "$out" | grep -qF "$bad"; then
                echo "  FAIL: mutation is not minimal -- another property also fired: $name"
                echo "        unexpected: $bad"
                ok=0
            fi
        done
    fi

    if [ "$ok" -eq 1 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        printf '%s\n' "$out" | sed 's/^/          /'
        failed=$((failed + 1)); status=1
    fi
    restore
}

# expect_green <files> <name>
expect_green() {
    files="$1"; name="$2"
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#$ROOT/} at all."
            failed=$((failed + 1)); status=1; restore; return
        fi
    done

    out=$(sh "$GATE" "$ROOT" 2>&1)
    if [ $? -eq 0 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        echo "  FAIL: the census rejected a legitimate tree: $name"
        printf '%s\n' "$out" | sed 's/^/          /'
        failed=$((failed + 1)); status=1
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
# ---------------------------------------------------------------------------
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    echo "  PASS: positive control - unmutated sandbox tree passes the census"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - unmutated sandbox tree FAILS the census, so no"
    echo "        negative control below can attribute a RED to its mutation"
    printf '%s\n' "$out" | sed 's/^/          /'
    failed=$((failed + 1)); status=1
fi

# ---------------------------------------------------------------------------
# META-CONTROL. The no-op detector must go red on a dead anchor. The anchor
# used here is a real one that broke before: sys_lock.c's bind_to_executive(),
# which vms-9fc deleted.
# ---------------------------------------------------------------------------
sed -i 's|^static void bind_to_executive(void)$|static void bind_to_executive(void) /* evasion */|' "$C"
if injection_landed "$C"; then
    echo "  FAIL: meta-control - an injection anchored to a function that does not"
    echo "        exist was reported as having landed, so a future rename would"
    echo "        silently disarm every control below"
    failed=$((failed + 1)); status=1
else
    echo "  PASS: meta-control - an injection whose anchor no longer matches is caught as a no-op"
    passed=$((passed + 1))
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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

# ---------------------------------------------------------------------------
# 2. GREEN CONTROL: the same entry point, declared. The escape hatch works,
#    and nothing else in the tree trips.
# ---------------------------------------------------------------------------
add_probe_decl
add_probe_def
add_decl_comment "OVMX-UNWIRED: vms_kif_negctl_probe (vms-7fb) -- negative control fixture"
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
    "$F_MALFORMED" "$F_UNDECL" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

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
    "vms_kif_negctl_probe" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

# ---------------------------------------------------------------------------
# 9. A declaration left behind on an entry point that IS wired. This is how a
#    census rots into an allowlist: wire the facility, keep the excuse.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_enq (vms-7fb) -- stale, it has been wired since"
expect_red "$H" \
    "a stale declaration on a wired entry point is rejected" \
    "$F_STALE" "$F_UNDECL" "$F_MALFORMED" "$F_UNKNOWN" "$F_DUP"

# ---------------------------------------------------------------------------
# 10. A declaration naming a function that does not exist protects nothing --
#     and reads as coverage.
# ---------------------------------------------------------------------------
add_decl_comment "OVMX-UNWIRED: vms_kif_no_such_entry (vms-7fb) -- typo or ghost"
expect_red "$H" \
    "a declaration naming a non-existent entry point is rejected" \
    "$F_UNKNOWN" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_DUP"

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
    "$F_DUP" "$F_UNDECL" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN"

# ---------------------------------------------------------------------------
# 12. THE REGRESSION CASE, and the one with day-to-day value: an existing wired
#     facility silently loses its only product caller. SHOW SYSTEM's
#     vms_kif_procscan() call is the reader that vms-8019 landed; delete it and
#     the executive process table is a facade again.
# ---------------------------------------------------------------------------
sed -i 's|^    while (vms_kif_procscan(&index, &info) & 1) {|    while (dcl_local_procscan(\&index, \&info) \& 1) {|' "$SHOW"
expect_red "$SHOW" \
    "an existing wired facility that loses its product caller is caught" \
    "vms_kif_procscan" "$F_MALFORMED" "$F_STALE" "$F_UNKNOWN" "$F_DUP"

# ---------------------------------------------------------------------------
echo "  controls: $passed passed, $failed failed"
if [ "$status" -eq 0 ]; then
    echo "vms_kif census negative controls: PASS"
else
    echo "vms_kif census negative controls: FAIL"
fi
exit "$status"
