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
#     header PROTOTYPES *or* vms_kif.c DEFINES -- including static helpers, so
#     that marking a definition static cannot drop it out either. Deleting a
#     prototype therefore does not shrink the universe by one; the definition
#     still holds the entry point in it, and the entry point still has to be
#     wired or declared.
#   - THE TWO READINGS MUST AGREE. A definition with no prototype, or a
#     prototype with no definition, is itself a RED that NAMES what vanished.
#     (A static helper is exempt from needing a prototype -- that is what static
#     means -- but the union above still counts it.)
#   - THE KERNEL OPCODE FLOOR. Deleting a prototype AND its definition AND its
#     declaration would shrink the universe honestly -- except that it strands
#     the kernel handler behind it. So every VMS_IOCTL_* opcode defined in
#     src/kernel/vms_ioctl.h must be issued by at least one wrapper in
#     vms_kif.c. That floor is derived from the tree, not written down here, and
#     it is the item's own subject stated on the kernel side: an executive
#     facility no userspace wrapper can reach is wired to nothing.
#     There is deliberately NO escape hatch for an orphaned opcode. If you add
#     an ioctl to vms.ko, land its wrapper in the same commit.
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
#   - The kernel opcode floor counts a MENTION of VMS_IOCTL_* in vms_kif.c, not
#     a proof that the opcode reaches an ioctl. A bare `(void)VMS_IOCTL_DEVSCAN;`
#     would satisfy it. Deciding that a value actually flows to KIF_CALL is data
#     flow, not a scan, and a census that guessed would be inventing an answer.
#     The floor's job is narrower than that and it is enough for it: it makes
#     DELETING a wrapper outright a RED instead of a smaller pass.
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

strip_comments < "$KIF_C" | call_edges defs \
    | awk -F'\t' '$2 ~ /^vms_kif_/' | sort -u > "$WORK/defs_all"
cut -f2 "$WORK/defs_all" | sort -u > "$WORK/defs"
awk -F'\t' '$1 == "extern" { print $2 }' "$WORK/defs_all" | sort -u > "$WORK/defs_extern"

sort -u "$WORK/protos" "$WORK/defs" > "$WORK/universe"

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

# 1a. A definition with no prototype. This is what a deleted prototype looks
#     like from the other side, and it is why the universe is the union: the
#     entry point is still counted, and this names what vanished.
orphan_defs=$(comm -13 "$WORK/protos" "$WORK/defs_extern")
if [ -n "$orphan_defs" ]; then
    echo "FAIL: defined in $(basename "$KIF_C") with NO prototype in $(basename "$KIF_H"):"
    printf '%s\n' "$orphan_defs" | sed 's/^/    /'
    echo "  -> the prototype vanished. The census universe is the union of both"
    echo "     readings, so this is a RED, not a smaller pass: an entry point"
    echo "     cannot leave the census by having its declaration deleted."
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
# 1c. THE FLOOR, derived from the kernel side.
#
# Deleting a prototype, its definition and its declaration together would shrink
# the universe with no disagreement to detect -- but it strands the kernel
# handler behind it. Every opcode vms.ko defines must be issued by a wrapper.
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
    strip_comments < "$f" | call_edges | cut -f2 | grep '^vms_kif_' \
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
echo "          so deleting either half is a RED, not a smaller pass"
echo "  floor:  ${n_issued:-0} of ${n_opcodes:-0} kernel opcode(s) issued by a wrapper"

if [ "$status" -eq 0 ]; then
    echo "vms_kif caller census: PASS"
else
    echo "vms_kif caller census: FAIL"
fi
exit "$status"
