#!/bin/sh
#
# test_userspace_service_register_negctl.sh - the evasions (rd vms-5b4, vms-d89).
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
# THE CONTROL THIS FILE EXISTS FOR NOW IS THE BUY-OFF (vms-d89). The gate used
# to exempt any service that REACHED the executive from declaring anything, and
# that exemption cost ONE IGNORED LINE: `(void)vms_kif_getmode(&x)` at the top
# of sys$gettim -- a declared facade still answering from clock_gettime -- made
# the gate demand the honest declaration be DELETED, and then passed the
# undeclared facade. Both halves of that are controls below: the flip must be
# red, AND the deletion that used to buy green must be red too. A fix that only
# reddens the first half leaves the evasion intact one step further along.
#
# AND THE TWO THAT SURVIVED THAT FIX, both measured end-to-end on the merged
# gate before they were controls (vms-ecf, vms-f26). Both are here because a
# closed hole that nobody re-runs is a claim, not a check:
#
#   - THE PRICE PAID IN A COMMENT. All four checks on an OVMX-EXECUTIVE claim
#     were source greps and the last was `grep -qF "$service" "$proof"`, which
#     a comment satisfies: one ignored vms_kif_readef() in sys$gettim plus the
#     single line `/* also covers sys$gettim */` appended to an untouched proof
#     took both this gate and the kif caller census to rc=0.
#   - THE UNIVERSE DERIVED FROM SOURCE SPELLING. Renaming the definition, the
#     prototype and the declaration together behind `__asm__("sys$gettim")`
#     took the universe from 88 services to 87 with rc=0 and PASS, leaving the
#     object symbol-identical to pristine. Every existing check saw a
#     consistent, smaller tree -- indistinguishable from an honest deletion.
#
# Two of the controls below are therefore about the GATE'S OWN MACHINERY rather
# than about a service: the manifest it prices claims against, and the compiler
# it derives the universe with. A gate that cannot compute its check must say
# so, not certify.
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
# tests/ IS PART OF THE SANDBOX NOW, and not for the harness's convenience: an
# OVMX-EXECUTIVE declaration names a proof file, and the gate checks that the
# file exists, lives under tests/qemu/, forks, and names the service. A sandbox
# without tests/ would redden every executive declaration in the tree for the
# wrong reason and drown every control below.
cp -a "$SRC_ROOT/tests" "$ROOT/tests"
# THE BUILD DESCRIPTION IS PART OF THE SANDBOX (vms-c19). The gate's compile
# set is no longer a directory glob -- it asks cmake what the product compiles,
# so a service can be relocated out of src/ and tools/ and still be found. A
# sandbox with no CMakeLists.txt would take the gate's "this tree describes no
# build" path and the controls for that reading would prove nothing.
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"

AST="$ROOT/src/libvms/syssvc/sys_ast.c"
EVENT="$ROOT/src/libvms/syssvc/sys_event.c"
TIME="$ROOT/src/libvms/syssvc/sys_time.c"
QIO="$ROOT/src/libvms/syssvc/sys_qio.c"
STR="$ROOT/src/libvms/rtl/str_routines.c"
STARLET="$ROOT/src/libvms/include/starlet.h"
PROOF="$ROOT/tests/qemu/test_syssvc_ef_mproc.c"
# A SECOND proof, from a DIFFERENT facility. The price is charged on which code
# a defect edits, so the controls that isolate that half need a service whose
# answer path is the event-flag executive while the proof it names is the lock
# manager's -- no defect can be in both.
LOCKPROOF="$ROOT/tests/qemu/test_syssvc_lock.c"
# The manifest of proven-reddenable assertions. It is MUTABLE because the
# OVMX-EXECUTIVE price is now paid in one of its entries (vms-ecf): the control
# that shows a full exemption CAN still be paid for has to add an entry, and
# the control that shows the price cannot be skipped has to take them away.
FDMAN="$ROOT/tests/qemu/facility_defects.sh"
# The executive dispatch switch. An OVMX-EXECUTIVE claim is priced on which
# EXECUTIVE CODE a proven defect mutates, and the hop from a service to that
# code is the ioctl this file dispatches. If the gate cannot read the bridge it
# must refuse to certify, and that refusal needs a control like anything else.
DISPATCH="$ROOT/src/kernel/vms_module.c"
# A REAL, ALREADY-DECLARED MIXTURE. The round-4 disqualifier is about services
# that are ALREADY OVMX-PARTIAL -- they satisfy every other check before any
# edit is made -- so the control for it cannot use a minted fixture service.
# sys$creprc is the one an adversary measured the buy-off on.
PROCESS="$ROOT/src/libvms/syssvc/sys_process.c"
# The build description, at both levels. The compile set is read out of what
# cmake says it compiles, so the controls that relocate a service out of the
# old glob have to edit the file that SHIPS it, and the control that makes the
# build description unreadable has to edit the top-level one.
TOPCM="$ROOT/CMakeLists.txt"
LIBVMSCM="$ROOT/src/libvms/CMakeLists.txt"
# A translation unit OUTSIDE src/ and tools/ -- the shape of the vms-c19
# escape. It does not exist in the pristine tree, so restore() deletes it
# rather than copying a saved original back.
OUTSIDE="$ROOT/runtime/ovmx_time.c"

MUTABLE="$AST $EVENT $TIME $QIO $STR $STARLET $PROOF $LOCKPROOF $FDMAN $DISPATCH $PROCESS $TOPCM $LIBVMSCM"

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
    rm -rf "$(dirname "$OUTSIDE")"
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
        printf '%s\n' "$out" | grep -E 'FAIL|SAYS NOTHING|DECLARE|PROTOTYPE|DEFINED|LOCAL HALF|PARTIAL|EXECUTIVE DECL' | sed 's/^/          /'
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
        printf '%s\n' "$out" | grep -E 'FAIL|SAYS NOTHING|DECLARE|PROTOTYPE|DEFINED|LOCAL HALF|PARTIAL|EXECUTIVE DECL' | sed 's/^/          /'
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

# WHY EVERY PATTERN BELOW THAT TOUCHES A DECLARATION MATCHES THE SERVICE NAME
# AND NOT THE ITEM ID (vms-fab). Five controls used to aim at sys$gettim's
# declaration with the literal `(vms-5b4)`. vms-fab repointed that declaration
# at vms-642, and every one of those seds became a NO-OP: the mutation never
# landed, the expected red never fired, and the suite went 49/49 -> 42/49 with
# seven controls reporting the gate had stopped enforcing properties it was in
# fact still enforcing. NOTHING WAS WRONG WITH THE GATE. The controls had been
# keyed on a fact that is supposed to change -- which item owns a facade -- so
# repointing a citation, the correct maintenance action, silently disarmed
# them. That is the same failure this whole suite exists to catch, one level
# up, and it is why the id is now read out of the tree (\1) or skipped over
# entirely rather than written down here.
#
# A control that names a service is aimed at something the tree will keep
# spelling the same way. A control that names an item is aimed at a pointer.

# ------------------------------------------------- THE BUY-OFF (vms-d89) --
# One ignored call, exactly as measured on the merged gate. sys$gettim still
# answers from clock_gettime(); nothing about where its answer comes from has
# changed. Under the old rule this flipped it to "exec" and the gate demanded
# its honest OVMX-USERSPACE line be deleted.
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr) {|uint32_t sys$gettim(uint64_t *timadr) {\n    { uint32_t ovmx_negctl_m = 0; (void)vms_kif_getmode(\&ovmx_negctl_m); }|' "$TIME"
expect_red "$TIME" "an ignored vms_kif_* call added to a declared facade (the buy-off, step 1)" \
    "DECLARED WHOLLY USERSPACE BUT REACHES THE EXECUTIVE: sys\$gettim"

# ...AND THE STEP THAT USED TO BUY GREEN. Taking the gate's old advice --
# delete the declaration the "reaches the executive" red complained about --
# must NOT pass. This is the control the whole revision exists for: a fix that
# only reddens step 1 leaves the evasion intact one move later.
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr) {|uint32_t sys$gettim(uint64_t *timadr) {\n    { uint32_t ovmx_negctl_m = 0; (void)vms_kif_getmode(\&ovmx_negctl_m); }|' "$TIME"
sed -i '/OVMX-USERSPACE: sys\$gettim (vms-/d' "$TIME"
expect_red "$TIME" "the buy-off completed: ignored call added AND the honest declaration deleted" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$gettim"

# THE SAME HOLE FROM THE OTHER SIDE. A brand-new service that reaches the
# executive and says nothing at all used to be exempt BY CONSTRUCTION -- this
# control was a GREEN one before vms-d89, asserting the exemption. It is the
# exemption that was wrong, so it is now a RED.
{
    echo ''
    echo 'uint32_t sys$negctl_wired(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
expect_red "$EVENT" "a new service that reaches the executive but declares nothing" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$negctl_wired"

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
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$negctl_impostor"

# THE HEADER-INLINE EVASION, which was a LIVE hole and not a theoretical one:
# on the merged gate this exact injection gave rc=0, PASS, and a universe
# unchanged in size. The whole definition hides in a header, so neither the
# .c-definition reading nor the prototype reading saw it.
{
    echo ''
    echo 'static int ovmx_negctl_hdr_state = 0;'
    echo 'static inline uint32_t sys$negctl_hdrinline(uint32_t v) {'
    echo '    ovmx_negctl_hdr_state += (int)v;'
    echo '    return (uint32_t)ovmx_negctl_hdr_state;'
    echo '}'
} >> "$STARLET"
expect_red "$STARLET" "a service whose entire body hides in a header as static inline" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$negctl_hdrinline"

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
    "malformed OVMX declaration"

# The reason is the register's content: an id alone records that somebody
# noticed, not what answers instead. Same consequential second red as above.
sed -i 's|\(OVMX-USERSPACE: sys\$setast (vms-as1)\) --.*|\1|' "$AST"
expect_red "$AST" "a declaration with an item id but no reason" \
    "malformed OVMX declaration"

# Deleting the declaration outright must not quietly re-hide the facade.
sed -i '/OVMX-USERSPACE: sys\$setast (vms-as1)/d' "$AST"
expect_red "$AST" "a declaration simply deleted" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$setast"

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
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$setast"

# THE ANTI-SHRINK PROPERTY FROM THE SIDE THAT USED TO WORK (vms-f26). The two
# controls above rename the definition OR delete the prototype; each is caught
# because the OTHER source reading still holds the name. Do BOTH IN LOCKSTEP
# and restore the exported symbol with an asm label, and every source reading
# loses the service together: measured on the merged gate, the universe went
# 88 -> 87 services / 87 declared / 86 prototypes with rc=0 and PASS, while the
# object stayed symbol-identical to pristine (nm shows `T sys$gettim` in both)
# and every caller linked unchanged. Nothing downstream could tell.
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr);|uint32_t ovmx_negctl_gettim(uint64_t *timadr) __asm__("sys$gettim");\n#define sys$gettim ovmx_negctl_gettim|' "$STARLET"
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr) {|uint32_t ovmx_negctl_gettim(uint64_t *timadr) {|' "$TIME"
sed -i '/OVMX-USERSPACE: sys\$gettim (vms-/d' "$TIME"
expect_red "$STARLET $TIME" "a service renamed behind an asm label, definition, prototype and declaration together" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$gettim"

# THE SAME ANTI-SHRINK PROPERTY, ESCAPED BY MOVING THE FILE (vms-c19). All
# three members of the universe used to be read from ONE place -- the path glob
# `src/` plus `tools/` -- so a definition RELOCATED out of those two
# directories left every reading at once while CMake went on shipping it.
# Measured on this tree before the fix: universe 88 -> 87, prototypes 87 -> 86,
# exported 88 -> 87, symonly 0, rc=0 and PASS, while `cmake --build --target
# vms` succeeded and `nm -D lib/LIBVMS$SHR.EXE` showed `T sys$gettim`. Every
# caller linked unchanged. The compile set is now asked of the build.
#
# The helper performs the relocation only; each control below decides what to
# do with the prototype, which is the half that separates the two readings.
ovmx_relocate_gettim() {
    mkdir -p "$(dirname "$OUTSIDE")"
    cat > "$OUTSIDE" <<'OUTSIDE_EOF'
#include <stdint.h>
#include <time.h>
#include "starlet.h"
#include "ssdef.h"

uint32_t sys$gettim(uint64_t *timadr) {
    struct timespec ts;
    if (!timadr) return SS$_BADPARAM;
    clock_gettime(CLOCK_REALTIME, &ts);
    *timadr = (uint64_t)ts.tv_sec * 10000000ULL + 0x007C95674BEB4000ULL;
    return SS$_NORMAL;
}
OUTSIDE_EOF
    sed -i '/^uint32_t sys\$gettim(uint64_t \*timadr) {$/,/^}$/d' "$TIME"
    sed -i '/OVMX-USERSPACE: sys\$gettim (vms-/d' "$TIME"
    sed -i 's|^    syssvc/sys_time\.c$|    syssvc/sys_time.c\n    ../../runtime/ovmx_time.c|' "$LIBVMSCM"
}

# THE PATH SCOPING, ISOLATED. Definition relocated, prototype and declaration
# deleted with it -- which under a path-scoped universe is INDISTINGUISHABLE
# from an honest deletion (Rule 10's second answer, green by design). The build
# is the only reading left that can tell the difference, and it can: CMake
# still compiles the file.
ovmx_relocate_gettim
sed -i '/^uint32_t sys\$gettim(uint64_t \*timadr);$/d' "$STARLET"
expect_red "$TIME $LIBVMSCM $STARLET" \
    "a service relocated out of src/ and tools/ while CMake still ships it, prototype deleted with it" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$gettim"

# ...AND THE FOUR-EDIT ESCAPE AS IT WAS MEASURED, which needs no deletion at
# all: the prototype STAYS, spelled with a parenthesized declarator. That one
# construct is the next control's subject; here it is doing the other half of
# the job, keeping the prototype reading quiet while the file move keeps the
# other two quiet.
ovmx_relocate_gettim
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr);$|uint32_t (sys$gettim)(uint64_t *timadr);|' "$STARLET"
expect_red "$TIME $LIBVMSCM $STARLET" \
    "a service relocated out of src/ and tools/ behind a parenthesized declarator (the measured escape)" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$gettim"

# THE PARENTHESIZED DECLARATOR, ISOLATED FROM THE SYMBOL SCAN. `uint32_t
# (sys$foo)(...)` is the same declaration as `uint32_t sys$foo(...)` to every C
# compiler, and read naively it binds the name to `uint32_t`. The two controls
# above would go red on the nm reading alone, so this one puts the whole body
# in a header as `static inline`, which exports NO SYMBOL: nm cannot see it,
# and only the definition reading can. Without the declarator fix the gate is
# green here with its universe unchanged.
{
    echo ''
    echo 'static int ovmx_negctl_paren_state = 0;'
    echo 'static inline uint32_t (sys$negctl_paren)(uint32_t v) {'
    echo '    ovmx_negctl_paren_state += (int)v;'
    echo '    return (uint32_t)ovmx_negctl_paren_state;'
    echo '}'
} >> "$STARLET"
expect_red "$STARLET" \
    "a header-inline service whose name hides inside a parenthesized declarator" \
    "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: sys\$negctl_paren"

# ...and the scan that makes that possible has to be able to say it BROKE. A
# product file that does not compile contributes no symbols, so "it compiled to
# nothing" must not read the same as "it exports nothing".
printf '#error negctl: this file deliberately does not compile\n' >> "$EVENT"
expect_red "$EVENT" "a product source file that does not compile at all" \
    "BROKEN SYMBOL SCAN: "

# One service, one owner: the register pairs a declaration with the DEFINING
# translation unit, so two definitions leave it with no unambiguous home.
{
    echo ''
    echo 'uint32_t sys$setast(uint32_t enbflg) { (void)enbflg; return 0; }'
} >> "$EVENT"
expect_red "$EVENT" "a second definition of a service in another translation unit" \
    "DEFINED MORE THAN ONCE: sys\$setast"

# ------------------------------- the consistency checks on what a human wrote --

# A PARTIAL claims the executive supplies part of the answer. On a service with
# no reachable vms_kif_* entry point at all, that claim contradicts the only
# thing a source scan CAN see.
{
    echo ''
    echo '/* OVMX-PARTIAL: sys$negctl_nothing (vms-5b4) -- exec: nothing, actually */'
    echo '/* OVMX-LOCAL: sys$negctl_nothing -- all of it */'
    echo 'uint32_t sys$negctl_nothing(uint32_t v) { return v + 1; }'
} >> "$EVENT"
expect_red "$EVENT" "a PARTIAL claim on a service that reaches no executive at all" \
    "PARTIAL DECLARATION ON A SERVICE THAT REACHES NOTHING: sys\$negctl_nothing"

# ...and the same contradiction in its strongest form: claiming the WHOLE
# answer comes from the executive while reaching nothing.
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_nothing (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_nothing(uint32_t v) { return v + 1; }'
} >> "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim on a service that reaches no executive at all" \
    "EXECUTIVE DECLARATION ON A SERVICE THAT REACHES NOTHING: sys\$negctl_nothing"

# A mixture is only described when BOTH halves are named. Deleting the local
# half leaves "the executive supplies part of it" with no statement of what
# supplies the rest -- which is the whole content the 20 exempt services were
# missing before vms-d89.
sed -i '/OVMX-LOCAL: sys\$qio --/d' "$QIO"
expect_red "$QIO" "a PARTIAL declaration whose local half was deleted" \
    "PARTIAL DECLARATION WITH NO LOCAL HALF: sys\$qio"

# ...and the reverse: a local half attached to a service that is not declared
# PARTIAL means nothing on its own.
printf '/* OVMX-LOCAL: sys$gettim -- negctl orphan local half */\n' >> "$TIME"
expect_red "$TIME" "a local half with no PARTIAL declaration to be the other half of" \
    "LOCAL HALF WITH NO PARTIAL DECLARATION: sys\$gettim"

# Two local halves: which one is the live statement?
printf '/* OVMX-LOCAL: sys$qio -- negctl duplicate local half */\n' >> "$QIO"
expect_red "$QIO" "one service with two local halves" \
    "LOCAL HALF DECLARED MORE THAN ONCE: sys\$qio"

# Both halves describe one implementation, so both move with it.
sed -i '/OVMX-LOCAL: sys\$qio --/d' "$QIO"
printf '/* OVMX-LOCAL: sys$qio -- negctl local half parked elsewhere */\n' >> "$EVENT"
expect_red "$QIO $EVENT" "a local half parked in a different translation unit from its PARTIAL" \
    "LOCAL HALF IN A DIFFERENT TRANSLATION UNIT: sys\$qio"

# ------------------------------------------- the price of the full exemption --
# OVMX-EXECUTIVE is the ONLY declaration that exempts a service from naming a
# process-local half, so it is the only one worth attacking. Four ways to claim
# it cheaply, four reds.

sed -i 's|proof=tests/qemu/test_syssvc_ef_mproc.c -- one-line$|proof=tests/qemu/test_negctl_nonexistent.c -- one-line|' "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim citing a proof file that does not exist" \
    "EXECUTIVE DECLARATION WHOSE PROOF DOES NOT EXIST: sys\$setef"

# A test that never boots vms.ko cannot testify that an answer came from it.
sed -i 's|proof=tests/qemu/test_syssvc_ef_mproc.c|proof=tests/integration/test_vms_hello.c|g' "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim citing a proof outside the suite that boots the executive" \
    "EXECUTIVE DECLARATION WHOSE PROOF DOES NOT RUN AGAINST THE EXECUTIVE: sys\$setef"

# A single-process test passes perfectly against a per-process fake -- which is
# exactly how the known facades survived. tests/qemu/test_syssvc_ef_local.c is
# a real, passing, executive-resident test that never forks, so this control
# uses a genuine file rather than a straw one.
sed -i 's|proof=tests/qemu/test_syssvc_ef_mproc.c|proof=tests/qemu/test_syssvc_ef_local.c|g' "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim citing a single-process proof" \
    "EXECUTIVE DECLARATION WHOSE PROOF IS SINGLE-PROCESS: sys\$setef"

# A proof that never calls the service is a proof about something else.
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_unnamed (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_unnamed(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim whose proof never calls the service" \
    "EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE: sys\$negctl_unnamed"

# ...AND THE MENTION PAID FOR IN A COMMENT (vms-ecf, round 1). A LIVE evasion
# measured end-to-end on the merged gate: `grep -qF "$pname" "$proof"` is
# satisfied by a comment, so the only full exemption the register grants cost
# ONE IGNORED CALL plus ONE COMMENT LINE in an otherwise untouched proof -- and
# the gate printed PASS with 11 EXECUTIVE claims. sys$gettim answers from
# clock_gettime() before and after. The proof is now read comment-stripped, so
# this dies at the CALLS check.
sed -i 's|^ \* OVMX-USERSPACE: sys\$gettim (\(vms-[0-9a-z.]*\)) -- clock_gettime(CLOCK_REALTIME)| * OVMX-EXECUTIVE: sys$gettim (\1) proof=tests/qemu/test_syssvc_ef_mproc.c -- bought|' "$TIME"
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr) {|uint32_t sys$gettim(uint64_t *timadr) {\n    { uint32_t ovmx_negctl_s = 0; (void)vms_kif_readef(0u, \&ovmx_negctl_s); }|' "$TIME"
printf '/* also covers sys$gettim */\n' >> "$PROOF"
expect_red "$TIME $PROOF" "an EXECUTIVE claim whose proof names it only in a COMMENT (the price paid in a comment)" \
    "EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE: sys\$gettim"

# ...AND THE PRICE PAID BY RE-WORDING AN ASSERTION (vms-ecf, round 2). THE
# CONTROL THIS ROUND EXISTS FOR, and the exact settling command that was
# measured against the round-2 gate: the price then was "name an assertion the
# manifest has proven reddenable that MENTIONS THIS SERVICE", and an assertion
# text is a string living in exactly two places, both editable in one commit.
# Appending ", clock via sys$gettim" to an already-proven assertion in the
# proof AND the manifest, in lockstep, plus one ignored call, paid in full and
# the gate printed "sys$gettim x1" as a paid claim. The price no longer reads
# the wording of anything: it reads which FILE a defect edits.
sed -i 's|^ \* OVMX-USERSPACE: sys\$gettim (\(vms-[0-9a-z.]*\)) -- clock_gettime(CLOCK_REALTIME)| * OVMX-EXECUTIVE: sys$gettim (\1) proof=tests/qemu/test_syssvc_ef_mproc.c -- bought|' "$TIME"
sed -i 's|^uint32_t sys\$gettim(uint64_t \*timadr) {|uint32_t sys$gettim(uint64_t *timadr) {\n    { uint32_t ovmx_negctl_s = 0; (void)vms_kif_readef(0u, \&ovmx_negctl_s); }|' "$TIME"
sed -i 's|child: sys\$ascefc joined the named common cluster|child: sys$ascefc joined the named common cluster, clock via sys$gettim|g' "$PROOF"
sed -i 's|child: sys\$ascefc joined the named common cluster|child: sys$ascefc joined the named common cluster, clock via sys$gettim|g' "$FDMAN"
expect_red "$TIME $PROOF $FDMAN" "an EXECUTIVE claim paid by RE-WORDING a proven assertion to mention the service" \
    "EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE: sys\$gettim"

# ...AND THE UPGRADE THAT COST NOTHING AT ALL (vms-ecf, round 4). THE CONTROL
# THIS ROUND EXISTS FOR, and the exact settling command an adversary measured
# against the round-3 gate. Every check up to here is evidence that the
# executive is REACHED, and an OVMX-PARTIAL service reaches it BY DEFINITION --
# so sys$creprc satisfied all of them before any edit. Flipping its declaration
# block to OVMX-EXECUTIVE and dropping the paired OVMX-LOCAL half -- ONE edit,
# no ignored call, no test file touched, no code change -- printed PASS with an
# extra full exemption. The evidence admitted in payment was creprc-handshake-
# eintr, which edits THIS FILE, pure userspace: evidence that the deleted half
# was true, spent on a claim that it is false.
sed -i '/OVMX-LOCAL: sys\$creprc --/d' "$PROCESS"
sed -i 's|OVMX-PARTIAL: sys\$creprc (\(vms-[0-9a-z.]*\)) -- exec:|OVMX-EXECUTIVE: sys$creprc (\1) proof=tests/qemu/test_syssvc_procnam.c --|' "$PROCESS"
expect_red "$PROCESS" "an already-PARTIAL service upgraded to a full exemption by deleting its own local half" \
    "EXECUTIVE DECLARATION REFUTED BY A DEFECT IN ITS USERSPACE REMAINDER: sys\$creprc"

# ...AND THE SAME MECHANISM ISOLATED, so the red above cannot be passing for
# some property peculiar to sys$creprc. This is the exact PAIR of the "full
# exemption, PAID FOR" green control below: identical claim, identical proof,
# identical paying defect in kernel/vms_eflag.c -- plus ONE fabricated manifest
# entry whose target is the service's OWN defining translation unit. The green
# one passes, this one must not, and the only difference between them is which
# side of the bridge the extra mutation lands on.
cat > "$WORK/negctl_branch" <<'BRANCH_EOF'
    negctl-paid)
        case "$_f" in
        targets)      echo "kernel/vms_eflag.c";;
        require_fail) echo 'negctl: sys$negctl_proven answers from the executive';;
        *)            echo "";;
        esac;;
    negctl-userspace-remainder)
        case "$_f" in
        targets)      echo "libvms/syssvc/sys_event.c";;
        require_fail) echo 'negctl: the userspace half still answers part of it';;
        *)            echo "";;
        esac;;
BRANCH_EOF
sed -i 's|^DEFECTS="|DEFECTS="negctl-paid\nnegctl-userspace-remainder\n|' "$FDMAN"
awk 'NR == FNR { b = b $0 ORS; next }
     { print }
     !ins && /^    case "\$_d" in$/ { printf "%s", b; ins = 1 }' \
    "$WORK/negctl_branch" "$FDMAN" > "$WORK/fdman.new" && mv "$WORK/fdman.new" "$FDMAN"
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_refuted (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_refuted(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
{
    echo 'void ovmx_negctl_refuted_caller(void);'
    echo 'void ovmx_negctl_refuted_caller(void) {'
    echo '    (void)sys$negctl_refuted(0);'
    echo '    printf("  FAIL: negctl: sys$negctl_proven answers from the executive\\n");'
    echo '}'
} >> "$PROOF"
expect_red "$EVENT $PROOF $FDMAN" "a fully paid EXECUTIVE claim refuted by one defect in its own defining translation unit" \
    "EXECUTIVE DECLARATION REFUTED BY A DEFECT IN ITS USERSPACE REMAINDER: sys\$negctl_refuted"

# THE PRICE MUST BE COMPUTABLE, or every EXECUTIVE claim is exempt for want of
# a check. With the manifest of proven-reddenable assertions emptied out, the
# gate has nothing to price against -- and the honest answer is to refuse, not
# to wave the claims through.
: > "$FDMAN"
expect_red "$FDMAN" "the manifest of proven-reddenable assertions emptied out" \
    "BROKEN PRICE CHECK: "

# ...AND THE SAME REFUSAL ONE HOP EARLIER. The price is charged against the
# files a defect edits, and the path from a service to the executive code that
# answers it crosses an ioctl -- read out of the dispatch switch, which is the
# one hop no call graph can walk. With the switch unreadable every answer path
# is empty, which would make EVERY claim unpayable OR (if the gate were written
# the other way round) every claim exempt. Neither is a verdict: a gate that
# cannot compute its check must say so.
sed -i 's|case VMS_IOCTL_|case OVMX_NEGCTL_NOT_AN_IOCTL_|g' "$DISPATCH"
expect_red "$DISPATCH" "the executive dispatch switch made unreadable, so no answer path can be derived" \
    "BROKEN IOCTL BRIDGE: "

# ...AND THE SAME REFUSAL FOR THE BUILD DESCRIPTION (vms-c19). The compile set
# is what CMake says the product compiles, so a tree that describes a build the
# gate cannot read is a tree whose universe cannot be derived. The honest
# answer is to refuse -- NOT to quietly fall back to the directory glob, which
# is the reading the relocation controls above just showed to be escapable.
printf '\nthis_is_not_cmake_syntax(((\n' >> "$TOPCM"
expect_red "$TOPCM" "the build description made unreadable, so what the product compiles is unknown" \
    "BROKEN BUILD-SET SCAN: "

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

# THE FULL EXEMPTION, PAID FOR. This is the control that keeps the reds above
# from being satisfiable by a gate that simply refuses every OVMX-EXECUTIVE
# line: a new service that reaches the executive AND whose named proof exists,
# lives under tests/qemu/, forks, names it, AND holds an assertion that
# tests/qemu/facility_defects.sh names as proven-reddenable, is green.
#
# NOTE WHAT THE MUTATION HAS TO TOUCH, because that IS the price: a defect
# entry whose TARGET FILE lands inside the service answer path -- here
# kernel/vms_eflag.c, which is where the executive handles the ioctl
# vms_kif_readef issues -- plus an assertion of that defect appearing in the
# proof IN CODE, plus the proof actually CALLING the service. This file only
# checks that such an entry exists. What makes an entry real is
# tests/qemu/run_facility_negctl.sh, which injects the defect in QEMU and
# requires the complete set of assertions that go red to EQUAL require_fail +
# knock_on_fail exactly. A fabricated entry naming an assertion no defect
# reddens fails THAT control. This one deliberately fabricates it, to show the
# shape the register accepts -- it is not evidence that the assertion is real.
cat > "$WORK/negctl_branch" <<'BRANCH_EOF'
    negctl-paid)
        case "$_f" in
        targets)      echo "kernel/vms_eflag.c";;
        require_fail) echo 'negctl: sys$negctl_proven answers from the executive';;
        *)            echo "";;
        esac;;
BRANCH_EOF
sed -i 's|^DEFECTS="|DEFECTS="negctl-paid\n|' "$FDMAN"
awk 'NR == FNR { b = b $0 ORS; next }
     { print }
     !ins && /^    case "\$_d" in$/ { printf "%s", b; ins = 1 }' \
    "$WORK/negctl_branch" "$FDMAN" > "$WORK/fdman.new" && mv "$WORK/fdman.new" "$FDMAN"
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_proven (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_proven(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
{
    echo 'void ovmx_negctl_paid_caller(void);'
    echo 'void ovmx_negctl_paid_caller(void) {'
    echo '    (void)sys$negctl_proven(0);'
    echo '    printf("  FAIL: negctl: sys$negctl_proven answers from the executive\\n");'
    echo '}'
} >> "$PROOF"
expect_green "$EVENT $PROOF $FDMAN" "an EXECUTIVE claim whose proof forks, calls it, and holds a proven-reddenable assertion from a defect in its answer path"

# ...and the SAME claim with the manifest entry withheld is a RED. Without this
# the control above proves only that the gate tolerates the shape; it is the
# pair that shows which half is load-bearing.
# A PROOF FROM THE WRONG FACILITY. This service answers through
# vms_kif_readef, so its answer path is sys_event.c plus the executive's
# event-flag file; the proof it names is the LOCK manager's, which forks, calls
# it, and is genuinely reddened by injected defects -- just never by one that
# edits any code this service's answer can come from. Under the round-2 price a
# proof that merely mentioned the service paid; under this one the mutation has
# to land in the right code.
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_unproven (vms-5b4) proof=tests/qemu/test_syssvc_lock.c -- negctl */'
    echo 'uint32_t sys$negctl_unproven(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
{
    echo 'void ovmx_negctl_unproven_caller(void);'
    echo 'void ovmx_negctl_unproven_caller(void) { (void)sys$negctl_unproven(0); }'
} >> "$LOCKPROOF"
expect_red "$EVENT $LOCKPROOF" "an EXECUTIVE claim whose proof is reddened by defects, but none that edits its answer path" \
    "EXECUTIVE DECLARATION NO INJECTED DEFECT IN ITS ANSWER PATH REDDENS: sys\$negctl_unproven"

# ...AND THE SAME ISOLATION WITH THE DEFECT HANDED TO IT. The control above
# leans on the real manifest; this one FABRICATES the most favourable defect an
# adversary could write and it still must not pay: it names a real assertion
# from the named proof (so the text half is satisfied outright, exactly as the
# round-2 price would have accepted) and it edits kernel/vms_lock.c, a genuine
# executive file that genuinely answers that proof -- just not for THIS service,
# whose answer comes through the event-flag path. The only thing separating the
# two is which code the mutation touches, which is the whole point.
cat > "$WORK/negctl_branch" <<'BRANCH_EOF'
    negctl-offpath)
        case "$_f" in
        targets)      echo "kernel/vms_lock.c";;
        require_fail) echo 'child: sys$enq CR+NOQUEUE denied while parent holds EX (public API)';;
        *)            echo "";;
        esac;;
BRANCH_EOF
sed -i 's|^DEFECTS="|DEFECTS="negctl-offpath\n|' "$FDMAN"
awk 'NR == FNR { b = b $0 ORS; next }
     { print }
     !ins && /^    case "\$_d" in$/ { printf "%s", b; ins = 1 }' \
    "$WORK/negctl_branch" "$FDMAN" > "$WORK/fdman.new" && mv "$WORK/fdman.new" "$FDMAN"
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_offpath (vms-5b4) proof=tests/qemu/test_syssvc_lock.c -- negctl */'
    echo 'uint32_t sys$negctl_offpath(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
{
    echo 'void ovmx_negctl_offpath_caller(void);'
    echo 'void ovmx_negctl_offpath_caller(void) { (void)sys$negctl_offpath(0); }'
} >> "$LOCKPROOF"
expect_red "$EVENT $LOCKPROOF $FDMAN" "an EXECUTIVE claim paid by a fabricated defect that reddens the proof but edits nothing in the answer path" \
    "EXECUTIVE DECLARATION NO INJECTED DEFECT IN ITS ANSWER PATH REDDENS: sys\$negctl_offpath"

# ...and a mixture that names both halves is green too, so the PARTIAL reds
# above are not satisfiable by a gate that rejects every PARTIAL line.
{
    echo ''
    echo '/* OVMX-PARTIAL: sys$negctl_mixed (vms-5b4) -- exec: the flag word */'
    echo '/* OVMX-LOCAL: sys$negctl_mixed -- the counter added to it */'
    echo 'static int ovmx_negctl_mixed_state = 0;'
    echo 'uint32_t sys$negctl_mixed(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st + (uint32_t)(ovmx_negctl_mixed_state++);'
    echo '}'
} >> "$EVENT"
expect_green "$EVENT" "a mixture that names both of its halves stays green"

# THE BUILD-DERIVED COMPILE SET MUST NOT OVER-FIRE. The three relocation reds
# above are all satisfiable by a gate that simply reddens anything outside
# src/ and tools/, which would be a worse gate, not a better one: the point of
# reading the build is that a translation unit is judged by whether the product
# COMPILES it, not by where it sits. So the same relocation, done HONESTLY --
# declaration moved with the implementation, prototype left alone -- is green.
ovmx_relocate_gettim
printf '/* OVMX-USERSPACE: sys$gettim (vms-642) -- clock_gettime(CLOCK_REALTIME) here */\n' >> "$OUTSIDE"
expect_green "$TIME $LIBVMSCM" \
    "a service honestly relocated outside src/ and tools/, its declaration moved with it"

# RULE 10'S SECOND ANSWER MUST STAY OPEN. Deleting the service outright --
# definition, prototype and declaration together -- is the honest fix, and the
# register must not stand in its way.
sed -i '/OVMX-USERSPACE: sys\$setast/d' "$AST"
sed -i 's|^uint32_t sys\$setast(uint32_t enbflg) {|static uint32_t ovmx_negctl_removed(uint32_t enbflg) {|' "$AST"
sed -i '/^uint32_t sys\$setast(/d' "$STARLET"
expect_green "$AST $STARLET" "deleting a service outright (definition + prototype + declaration) stays green"

# --------------------------------------------------------- structural guards --
# Three of the gate's FAIL paths are not properties of an individual service:
# they are the gate refusing to certify anything because it could not read
# the tree, or because the tree it read holds no sys$ services at all. Each
# needs its own throwaway fixture -- removing src/ or emptying it out of
# $ROOT would invalidate every RED/GREEN control above, which depend on it
# staying a real copy of the product tree.
guard() {
    fixture="$1"; name="$2"; need="$3"
    printf '%s\n' "$need" >> "$WORK/needs"
    out=$(sh "$GATE" "$fixture" 2>&1)
    rc=$?
    ok=1
    if [ "$rc" -eq 0 ]; then
        echo "  FAIL: the register CERTIFIED the evasion: $name"
        ok=0
    elif ! printf '%s\n' "$out" | grep -qF "$need"; then
        echo "  FAIL: went red for the WRONG reason: $name"
        echo "        expected output to contain: $need"
        ok=0
    fi
    record_verdict "$name" $ok
}

NOSRC="$WORK/fixture-no-src"
mkdir -p "$NOSRC"
guard "$NOSRC" "a tree with no src/ directory at all" \
    "this gate scans the product tree."

NOFACTS="$WORK/fixture-empty-src"
mkdir -p "$NOFACTS/src" "$NOFACTS/tools"
guard "$NOFACTS" "a tree with a src/ directory but zero .c files anywhere" \
    "the source scan produced no facts at all."

# THE FLOOR. C code exists (so the "no facts" guard above does not fire) but
# none of it is a sys$ service -- every property the gate checks would pass
# vacuously because the universe it checks them against is empty.
NOSYS="$WORK/fixture-no-sys-services"
mkdir -p "$NOSYS/src" "$NOSYS/tools"
printf 'int ovmx_negctl_not_a_service(void) { return 0; }\n' > "$NOSYS/src/plain.c"
guard "$NOSYS" "a tree with C code but zero sys\$ services in it" \
    "THE FLOOR: zero sys\$ services found in the product compile set."

# ---------------------------------------------------------------- coverage --
# "EVERY property has an evasion" is a claim this file used to make in its own
# closing line while the enumeration behind it lived only in the author's head.
# So it is DERIVED instead: every distinct failure message the gate can emit is
# read out of the gate itself, and each one must be named by some control's
# required fragment above. Add a new failure mode to the gate without a control
# and this goes red -- the coverage cannot silently fall behind the gate.
: > "$WORK/derived"
sed -n 's/.*errors\[++nerr\] = "\([A-Z][^"]*: \).*/\1/p' "$GATE" | sort -u >> "$WORK/derived"
# The gate emits the OVMX-EXECUTIVE proof failures from shell printf rather
# than from the awk errors[] array, because only the shell can stat and grep
# the cited file. Pulled from the gate's own literal text the same way, so a
# renamed message desyncs this check instead of slipping past it.
sed -n "s/.*printf '\([A-Z][^:]*: \)%s.*/\1/p" "$GATE" | sort -u >> "$WORK/derived"
grep -oE 'malformed OVMX declaration' "$GATE" | sort -u >> "$WORK/derived"
# The errors[] array, the printf paths and decl_bad cover the PER-SERVICE
# properties; the gate also has three FAIL paths that are not about any one
# service -- it refusing to run at all, and the floor.
grep -oE 'this gate scans the product tree\.' "$GATE" | sort -u >> "$WORK/derived"
grep -oE 'the source scan produced no facts at all\.' "$GATE" | sort -u >> "$WORK/derived"
# The exported-symbol scan's own broken-scan path (vms-f26). Like the two
# above it is an echo, not an errors[] entry, because it aborts the run rather
# than accusing a service.
#
# STATED PLAINLY BECAUSE THE PREFIX IS SHARED: the gate spells "BROKEN SYMBOL
# SCAN: " twice -- once when a product file will not compile (provoked by a
# control above) and once when there is no compiler or no nm at all. This
# extraction cannot tell them apart, so the coverage PASS below covers the
# first and NOT the second. That is deliberate: "no compiler" is a
# PREREQUISITE failure, the same class as this file's own `command -v cmp`
# check, and no mutation of the TREE provokes it.
grep -oE 'BROKEN SYMBOL SCAN: ' "$GATE" | sort -u >> "$WORK/derived"
# The ioctl bridge's own broken-scan path (vms-ecf round 3), same shape: an
# echo that aborts the run rather than accusing a service.
grep -oE 'BROKEN IOCTL BRIDGE: ' "$GATE" | sort -u >> "$WORK/derived"
# The build-set scan's own refusal (vms-c19), same shape again. THE PREFIX IS
# SHARED THREE WAYS, exactly as "BROKEN SYMBOL SCAN: " is shared two ways: the
# gate spells it for an unreadable configure (provoked by a control above), for
# a compile_commands.json it cannot parse, and for cmake being absent. This
# extraction cannot tell them apart, so the coverage PASS below covers the
# first only. "cmake is absent" is a PREREQUISITE failure that no mutation of
# the TREE provokes; the unparseable-json path is provoked by nothing here and
# is named as an open gap rather than claimed.
grep -oE 'BROKEN BUILD-SET SCAN: ' "$GATE" | sort -u >> "$WORK/derived"
# The gate's own source has to spell this one "sys\$" (escaped for the shell
# double-quote it lives in); what actually prints at runtime is "sys$", with
# no backslash. Normalize the same way so this line matches the fixture's
# real output below instead of failing on a backslash that only exists on
# disk.
grep -oF 'THE FLOOR: zero sys\$ services found in the product compile set.' "$GATE" \
    | sed 's/\\\$/$/' | sort -u >> "$WORK/derived"
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
