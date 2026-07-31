#!/bin/sh
#
# test_kif_caller_census.sh - standing gate (rd vms-7fb): every kernel-interface
# entry point is either REACHED FROM THE PRODUCT or DECLARED UNWIRED against an
# item. Nothing ships a wrapper that nothing calls.
#
# WHY THIS GATE EXISTS. The vms-14f Phase 2 veracity pass ran a comment-stripped
# caller census over src/libvmssys/vms_kif.h and found that most of the executive
# interface had NO PRODUCT PATH AT ALL: a kernel facility, a userspace wrapper and
# a test suite had been merged, and no line of OVMX ever called any of it. Four
# separate interfaces shipped that way and were therefore never once exercised:
#
#   1. vms_kif_register() itself (vms-9fc). Every /dev/vms ioctl in the product
#      failed -ESRCH -- INCLUDING through src/libvms/syssvc/sys_lock.c, the file
#      the executive-retrofit design named as "the one facility already wired"
#      and told every other implementer to copy.
#   2. The prvdef privilege agreement lock (vms-2b8), wrapped in an #ifdef nothing
#      compiled -- so the #error inside it never fired and a deliberately wrong
#      constant still built clean.
#   3. vms_kif_setident (vms-2b8), the one wrapper that issued its ioctl directly
#      instead of through KIF_CALL, so it never bound. SETIDENT is by definition
#      the FIRST executive call LOGINOUT makes: every identity establishment in
#      the product would have failed.
#   4. The whole event-flag family (vms-2a8). sys$ascefc's body was
#      `return SS$_NORMAL;` with a TODO, and in its public-API suite THE ONLY TWO
#      ASSERTIONS THAT PASSED WERE THE TWO FABRICATED SUCCESSES.
#
# A caller census catches all four, at build time, before merge.
#
# WHY NO EXISTING GATE CATCHES THIS CLASS. tests/integration/test_runtime_target.sh
# hunts text patterns near the string "/dev/vms" -- and A FACILITY IMPLEMENTED
# WHOLLY IN USERSPACE NEVER MENTIONS IT. The Rule 9 gate is shaped to catch a
# FALLBACK; this one is shaped to catch an ABSENCE.
#
# WHAT COUNTS AS A CALLER, and every clause here is load-bearing:
#
#   - COMMENT-STRIPPED. A mention in a comment is NOT a caller. That is exactly
#     how vms_kif_devscan and vms_kif_setident looked wired: the only occurrences
#     outside vms_kif.c were a comment at src/vmsdcl/dcl_cmd_show.c saying the
#     conversion was future work, and a comment in src/ovmx_init/ovmx_init.c
#     describing the call it would make one day.
#   - PRODUCT ONLY: src/ and tools/. A caller in tests/ is not a product path --
#     "kernel facility + wrapper + test suite, and nothing else" is the precise
#     shape of the defect, so a test caller must not be able to satisfy the gate.
#   - REACHABILITY, not mere presence. Calls inside vms_kif.c count only when the
#     calling function is itself reachable from a caller outside vms_kif.c. That
#     is how vms_kif_open/register/kerr_to_ss legitimately pass: nothing outside
#     names them, but kif_bind() does, and kif_bind() is reached from every wired
#     wrapper through KIF_CALL -> kif_call. A family that only calls itself is
#     NOT reachable and does not pass.
#
# THE ESCAPE HATCH, and its price. An entry point with no product path must be
# declared in src/libvmssys/vms_kif.h with a line of the form
#
#     OVMX-UNWIRED: vms_kif_foo (vms-abc) -- one line on why
#
# The item id is REQUIRED: "deliberately unwired" as free text with nothing
# tracking it is how this state persisted through two merged items. The
# declaration is also checked in the other direction -- declaring an entry point
# that DOES have a caller fails the gate, so wiring a facility forces its
# declaration to be deleted in the same commit and the census can never drift
# into a stale allowlist.
#
# The gate does NOT verify that the item is open (rd is nostr-backed and not
# reachable from CI). It verifies that a human wrote an id down.
#
# WHY THE UNIVERSE IS PINNED, and this is the property that makes the escape
# hatch safe. An earlier revision derived the list of entry points by grepping
# vms_kif.h ALONE -- so DELETING A PROTOTYPE SILENTLY SHRANK THE UNIVERSE, and
# the census reported a smaller PASS instead of a RED. A gate that can be
# disarmed by removing the thing it counts is the same family as the defects
# that made this dispatch necessary: a guard compiled nowhere, a fixture that
# silently no-ops, an assertion satisfiable by something else. So the universe
# is read from the tree TWICE, independently, and a third reading pins the floor:
#
#   - THE UNION, not the header. The universe is every vms_kif_* function the
#     header PROTOTYPES, UNION EVERY FILE-SCOPE FUNCTION vms_kif.c DEFINES --
#     including static helpers, so that marking a definition static cannot drop
#     it out either. Deleting a prototype therefore does not shrink the universe
#     by one; the definition still holds the entry point in it, and the entry
#     point still has to be wired or declared.
#     THE DEFINITION READING IS NOT FILTERED ON THE vms_kif_ PREFIX, and that is
#     load-bearing rather than tidy. When both readings were filtered on the
#     prefix, ONE name filter gated BOTH halves of the union, so a RENAME left
#     the census through both doors at once: `sed -i s/vms_kif_devscan/
#     kif_devscan_impl/` across the header and the source, plus deleting the now
#     stale declaration, took the census from 38 entry points to 37 and PASSED,
#     with the wrapper still in the tree, still unwired, still compiled. That is
#     the deleted-prototype defect reached by a different one-line edit. So the
#     universe on the definition side is EVERY file-scope definition in
#     vms_kif.c: that file IS the interface translation unit, and a function
#     defined in it does not stop being part of the interface by being renamed
#     out of the namespace. The three static helpers this admits -- kif_bind,
#     kif_call, getjpi_common -- are all reached from wired wrappers and need no
#     declaration.
#     THE PRICE, stated rather than hidden: renaming an externally-linked entry
#     point out of vms_kif_ is a RED even when it stays wired, because the header
#     reading is still namespaced and the definition then has no prototype the
#     census can see. That is a deliberate lint on the interface's naming
#     convention, not a bug. If the convention ever changes, teach both readings
#     -- do NOT reintroduce a prefix filter on the definition side, which is the
#     hole this closes.
#   - THE TWO READINGS MUST AGREE. A definition with no prototype, or a
#     prototype with no definition, is itself a RED that NAMES what vanished.
#     (A static helper is exempt from needing a prototype -- that is what static
#     means -- but the union above still counts it.)
#   - THE KERNEL FLOOR, IN TWO GRAINS. Deleting a prototype AND its definition
#     AND its declaration would shrink the universe honestly -- except that it
#     strands the kernel handler behind it. So the kernel side is read too, and
#     both readings are derived from the tree rather than written down here:
#
#       * every VMS_IOCTL_* opcode defined in src/kernel/vms_ioctl.h must be
#         issued by at least one wrapper in vms_kif.c;
#       * every VMS_*_SEL_* selector defined there must be named by one too.
#
#     The selector grain is not decoration. An opcode floor alone only catches
#     the SOLE ISSUER of an opcode, and vms_ioctl_getdvi() dispatches on
#     args.select: deleting vms_kif_getdvi_chan outright -- prototype,
#     definition and declaration -- left VMS_IOCTL_GETDVI still issued by
#     vms_kif_getdvi_devnam, so an opcode-only floor certified it green while
#     the VMS_DVI_SEL_CHAN path inside the kernel handler became unreachable
#     from userspace. Where a selector names a distinct path through one opcode,
#     it is an entry point in everything but name, and the floor counts it.
#     The exact reach of the two grains together, MEASURED rather than asserted
#     -- by deleting each of the 41 file-scope definitions in vms_kif.c in turn,
#     with its prototype and its declaration, and running this gate:
#
#       37 of 41 go RED. FOUR are a silent PASS: vms_kif_open, vms_kif_close,
#       vms_kif_kerr_to_ss and the static vms_kif_alloc_op.
#
#     Those four are exactly the definitions whose bodies issue no VMS_IOCTL_*
#     opcode and name no VMS_*_SEL_* selector -- open/close manage the fd,
#     kerr_to_ss maps an errno, and alloc_op takes its opcode as a PARAMETER --
#     so there is nothing on the kernel side for their removal to strand. The
#     claim this floor can support is therefore the narrow one, and it is the
#     one to quote: NO WRAPPER THAT ISSUES AN OPCODE OR NAMES A SELECTOR can
#     have its definition deleted without a RED. It is NOT "no wrapper".
#     (An earlier revision of this comment said "no wrapper", and the very next
#     paragraph contradicted it. Execution settled it. In the one file whose
#     declared subject is untested assertions, do not write an emphatic claim
#     here you have not run.)
#     The residual risk is low but it is REAL, not zero: all four are called
#     today, so deleting any of them fails to compile rather than shipping.
#     There is deliberately NO escape hatch for an orphaned opcode or selector.
#     If you add an ioctl or a selector to vms.ko, land its wrapper in the same
#     commit.
#
# WHAT THIS GATE DOES NOT SEE, stated so its PASS is never read as more than it
# is. It is a SOURCE SCAN, not a build and not an execution:
#
#   - A call inside a preprocessor block that never compiles counts as a caller.
#     That is not hypothetical -- vms-2b8's prvdef agreement lock was wrapped in
#     an #ifdef nothing compiled, so an #error inside it never fired. Deciding
#     which #ifdef is live needs the build configuration, and a census that
#     GUESSED at it would be inventing a plausible answer. There is no #if 0 in
#     src/ or tools/ today; if one appears around a vms_kif call, this gate will
#     be satisfied by it and a human has to catch it.
#   - A caller in a function nothing calls still counts. The census answers "is
#     there a product path", not "is that path executed".
#   - It says NOTHING about whether the facility behind the call is real. An
#     entry point can be wired to a per-process fake and pass here. That is the
#     A-writes/B-reads question (CLAUDE.md Rule 11) and it belongs to the QEMU
#     suites and the veracity passes, not to a grep.
#   - The kernel floor counts a MENTION of VMS_IOCTL_* / VMS_*_SEL_* in
#     vms_kif.c, not a proof that the value reaches an ioctl. A bare
#     `(void)VMS_IOCTL_DEVSCAN;` would satisfy it. Deciding that a value
#     actually flows to KIF_CALL is data flow, not a scan, and a census that
#     guessed would be inventing an answer.
#   - THE FLOOR'S GRAIN IS THE OPCODE AND THE SELECTOR, AND NOTHING FINER. It
#     makes deleting a wrapper outright a RED whenever that wrapper is the last
#     userspace mention of an opcode or of a selector. It is NOT a general proof
#     that a wrapper cannot vanish, in two measured ways:
#       * A definition that issues NO opcode and names NO selector strands
#         nothing, so deleting it is a silent PASS. Four exist today
#         (vms_kif_open, vms_kif_close, vms_kif_kerr_to_ss, vms_kif_alloc_op);
#         see the floor section above for the brute-force result.
#       * If two wrappers ever share an opcode AND its selector, deleting one of
#         them strands nothing either. No such pair exists in the tree right now.
#     The census output prints both counts so the slack becomes visible if one
#     appears. This is a stated boundary rather than a claim: do not read a PASS
#     as "no wrapper was deleted".
#   - A PRODUCT CALLER IS CREDITED BY NAME, not by resolved linkage. The seedable
#     set (1' below) removes the case where that is semantically impossible -- a
#     static defined only in vms_kif.c -- but for an EXTERNALLY-LINKED name the
#     census still credits any same-named call in src/ or tools/. The namespaced
#     readings are what make that hard to abuse rather than free: a name is
#     externally linked here only if it is prototyped in vms_kif.h (which greps
#     for vms_kif_*) or defined non-static in vms_kif.c, and a non-static
#     definition with no vms_kif_ prototype is already a RED at 1a. So collision
#     requires a product function actually named vms_kif_something, which is a
#     duplicate-symbol problem of its own. Verified: the extern form of the
#     control-20 evasion is caught at 1a.
#
# If you are here because this failed: do NOT add a declaration to make it pass
# unless the entry point genuinely has no product path yet AND you have an item
# for it. The declaration says "this facility is not wired" out loud. Adding one
# for something you just shipped a caller-less wrapper for is the defect, spelled.
#
# Usage: test_kif_caller_census.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
KIF_H="$SRC_ROOT/src/libvmssys/vms_kif.h"
KIF_C="$SRC_ROOT/src/libvmssys/vms_kif.c"
IOCTL_H="$SRC_ROOT/src/kernel/vms_ioctl.h"
status=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms_kif caller census: every entry point is wired or declared unwired"

if [ ! -f "$KIF_H" ] || [ ! -f "$KIF_C" ]; then
    echo "FAIL: cannot find the kernel interface ($KIF_H / $KIF_C)"
    echo "  -> if it moved, move this gate with it; do not delete the census."
    exit 1
fi

# ---------------------------------------------------------------------------
# strip_comments: remove /* */ and // comments from stdin.
#
# Same reader as tests/integration/test_runtime_target.sh. Copied rather than
# shared because a gate that depends on another gate's internals can be disarmed
# by editing the other file; each gate reads the tree with its own eyes.
# ---------------------------------------------------------------------------
strip_comments() {
    awk '
        {
            line = $0; out = ""; i = 1
            while (i <= length(line)) {
                if (inblk) {
                    p = index(substr(line, i), "*/")
                    if (p == 0) { i = length(line) + 1 } else { inblk = 0; i = i + p + 1 }
                    continue
                }
                two = substr(line, i, 2)
                if (two == "/*") { inblk = 1; i += 2; out = out " "; continue }
                if (two == "//") { break }
                out = out substr(line, i, 1); i++
            }
            print out
        }
    '
}

# ---------------------------------------------------------------------------
# call_edges [calls|defs]: read comment-stripped C on stdin.
#
#   calls (default) - print "ENCLOSING<TAB>CALLEE" for every call expression.
#   defs            - print "static|extern<TAB>NAME" for every function
#                     DEFINITION at file scope. Definitions, not prototypes:
#                     the same depth-0 rule that stops a prototype counting as
#                     a call is what distinguishes them, so both readings of
#                     the tree come from one reader and cannot disagree about
#                     what a definition is.
#
# It is a character-level reader, not a token search, for three reasons:
#   - a call at brace depth 0 is a PROTOTYPE or a DEFINITION, not a call, so the
#     definition of vms_kif_setef must not count as a caller of itself;
#   - string and character literals are skipped, so a brace inside "{" cannot
#     desynchronise the depth counter;
#   - the enclosing function has to be known for the reachability pass below,
#     and function-like MACROS are nodes too -- KIF_CALL is the only thing that
#     names kif_call(), so a reader blind to macro bodies would conclude the
#     bind path is dead and mark every entry point unwired.
# ---------------------------------------------------------------------------
call_edges() {
    awk -v want="${1:-calls}" '
        function scan(s, node, ismac,    n, i, j, k, c, id) {
            n = length(s); i = 1
            while (i <= n) {
                c = substr(s, i, 1)
                if (c == "\"" || c == "'"'"'") {
                    q = c; i++
                    while (i <= n) {
                        if (substr(s, i, 1) == "\\") { i += 2; continue }
                        if (substr(s, i, 1) == q) { i++; break }
                        i++
                    }
                    continue
                }
                if (!ismac && c == "{") {
                    if (depth == 0) {
                        curfn = pending; pending = ""
                        # A body opening at depth 0 behind a name that was
                        # followed by "(" is a DEFINITION. A prototype never
                        # gets here: its ";" clears pending below.
                        if (want == "defs" && curfn != "")
                            print (sawstatic ? "static" : "extern") "\t" curfn
                        sawstatic = 0
                    }
                    depth++; i++; continue
                }
                if (!ismac && c == "}") {
                    depth--
                    if (depth <= 0) { depth = 0; curfn = ""; sawstatic = 0 }
                    i++; continue
                }
                if (!ismac && c == ";" && depth == 0) {
                    pending = ""; sawstatic = 0; i++; continue
                }
                if (c ~ /[A-Za-z_]/) {
                    j = i
                    while (j <= n && substr(s, j, 1) ~ /[A-Za-z0-9_]/) j++
                    id = substr(s, i, j - i)
                    k = j
                    while (k <= n && (substr(s, k, 1) == " " || substr(s, k, 1) == "\t")) k++
                    if (substr(s, k, 1) == "(") {
                        if (ismac) { if (want == "calls") print node "\t" id }
                        else if (depth >= 1) { if (want == "calls") print curfn "\t" id }
                        else pending = id
                    } else if (!ismac && depth == 0 && id == "static") {
                        sawstatic = 1
                    }
                    i = j; continue
                }
                i++
            }
        }
        BEGIN { depth = 0; pending = ""; curfn = ""; inmac = 0; macnode = ""; sawstatic = 0 }
        {
            line = $0
            if (inmac) {
                scan(line, macnode, 1)
                if (line !~ /\\[ \t]*$/) inmac = 0
                next
            }
            if (line ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*\(/) {
                macnode = line
                sub(/^[ \t]*#[ \t]*define[ \t]+/, "", macnode)
                sub(/\(.*$/, "", macnode)
                rest = line
                sub(/^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*/, "", rest)
                scan(rest, macnode, 1)
                if (line ~ /\\[ \t]*$/) inmac = 1
                next
            }
            if (line ~ /^[ \t]*#/) next
            scan(line, "", 0)
        }
    '
}

# ---------------------------------------------------------------------------
# 1. The universe, derived from the tree at check time and PINNED.
#
# Never a hardcoded list: main moves under this gate constantly, and a list is
# what the census exists to replace. But "derived from the header" alone let a
# deleted prototype shrink the universe silently -- so the universe is the UNION
# of two independent readings, and their disagreement is itself a RED.
# ---------------------------------------------------------------------------
strip_comments < "$KIF_H" \
    | grep -oE 'vms_kif_[A-Za-z0-9_]+[ \t]*\(' \
    | sed -E 's/[ \t]*\($//' | sort -u > "$WORK/protos"

strip_comments < "$KIF_C" | call_edges defs | sort -u > "$WORK/defs_all"
cut -f2 "$WORK/defs_all" | sort -u > "$WORK/defs"
awk -F'\t' '$1 == "extern" { print $2 }' "$WORK/defs_all" | sort -u > "$WORK/defs_extern"

sort -u "$WORK/protos" "$WORK/defs" > "$WORK/universe"

# 1'. THE SEEDABLE SET: the universe MINUS the names that only C's linkage rules
#     say are unreachable from outside this file.
#
# The universe is what must be ACCOUNTED FOR. The seedable set is what a product
# file is allowed to VOUCH FOR, and they are not the same list. A name defined
# `static` in vms_kif.c and prototyped nowhere cannot, by the meaning of `static`,
# be called from another translation unit: an identical name in a product file is
# a DIFFERENT function that merely spells the same. Crediting it would let an
# unrelated product call mark an unwired entry point as REACHED -- and that is
# strictly worse than the shrinks this gate already blocks, because the census
# would not merely LOSE the wrapper, it would AFFIRMATIVELY CERTIFY it as wired
# and move the reached count UP to do it.
#
# NOT HYPOTHETICAL, and it was a hole this gate opened itself. When the seeding
# filter was widened from `^vms_kif_` to the whole universe, three mechanical
# edits -- delete the OVMX-UNWIRED line, delete the prototype, and rename the
# definition to `static uint32_t lnm_init(` -- took the census to 41 entry points
# / 15 REACHED / PASS, crediting the entirely unrelated lnm_init() call in
# src/vmslnm/lnm_client.c. The wrapper was still in the tree, still unwired,
# still compiled. Negative control 20 is that evasion.
#
# The widening itself was right for EXTERNALLY-LINKED names -- an entry point
# renamed out of the vms_kif_ namespace but genuinely called from the product
# must still count as wired, or the gate would demand a false declaration. It was
# only ever wrong for statics, where it is not a trade-off but a bug. So the
# seeding list drops exactly the names for which an outside caller is
# semantically impossible, and nothing else.
#
# A static is NOT thereby excused: it stays in the universe and it can still be
# REACHED -- but only through the in-file reachability pass in step 3, which is
# the only way a static can be reached in C. That is how kif_bind, kif_call and
# getjpi_common pass today.
awk -F'\t' '$1 == "static" { print $2 }' "$WORK/defs_all" | sort -u > "$WORK/statics"
sort -u "$WORK/protos" "$WORK/defs_extern" > "$WORK/linkable"
comm -23 "$WORK/statics" "$WORK/linkable" > "$WORK/static_only"
comm -23 "$WORK/universe" "$WORK/static_only" > "$WORK/seedable"

ENTRIES=$(cat "$WORK/universe")
n_entries=$(grep -c . "$WORK/universe" || true)
n_protos=$(grep -c . "$WORK/protos" || true)
n_defs=$(grep -c . "$WORK/defs" || true)

if [ "$n_protos" -eq 0 ] || [ "$n_defs" -eq 0 ]; then
    echo "FAIL: one of the two readings of the interface came back empty"
    echo "        $n_protos prototype(s) in $(basename "$KIF_H")"
    echo "        $n_defs definition(s) in $(basename "$KIF_C")"
    echo "  -> the census reader is broken, or the interface moved. Fix the gate;"
    echo "     an empty universe is a vacuous PASS, which is the whole defect."
    exit 1
fi

# 1a. An externally-linked definition with no vms_kif_ prototype. This is what a
#     deleted prototype looks like from the other side, AND what a rename out of
#     the namespace looks like: either way the entry point is still counted,
#     because the definition reading is unfiltered, and this names what vanished.
orphan_defs=$(comm -13 "$WORK/protos" "$WORK/defs_extern")
if [ -n "$orphan_defs" ]; then
    echo "FAIL: defined in $(basename "$KIF_C") with NO prototype in $(basename "$KIF_H"):"
    printf '%s\n' "$orphan_defs" | sed 's/^/    /'
    echo "  -> the prototype vanished, or the entry point was RENAMED out of the"
    echo "     vms_kif_ namespace. The census universe is the union of both"
    echo "     readings, so this is a RED, not a smaller pass: an entry point"
    echo "     cannot leave the census by having its declaration deleted, and a"
    echo "     rename does not stop a function defined in the interface"
    echo "     translation unit from being part of the interface."
    status=1
fi

# 1b. A prototype with no definition. The dangling half of the same shrink.
orphan_protos=$(comm -23 "$WORK/protos" "$WORK/defs")
if [ -n "$orphan_protos" ]; then
    echo "FAIL: prototyped in $(basename "$KIF_H") with NO definition in $(basename "$KIF_C"):"
    printf '%s\n' "$orphan_protos" | sed 's/^/    /'
    echo "  -> the implementation vanished, or it moved out of $(basename "$KIF_C")."
    echo "     If the interface genuinely spans more files now, teach this gate to"
    echo "     read them; do not delete the prototype to quiet it."
    status=1
fi

# ---------------------------------------------------------------------------
# 1c. THE FLOOR, derived from the kernel side, in two grains.
#
# Deleting a prototype, its definition and its declaration together would shrink
# the universe with no disagreement to detect -- but it strands the kernel
# handler behind it. Every opcode vms.ko defines must be issued by a wrapper,
# and every SELECTOR it defines must be named by one: a selector is a distinct
# path through a shared opcode, so an opcode-only floor lets a shared-opcode
# wrapper vanish and takes its kernel path out of userspace reach unnoticed.
# ---------------------------------------------------------------------------
if [ ! -f "$IOCTL_H" ]; then
    echo "FAIL: cannot find the kernel opcode header ($IOCTL_H)"
    echo "  -> the census floor is read from it; if it moved, move this with it."
    status=1
else
    strip_comments < "$IOCTL_H" \
        | grep -oE '^[ \t]*#[ \t]*define[ \t]+VMS_IOCTL_[A-Z0-9_]+' \
        | grep -oE 'VMS_IOCTL_[A-Z0-9_]+' | sort -u > "$WORK/opcodes"
    strip_comments < "$KIF_C" \
        | grep -oE 'VMS_IOCTL_[A-Z0-9_]+' | sort -u > "$WORK/opcodes_issued"

    n_opcodes=$(grep -c . "$WORK/opcodes" || true)
    if [ "$n_opcodes" -eq 0 ]; then
        echo "FAIL: no VMS_IOCTL_* opcodes found in $(basename "$IOCTL_H")"
        echo "  -> the floor reader is broken; without it the universe can be"
        echo "     shrunk by deleting a wrapper outright."
        status=1
    fi

    n_issued=$(comm -12 "$WORK/opcodes" "$WORK/opcodes_issued" | grep -c . || true)
    orphan_opcodes=$(comm -23 "$WORK/opcodes" "$WORK/opcodes_issued")
    if [ -n "$orphan_opcodes" ]; then
        echo "FAIL: kernel opcode(s) no wrapper in $(basename "$KIF_C") ever issues:"
        printf '%s\n' "$orphan_opcodes" | sed 's/^/    /'
        echo "  -> an executive facility userspace cannot reach is wired to nothing,"
        echo "     which is this gate's whole subject stated on the kernel side."
        echo "     Land the wrapper, or delete the opcode and its handler. There is"
        echo "     deliberately no declaration that excuses an orphaned opcode."
        status=1
    fi

    # The second grain. VMS_*_SEL_* constants select a distinct path INSIDE one
    # ioctl handler (vms_ioctl_getdvi dispatches on args.select, so does
    # vms_ioctl_getjpi), so the wrapper that names one is that path's only door
    # out of userspace -- and deleting it strands nothing at opcode grain.
    strip_comments < "$IOCTL_H" \
        | grep -oE '^[ \t]*#[ \t]*define[ \t]+VMS_[A-Z0-9_]+_SEL_[A-Z0-9_]+' \
        | grep -oE 'VMS_[A-Z0-9_]+_SEL_[A-Z0-9_]+' | sort -u > "$WORK/selectors"
    strip_comments < "$KIF_C" \
        | grep -oE 'VMS_[A-Z0-9_]+_SEL_[A-Z0-9_]+' | sort -u > "$WORK/selectors_used"

    n_selectors=$(grep -c . "$WORK/selectors" || true)
    n_sel_named=$(comm -12 "$WORK/selectors" "$WORK/selectors_used" | grep -c . || true)
    orphan_selectors=$(comm -23 "$WORK/selectors" "$WORK/selectors_used")
    if [ -n "$orphan_selectors" ]; then
        echo "FAIL: kernel selector(s) no wrapper in $(basename "$KIF_C") ever names:"
        printf '%s\n' "$orphan_selectors" | sed 's/^/    /'
        echo "  -> the opcode is still issued, so the opcode floor is satisfied and"
        echo "     the universe shrank without a disagreement -- but this path"
        echo "     through the kernel handler no longer has a userspace door. That"
        echo "     is the same defect one grain finer: a facility userspace cannot"
        echo "     reach. Land the wrapper, or delete the selector and the branch"
        echo "     in vms.ko that dispatches on it."
        status=1
    fi
fi

# ---------------------------------------------------------------------------
# 2. Direct product callers, outside vms_kif.c: these are the census roots.
#
# src/ and tools/ only. tests/ is deliberately NOT scanned -- see the header.
# ---------------------------------------------------------------------------
: > "$WORK/direct"
: > "$WORK/sites"
for f in $(find "$SRC_ROOT/src" "$SRC_ROOT/tools" \
             \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null | sort); do
    case "$f" in
        */src/libvmssys/vms_kif.c|*/src/libvmssys/vms_kif.h) continue ;;
    esac
    # Seeded from the SEEDABLE set, not from the vms_kif_ prefix and NOT from
    # the whole universe. The prefix is wrong because the definition reading is
    # unfiltered, so a product caller of an entry point renamed out of the
    # namespace must still count. The whole universe is wrong because it now
    # contains un-namespaced static helper names, and a same-named product
    # function would then certify an unwired wrapper as REACHED. See 1' above.
    strip_comments < "$f" | call_edges | cut -f2 | grep -Fx -f "$WORK/seedable" \
        | sed "s|^|${f#$SRC_ROOT/} |" >> "$WORK/sites" || true
done
cut -d' ' -f2 "$WORK/sites" | sort -u > "$WORK/direct"

# ---------------------------------------------------------------------------
# 3. Reachability inside vms_kif.c, seeded by those roots.
#
# An entry point named only inside vms_kif.c is wired only if the function that
# names it can be reached from a root. kif_bind() calls vms_kif_open() and
# vms_kif_register(); KIF_CALL -> kif_call -> kif_bind is what connects it to
# every wired wrapper. A wrapper family that only calls itself never joins.
# ---------------------------------------------------------------------------
strip_comments < "$KIF_C" | call_edges | sort -u > "$WORK/edges"

awk -F'\t' -v seedfile="$WORK/direct" '
    BEGIN {
        while ((getline l < seedfile) > 0)
            if (l != "" && !(l in reach)) { reach[l] = 1; q[++qn] = l }
    }
    { edge[$1] = edge[$1] " " $2 }
    END {
        for (i = 1; i <= qn; i++) {
            m = split(edge[q[i]], a, " ")
            for (j = 1; j <= m; j++)
                if (a[j] != "" && !(a[j] in reach)) { reach[a[j]] = 1; q[++qn] = a[j] }
        }
        for (k in reach) print k
    }
' "$WORK/edges" | sort -u > "$WORK/reachable"

# ---------------------------------------------------------------------------
# 4. The declarations, read from the RAW header (they live in comments).
# ---------------------------------------------------------------------------
grep -n 'OVMX-UNWIRED:' "$KIF_H" > "$WORK/decl_lines" 2>/dev/null || true
: > "$WORK/declared"
while IFS= read -r dl; do
    [ -n "$dl" ] || continue
    parsed=$(printf '%s\n' "$dl" \
             | grep -oE 'OVMX-UNWIRED:[ \t]*vms_kif_[A-Za-z0-9_]+[ \t]*\(vms-[0-9a-z]+(\.[0-9a-z]+)?\)' \
             || true)
    if [ -z "$parsed" ]; then
        echo "FAIL: malformed unwired declaration: $dl"
        echo "  -> the form is: OVMX-UNWIRED: vms_kif_foo (vms-abc) -- why"
        echo "     an excuse with no item id is not a declaration; that is how"
        echo "     two merged items stayed unwired without anyone tracking it."
        status=1
        continue
    fi
    name=$(printf '%s\n' "$parsed" | grep -oE 'vms_kif_[A-Za-z0-9_]+')
    item=$(printf '%s\n' "$parsed" | grep -oE 'vms-[0-9a-z]+(\.[0-9a-z]+)?')
    printf '%s %s\n' "$name" "$item" >> "$WORK/declared"
done < "$WORK/decl_lines"

dups=$(cut -d' ' -f1 "$WORK/declared" | sort | uniq -d)
if [ -n "$dups" ]; then
    echo "FAIL: entry point declared unwired more than once:"
    printf '%s\n' "$dups" | sed 's/^/  /'
    status=1
fi

# ---------------------------------------------------------------------------
# 5. The census.
# ---------------------------------------------------------------------------
wired=0
unwired=0
undeclared=""
for e in $ENTRIES; do
    if grep -qx "$e" "$WORK/reachable"; then
        wired=$((wired + 1))
        if grep -q "^$e " "$WORK/declared"; then
            echo "FAIL: $e is declared unwired but has a product caller:"
            grep -E "[[:space:]]$e\$" "$WORK/sites" | sed 's/^/    /' | head -5
            echo "  -> delete its OVMX-UNWIRED line. A facility that got wired and"
            echo "     kept its declaration is how a census rots into an allowlist."
            status=1
        fi
    else
        unwired=$((unwired + 1))
        if ! grep -q "^$e " "$WORK/declared"; then
            undeclared="$undeclared $e"
        fi
    fi
done

# Declarations naming something that is not an entry point.
while read -r name item; do
    [ -n "$name" ] || continue
    if ! grep -qx "$name" "$WORK/universe"; then
        echo "FAIL: unwired declaration names $name ($item), which is not an entry"
        echo "      point of the interface (neither prototyped nor defined)"
        echo "  -> a declaration for a function that does not exist protects nothing."
        status=1
    fi
done < "$WORK/declared"

if [ -n "$undeclared" ]; then
    echo "FAIL: entry point(s) with NO product caller and NO unwired declaration:"
    for e in $undeclared; do echo "    $e"; done
    echo "  -> either wire it to a product path, or declare it in $(basename "$KIF_H"):"
    echo "         OVMX-UNWIRED: <name> (vms-abc) -- why it is not wired yet"
    echo "     Shipping a kernel facility, a wrapper and a test suite with no"
    echo "     product caller is the defect this gate exists to make impossible."
    status=1
fi

echo "  census: $n_entries entry points — $wired reached from the product,"
echo "          $unwired with no product path"
echo "  universe pinned: $n_protos prototype(s) + $n_defs definition(s) — the union,"
echo "          so deleting either half, or renaming out of the namespace, is a"
echo "          RED, not a smaller pass"
echo "  floor:  ${n_issued:-0} of ${n_opcodes:-0} kernel opcode(s) issued by a wrapper,"
echo "          ${n_sel_named:-0} of ${n_selectors:-0} selector(s) named by one"

if [ "$status" -eq 0 ]; then
    echo "vms_kif caller census: PASS"
else
    echo "vms_kif caller census: FAIL"
fi
exit "$status"
