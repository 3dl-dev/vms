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

AST="$ROOT/src/libvms/syssvc/sys_ast.c"
EVENT="$ROOT/src/libvms/syssvc/sys_event.c"
TIME="$ROOT/src/libvms/syssvc/sys_time.c"
QIO="$ROOT/src/libvms/syssvc/sys_qio.c"
STR="$ROOT/src/libvms/rtl/str_routines.c"
STARLET="$ROOT/src/libvms/include/starlet.h"
PROOF="$ROOT/tests/qemu/test_syssvc_ef_mproc.c"

MUTABLE="$AST $EVENT $TIME $QIO $STR $STARLET $PROOF"

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
sed -i '/OVMX-USERSPACE: sys\$gettim (vms-5b4)/d' "$TIME"
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

# A proof that never mentions the service is a proof about something else.
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_unnamed (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_unnamed(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
expect_red "$EVENT" "an EXECUTIVE claim whose proof never names the service" \
    "EXECUTIVE DECLARATION WHOSE PROOF DOES NOT NAME THE SERVICE: sys\$negctl_unnamed"

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

# THE FULL EXEMPTION, PAID FOR. This is the control that keeps the four reds
# above from being satisfiable by a gate that simply refuses every
# OVMX-EXECUTIVE line: a new service that reaches the executive AND whose named
# proof exists, lives under tests/qemu/, forks, and names it, is green. Note
# what the mutation has to touch -- the proof file itself. That is the price.
{
    echo ''
    echo '/* OVMX-EXECUTIVE: sys$negctl_proven (vms-5b4) proof=tests/qemu/test_syssvc_ef_mproc.c -- negctl */'
    echo 'uint32_t sys$negctl_proven(uint32_t efn) {'
    echo '    uint32_t st = 0;'
    echo '    (void)vms_kif_readef(efn, &st);'
    echo '    return st;'
    echo '}'
} >> "$EVENT"
printf '/* negctl: this proof names sys$negctl_proven */\n' >> "$PROOF"
expect_green "$EVENT $PROOF" "an EXECUTIVE claim whose proof exists, boots the executive, forks and names it"

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
    "THE FLOOR: zero sys\$ services found under src/ and tools/."

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
# The gate's own source has to spell this one "sys\$" (escaped for the shell
# double-quote it lives in); what actually prints at runtime is "sys$", with
# no backslash. Normalize the same way so this line matches the fixture's
# real output below instead of failing on a backslash that only exists on
# disk.
grep -oF 'THE FLOOR: zero sys\$ services found under src/ and tools/.' "$GATE" \
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
