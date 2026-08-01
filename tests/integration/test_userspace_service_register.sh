#!/bin/sh
#
# test_userspace_service_register.sh - standing gate (rd vms-5b4, vms-d89):
# every SYS$ system service SAYS, against an item, where the answer it returns
# comes from. No public VMS service answers from process-local memory while
# nothing in the tree says so.
#
# THE EXEMPTION WAS THE BUG (vms-d89). This gate used to exempt any service
# that REACHED the executive -- a transitive call to a vms_kif_* entry point --
# from saying anything. That exemption could be bought for ONE IGNORED LINE,
# measured: adding `(void)vms_kif_getmode(&x)` to the top of sys$gettim (a
# declared facade still answering from clock_gettime) flipped it to "exec",
# whereupon the gate demanded its honest declaration be DELETED, and the
# undeclared facade passed. The remedy the gate insisted on was the evasion.
#
# "Contains a call" is a syntactic proxy for "the answer came from there", and
# every syntactic proxy is purchasable. So there is no computed exemption here
# any more. Every service in the universe declares, in one of three forms, and
# the only FULL exemption -- OVMX-EXECUTIVE -- is priced in an artifact:
#
#     OVMX-USERSPACE:  sys$foo (vms-abc) -- what answers instead
#     OVMX-PARTIAL:    sys$foo (vms-abc) -- exec: what the executive supplies
#     OVMX-LOCAL:      sys$foo -- what it does not (the other half; required)
#     OVMX-EXECUTIVE:  sys$foo (vms-abc) proof=tests/qemu/... -- why it settles it
#
# Reaching the executive is still checked, but only as a CONSISTENCY check on
# what a human wrote: a service declared wholly userspace that reaches a
# vms_kif_* entry point is a RED whose remedy is to UPGRADE the declaration,
# never to delete it; and a PARTIAL or EXECUTIVE claim on a service that
# reaches nothing at all is a RED the other way.
#
# WHY THIS GATE EXISTS, AND WHY THE CENSUS CANNOT COVER IT.
#
# tests/integration/test_kif_caller_census.sh (vms-7fb) asks one question:
# is this vms_kif_* entry point reached from the product? Its escape hatch,
# OVMX-UNWIRED, records that a kernel-interface wrapper has no product caller.
# That statement is true, machine-checked, and NOT the statement a reader takes
# from it. "vms_kif_setast has no product caller" reads as NOT BUILT YET. The
# reality measured by the vms-b33 Phase 2 gate was BUILT SOMEWHERE ELSE: for
# each family whose wrapper is honestly declared unwired, a LIVE per-process
# userspace implementation of the same public VMS service stands in its place --
# src/libvms/syssvc/sys_ast.c for ASTs, sys_assign.c + sys_device.c for channels
# and devices, src/vmsprocess/access_modes.c and the PCB privilege mask for
# modes and privileges. A VMS program calling $SETAST today gets a per-process
# answer and never learns the executive was not consulted.
#
# A caller census cannot see this, and that is not a defect in it: it counts
# calls TO the real thing, and a facade is a call to something ELSE. So this is
# a separate gate with its own, narrower claim -- deliberately NOT bolted onto
# the census, whose stated property is correct as it stands (vms-945, vms-8cc
# and vms-02c exist because a gate's implementation drifted wider than its
# claim).
#
# THE CLAIM, STATED SO IT CAN BE CHECKED AGAINST WHAT THE CODE DOES:
#
#   Every sys$* function DEFINED in the product carries, in the SAME
#   translation unit that defines it, exactly one well-formed declaration of
#   where its answer comes from, naming an rd item; and that declaration is
#   consistent with what the call graph can see.
#
# WHAT THIS GATE DOES **NOT** CLAIM, and these matter more than what it does:
#
#   - It does NOT claim a declared service is wrong. $FAO formats the caller's
#     control string into the caller's buffer and reads no system state at all;
#     that may well be what OpenVMS does too, but THIS GATE DOES NOT KNOW and
#     is not authorised to decide. The register records WHERE THE ANSWER COMES
#     FROM; whether that is a facade or a faithful match is the per-family
#     Rule 10 decision, pinned to the oracle in the item the declaration names.
#   - It does NOT itself decide that an EXECUTIVE-reaching service is wholly
#     executive-resident. A call-graph scan can decide "reaches the executive
#     at all"; deciding which PART of an answer came from there needs the
#     per-facility A-writes/B-reads proof, not a source scan. That is why the
#     mixtures DECLARE their two halves by hand instead of the gate guessing
#     them, and why OVMX-EXECUTIVE costs a named proof. Do not quote this gate
#     as evidence that a service is fully wired; quote the proof it names.
#   - It does NOT read the "state" column as a verdict, and nobody else should
#     either. That column marks a service that transitively touches ANY
#     file-scope object -- and vms_kif.c's /dev/vms file descriptor IS a
#     file-scope object, so holding the executive's CONNECTION scores exactly
#     like holding the ANSWER. The run prints how many executive-reaching
#     services are "state" for that reason alone; read the number, do not
#     recite one from here. An "is stateless" exemption was rejected earlier
#     for scoring three impostors pure. A "touches state" REQUIREMENT fails
#     the other way and was rejected too: it would have left the one-line
#     buy-off working on sys$gettim, which touches no file-scope object at
#     all, so the ignored call would still have bought silence.
#   - It does NOT police the RTL, and this is a scope decision, not an
#     oversight: lib$, str$, mth$, ots$ and dsc$ routines are userspace in
#     OVMX and are not candidates for executive residency here, so they are
#     OUT OF SCOPE and outside the universe entirely -- ZERO of them are
#     measured, on purpose (measured: 55 lib$ file-scope definitions alone
#     exist under src/ today, none of them in this gate's table). That is why
#     adding pure computation to the tree does not redden this gate -- and why
#     the negative controls prove that with a green control instead of
#     asserting it, twice: once with a pure routine and once with a STATEFUL
#     one (both in str$, the sibling namespace, to keep the fixture small),
#     so the first cannot be passing merely for want of state. If lib$ is
#     ever brought into this gate's scope, that is a design decision needing
#     its own item -- it does not fall out of this file changing quietly.
#
# THE HEADER-INLINE HOLE IS CLOSED, and the sentence that used to sit here --
# "a known gap in the scan, not a live evasion" -- was measured FALSE (vms-d89):
# one `static inline sys$foo(...) { ... }` added to a header, with a file-scope
# static beside it, left this gate at rc=0 with its universe unchanged. A
# service that answers from process-local memory was invisible to the gate whose
# entire job is to find them. Headers are now scanned for DEFINITIONS with the
# same scanner as .c files, so a header-inline body contributes its definition,
# its call edges and its state references exactly as a .c body does, and its
# declaration must live in the header that defines it.
#
# THE UNIVERSE, AND WHY IT IS A UNION.
#
# The universe is every sys$* file-scope function DEFINITION under src/ and
# tools/ -- in .c files AND in headers, comment-stripped, product only (a
# definition in tests/ is not a product service) -- UNIONED with every sys$*
# PROTOTYPE declared in any header under src/. The union is the anti-shrink
# property, and it is the lesson the census paid for: a gate whose universe is
# read from ONE place can be disarmed by deleting the thing it counts. Here:
#
#   - Deleting a prototype does not shrink the universe: the definition still
#     holds the service in it, and it still needs a declaration.
#   - Renaming a definition out of the sys$ namespace does not shrink it
#     either: the prototype is then a prototype with no definition, which is
#     its own RED that NAMES what vanished.
#   - Deleting the prototype AND the definition DOES shrink it -- honestly.
#     That is Rule 10's second answer (the service is gone, so the condition is
#     unreachable), and it is meant to be green. It cannot be done quietly,
#     because the now-orphaned declaration is a RED until it is deleted too.
#
# A definition with no prototype is NOT a red. It is still in the universe and
# still needs a declaration; sys$fao_count_args is the live example -- an
# internal helper that took a sys$ name and never got a public prototype.
# Requiring a prototype for it would push a public header edit into every
# helper's path for no gain in what this gate can prove.
#
# THE DECLARATION LIVES WITH THE IMPLEMENTATION, not in a central list, and
# that is the anti-drift property. A register in one file rots the moment an
# implementation moves; a declaration in the defining translation unit moves
# with the code and is deleted by the same hunk that deletes the impostor. The
# gate enforces the pairing in both directions:
#
#   - a declaration naming a service NOT defined in that file is a RED (a
#     stale entry, a typo, or a register line parked where nobody will see it);
#   - an OVMX-USERSPACE declaration on a service that DOES reach the executive
#     is a RED, so wiring a facility forces its declaration to be REWRITTEN in
#     the same commit and the register can never drift into an allowlist of
#     things that were fixed years ago. Note the remedy: rewritten, not
#     deleted. "Delete it" was the old remedy and it was the buy-off.
#
# THE ITEM ID, AND WHAT IT IS HONESTLY WORTH. It is REQUIRED, because "answers
# from userspace" as free text with nothing tracking it is how this state
# persisted through the whole vms-14f dispatch. But it does NOT mean "open work
# is carrying this", and this gate must not be read as claiming it does: rd is
# nostr-backed and unreachable from CI, so nothing here can check an item's
# status. What the id records is WHERE THE DECISION IS WRITTEN DOWN. The run
# prints the distinct items cited and how many declarations each carries, so
# the concentration is visible every time without anyone maintaining a number
# -- a register whose declarations nearly all cite one item is describing one
# past sweep. Verifying that a cited item is still OPEN needs a mechanism with
# rd credentials, and it is the SAME missing mechanism the kif caller census
# needs (vms-8cc); it belongs in one place that both gates consume, not
# reimplemented here.
#
# The reason text is required too, and unlike the census this gate insists on
# it: the reason IS the register's content. An id alone records that somebody
# noticed; the reason records WHAT ANSWERS INSTEAD, which is the fact a reader
# of $SETAST needs.
#
# EVERY CARDINAL BELOW IS DERIVED AND PRINTED BY THIS SCRIPT. There is no count
# recited in this comment for a human to keep in step with the tree -- read the
# run.
#
set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "userspace service register: every sys\$ service reaches the executive or is declared"

if [ ! -d "$SRC_ROOT/src" ]; then
    echo "FAIL: cannot find $SRC_ROOT/src -- this gate scans the product tree."
    echo "  -> if the tree moved, move this gate with it; do not delete the register."
    exit 1
fi

# ---------------------------------------------------------------- strippers --
# A mention in a comment is not a definition and not a call. The DECLARATIONS
# are read from the raw file precisely because they live in comments.
cat > "$WORK/strip.awk" <<'STRIP_EOF'
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
STRIP_EOF

# ------------------------------------------------------------------ scanner --
# Emits, per translation unit:
#   D <file> <linkage> <fn>       file-scope function definition (or macro)
#   C <file> <fn> <callee>        call edge inside <fn>
#   V <file> <linkage> <obj>      file-scope MUTABLE object definition
#   R <file> <fn> <ident>         non-call identifier referenced inside <fn>
# V/R feed the printed "answers from retained per-process state" column only.
# They are evidence, not a verdict: see WHAT THIS GATE DOES NOT CLAIM.
cat > "$WORK/scan.awk" <<'SCAN_EOF'
function emitobj() {
    if (!pendfn && !td && !cn && !ex && pend != "" && !(pend in KW))
        print "V\t" SRC "\t" (st ? "static" : "extern") "\t" pend
}
function scan(line, node, ismac,   n, i, j, k, c, id, nxt, q) {
    n = length(line); i = 1
    while (i <= n) {
        c = substr(line, i, 1)
        if (c == "\"" || c == "'") {
            q = c; i++
            while (i <= n) {
                if (substr(line, i, 1) == "\\") { i += 2; continue }
                if (substr(line, i, 1) == q) { i++; break }
                i++
            }
            continue
        }
        if (ismac) {
            if (node == "") { i++; continue }
            if (c ~ /[A-Za-z_$]/) {
                j = i; while (j <= n && substr(line, j, 1) ~ /[A-Za-z0-9_$]/) j++
                id = substr(line, i, j - i); k = j
                while (k <= n && substr(line, k, 1) ~ /[ \t]/) k++
                if (substr(line, k, 1) == "(") print "C\t" SRC "\t" node "\t" id
                else print "R\t" SRC "\t" node "\t" id
                i = j; continue
            }
            i++; continue
        }
        if (c == "{") {
            if (depth == 0) {
                if (pendfn && pend != "" && !initmode) {
                    curfn = pend
                    print "D\t" SRC "\t" (st ? "static" : "extern") "\t" curfn
                } else curfn = ""
                pend = ""; pendfn = 0
            }
            depth++; i++; continue
        }
        if (c == "}") {
            depth--
            if (depth <= 0) {
                depth = 0; curfn = ""; st = 0; cn = 0; td = 0; ex = 0
                pend = ""; pendfn = 0; initmode = 0; pdepth = 0; bdepth = 0
            }
            i++; continue
        }
        if (depth == 0) {
            if (c == "(") { pdepth++; if (pdepth == 1 && pend != "" && !initmode) pendfn = 1; i++; continue }
            if (c == ")") { if (pdepth > 0) pdepth--; i++; continue }
            if (c == "[") { bdepth++; i++; continue }
            if (c == "]") { if (bdepth > 0) bdepth--; i++; continue }
            if (pdepth == 0 && bdepth == 0) {
                if (c == ";") { emitobj(); pend = ""; pendfn = 0; st = 0; cn = 0; td = 0; ex = 0; initmode = 0; i++; continue }
                if (c == ",") { emitobj(); pend = ""; i++; continue }
                if (c == "=") { emitobj(); pend = ""; initmode = 1; i++; continue }
            }
        }
        if (c ~ /[A-Za-z_$]/) {
            j = i; while (j <= n && substr(line, j, 1) ~ /[A-Za-z0-9_$]/) j++
            id = substr(line, i, j - i); k = j
            while (k <= n && substr(line, k, 1) ~ /[ \t]/) k++
            nxt = substr(line, k, 1)
            if (depth >= 1) {
                if (curfn != "") {
                    if (nxt == "(") print "C\t" SRC "\t" curfn "\t" id
                    else print "R\t" SRC "\t" curfn "\t" id
                }
            } else if (pdepth == 0 && bdepth == 0 && !initmode) {
                if (id == "static") st = 1
                else if (id == "const") cn = 1
                else if (id == "typedef") td = 1
                else if (id == "extern") ex = 1
                else pend = id
            }
            i = j; continue
        }
        i++
    }
}
BEGIN {
    depth = 0; pdepth = 0; bdepth = 0; curfn = ""; pend = ""; pendfn = 0
    st = 0; cn = 0; td = 0; ex = 0; initmode = 0; inmac = 0; macnode = ""
    split("void int char long short float double signed unsigned struct union enum register volatile inline restrict _Atomic _Noreturn __attribute__ __extension__ __asm__ asm", _k, " ")
    for (_i in _k) KW[_k[_i]] = 1
}
{
    line = $0
    if (line ~ /extern[ \t]*"C"/) next
    if (inmac) {
        scan(line, macnode, 1)
        if (line !~ /\\[ \t]*$/) inmac = 0
        next
    }
    if (line ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_$]*\(/) {
        macnode = line
        sub(/^[ \t]*#[ \t]*define[ \t]+/, "", macnode)
        sub(/\(.*$/, "", macnode)
        print "D\t" SRC "\tmacro\t" macnode
        rest = line
        sub(/^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_$]*/, "", rest)
        scan(rest, macnode, 1)
        if (line ~ /\\[ \t]*$/) inmac = 1
        next
    }
    if (line ~ /^[ \t]*#/) {
        if (line ~ /\\[ \t]*$/) { macnode = ""; inmac = 1 }
        next
    }
    scan(line, "", 0)
}
SCAN_EOF

# ------------------------------------------------------- prototype scanner --
cat > "$WORK/proto.awk" <<'PROTO_EOF'
BEGIN { depth = 0; pdepth = 0; pend = ""; isfn = 0 }
{
    line = $0
    if (line ~ /^[ \t]*#/) next
    if (line ~ /extern[ \t]*"C"/) next
    n = length(line); i = 1
    while (i <= n) {
        c = substr(line, i, 1)
        if (c == "\"" || c == "'") {
            q = c; i++
            while (i <= n) {
                if (substr(line, i, 1) == "\\") { i += 2; continue }
                if (substr(line, i, 1) == q) { i++; break }
                i++
            }
            continue
        }
        if (c == "{") { depth++; i++; continue }
        if (c == "}") { if (depth > 0) depth--; i++; continue }
        if (depth == 0) {
            if (c == "(") { pdepth++; if (pdepth == 1 && pend != "") isfn = 1; i++; continue }
            if (c == ")") { if (pdepth > 0) pdepth--; i++; continue }
            if (c == ";" && pdepth == 0) {
                if (isfn && pend ~ /^sys\$/) print "P\t" SRC "\t" pend
                pend = ""; isfn = 0; i++; continue
            }
        }
        if (c ~ /[A-Za-z_$]/) {
            j = i; while (j <= n && substr(line, j, 1) ~ /[A-Za-z0-9_$]/) j++
            if (depth == 0 && pdepth == 0) pend = substr(line, i, j - i)
            i = j; continue
        }
        i++
    }
}
PROTO_EOF

# ------------------------------------------------------------- fact gather --
# HEADERS ARE SCANNED FOR DEFINITIONS TOO, and that is not tidiness: a
# `static inline sys$foo(...) { ... }` in a header is a service definition the
# .c-only reading never saw. That was a MEASURED live evasion, not a
# theoretical one -- one such definition, with a file-scope static beside it,
# left this gate green with its universe unchanged. The same scanner is used,
# so a header-inline body contributes its definition, its call edges and its
# state references exactly as a .c body does, and its declaration must sit in
# the header that defines it.
: > "$WORK/facts"
find "$SRC_ROOT/src" "$SRC_ROOT/tools" \( -name '*.c' -o -name '*.h' \) 2>/dev/null | sort | while read -r f; do
    awk -f "$WORK/strip.awk" "$f" | awk -v SRC="${f#"$SRC_ROOT"/}" -f "$WORK/scan.awk"
done > "$WORK/facts"

: > "$WORK/protos"
find "$SRC_ROOT/src" -name '*.h' 2>/dev/null | sort | while read -r h; do
    awk -f "$WORK/strip.awk" "$h" | awk -v SRC="${h#"$SRC_ROOT"/}" -f "$WORK/proto.awk"
done > "$WORK/protos"

if [ ! -s "$WORK/facts" ]; then
    echo "FAIL: BROKEN GATE: the source scan produced no facts at all."
    echo "  -> a gate that reads nothing certifies everything. Fix the scan."
    exit 1
fi

# ------------------------------------------------------------ declarations --
# Read from the RAW files: the declaration lives in a comment by design.
# Headers are read too, because a header-inline definition's declaration has
# to sit in the header that defines it.
: > "$WORK/decl_raw"
find "$SRC_ROOT/src" "$SRC_ROOT/tools" \( -name '*.c' -o -name '*.h' \) 2>/dev/null | sort | while read -r f; do
    grep -n 'OVMX-USERSPACE:\|OVMX-PARTIAL:\|OVMX-LOCAL:\|OVMX-EXECUTIVE:' "$f" 2>/dev/null \
        | sed "s|^|${f#"$SRC_ROOT"/}:|" || true
done > "$WORK/decl_raw"

# decl_ok records, per accepted declaration line:
#   <file> <kind> <sys$name> <item-or-dash> <proof-path-or-dash>
: > "$WORK/decl_ok"
: > "$WORK/decl_bad"
while IFS= read -r rawline; do
    [ -n "$rawline" ] || continue
    dfile=${rawline%%:*}
    good=$(printf '%s\n' "$rawline" \
        | grep -oE 'OVMX-USERSPACE:[[:space:]]*sys\$[A-Za-z0-9_]+[[:space:]]*\(vms-[0-9a-z]+(\.[0-9a-z]+)?\)[[:space:]]*--[[:space:]]*[^[:space:]]' \
        | head -1)
    if [ -n "$good" ]; then
        dname=$(printf '%s\n' "$good" | sed -E 's/^OVMX-USERSPACE:[[:space:]]*(sys\$[A-Za-z0-9_]+).*/\1/')
        ditem=$(printf '%s\n' "$good" | sed -E 's/.*\((vms-[0-9a-z.]+)\).*/\1/')
        printf '%s\tUSERSPACE\t%s\t%s\t-\n' "$dfile" "$dname" "$ditem" >> "$WORK/decl_ok"
        continue
    fi
    good=$(printf '%s\n' "$rawline" \
        | grep -oE 'OVMX-PARTIAL:[[:space:]]*sys\$[A-Za-z0-9_]+[[:space:]]*\(vms-[0-9a-z]+(\.[0-9a-z]+)?\)[[:space:]]*--[[:space:]]*exec:[[:space:]]*[^[:space:]]' \
        | head -1)
    if [ -n "$good" ]; then
        dname=$(printf '%s\n' "$good" | sed -E 's/^OVMX-PARTIAL:[[:space:]]*(sys\$[A-Za-z0-9_]+).*/\1/')
        ditem=$(printf '%s\n' "$good" | sed -E 's/.*\((vms-[0-9a-z.]+)\).*/\1/')
        printf '%s\tPARTIAL\t%s\t%s\t-\n' "$dfile" "$dname" "$ditem" >> "$WORK/decl_ok"
        continue
    fi
    good=$(printf '%s\n' "$rawline" \
        | grep -oE 'OVMX-LOCAL:[[:space:]]*sys\$[A-Za-z0-9_]+[[:space:]]*--[[:space:]]*[^[:space:]]' \
        | head -1)
    if [ -n "$good" ]; then
        dname=$(printf '%s\n' "$good" | sed -E 's/^OVMX-LOCAL:[[:space:]]*(sys\$[A-Za-z0-9_]+).*/\1/')
        printf '%s\tLOCAL\t%s\t-\t-\n' "$dfile" "$dname" >> "$WORK/decl_ok"
        continue
    fi
    good=$(printf '%s\n' "$rawline" \
        | grep -oE 'OVMX-EXECUTIVE:[[:space:]]*sys\$[A-Za-z0-9_]+[[:space:]]*\(vms-[0-9a-z]+(\.[0-9a-z]+)?\)[[:space:]]*proof=[^[:space:]]+[[:space:]]*--[[:space:]]*[^[:space:]]' \
        | head -1)
    if [ -n "$good" ]; then
        dname=$(printf '%s\n' "$good" | sed -E 's/^OVMX-EXECUTIVE:[[:space:]]*(sys\$[A-Za-z0-9_]+).*/\1/')
        ditem=$(printf '%s\n' "$good" | sed -E 's/.*\((vms-[0-9a-z.]+)\).*/\1/')
        dproof=$(printf '%s\n' "$good" | sed -E 's/.*proof=([^[:space:]]+)[[:space:]]*--.*/\1/')
        printf '%s\tEXECUTIVE\t%s\t%s\t%s\n' "$dfile" "$dname" "$ditem" "$dproof" >> "$WORK/decl_ok"
        continue
    fi
    printf '%s\n' "$rawline" >> "$WORK/decl_bad"
done < "$WORK/decl_raw"

# ---------------------------------------------------------------- analysis --
awk -F'\t' \
    -v declf="$WORK/decl_ok" \
    -v protof="$WORK/protos" \
    -v out_universe="$WORK/universe" \
    -v out_err="$WORK/errors" \
    -v out_items="$WORK/items" \
    -v out_table="$WORK/table" '
$1 == "D" {
    if ($3 == "static" || $3 == "macro") { local[$2 SUBSEP $4] = 1; node = $2 ":" $4 }
    else node = $4
    isdef[node] = 1; deffile[node] = $2; defname[node] = $4
    if ($4 ~ /^sys\$/ && $3 == "extern") {
        if ($4 in defcount) { defcount[$4] = defcount[$4] + 1; dupdef[$4] = dupdef[$4] " " $2 }
        else { defcount[$4] = 1; dupdef[$4] = $2 }
    }
    next
}
$1 == "V" { if ($3 == "static") vstatic[$2 SUBSEP $4] = 1; else vglobal[$4] = 1; next }
$1 == "C" { ncall++; cf[ncall] = $2; cc[ncall] = $3; ce[ncall] = $4; next }
$1 == "R" {
    if (($2 SUBSEP $4) in vstatic || ($4 in vglobal)) {
        n = (($2 SUBSEP $3) in local) ? $2 ":" $3 : $3
        stateful[n] = 1
        if (statewhy[n] == "") statewhy[n] = $4
    }
    next
}
END {
    while ((getline l < protof) > 0) {
        split(l, p, "\t")
        # length(array) is a gawk extension; mawk is the default awk on the
        # Ubuntu runner and would die on it. Count as we read instead.
        if (p[1] == "P" && !(p[3] in protoname)) { nproto++ }
        if (p[1] == "P") { protoname[p[3]] = 1; protofile[p[3]] = p[2] }
    }
    close(protof)
    while ((getline l < declf) > 0) {
        split(l, d, "\t")
        dfile = d[1]; dkind = d[2]; dnm = d[3]; ditem = d[4]; dproof = d[5]
        if (dkind == "LOCAL") {
            localhalf[dnm] = 1
            localfile[dnm] = dfile
            localcount[dnm] = localcount[dnm] + 1
            continue
        }
        declfile[dnm] = dfile; declitem[dnm] = ditem
        declkind[dnm] = dkind; declproof[dnm] = dproof
        declcount[dnm] = declcount[dnm] + 1
        itemuse[ditem] = itemuse[ditem] + 1
    }
    close(declf)

    for (i = 1; i <= ncall; i++) {
        from = ((cf[i] SUBSEP cc[i]) in local) ? cf[i] ":" cc[i] : cc[i]
        to   = ((cf[i] SUBSEP ce[i]) in local) ? cf[i] ":" ce[i] : ce[i]
        if (!((from SUBSEP to) in seen)) { seen[from SUBSEP to] = 1; ne++; ef[ne] = from; et[ne] = to }
        if (ce[i] ~ /^vms_kif_/) execn[to] = 1
    }
    changed = 1
    while (changed) {
        changed = 0
        for (i = 1; i <= ne; i++) {
            if (execn[et[i]] && !execn[ef[i]]) { execn[ef[i]] = 1; changed = 1 }
            if (stateful[et[i]] && !stateful[ef[i]]) {
                stateful[ef[i]] = 1
                if (statewhy[ef[i]] == "") statewhy[ef[i]] = statewhy[et[i]] " (via " et[i] ")"
                changed = 1
            }
        }
    }

    nerr = 0
    nuni = 0; nexec = 0; ndecl = 0; nmix = 0; nstate = 0
    for (n in isdef) {
        if (defname[n] !~ /^sys\$/) continue
        nm = defname[n]
        universe[nm] = deffile[n]
        nuni++
        e = execn[n] ? 1 : 0
        s = stateful[n] ? 1 : 0
        if (e) nexec++
        if (s) nstate++
        if (e && s) {
            nmix++
            # The state this service touches, at its root. When it is the
            # /dev/vms descriptor, "state" is recording that the service holds
            # CONNECTION TO THE EXECUTIVE -- the opposite of a process-local
            # answer. Counted so the distinction is printed, not remembered.
            split(statewhy[n], _w, " ")
            if (_w[1] == "vms_dev_fd") nfdonly++
        }
        isexec[nm] = e; isstate[nm] = s; whystate[nm] = statewhy[n]
    }

    # (1) a prototyped service with no definition NAMES what vanished
    for (nm in protoname)
        if (!(nm in universe))
            errors[++nerr] = "PROTOTYPE WITH NO DEFINITION: " nm " is declared in " protofile[nm] \
                             " but nothing under src/ or tools/ defines it.\n" \
                             "  -> a rename or a deleted definition cannot shrink this universe quietly.\n" \
                             "     Delete the prototype too if the service is genuinely gone."

    # (2) one owner per service
    for (nm in defcount)
        if (defcount[nm] > 1)
            errors[++nerr] = "DEFINED MORE THAN ONCE: " nm " has external definitions in:" dupdef[nm] \
                             "\n  -> the register pairs a declaration with the DEFINING translation" \
                             "\n     unit, so a service with two owners has no unambiguous home."

    # (3) the register is bidirectional: no stale, orphan or duplicate entries
    for (nm in declfile) {
        if (declcount[nm] > 1)
            errors[++nerr] = "DECLARED MORE THAN ONCE: " nm " carries " declcount[nm] \
                             " OVMX-USERSPACE lines.\n  -> one service, one declaration."
        if (!(nm in universe)) {
            errors[++nerr] = "DECLARES SOMETHING THAT IS NOT A SERVICE: " nm " is declared in " \
                             declfile[nm] " but no sys$ definition of that name exists.\n" \
                             "  -> typo, or the service was deleted and its declaration was not."
            continue
        }
        if (declfile[nm] != universe[nm])
            errors[++nerr] = "DECLARED IN THE WRONG TRANSLATION UNIT: " nm " is declared in " \
                             declfile[nm] " but defined in " universe[nm] ".\n" \
                             "  -> the declaration must sit with the implementation it describes."
        if (declkind[nm] == "USERSPACE" && isexec[nm])
            errors[++nerr] = "DECLARED WHOLLY USERSPACE BUT REACHES THE EXECUTIVE: " nm " (" universe[nm] ")\n" \
                             "  -> UPGRADE the declaration; do NOT delete it. Deleting it is how the\n" \
                             "     one-line buy-off used to work: an ignored vms_kif_* call flipped a\n" \
                             "     facade to 'exec', the gate demanded the honest line be deleted, and\n" \
                             "     the facade came out undeclared and green. Say instead which part of\n" \
                             "     the answer the executive supplies (OVMX-PARTIAL + OVMX-LOCAL), or\n" \
                             "     claim all of it with OVMX-EXECUTIVE and its proof."
        if (declkind[nm] == "PARTIAL" && !isexec[nm])
            errors[++nerr] = "PARTIAL DECLARATION ON A SERVICE THAT REACHES NOTHING: " nm " (" universe[nm] ")\n" \
                             "  -> its exec: half claims the executive supplies part of the answer, but\n" \
                             "     no vms_kif_* entry point is reachable from it at all. Use\n" \
                             "     OVMX-USERSPACE."
        if (declkind[nm] == "EXECUTIVE" && !isexec[nm])
            errors[++nerr] = "EXECUTIVE DECLARATION ON A SERVICE THAT REACHES NOTHING: " nm " (" universe[nm] ")\n" \
                             "  -> it claims the whole answer comes from the executive, but no vms_kif_*\n" \
                             "     entry point is reachable from it at all."
        if (declkind[nm] == "PARTIAL" && !(nm in localhalf))
            errors[++nerr] = "PARTIAL DECLARATION WITH NO LOCAL HALF: " nm " (" universe[nm] ")\n" \
                             "  -> a mixture is only described when BOTH halves are named. Add, beside\n" \
                             "     it:  OVMX-LOCAL: " nm " -- what the executive does NOT supply"
        ndecl++
    }

    # (3b) an OVMX-LOCAL half with no OVMX-PARTIAL to be the other half of
    for (nm in localhalf) {
        if (localcount[nm] > 1)
            errors[++nerr] = "LOCAL HALF DECLARED MORE THAN ONCE: " nm " carries " localcount[nm] \
                             " OVMX-LOCAL lines.\n  -> one service, one local half."
        if (declkind[nm] != "PARTIAL")
            errors[++nerr] = "LOCAL HALF WITH NO PARTIAL DECLARATION: " nm " (" localfile[nm] ")\n" \
                             "  -> OVMX-LOCAL is the second half of an OVMX-PARTIAL and means nothing\n" \
                             "     on its own. Either pair it with one or delete it."
        else if (localfile[nm] != declfile[nm])
            errors[++nerr] = "LOCAL HALF IN A DIFFERENT TRANSLATION UNIT: " nm " has its OVMX-LOCAL in " \
                             localfile[nm] " and its OVMX-PARTIAL in " declfile[nm] ".\n" \
                             "  -> both halves describe one implementation and move with it."
    }

    # (4) THE PROPERTY: EVERY service says where its answer comes from.
    #
    # There is NO computed exemption here any more, and its removal is the
    # whole point of this revision. "Contains a transitive vms_kif_* call"
    # used to buy silence, and it could be bought with ONE IGNORED CALL:
    # `(void)vms_kif_getmode(&x)` at the top of a declared facade flipped it
    # to exec, whereupon the gate demanded the honest declaration be deleted
    # and passed the now-undeclared facade. A syntactic proxy for "the answer
    # came from the executive" is purchasable by definition. So the exemption
    # is no longer computed: it is DECLARED, and full exemption costs an
    # OVMX-EXECUTIVE line naming a proof that exists and names the service.
    for (nm in universe) {
        if (nm in declfile) continue
        errors[++nerr] = "SAYS NOTHING ABOUT WHERE ITS ANSWER COMES FROM: " nm " (" universe[nm] ")\n" \
                         "  -> add ONE of these, in " universe[nm] ", beside the definition:\n" \
                         "       OVMX-USERSPACE:  " nm " (vms-abc) -- what answers instead\n" \
                         "       OVMX-PARTIAL:    " nm " (vms-abc) -- exec: what the executive supplies\n" \
                         "       OVMX-LOCAL:      " nm " -- what it does not\n" \
                         "       OVMX-EXECUTIVE:  " nm " (vms-abc) proof=tests/... -- why that proof settles it\n" \
                         "     Reaching a vms_kif_* entry point is NOT enough on its own: it is\n" \
                         "     necessary for the answer to come from the executive and nowhere near\n" \
                         "     sufficient, and an ignored call satisfies it."
    }

    for (i = 1; i <= nerr; i++) print errors[i] > out_err

    nkind_u = 0; nkind_p = 0; nkind_e = 0
    for (nm in universe) {
        k = (nm in declfile) ? declkind[nm] : "NONE"
        if (k == "USERSPACE") nkind_u++
        else if (k == "PARTIAL") nkind_p++
        else if (k == "EXECUTIVE") nkind_e++
        printf "%-22s %-8s %-8s %-7s %-10s %-10s %s\n", nm,
               (isexec[nm] ? "exec" : "-"),
               (isstate[nm] ? "state" : "-"),
               ((nm in protoname) ? "proto" : "-"),
               ((k == "NONE") ? "-" : tolower(k)),
               ((nm in declfile) ? declitem[nm] : "-"),
               universe[nm] > out_table
    }
    # The rd items the register cites, and how much each one is carrying.
    # Derived and printed, never recited: the concentration IS the finding.
    nitems = 0
    for (it in itemuse) { nitems++; printf "item %s %d\n", it, itemuse[it] > out_items }
    printf "universe %d\nexec %d\nstate %d\nexec+state %d\nfd-only %d\ndeclared %d\nuserspace %d\npartial %d\nexecutive %d\nprotos %d\nitems %d\nerrors %d\n",
           nuni, nexec, nstate, nmix, nfdonly, ndecl, nkind_u, nkind_p, nkind_e, nproto, nitems, nerr > out_universe
}' "$WORK/facts"

# ------------------------------------------------- the price of exemption --
# An OVMX-EXECUTIVE line is the ONLY full exemption this gate grants, so it is
# the only place worth attacking, and it is priced accordingly. Four checks,
# each buying back something the old "contains a vms_kif_* call" exemption gave
# away for a token call:
#
#   1. the proof EXISTS in this tree;
#   2. it lives under tests/qemu/ -- the suite whose programs are booted into
#      the real runtime with vms.ko loaded and /dev/vms present (CLAUDE.md
#      Rule 9), and which CI runs as the Kernel Executive job. A test that
#      never sees the executive cannot testify that an answer came from it.
#      Other paths reach the real runtime too (tests/uat drives a console
#      session); this check names ONE directory on purpose, because a
#      per-service C proof is what the register needs to be able to point at;
#   3. it FORKS. Rule 11's decisive test is A-writes / B-reads, and a
#      single-process test passes perfectly against a per-process fake --
#      which is exactly how the known facades survived. fork() is a coarse
#      proxy for "more than one process is involved", and a coarse proxy that
#      costs a second process is not purchasable with one line;
#   4. it NAMES the service, so it is a proof ABOUT this service and not some
#      other file that happened to satisfy 1-3.
#
# WHAT THIS DOES NOT DO, stated so nobody quotes it as more than it is: it does
# NOT check that the named test ASSERTS anything about the service, still less
# that the assertion is right. It checks that a specific, multi-process,
# executive-resident, service-naming artifact was produced. The point is not
# that the artifact cannot be gamed -- it is that gaming it costs a test in the
# suite that boots the executive, instead of one ignored function call.
while IFS="$(printf '\t')" read -r pfile pkind pname pitem pproof; do
    [ "$pkind" = "EXECUTIVE" ] || continue
    if [ ! -f "$SRC_ROOT/$pproof" ]; then
        printf 'EXECUTIVE DECLARATION WHOSE PROOF DOES NOT EXIST: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> proof=%s is not a file in this tree.\n' "$pproof" >> "$WORK/errors"
        continue
    fi
    case "$pproof" in
        tests/qemu/*) ;;
        *)
            printf 'EXECUTIVE DECLARATION WHOSE PROOF DOES NOT RUN AGAINST THE EXECUTIVE: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
            printf '  -> proof=%s is not under tests/qemu/, the per-service suite booted with\n' "$pproof" >> "$WORK/errors"
            printf '     vms.ko loaded and /dev/vms present. A test that never sees the\n' >> "$WORK/errors"
            printf '     executive cannot show that an answer came from it.\n' >> "$WORK/errors"
            continue ;;
    esac
    if ! grep -q 'fork[[:space:]]*(' "$SRC_ROOT/$pproof" 2>/dev/null; then
        printf 'EXECUTIVE DECLARATION WHOSE PROOF IS SINGLE-PROCESS: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> proof=%s never forks, so it cannot be an A-writes/B-reads proof.\n' "$pproof" >> "$WORK/errors"
        printf '     A per-process fake can pass every single-process test perfectly.\n' >> "$WORK/errors"
        continue
    fi
    if ! grep -qF "$pname" "$SRC_ROOT/$pproof" 2>/dev/null; then
        printf 'EXECUTIVE DECLARATION WHOSE PROOF DOES NOT NAME THE SERVICE: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> proof=%s never mentions %s, so it is not a proof about it.\n' "$pproof" "$pname" >> "$WORK/errors"
    fi
done < "$WORK/decl_ok"

# --------------------------------------------------------------- reporting --
if [ -s "$WORK/decl_bad" ]; then
    echo
    echo "  FAIL: malformed OVMX declaration(s):"
    sed 's/^/    /' "$WORK/decl_bad"
    echo "  -> the forms are:"
    echo "       OVMX-USERSPACE:  sys\$foo (vms-abc) -- what answers instead"
    echo "       OVMX-PARTIAL:    sys\$foo (vms-abc) -- exec: what the executive supplies"
    echo "       OVMX-LOCAL:      sys\$foo -- what it does not"
    echo "       OVMX-EXECUTIVE:  sys\$foo (vms-abc) proof=tests/... -- why that proof settles it"
    echo "     The item id and the reason are BOTH required (OVMX-LOCAL carries the"
    echo "     reason only -- its item id is on the OVMX-PARTIAL half it belongs to)."
    echo "     An id alone records that somebody noticed; the reason records what"
    echo "     answers instead."
    status=1
fi

echo
echo "  sys\$ services, by where the answer comes from:"
echo "    name                   exec     state    proto   says       item       defined in"
sort "$WORK/table" | sed 's/^/    /'

echo
nuniverse=0
while IFS=' ' read -r k v; do
    case "$k" in
        universe)   echo "  $v sys\$ services defined under src/ and tools/"; nuniverse=$v ;;
        protos)     echo "  $v sys\$ prototypes declared in headers under src/" ;;
        exec)       echo "  $v reach the executive (transitive call to a vms_kif_* entry point)" ;;
        state)      echo "  $v touch retained per-process state (file-scope object, transitively)" ;;
        exec+state) echo "  $v do BOTH -- touch a file-scope object AND reach the executive (evidence, not a verdict)" ;;
        fd-only)    echo "    $v of those touch NOTHING but vms_kif.c's /dev/vms descriptor -- i.e. the" ;
                    echo "       'state' they hold is the executive's CONNECTION, not a process-local answer." ;;
        declared)   echo "  $v carry a well-formed declaration of where their answer comes from" ;;
        userspace)  echo "    $v say OVMX-USERSPACE -- no part of the answer is the executive's" ;;
        partial)    echo "    $v say OVMX-PARTIAL   -- a named part is, a named part is not" ;;
        executive)  echo "    $v say OVMX-EXECUTIVE -- all of it is, and name a proof that names them" ;;
        items)      echo "  $v distinct rd item(s) carry those declarations:" ;;
        errors)     [ "$v" = "0" ] || status=1 ;;
    esac
done < "$WORK/universe"

# THE CONCENTRATION IS THE POINT OF PRINTING THIS, and it is derived, not
# recited. This gate CANNOT check that a cited item is still open -- rd is
# nostr-backed and unreachable from CI -- so it does not claim to. What it can
# do is show how much of the register is hanging off how few items, every run,
# without anybody remembering a number. A register whose declarations nearly
# all cite one item is a register describing one past sweep, not live work.
if [ -s "$WORK/items" ]; then
    sort -k3,3nr -k2,2 "$WORK/items" | while read -r _ it n; do
        printf '      %-10s x%s\n' "$it" "$n"
    done
fi

# THE FLOOR. A tree with facts (so the "no facts at all" guard above did not
# fire) but ZERO sys$-prefixed definitions passes every check above
# vacuously -- there is nothing in the universe for any of them to fail on.
# That is the same shrink-into-a-smaller-pass shape the kif caller census
# (vms-7fb) gained a floor for. The floor is deliberately just ">0", not a
# recited count: this gate's claim is "every sys$ service is accounted for",
# and that claim is vacuous, not narrowly-true, at zero services -- no larger
# threshold is defensible without becoming a magic number that drifts as the
# product grows. If OVMX genuinely ships no sys$ services this gate has
# nothing left to prove and should be retired, not left green by accident.
if [ "$nuniverse" -eq 0 ]; then
    echo
    echo "  FAIL: THE FLOOR: zero sys\$ services found under src/ and tools/."
    echo "  -> a scan that finds nothing certifies everything else vacuously."
    status=1
fi

if [ -s "$WORK/errors" ]; then
    echo
    echo "  FAIL:"
    sed 's/^/    /' "$WORK/errors"
    status=1
fi

echo
if [ "$status" -eq 0 ]; then
    echo "PASS: every sys\$ service declares, against an item, where its answer comes from."
else
    echo "FAIL: the userspace service register does not account for every sys\$ service."
fi
exit $status
