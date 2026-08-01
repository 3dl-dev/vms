#!/bin/sh
#
# test_userspace_service_register.sh - standing gate (rd vms-5b4): every SYS$
# system service either REACHES THE EXECUTIVE or is DECLARED, against an item,
# as answering from userspace. No public VMS service answers from process-local
# memory while nothing in the tree says so.
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
#   Every sys$* function DEFINED in the product either has, in its transitive
#   product call graph, a call to a vms_kif_* entry point -- or carries, in the
#   SAME translation unit that defines it, a line of the form
#
#       OVMX-USERSPACE: sys$foo (vms-abc) -- one line on where the answer
#                                            comes from instead
#
# WHAT THIS GATE DOES **NOT** CLAIM, and these matter more than what it does:
#
#   - It does NOT claim a declared service is wrong. $FAO formats the caller's
#     control string into the caller's buffer and reads no system state at all;
#     that may well be what OpenVMS does too, but THIS GATE DOES NOT KNOW and
#     is not authorised to decide. The register records WHERE THE ANSWER COMES
#     FROM; whether that is a facade or a faithful match is the per-family
#     Rule 10 decision, pinned to the oracle in the item the declaration names.
#   - It does NOT claim an EXECUTIVE-reaching service is wholly executive-
#     resident. sys$qio reaches the executive only through sys$setef; its
#     channel-to-fd answer still comes from the process-local PCB. The gate
#     prints that mixture (the "exec+state" line below) but does not fail on
#     it: a call-graph scan can decide "reaches the executive at all", but
#     deciding which PART of an answer came from there needs the per-facility
#     A-writes/B-reads proof, not a source scan. Do not quote this gate as
#     evidence that a service is fully wired.
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
#   - It does NOT see a sys$* function whose ENTIRE definition -- body and
#     all -- lives inside a header as `static inline`. The universe is built
#     from two readings (file-scope definitions in .c files, prototypes
#     terminated by `;` in headers) and an inline function defined in a
#     header is neither: not a .c definition, and not a prototype, because
#     its declarator is closed by `}` and never reaches the top-level `;`
#     the prototype reader requires. Nothing in this codebase currently
#     defines a sys$ service this way (measured: no header under src/
#     contains `sys$<name>(...) {` -- a function body opened directly in
#     the header, comment-stripped or not), so this is a known gap in the
#     scan, not a live evasion -- if that ever changes, the service it
#     hides is a silent hole in the universe until this gate is taught to
#     read header-inline bodies too.
#
# THE UNIVERSE, AND WHY IT IS A UNION.
#
# The universe is every sys$* file-scope function DEFINITION under src/ and
# tools/ (comment-stripped, product only -- a definition in tests/ is not a
# product service), UNIONED with every sys$* PROTOTYPE declared in any header
# under src/. The union is the anti-shrink property, and it is the lesson the
# census paid for: a gate whose universe is read from ONE place can be disarmed
# by deleting the thing it counts. Here:
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
#   - a declaration for a service that DOES reach the executive is a RED, so
#     wiring a facility forces its declaration to be deleted in the same
#     commit and the register can never drift into an allowlist of things that
#     were fixed years ago.
#
# The item id is REQUIRED. "Answers from userspace" as free text with nothing
# tracking it is how this state persisted through the whole vms-14f dispatch.
# The gate does NOT verify the item is open -- rd is nostr-backed and not
# reachable from CI. It verifies that a human wrote an id down. The reason text
# is required too, and unlike the census this gate insists on it: the reason IS
# the register's content. An id alone records that somebody noticed; the reason
# records WHAT ANSWERS INSTEAD, which is the fact a reader of $SETAST needs.
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
: > "$WORK/facts"
find "$SRC_ROOT/src" "$SRC_ROOT/tools" -name '*.c' 2>/dev/null | sort | while read -r f; do
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
: > "$WORK/decl_raw"
find "$SRC_ROOT/src" "$SRC_ROOT/tools" -name '*.c' 2>/dev/null | sort | while read -r f; do
    grep -n 'OVMX-USERSPACE:' "$f" 2>/dev/null \
        | sed "s|^|${f#"$SRC_ROOT"/}:|" || true
done > "$WORK/decl_raw"

: > "$WORK/decl_ok"
: > "$WORK/decl_bad"
while IFS= read -r rawline; do
    [ -n "$rawline" ] || continue
    dfile=${rawline%%:*}
    good=$(printf '%s\n' "$rawline" \
        | grep -oE 'OVMX-USERSPACE:[ \t]*sys\$[A-Za-z0-9_]+[ \t]*\(vms-[0-9a-z]+(\.[0-9a-z]+)?\)[ \t]*--[ \t]*[^ \t]' \
        | head -1)
    if [ -z "$good" ]; then
        printf '%s\n' "$rawline" >> "$WORK/decl_bad"
        continue
    fi
    dname=$(printf '%s\n' "$good" | sed -E 's/^OVMX-USERSPACE:[ \t]*(sys\$[A-Za-z0-9_]+).*/\1/')
    ditem=$(printf '%s\n' "$good" | sed -E 's/.*\((vms-[0-9a-z.]+)\).*/\1/')
    printf '%s\t%s\t%s\n' "$dfile" "$dname" "$ditem" >> "$WORK/decl_ok"
done < "$WORK/decl_raw"

# ---------------------------------------------------------------- analysis --
awk -F'\t' \
    -v declf="$WORK/decl_ok" \
    -v protof="$WORK/protos" \
    -v out_universe="$WORK/universe" \
    -v out_err="$WORK/errors" \
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
        declfile[d[2]] = d[1]; declitem[d[2]] = d[3]
        declcount[d[2]] = declcount[d[2]] + 1
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
        if (e && s) nmix++
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
        if (isexec[nm])
            errors[++nerr] = "DECLARED USERSPACE BUT REACHES THE EXECUTIVE: " nm " (" universe[nm] ")\n" \
                             "  -> delete its OVMX-USERSPACE line. A service that got wired must lose\n" \
                             "     its declaration in the same commit, or the register becomes an\n" \
                             "     allowlist of things that were fixed years ago."
        ndecl++
    }

    # (4) THE PROPERTY: no undeclared service answers without the executive
    for (nm in universe) {
        if (isexec[nm]) continue
        if (nm in declfile) continue
        errors[++nerr] = "ANSWERS WITHOUT THE EXECUTIVE AND IS NOT DECLARED: " nm " (" universe[nm] ")\n" \
                         "  -> add, in " universe[nm] ", beside the definition:\n" \
                         "       OVMX-USERSPACE: " nm " (vms-abc) -- where the answer comes from instead\n" \
                         "     or route it to the executive through vms_kif_*."
    }

    for (i = 1; i <= nerr; i++) print errors[i] > out_err

    for (nm in universe) {
        printf "%-22s %-8s %-8s %-7s %-10s %s\n", nm,
               (isexec[nm] ? "exec" : "-"),
               (isstate[nm] ? "state" : "-"),
               ((nm in protoname) ? "proto" : "-"),
               ((nm in declfile) ? declitem[nm] : "-"),
               universe[nm] > out_table
    }
    printf "universe %d\nexec %d\nstate %d\nexec+state %d\ndeclared %d\nprotos %d\nerrors %d\n",
           nuni, nexec, nstate, nmix, ndecl, nproto, nerr > out_universe
}' "$WORK/facts"

# --------------------------------------------------------------- reporting --
if [ -s "$WORK/decl_bad" ]; then
    echo
    echo "  FAIL: malformed OVMX-USERSPACE declaration(s):"
    sed 's/^/    /' "$WORK/decl_bad"
    echo "  -> the form is: OVMX-USERSPACE: sys\$foo (vms-abc) -- one line on why"
    echo "     The item id and the reason are BOTH required. An id alone records"
    echo "     that somebody noticed; the reason records what answers instead."
    status=1
fi

echo
echo "  sys\$ services, by where the answer comes from:"
echo "    name                   exec     state    proto   item       defined in"
sort "$WORK/table" | sed 's/^/    /'

echo
nuniverse=0
while IFS=' ' read -r k v; do
    case "$k" in
        universe)   echo "  $v sys\$ services defined under src/ and tools/"; nuniverse=$v ;;
        protos)     echo "  $v sys\$ prototypes declared in headers under src/" ;;
        exec)       echo "  $v reach the executive (transitive call to a vms_kif_* entry point)" ;;
        state)      echo "  $v touch retained per-process state (file-scope object, transitively)" ;;
        exec+state) echo "  $v do BOTH -- partly executive, partly process-local (evidence, not a verdict)" ;;
        declared)   echo "  $v carry a well-formed OVMX-USERSPACE declaration" ;;
        errors)     [ "$v" = "0" ] || status=1 ;;
    esac
done < "$WORK/universe"

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
    echo "PASS: every sys\$ service reaches the executive or is declared against an item."
else
    echo "FAIL: the userspace service register does not account for every sys\$ service."
fi
exit $status
