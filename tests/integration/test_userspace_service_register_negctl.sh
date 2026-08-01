#!/bin/sh
#
# test_userspace_service_register_negctl.sh - the evasions (rd vms-5b4).
#
# A gate nobody tries to evade asserts nothing. This runs one MINIMAL mutation
# per property of tests/integration/test_userspace_service_register.sh against a
# sandbox copy of the tree, and requires each one to drive that gate RED FOR ITS
# OWN NAMED REASON -- not merely red. It also runs GREEN controls, which are the
# half that matters most here: the register's whole design rests on the claim
# that it fires on an undeclared IMPOSTOR and not on legitimate pure
# computation, and on the claim that Rule 10's second answer (delete the
# service, so the condition is unreachable) stays green. Neither is worth
# anything as prose.
#
# WHERE A MUTATION TRIPS TWO PROPERTIES, THIS FILE SAYS SO rather than pretending
# the mutation is narrower than it is. A malformed declaration is also an ABSENT
# one, because the gate parses the register out of the same line it validates:
# the two controls below assert the malformed message and note the second red as
# a consequence of the first, not as a second defect.
#
# The POSITIVE CONTROL runs first: if the pristine tree is not green, every
# verdict below is unfounded and this script says that instead of passing.
#
set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_userspace_service_register.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "userspace service register negative controls: every property needs an evasion that trips it"

if ! command -v cmp >/dev/null 2>&1; then
    echo "  FAIL: cmp(1) unavailable -- this file cannot verify that its own"
    echo "        injections landed, so its verdicts would be unfounded"
    exit 1
fi

ROOT="$WORK/tree"
mkdir -p "$ROOT" "$WORK/orig"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp -a "$SRC_ROOT/tools" "$ROOT/tools"

AST="$ROOT/src/libvms/syssvc/sys_ast.c"
EVENT="$ROOT/src/libvms/syssvc/sys_event.c"
STR="$ROOT/src/libvms/rtl/str_routines.c"
STARLET="$ROOT/src/libvms/include/starlet.h"

MUTABLE="$AST $EVENT $STR $STARLET"

key_of() { printf '%s' "${1#"$ROOT"/}" | tr '/.' '__'; }

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

injection_landed() {
    cmp -s "$1" "$WORK/orig/$(key_of "$1")" && return 1
    return 0
}

record_verdict() {
    if [ "$2" -eq 1 ]; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        failed=$((failed + 1)); status=1
    fi
}

# expect_red <files-that-must-have-changed> <name> <required-output-fragment>
expect_red() {
    files="$1"; name="$2"; need="$3"
    printf '%s\n' "$need" >> "$WORK/needs"
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#"$ROOT"/} at all -- its anchor no"
            echo "        longer matches the source, so this control ran the gate against an"
            echo "        UNMUTATED tree and proved nothing. Re-anchor it; do NOT relax the gate."
            record_verdict "$name" 0; restore; return
        fi
    done
    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    if [ "$rc" -eq 0 ]; then
        echo "  FAIL: the register CERTIFIED the evasion: $name"
        ok=0
    elif ! printf '%s\n' "$out" | grep -qF "$need"; then
        echo "  FAIL: went red for the WRONG reason: $name"
        echo "        expected output to contain: $need"
        printf '%s\n' "$out" | grep -E 'FAIL|ANSWERS|DECLARE|PROTOTYPE|DEFINED' | sed 's/^/          /'
        ok=0
    fi
    record_verdict "$name" $ok
    restore
}

# expect_green <files-that-must-have-changed> <name>
expect_green() {
    files="$1"; name="$2"
    for _f in $files; do
        if ! injection_landed "$_f"; then
            echo "  FAIL: BROKEN FIXTURE (not a broken gate): $name"
            echo "        the injection did not change ${_f#"$ROOT"/}, so this control"
            echo "        proved nothing about what the gate tolerates."
            record_verdict "$name" 0; restore; return
        fi
    done
    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL: the register went RED on something it must tolerate: $name"
        printf '%s\n' "$out" | grep -E 'FAIL|ANSWERS|DECLARE|PROTOTYPE|DEFINED' | sed 's/^/          /'
        ok=0
    fi
    record_verdict "$name" $ok
    restore
}

# --------------------------------------------------------- positive control --
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -ne 0 ]; then
    echo "  FAIL: BROKEN BASELINE: the pristine sandbox copy is already RED."
    printf '%s\n' "$out" | tail -30 | sed 's/^/    /'
    echo "  -> every verdict below would be unfounded. Fix the tree or the gate first."
    exit 1
fi
echo "  PASS: positive control -- the pristine sandbox copy is green"
passed=$((passed + 1))

# ------------------------------------------------------------- RED controls --

# THE PROPERTY ITSELF. A new public service that answers from a new file-scope
# object, with no declaration -- the exact shape of sys_ast.c, minted fresh.
{
    echo ''
    echo 'static int ovmx_negctl_shadow_state = 0;'
    echo 'uint32_t sys$negctl_impostor(uint32_t v) {'
    echo '    ovmx_negctl_shadow_state += (int)v;'
    echo '    return (uint32_t)ovmx_negctl_shadow_state;'
    echo '}'
} >> "$EVENT"
expect_red "$EVENT" "an undeclared impostor answering from a new file-scope static" \
    "ANSWERS WITHOUT THE EXECUTIVE AND IS NOT DECLARED: sys\$negctl_impostor"

# THE ANTI-STALE DIRECTION. Declaring a service that DOES reach the executive
# must fail, or the register decays into an allowlist of things already fixed.
printf '/* OVMX-USERSPACE: sys$setef (vms-5b4) -- negctl stale declaration */\n' >> "$EVENT"
expect_red "$EVENT" "a declaration on a service that already reaches the executive" \
    "DECLARED USERSPACE BUT REACHES THE EXECUTIVE: sys\$setef"

# A declaration naming something that is not a service at all: a typo, or the
# leftover of a service that was deleted and whose register line was not.
printf '/* OVMX-USERSPACE: sys$negctl_ghost (vms-5b4) -- negctl orphan */\n' >> "$EVENT"
expect_red "$EVENT" "a declaration naming a service that does not exist" \
    "DECLARES SOMETHING THAT IS NOT A SERVICE: sys\$negctl_ghost"

# The declaration must sit with the implementation. Parked in another file it
# is a central register again, and it rots when the implementation moves.
sed -i 's|^ \* OVMX-USERSPACE: sys\$setast (vms-as1)|  * NEGCTL-MOVED-AWAY: sys$setast (vms-as1)|' "$AST"
printf '/* OVMX-USERSPACE: sys$setast (vms-as1) -- negctl wrong translation unit */\n' >> "$EVENT"
expect_red "$AST $EVENT" "a declaration parked in a translation unit that does not define the service" \
    "DECLARED IN THE WRONG TRANSLATION UNIT: sys\$setast"

# Two declarations for one service: which item is the live one?
printf '/* OVMX-USERSPACE: sys$setast (vms-as1) -- negctl duplicate */\n' >> "$AST"
expect_red "$AST" "one service declared twice" \
    "DECLARED MORE THAN ONCE: sys\$setast"

# The item id is the whole point of "declared against an item". Removing it
# also removes the declaration (an unparseable line declares nothing) -- that
# second red is a consequence of this one, not a separate defect.
sed -i 's|OVMX-USERSPACE: sys\$setast (vms-as1) --|OVMX-USERSPACE: sys$setast --|' "$AST"
expect_red "$AST" "a declaration with no item id" \
    "malformed OVMX-USERSPACE declaration"

# The reason is the register's content: an id alone records that somebody
# noticed, not what answers instead. Same consequential second red as above.
sed -i 's|\(OVMX-USERSPACE: sys\$setast (vms-as1)\) --.*|\1|' "$AST"
expect_red "$AST" "a declaration with an item id but no reason" \
    "malformed OVMX-USERSPACE declaration"

# Deleting the declaration outright must not quietly re-hide the facade.
sed -i '/OVMX-USERSPACE: sys\$setast (vms-as1)/d' "$AST"
expect_red "$AST" "a declaration simply deleted" \
    "ANSWERS WITHOUT THE EXECUTIVE AND IS NOT DECLARED: sys\$setast"

# THE ANTI-SHRINK PROPERTY. Renaming the definition out of the sys$ namespace
# (and taking its declaration with it) removes the service from the DEFINITION
# reading. The prototype half of the union still holds it, and NAMES it.
sed -i 's|^uint32_t sys\$setast(|uint32_t ovmx_negctl_setast(|' "$AST"
sed -i '/OVMX-USERSPACE: sys\$setast (vms-as1)/d' "$AST"
expect_red "$AST" "a service renamed out of the sys\$ namespace to shrink the universe" \
    "PROTOTYPE WITH NO DEFINITION: sys\$setast"

# THE OTHER HALF OF THE UNION. Deleting the PROTOTYPE removes the service from
# the header reading; the definition reading still holds it, so dropping the
# declaration alongside must still be a RED. Without this control the union is
# only proven from the prototype side (the rename above), and the header could
# quietly become the whole universe.
sed -i '/^uint32_t sys\$setast(/d' "$STARLET"
sed -i '/OVMX-USERSPACE: sys\$setast/d' "$AST"
expect_red "$STARLET $AST" "a prototype deleted to shrink the universe, declaration dropped with it" \
    "ANSWERS WITHOUT THE EXECUTIVE AND IS NOT DECLARED: sys\$setast"

# One service, one owner: the register pairs a declaration with the DEFINING
# translation unit, so two definitions leave it with no unambiguous home.
{
    echo ''
    echo 'uint32_t sys$setast(uint32_t enbflg) { (void)enbflg; return 0; }'
} >> "$EVENT"
expect_red "$EVENT" "a second definition of a service in another translation unit" \
    "DEFINED MORE THAN ONCE: sys\$setast"

# ----------------------------------------------------------- GREEN controls --

# THE CLAIM THIS GATE MAKES ABOUT ITS OWN SCOPE. Pure computation added to the
# RTL is not a system service and must not redden the register. Without this,
# "it does not fire on legitimate pure computation" is an untested boast.
{
    echo ''
    echo 'uint32_t str$negctl_pure(uint32_t a, uint32_t b) {'
    echo '    return (a > b) ? (a - b) : (b - a);'
    echo '}'
} >> "$STR"
expect_green "$STR" "pure computation added to the RTL stays green"

# ...and so does a stateful RTL routine. The universe is sys\$, not "anything
# that touches a static": without this control the previous one could be
# passing for the wrong reason (no state) rather than the right one (not a
# system service).
{
    echo ''
    echo 'static int ovmx_negctl_rtl_state = 0;'
    echo 'uint32_t str$negctl_stateful(uint32_t v) {'
    echo '    ovmx_negctl_rtl_state += (int)v;'
    echo '    return (uint32_t)ovmx_negctl_rtl_state;'
    echo '}'
} >> "$STR"
expect_green "$STR" "a stateful RTL routine stays green -- the universe is sys\$, not statefulness"

# A NEW SERVICE THAT REACHES THE EXECUTIVE NEEDS NO DECLARATION. This pins the
# exemption to the executive path rather than to the name: without it, the RED
# controls above would also pass a gate that simply demanded a declaration for
# every new sys$ definition.
{
    echo ''
    echo 'uint32_t sys$negctl_wired(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
expect_green "$EVENT" "a new service that reaches the executive needs no declaration"

# RULE 10'S SECOND ANSWER MUST STAY OPEN. Deleting the service outright --
# definition, prototype and declaration together -- is the honest fix, and the
# register must not stand in its way.
sed -i '/OVMX-USERSPACE: sys\$setast/d' "$AST"
sed -i 's|^uint32_t sys\$setast(uint32_t enbflg) {|static uint32_t ovmx_negctl_removed(uint32_t enbflg) {|' "$AST"
sed -i '/^uint32_t sys\$setast(/d' "$STARLET"
expect_green "$AST $STARLET" "deleting a service outright (definition + prototype + declaration) stays green"

# ---------------------------------------------------------------- coverage --
# "EVERY property has an evasion" is a claim this file used to make in its own
# closing line while the enumeration behind it lived only in the author's head.
# So it is DERIVED instead: every distinct failure message the gate can emit is
# read out of the gate itself, and each one must be named by some control's
# required fragment above. Add a new failure mode to the gate without a control
# and this goes red -- the coverage cannot silently fall behind the gate.
: > "$WORK/derived"
sed -n 's/.*errors\[++nerr\] = "\([A-Z][^"]*: \).*/\1/p' "$GATE" | sort -u >> "$WORK/derived"
grep -oE 'malformed OVMX-USERSPACE declaration' "$GATE" | sort -u >> "$WORK/derived"
: > "$WORK/uncovered"
ncov=0
while IFS= read -r msg; do
    [ -n "$msg" ] || continue
    ncov=$((ncov + 1))
    grep -qF "$msg" "$WORK/needs" || printf '%s\n' "$msg" >> "$WORK/uncovered"
done < "$WORK/derived"

if [ "$ncov" -eq 0 ]; then
    echo "  FAIL: BROKEN COVERAGE CHECK: no failure message was extracted from the gate,"
    echo "        so 'every property has an evasion' would be vacuously true."
    status=1; failed=$((failed + 1))
elif [ -s "$WORK/uncovered" ]; then
    echo "  FAIL: the gate can fail in ways no control here provokes:"
    sed 's/^/    /' "$WORK/uncovered"
    echo "  -> add a control for each, or the closing line below is a boast."
    status=1; failed=$((failed + 1))
else
    echo "  PASS: coverage -- all $ncov failure message(s) the gate can emit are provoked by a control above"
    passed=$((passed + 1))
fi

# ------------------------------------------------------------------ verdict --
echo
echo "  $passed control(s) passed, $failed failed"
if [ "$status" -eq 0 ]; then
    echo "PASS: every property of the userspace service register has an evasion that trips it,"
    echo "      and the things it must tolerate do not trip it."
else
    echo "FAIL: a property of the userspace service register is not actually enforced."
fi
exit $status
