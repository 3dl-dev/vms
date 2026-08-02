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
#     them, and why OVMX-EXECUTIVE costs a named proof that an injected defect
#     in the service answer path is known to redden. Do not quote this gate as
#     evidence that a service is fully wired; quote the proof it names.
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
# WHAT THE GATE READS, AND WHERE IT LOOKS FOR IT.
#
# Two questions, and they used to be answered by one thing. WHICH FILES are
# read is THE PRODUCT COMPILE SET; WHAT IS FOUND IN THEM is the union of three
# readings below. Both matter, and the second is worth nothing without the
# first.
#
# THE PRODUCT COMPILE SET is derived twice over and unioned (see "the product
# compile set" below): the old directory glob -- src/ and tools/ -- and every
# translation unit CMAKE SAYS THE PRODUCT COMPILES, read out of a
# compile_commands.json this gate generates. The second reading exists because
# the first is a PATH, and a path is a place a file can be moved out of.
# Measured on this tree (vms-c19): relocate a definition into a new file
# outside src/ and tools/, add that file to src/libvms/CMakeLists.txt, and the
# universe went 88 -> 87 with rc=0 and PASS while `cmake --build --target vms`
# succeeded, `nm -D lib/LIBVMS$SHR.EXE` showed `T sys$gettim`, and every caller
# linked unchanged. The remedy is NOT a wider glob -- that is the same defect
# one directory further out. It is to ask the build what it compiles.
#
# THE UNIVERSE, AND WHY IT IS A UNION.
#
# Within that compile set the universe is every sys$* file-scope function
# DEFINITION -- in .c files AND in headers, comment-stripped, product only (a
# definition in tests/ is not a product service) -- UNIONED with every sys$*
# PROTOTYPE declared in any header the product compiles against, UNIONED with
# every sys$* symbol THE COMPILED PRODUCT ACTUALLY EXPORTS (nm, below). The
# union is the anti-shrink property, and it is the lesson the census paid for:
# a gate whose universe is read from ONE place can be disarmed by deleting the
# thing it counts. Here:
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
# THE THIRD MEMBER OF THE UNION IS NOT SOURCE TEXT, and it is here because the
# first two were both source text and both were escaped by ONE construct
# (vms-f26). Measured on a clean archive of the tree: rename the definition
# `sys$gettim` -> `ovmx_gettim`, rename its prototype in lockstep, and restore
# the public name with
#
#     uint32_t ovmx_gettim(uint64_t *timadr) __asm__("sys$gettim");
#     #define sys$gettim ovmx_gettim
#
# The service leaves the definition reading AND the prototype reading TOGETHER,
# so the mismatch checks -- which is all the union could see -- stay silent: the
# universe went 88 -> 87 services / 87 declared / 86 prototypes and the gate
# printed PASS. The object file is symbol-identical to pristine (`nm` shows
# `T sys$gettim` in both), so nothing downstream can tell, and every caller
# links unchanged. A shrinking universe was indistinguishable, to every check
# here, from a service that was legitimately deleted.
#
# The asm label is one such construct and there is no reason to believe it is
# the only one, so the fix is not to recognise it. THE UNIVERSE IS DERIVED FROM
# WHAT THE PRODUCT EXPORTS: this gate compiles every product .c file it scans
# and reads the defined, global sys$* symbols out of the objects with nm. That
# is a byproduct of the build, not a spelling, so it does not care how the
# source names the definition.
#
# WHAT THAT DOES AND DOES NOT BUY, stated so nobody quotes it as more:
#   - it buys that a C-defined service does not leave the universe while the
#     product still exports it. Whatever the source calls it, it is in the
#     table under its EXPORTED name and it needs a declaration. Measured: the
#     rename above is now a RED naming sys$gettim, and the universe stays 88.
#   - it now reads ASSEMBLY TOO, and only because the build says which
#     assembly to read. This bullet used to record the opposite as an open
#     residual: a `.globl sys$foo` in a .S file would export a service the scan
#     never assembled. The compile set closed it for free -- CMake names the .S
#     files it assembles FOR THIS ARCHITECTURE, so they are assembled and read
#     with nm like anything else. They are deliberately NOT globbed:
#     src/libvmssys/arch/ holds x86_64 and aarch64 sources side by side and a
#     glob would hand the assembler the wrong one. What this buys today is
#     nothing and says so: the 3 assembly files in this tree export no sys$
#     symbol, so the exported count is unchanged. It buys that the NEXT one
#     does not have to be noticed by a human.
#   - the SOURCE readings still do not read assembly, and cannot: scan.awk
#     parses C. An assembly-defined service is in the universe under its
#     EXPORTED name with "-" in its exec and state columns, exactly like an
#     aliased one below.
#   - it does NOT make the call graph follow an alias. The exec/state columns
#     are keyed on the SOURCE name, so a service exported under a name its
#     source does not spell shows "-" in both, and the consistency checks
#     (USERSPACE-but-reaches-the-executive, PARTIAL/EXECUTIVE-reaching-nothing)
#     go quiet for it. That is a real residual, tracked rather than papered
#     over; it is not a REGRESSION, because before this scan the service was
#     not in the universe at all.
#   - src/kernel/ is EXCLUDED from the object scan and the exclusion is
#     declared below, not glob-shaped: those files are a separate binary built
#     against kernel headers and cannot be compiled here. They are still read
#     by the SOURCE scan, so a sys$ definition there is still in the universe;
#     what the exclusion costs is alias detection inside the kernel module,
#     which links nothing into libvms and exports no sys$ symbol today.
#   - IT MAKES THIS GATE'S DEPENDENCIES THE WHOLE PRODUCT'S DEPENDENCIES, and
#     that is a real cost, paid on purpose. Every product .c file is compiled
#     here whether or not CMake would configure it -- which is precisely why
#     the compile set UNIONS the glob with the build's answer instead of
#     replacing one with the other. A source guarded by an OPTIONAL
#     third-party library still has to build: src/vmsssh/vmssshd.c includes
#     <libssh/libssh.h>, and CMake does not configure src/vmsssh at all
#     without libssh, so the build's answer alone would have DROPPED that file
#     and retired this refusal silently. The glob keeps offering it, and an
#     environment without libssh-dev makes this gate REFUSE rather than certify
#     a universe with a hole in it (measured, vms-ecf r5 -- it reddened the CI
#     Build & Test job, correctly; re-measured under the compile set with the
#     libssh include made unresolvable, which refuses at 129 of 130). The fix
#     taken was to install the dependency in the job that runs this gate, NOT
#     to tolerate the short count; see the comment on that step in
#     .github/workflows/ci.yml. If a future source genuinely cannot be compiled
#     in any environment that runs this gate, the second legal answer is to
#     extend SYMSCAN_EXCLUDE_DIR and say why HERE -- visibly, with what the
#     exclusion costs -- never to skip the file quietly.
#   - IT MAKES CMAKE A DEPENDENCY OF THIS GATE. A tree with a CMakeLists.txt
#     that cmake cannot configure is a tree whose compile set is unknown, and
#     this gate refuses rather than falling back to the glob -- the glob is the
#     reading vms-c19 showed to be escapable, so falling back to it silently is
#     the whole defect wearing a different hat. A tree with NO CMakeLists.txt
#     describes no build at all; there the glob is the honest whole reading,
#     and the floor fixtures next door are exactly that shape.
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
# THE PARENTHESIZED DECLARATOR (vms-c19). `uint32_t (sys$gettim)(uint64_t *)`
# is the same declaration as `uint32_t sys$gettim(uint64_t *)` to every C
# compiler and to every caller, and it produces a byte-identical symbol. Read
# naively -- bind the name to the last identifier before the first `(` at
# top-level paren depth -- it binds `uint32_t`, and the service leaves BOTH
# source readings at once, in ONE construct, with no declaration of any kind.
# Measured on this tree before the fix: the definition reading emitted
# `D <file> extern uint32_t` and the prototype reading emitted nothing.
#
# So a top-level paren group is tracked while it is open. A group that holds
# EXACTLY ONE identifier and nothing else, and that is immediately followed by
# another `(`, is a parenthesized declarator, and the name is the identifier
# inside it. The "nothing else" is what keeps `(*fp)(int)` -- a function
# POINTER object, not a function -- from being read as a function declarator,
# and the "followed by `(`" is what keeps an ordinary one-identifier parameter
# list `(void)` from being read as one.
#
# THE SPELLINGS THIS WAS RUN AGAINST, here and in proto.awk: plain;
# parenthesized; parenthesized with a space before the parameter list;
# parenthesized split across lines; pointer return; pointer return
# parenthesized; struct return; leading __attribute__; function-pointer
# parameters, with and without a parenthesized name; array parameters; and
# `(void)`. The four that must NOT bind -- a `(*fp)(int)` object, an array of
# them, an `extern` one, and a typedef of one -- bound nothing before this
# change and bind nothing after it.
#
# ONE KNOWN NON-BINDING CASE, PRE-EXISTING AND UNCHANGED: a TRAILING
# `__attribute__((...))` after a prototype's parameter list rebinds the name to
# `__attribute__`, so `uint32_t sys$foo(int) __attribute__((pure));` is not
# read as a prototype. It never was, before or after; the definition and nm
# readings still hold such a service, which is the union doing its job.
function grpopen() { gids = 0; gid = ""; gclean = 1 }
function grpclose() { if (gclean && gids == 1) { gpend = 1; gname = gid } else gpend = 0 }
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
            if (depth == 0) { if (pdepth == 1) gclean = 0; else if (pdepth == 0) gpend = 0 }
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
                pend = ""; pendfn = 0; gpend = 0
            }
            depth++; i++; continue
        }
        if (c == "}") {
            depth--
            if (depth <= 0) {
                depth = 0; curfn = ""; st = 0; cn = 0; td = 0; ex = 0
                pend = ""; pendfn = 0; initmode = 0; pdepth = 0; bdepth = 0
                gpend = 0; gclean = 1; gids = 0
            }
            i++; continue
        }
        if (depth == 0) {
            if (c == "(") {
                if (pdepth == 0) { if (gpend) { pend = gname; gpend = 0 }; grpopen() }
                else if (pdepth == 1) gclean = 0
                pdepth++
                if (pdepth == 1 && pend != "" && !initmode) pendfn = 1
                i++; continue
            }
            if (c == ")") { if (pdepth > 0) pdepth--; if (pdepth == 0) grpclose(); i++; continue }
            if (c == "[") { if (pdepth == 1) gclean = 0; else if (pdepth == 0) gpend = 0; bdepth++; i++; continue }
            if (c == "]") { if (pdepth == 1) gclean = 0; else if (pdepth == 0) gpend = 0; if (bdepth > 0) bdepth--; i++; continue }
            if (pdepth == 0 && bdepth == 0) {
                if (c == ";") { emitobj(); pend = ""; pendfn = 0; st = 0; cn = 0; td = 0; ex = 0; initmode = 0; gpend = 0; i++; continue }
                if (c == ",") { emitobj(); pend = ""; gpend = 0; i++; continue }
                if (c == "=") { emitobj(); pend = ""; initmode = 1; gpend = 0; i++; continue }
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
                gpend = 0
            } else if (pdepth == 1) { gids++; gid = id }
            i = j; continue
        }
        if (depth == 0 && c != " " && c != "\t") {
            if (pdepth == 1) gclean = 0
            else if (pdepth == 0) gpend = 0
        }
        i++
    }
}
BEGIN {
    depth = 0; pdepth = 0; bdepth = 0; curfn = ""; pend = ""; pendfn = 0
    st = 0; cn = 0; td = 0; ex = 0; initmode = 0; inmac = 0; macnode = ""
    gpend = 0; gname = ""; gids = 0; gid = ""; gclean = 1
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
# The parenthesized declarator is read here exactly as it is in scan.awk above,
# and for the same reason: `uint32_t (sys$gettim)(uint64_t *);` is a prototype
# for sys$gettim, and binding the name to the last identifier before the first
# `(` binds `uint32_t` instead. One construct defeated BOTH readings (vms-c19).
function grpopen() { gids = 0; gid = ""; gclean = 1 }
function grpclose() { if (gclean && gids == 1) { gpend = 1; gname = gid } else gpend = 0 }
BEGIN {
    depth = 0; pdepth = 0; pend = ""; isfn = 0
    gpend = 0; gname = ""; gids = 0; gid = ""; gclean = 1
}
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
            if (depth == 0) { if (pdepth == 1) gclean = 0; else if (pdepth == 0) gpend = 0 }
            continue
        }
        if (c == "{") { depth++; gpend = 0; i++; continue }
        if (c == "}") { if (depth > 0) depth--; gpend = 0; i++; continue }
        if (depth == 0) {
            if (c == "(") {
                if (pdepth == 0) { if (gpend) { pend = gname; gpend = 0 }; grpopen() }
                else if (pdepth == 1) gclean = 0
                pdepth++
                if (pdepth == 1 && pend != "") isfn = 1
                i++; continue
            }
            if (c == ")") { if (pdepth > 0) pdepth--; if (pdepth == 0) grpclose(); i++; continue }
            if (c == ";" && pdepth == 0) {
                if (isfn && pend ~ /^sys\$/) print "P\t" SRC "\t" pend
                pend = ""; isfn = 0; gpend = 0; i++; continue
            }
        }
        if (c ~ /[A-Za-z_$]/) {
            j = i; while (j <= n && substr(line, j, 1) ~ /[A-Za-z0-9_$]/) j++
            id = substr(line, i, j - i)
            if (depth == 0 && pdepth == 0) { pend = id; gpend = 0 }
            else if (depth == 0 && pdepth == 1) { gids++; gid = id }
            i = j; continue
        }
        if (depth == 0 && c != " " && c != "\t") {
            if (pdepth == 1) gclean = 0
            else if (pdepth == 0) gpend = 0
        }
        i++
    }
}
PROTO_EOF

# ------------------------------------------------- the product compile set --
# WHAT THE BUILD COMPILES, ASKED OF THE BUILD (vms-c19). Every reading in this
# gate used to start from one glob -- `src/` plus `tools/` -- so the three
# members of the universe were three readings of ONE PLACE, and the place was a
# PATH. Measured on this tree: move a service's definition to a new file
# OUTSIDE those two directories, add that file to src/libvms/CMakeLists.txt,
# and the universe went 88 -> 87 with rc=0 and PASS while `cmake --build
# --target vms` succeeded and `nm -D lib/LIBVMS$SHR.EXE` showed `T sys$gettim`.
# The product still shipped the service; the gate had simply stopped looking
# where it lives. Widening the glob would have been the same defect one level
# out -- the next file goes one directory further.
#
# So the compile set is asked of the build system: cmake is run with
# CMAKE_EXPORT_COMPILE_COMMANDS into a throwaway directory, and every
# translation unit in the resulting compile_commands.json is in the set,
# wherever in the tree it lives. That is a byproduct of the build description,
# not a path convention, and a file CMake does not compile ships nothing.
#
# IT IS A UNION WITH THE OLD GLOB, NOT A REPLACEMENT, and that direction
# matters: a configure only sees the options it was given, so a source guarded
# by an option that is OFF here (src/imgact/, which needs OVMX_IMGACT) drops
# out of compile_commands.json while still being product source. Measured on
# this tree: the glob offers 127 files to the symbol scan and this configure
# offers 122, overlapping in 118. Taking only the build's answer would have
# SHRUNK the scan -- and would have quietly retired the libssh refusal below,
# because CMake does not configure src/vmsssh/ without libssh while the glob
# compiles it regardless. Union, so neither reading can lose a file the other
# holds.
#
# WHAT IS OUT OF SCOPE AND WHY. tests/ has never been in this gate's universe
# -- a definition in a test is not a product service -- so a source under
# tests/ is in the compile set only when the target that compiles it is a
# PRODUCT target: one declared outside tests/ that also compiles a source
# outside tests/. Moving a definition into tests/ and adding it to the `vms`
# library therefore does NOT get it out of the scan. Both halves of that
# condition are load-bearing and the second was added after measurement:
# tests/vmsssh/ builds test_term_mapping from its own test source PLUS
# src/vmsssh/term_map.c, and without the "declared outside tests/" half the
# test source came into the scan with it. It contributed no service, but a
# scan that reaches into test programs is not the scope this gate states.
# WHAT REMAINS OPEN, said here rather than discovered later:
#   - a target declared under tests/, holding a sys$ definition in a source
#     under tests/, that is nevertheless installed;
#   - a source CMake compiles only under an option this configure leaves OFF
#     AND that lives outside src/ and tools/. Inside those two directories the
#     glob still holds it (that is what caught src/imgact/ here); outside them
#     neither reading does. A configure is only as wide as its options.
#   - what the compile set decides is WHICH FILES ARE READ. It says nothing
#     about whether a service's declaration is true; that is the per-family
#     Rule 10 decision, pinned to the oracle in the item the declaration names.
#
# IF THE BUILD DESCRIPTION CANNOT BE READ, THIS REFUSES. A tree with a
# CMakeLists.txt and no way to read what it compiles is a tree this gate cannot
# derive its universe from, and the precedent is already set twice over (no
# compiler, unreadable ioctl bridge). A tree with NO CMakeLists.txt at all is
# a different case -- it describes no build, so the glob is the whole honest
# reading, and the gate's own floor fixtures are exactly that shape.
BUILDSET_LOG="$WORK/cmk.log"
: > "$WORK/buildset"
if [ -f "$SRC_ROOT/CMakeLists.txt" ]; then
    if ! command -v cmake >/dev/null 2>&1; then
        echo "FAIL: BROKEN BUILD-SET SCAN: $SRC_ROOT/CMakeLists.txt describes a build"
        echo "      but cmake(1) is not available to read what it compiles."
        echo "  -> the universe of services is derived from what the BUILD compiles, not"
        echo "     from a directory glob: a service relocated out of src/ and tools/ and"
        echo "     added to a CMake target still ships (measured, vms-c19). Without cmake"
        echo "     this gate cannot see that file, so it does not get to certify that"
        echo "     every service is accounted for. Install cmake; do not add a skip."
        exit 1
    fi
    if ! cmake -S "$SRC_ROOT" -B "$WORK/cmk" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
              >"$BUILDSET_LOG" 2>&1 || [ ! -f "$WORK/cmk/compile_commands.json" ]; then
        echo "FAIL: BROKEN BUILD-SET SCAN: cmake could not configure $SRC_ROOT, so the"
        echo "      set of translation units the product compiles is unknown."
        tail -15 "$BUILDSET_LOG" 2>/dev/null | sed 's/^/    /'
        echo "  -> a gate that cannot ask the build what it compiles cannot tell a"
        echo "     service that was deleted from one that was moved somewhere the glob"
        echo "     does not look. Fix the build description; do not add a skip."
        exit 1
    fi
    # compile_commands.json, one field per line as CMake writes it. Each entry
    # gives the source file and the object it produces; the object path names
    # the TARGET (the component before ".dir/"), which is how a test-only
    # target is told from a product one.
    awk -v ROOT="$SRC_ROOT/" '
        /^[ \t]*"file"[ \t]*:/ {
            f = $0; sub(/^[^:]*:[ \t]*"/, "", f); sub(/",?[ \t]*$/, "", f)
            curf = f; curt = ""; next
        }
        /^[ \t]*"output"[ \t]*:/ {
            o = $0; sub(/^[^:]*:[ \t]*"/, "", o); sub(/",?[ \t]*$/, "", o)
            curt = o; next
        }
        /^[ \t]*\}/ {
            if (curf != "" && index(curf, ROOT) == 1) {
                rel = substr(curf, length(ROOT) + 1)
                p = index(curt, ".dir/")
                tgt = (p > 0) ? substr(curt, 1, p + 3) : "?"
                q = index(curt, "/CMakeFiles/")
                tdir = (q > 0) ? substr(curt, 1, q - 1) : ""
                n++; src[n] = rel; tg[n] = tgt
                if (rel !~ /^tests\// && tdir !~ /^tests\//) prodtgt[tgt] = 1
            }
            curf = ""; curt = ""; next
        }
        END {
            for (i = 1; i <= n; i++) {
                if (src[i] ~ /^tests\// && !(tg[i] in prodtgt)) continue
                if (src[i] in seen) continue
                seen[src[i]] = 1; print src[i]
            }
        }' "$WORK/cmk/compile_commands.json" | sort > "$WORK/buildset"
    if [ ! -s "$WORK/buildset" ]; then
        echo "FAIL: BROKEN BUILD-SET SCAN: cmake configured $SRC_ROOT but no translation"
        echo "      unit could be read out of its compile_commands.json."
        echo "  -> either the build compiles nothing, or this parse no longer matches"
        echo "     what CMake writes. Both make the build half of the universe empty,"
        echo "     which would silently reduce it to the directory glob it exists to"
        echo "     back up. Fix the parse; do not let it fall back."
        exit 1
    fi
fi

# The .c set every source reading walks, and the .c/.S set the symbol scan
# compiles: the glob UNION what the build compiles.
find "$SRC_ROOT/src" "$SRC_ROOT/tools" -name '*.c' 2>/dev/null \
    | sed "s|^$(printf '%s' "$SRC_ROOT" | sed 's/[\\&|]/\\\\&/g')/||" > "$WORK/prodc.raw"
grep -E '\.c$' "$WORK/buildset" >> "$WORK/prodc.raw" 2>/dev/null || true
sort -u "$WORK/prodc.raw" > "$WORK/prodc"
# ASSEMBLY, and ONLY from the build. The header used to record "it does NOT
# read ASSEMBLY -- a .globl sys$foo in a .S file would define an exported
# service this scan never compiles" as an open residual. The build knows which
# assembly files it assembles for THIS architecture, so they are assembled and
# read with nm like anything else. They are NOT globbed: src/libvmssys/arch/
# holds both x86_64 and aarch64 sources and globbing would hand the assembler
# the wrong one.
grep -E '\.[sS]$' "$WORK/buildset" > "$WORK/prodasm" 2>/dev/null || : > "$WORK/prodasm"

# --------------------------------------------------- exported-symbol scan --
# THE UNIVERSE'S THIRD MEMBER, and the only one that is not source text. Every
# product translation unit outside the declared exclusion is compiled here and
# its defined, global sys$* symbols are read out with nm. See the header: the
# two source readings were escaped TOGETHER by an asm-label rename that kept
# the exported symbol byte-identical.
#
# Emits into the fact stream:
#   S <file> <symbol>   a sys$ symbol the compiled product exports
SYMSCAN_EXCLUDE_DIR="src/kernel"

SYMCC=""
for _c in cc gcc; do
    if command -v "$_c" >/dev/null 2>&1; then SYMCC="$_c"; break; fi
done
if [ -z "$SYMCC" ] || ! command -v nm >/dev/null 2>&1; then
    echo "FAIL: BROKEN SYMBOL SCAN: no C compiler (cc/gcc) or no nm(1) available."
    echo "  -> the universe is derived from the symbols the product EXPORTS, because"
    echo "     the source-text readings were both escaped by one asm-label rename"
    echo "     (vms-f26). Without a compiler this gate cannot see a service exported"
    echo "     under a name its source does not spell, so it does not get to certify"
    echo "     that every service is accounted for. Install a compiler; do not add a"
    echo "     skip."
    exit 1
fi

SYMINC=""
for _d in $(find "$SRC_ROOT/src" -name '*.h' -exec dirname {} \; 2>/dev/null | sort -u); do
    SYMINC="$SYMINC -I$_d"
done
for _d in "$SRC_ROOT"/src/*/include; do
    [ -d "$_d" ] && SYMINC="$SYMINC -I$_d"
done

OBJDIR="$WORK/obj"
FAILLIST="$WORK/ccfail"
mkdir -p "$OBJDIR"
: > "$FAILLIST"
export OBJDIR FAILLIST SYMCC SYMINC

# -w plus the -Wno-error= forms: this is a SYMBOL scan, not a build gate. It
# must read the symbols out of code that a stricter compiler would reject --
# the negative controls next door deliberately delete prototypes and rename
# definitions, and a gcc that promotes an implicit declaration to an error
# (gcc 14 and later do) would turn those controls into "the scan broke"
# instead of the reds they are testing for.
#
# EACH FLAG IS PROBED, because the first draft of this file assumed unknown
# -Wno-* forms were ignored and MEASURED OTHERWISE: gcc 13.3 rejects
# -Wno-error=return-mismatch (the warning arrives in gcc 14) with a cc1 error,
# and all 127 files failed to compile. An empty translation unit is enough to
# provoke it, so the probe is one compile per candidate.
: > "$WORK/probe.c"
SYMFLAGS="-w"
for _fl in -Wno-implicit-function-declaration \
           -Wno-error=implicit-function-declaration \
           -Wno-error=implicit-int -Wno-error=int-conversion \
           -Wno-error=incompatible-pointer-types \
           -Wno-error=return-mismatch; do
    if $SYMCC $_fl -c "$WORK/probe.c" -o "$WORK/probe.o" >/dev/null 2>&1; then
        SYMFLAGS="$SYMFLAGS $_fl"
    fi
done
rm -f "$WORK/probe.o"
export SYMFLAGS

# -MD, so the compile also reports WHICH HEADERS IT READ. The header reading
# below is a glob like the .c reading was, and a header-inline definition
# (`static inline sys$foo(...) { ... }`) is a service the nm scan cannot catch
# -- a static inline exports no symbol. The dependency files are the same kind
# of evidence as compile_commands.json: what the build actually read, not where
# somebody expected it to be.
cat > "$WORK/cc1.sh" <<'CC_EOF'
#!/bin/sh
o="$OBJDIR/$(printf '%s' "$1" | tr -c 'A-Za-z0-9' '_').o"
e="$o.err"
if ! $SYMCC -c -std=gnu11 -D_GNU_SOURCE $SYMINC $SYMFLAGS -MD -MF "$o.d" "$1" -o "$o" 2>"$e"; then
    { printf '%s\n' "$1"; head -2 "$e" | sed 's/^/        /'; } >> "$FAILLIST"
fi
rm -f "$e"
CC_EOF
chmod +x "$WORK/cc1.sh"

# -path/-prune, not a grep on the path: SRC_ROOT is an arbitrary directory and
# a regexp match on it would be at the mercy of whatever punctuation it holds.
{ cat "$WORK/prodc"; cat "$WORK/prodasm"; } | sort -u \
    | grep -v "^$SYMSCAN_EXCLUDE_DIR/" \
    | sed "s|^|$SRC_ROOT/|" | sort > "$WORK/csrc"

njobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
if [ -s "$WORK/csrc" ]; then
    if echo x | xargs -P 2 -n 1 true >/dev/null 2>&1; then
        xargs -P "$njobs" -n 1 "$WORK/cc1.sh" < "$WORK/csrc"
    else
        while IFS= read -r _f; do "$WORK/cc1.sh" "$_f"; done < "$WORK/csrc"
    fi
fi

nsrc=$(wc -l < "$WORK/csrc")
nobj=$(find "$OBJDIR" -name '*.o' 2>/dev/null | wc -l)
if [ -s "$FAILLIST" ] || [ "$nobj" -ne "$nsrc" ]; then
    echo "FAIL: BROKEN SYMBOL SCAN: $nobj of $nsrc product source file(s) compiled."
    [ -s "$FAILLIST" ] && sed 's/^/    /' "$FAILLIST"
    echo "  -> the universe of services is derived from the symbols the compiled"
    echo "     product EXPORTS. A file that does not compile contributes no symbols,"
    echo "     so a service could hide behind a build error. Fix the file, or -- if it"
    echo "     genuinely cannot be compiled here -- extend SYMSCAN_EXCLUDE_DIR and say"
    echo "     why in the header. Do NOT let a compile failure pass silently."
    echo "     (A short count with NO failing file above means two source paths"
    echo "     mangled to the same object name -- also a lost file, also not a pass.)"
    exit 1
fi

: > "$WORK/symbols"
while IFS= read -r _f; do
    _o="$OBJDIR/$(printf '%s' "$_f" | tr -c 'A-Za-z0-9' '_').o"
    nm "$_o" 2>/dev/null | awk -v SRC="${_f#"$SRC_ROOT"/}" '
        NF == 3 && $2 ~ /^[A-Z]$/ && $2 != "U" && $3 ~ /^sys\$/ {
            print "S\t" SRC "\t" $3
        }'
done < "$WORK/csrc" | sort -u >> "$WORK/symbols"

# The headers the product ACTUALLY INCLUDES, read out of the dependency files
# the compiles above just wrote, UNIONED with the header glob for the same
# reason the .c set is a union: a header nothing includes is still source in
# this tree, and a union cannot lose one.
find "$SRC_ROOT/src" "$SRC_ROOT/tools" -name '*.h' 2>/dev/null \
    | sed "s|^$(printf '%s' "$SRC_ROOT" | sed 's/[\\&|]/\\\\&/g')/||" > "$WORK/prodh.raw"
# A dependency file is MAKE syntax, not a path list: `$` is written `$$` and
# the paths are not normalised. Left alone, `src/libvms/../kernel/vms_ioctl.h`
# and `src/kernel/vms_ioctl.h` are two spellings of one file and the scanner
# reads it TWICE -- which is a DEFINED MORE THAN ONCE red manufactured out of
# nothing. Unescape and normalise, and drop anything that climbs out of the
# tree rather than guessing what it meant.
find "$OBJDIR" -name '*.d' -exec cat {} + 2>/dev/null \
    | tr ' \\' '\n\n' \
    | grep '\.h$' \
    | awk -v ROOT="$SRC_ROOT/" '
        function norm(p,   a, m, i, k, o, s) {
            m = split(p, a, "/"); k = 0
            for (i = 1; i <= m; i++) {
                if (a[i] == "" || a[i] == ".") continue
                if (a[i] == "..") { if (k == 0) return ""; k--; continue }
                o[++k] = a[i]
            }
            s = ""
            for (i = 1; i <= k; i++) s = s (i > 1 ? "/" : "") o[i]
            return s
        }
        {
            gsub(/\$\$/, "$")
            if (index($0, ROOT) != 1) next
            r = norm(substr($0, length(ROOT) + 1))
            if (r != "") print r
        }' >> "$WORK/prodh.raw"
sort -u "$WORK/prodh.raw" > "$WORK/prodh"

# ------------------------------------------------------------- fact gather --
# HEADERS ARE SCANNED FOR DEFINITIONS TOO, and that is not tidiness: a
# `static inline sys$foo(...) { ... }` in a header is a service definition the
# .c-only reading never saw. That was a MEASURED live evasion, not a
# theoretical one -- one such definition, with a file-scope static beside it,
# left this gate green with its universe unchanged. The same scanner is used,
# so a header-inline body contributes its definition, its call edges and its
# state references exactly as a .c body does, and its declaration must sit in
# the header that defines it.
cat "$WORK/prodc" "$WORK/prodh" | sort -u > "$WORK/scanset"
: > "$WORK/facts"
while read -r f; do
    [ -f "$SRC_ROOT/$f" ] || continue
    awk -f "$WORK/strip.awk" "$SRC_ROOT/$f" | awk -v SRC="$f" -f "$WORK/scan.awk"
done < "$WORK/scanset" > "$WORK/facts"

: > "$WORK/protos"
while read -r h; do
    [ -f "$SRC_ROOT/$h" ] || continue
    awk -f "$WORK/strip.awk" "$SRC_ROOT/$h" | awk -v SRC="$h" -f "$WORK/proto.awk"
done < "$WORK/prodh" > "$WORK/protos"

if [ ! -s "$WORK/facts" ]; then
    echo "FAIL: BROKEN GATE: the source scan produced no facts at all."
    echo "  -> a gate that reads nothing certifies everything. Fix the scan."
    exit 1
fi
cat "$WORK/symbols" >> "$WORK/facts"

# ------------------------------------------------------------ declarations --
# Read from the RAW files: the declaration lives in a comment by design.
# Headers are read too, because a header-inline definition's declaration has
# to sit in the header that defines it.
: > "$WORK/decl_raw"
while read -r f; do
    [ -f "$SRC_ROOT/$f" ] || continue
    grep -n 'OVMX-USERSPACE:\|OVMX-PARTIAL:\|OVMX-LOCAL:\|OVMX-EXECUTIVE:' "$SRC_ROOT/$f" 2>/dev/null \
        | sed "s|^|$f:|" || true
done < "$WORK/scanset" > "$WORK/decl_raw"

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

# -------------------------------------------------------- the ioctl bridge --
# THE ONE HOP THE CALL GRAPH CANNOT WALK. A service's answer path runs
#
#   sys$readef            src/libvms/syssvc/sys_event.c
#     -> vms_kif_readef   src/libvmssys/vms_kif.c        [ a call ]
#     -> VMS_IOCTL_READEF                                [ the ioctl it issues ]
#     -> vms_ioctl_readef                                [ the executive's handler ]
#     -> src/kernel/vms_eflag.c                          [ where that handler lives ]
#
# and the third hop is an ioctl number crossing into the kernel, not a C call,
# so the call graph stops dead at the wrapper. It is read HERE, out of the
# executive's OWN dispatch switch in src/kernel/vms_module.c: each
# `case VMS_IOCTL_X:` arm is paired with the functions that arm calls. Both
# ends are code -- a case label and a call -- so the bridge is a byproduct of
# the dispatcher, not a name anybody types into a manifest.
#
# It runs AFTER the declarations are parsed, and its refusal is conditional on
# there being an OVMX-EXECUTIVE claim to price: a tree that claims no full
# exemption needs no bridge, and making the refusal unconditional would have
# turned the gate's own floor fixture -- C code, no services, no kernel
# directory -- into a bridge failure instead of the floor red it is there to
# provoke. (Measured: it did exactly that.)
#
# Emits:  X <VMS_IOCTL_NAME> <handler-function>
: > "$WORK/ioctlmap"
DISPATCH="$SRC_ROOT/src/kernel/vms_module.c"
if [ -f "$DISPATCH" ]; then
    awk -f "$WORK/strip.awk" "$DISPATCH" | awk '
        {
            line = $0
            if (line ~ /case[ \t]+VMS_IOCTL_[A-Za-z0-9_]+[ \t]*:/) {
                match(line, /VMS_IOCTL_[A-Za-z0-9_]+/)
                cur = substr(line, RSTART, RLENGTH)
                sub(/^.*case[ \t]+VMS_IOCTL_[A-Za-z0-9_]+[ \t]*:/, "", line)
            } else if (line ~ /(^|[^A-Za-z0-9_])default[ \t]*:/) {
                cur = ""
            }
            if (cur == "") next
            rest = line
            while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
                h = substr(rest, RSTART, RLENGTH)
                sub(/[ \t]*\(/, "", h)
                if (h != "if" && h != "while" && h != "for" && h != "switch" &&
                    h != "return" && h != "sizeof")
                    print "X\t" cur "\t" h
                rest = substr(rest, RSTART + RLENGTH)
            }
        }' | sort -u > "$WORK/ioctlmap"
fi

if [ ! -s "$WORK/ioctlmap" ] && grep -q '	EXECUTIVE	' "$WORK/decl_ok" 2>/dev/null; then
    echo "FAIL: BROKEN IOCTL BRIDGE: no VMS_IOCTL_* dispatch arm was readable in"
    echo "      src/kernel/vms_module.c."
    echo "  -> an OVMX-EXECUTIVE claim is priced on WHICH EXECUTIVE CODE a proven"
    echo "     defect mutates, and the path from a service to that code runs through"
    echo "     the ioctl the executive dispatches. With the bridge unreadable every"
    echo "     answer path would be empty and every claim exempt for want of a"
    echo "     check. A price that cannot be computed must refuse to certify. Fix"
    echo "     the dispatcher or this scan; do not add a skip."
    exit 1
fi

# ---------------------------------------------------------------- analysis --
: > "$WORK/answerpath"
: > "$WORK/usremainder"
awk -F'\t' \
    -v declf="$WORK/decl_ok" \
    -v protof="$WORK/protos" \
    -v out_universe="$WORK/universe" \
    -v out_err="$WORK/errors" \
    -v out_items="$WORK/items" \
    -v out_symonly="$WORK/symonly" \
    -v ioctlf="$WORK/ioctlmap" \
    -v out_apath="$WORK/answerpath" \
    -v out_upath="$WORK/usremainder" \
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
$1 == "S" { exported[$3] = $2; next }
$1 == "V" { if ($3 == "static") vstatic[$2 SUBSEP $4] = 1; else vglobal[$4] = 1; next }
$1 == "C" { ncall++; cf[ncall] = $2; cc[ncall] = $3; ce[ncall] = $4; next }
$1 == "R" {
    if (($2 SUBSEP $4) in vstatic || ($4 in vglobal)) {
        n = (($2 SUBSEP $3) in local) ? $2 ":" $3 : $3
        stateful[n] = 1
        if (statewhy[n] == "") statewhy[n] = $4
    }
    # THE IOCTL A FUNCTION ISSUES, recorded per function. This is the near side
    # of the bridge above: the wrapper names the ioctl number in code, the
    # dispatcher names it in a case label, and the two ends meet on a constant
    # both halves of the product compile against.
    if ($4 ~ /^VMS_IOCTL_/) {
        n = (($2 SUBSEP $3) in local) ? $2 ":" $3 : $3
        if (!((n SUBSEP $4) in ioctlseen)) {
            ioctlseen[n SUBSEP $4] = 1
            ioctlrefs[n] = ioctlrefs[n] " " $4
        }
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

    # THE UNIVERSE ABSORBS WHAT THE PRODUCT EXPORTS. A symbol nm found that no
    # source reading saw is a service exported under a name its source does not
    # spell -- the asm-label rename (vms-f26). It joins the universe under its
    # EXPORTED name, which is the name every caller and every other image sees,
    # and from there it needs a declaration like anything else. Its exec/state
    # columns stay "-": the call graph is keyed on the source spelling and
    # cannot follow the alias (stated in the header, not silently absorbed).
    nexported = 0; nsymonly = 0
    for (s in exported) {
        nexported++
        if (s in universe) continue
        universe[s] = exported[s]
        nuni++; nsymonly++
        printf "%s %s\n", s, exported[s] > out_symonly
    }

    # ------------------------------------------------ THE ANSWER PATH ------
    # For each service claiming OVMX-EXECUTIVE, its answer path is split into
    # TWO DISJOINT HALVES, and the split is the whole point: an OVMX-EXECUTIVE
    # line claims that ALL of the answer comes from the executive, so evidence
    # about the two halves does OPPOSITE things to it.
    #
    #   THE EXECUTIVE HALF -- the executive files that answer it: for every
    #   function reachable from the service, the file holding the handler for
    #   every ioctl that function issues. A defect mutating this half and
    #   reddening the named proof PAYS for the claim.
    #
    #   THE USERSPACE REMAINDER -- the translation unit that DEFINES the
    #   service, which is product code running in the CALLING PROCESS. A defect
    #   mutating THIS half DISQUALIFIES the claim -- see "the price of
    #   exemption" below.
    #
    # Every hop of the executive half is read out of code -- the call edges from
    # the scanner, the ioctl references from the wrapper bodies, the
    # ioctl->handler pairing from the dispatch switch itself, and the
    # handler->file mapping from the definition scan of src/kernel/. No part of
    # either set is read from a manifest.
    #
    # WHY THE REMAINDER IS THE DEFINING UNIT AND NOT "EVERY REACHABLE USERSPACE
    # FILE", which was written first and MEASURED WORSE. The call graph is
    # keyed on function NAMES across the whole product, so its transitive
    # closure over-reaches: the first draft put src/imgact/imgact.c and
    # src/libvms/syssvc/sys_event.c into the remainder of sys$enq, neither of
    # which its answer can come from. An over-broad remainder does not merely
    # look wrong, it MANUFACTURES REFUTATIONS -- a defect entered against
    # imgact.c would have withdrawn the lock manager exemptions for no
    # reason. It would also have caught src/libvmssys/vms_kif.c, the transport
    # that CARRIES the request to the executive: bind-client-no-register edits
    # it and reddens assertions in tests/qemu/test_syssvc_ef_mproc.c, so every
    # event-flag claim would have been disqualified by the wire being cut,
    # which is not the same fact as the answer being computed locally.
    #
    # The defining unit is the principled boundary rather than a convenient
    # one: this register already REQUIRES both halves of a mixture to live in
    # the translation unit that defines the service (LOCAL HALF IN A DIFFERENT
    # TRANSLATION UNIT is a red above). So the defining unit is exactly the
    # code whose remainder a declaration here is able to describe at all.
    #
    # WHAT THAT LEAVES OPEN, stated rather than discovered later: a service
    # that delegates its userspace half to a DIFFERENT file is not refuted by a
    # defect in that file. The register cannot express such a remainder either,
    # so this is a limit of the declaration form, not of this check alone.
    for (i = 1; i <= ne; i++) adj[ef[i]] = adj[ef[i]] " " et[i]
    while ((getline l < ioctlf) > 0) {
        split(l, x, "\t")
        if (x[1] == "X") hnd[x[2]] = hnd[x[2]] " " x[3]
    }
    close(ioctlf)

    for (nm in declfile) {
        if (declkind[nm] != "EXECUTIVE") continue
        if (!(nm in deffile)) continue
        delete reach; delete q; delete apf; delete upf
        qh = 0; qt = 1; q[1] = nm; reach[nm] = 1
        while (qh < qt) {
            qh++
            k = split(adj[q[qh]], nb, " ")
            for (j = 1; j <= k; j++)
                if (nb[j] != "" && !(nb[j] in reach)) { reach[nb[j]] = 1; qt++; q[qt] = nb[j] }
        }
        for (cur in reach) {
            k = split(ioctlrefs[cur], ir, " ")
            for (j = 1; j <= k; j++) {
                if (ir[j] == "") continue
                m = split(hnd[ir[j]], hs, " ")
                for (hi = 1; hi <= m; hi++)
                    if (hs[hi] != "" && (hs[hi] in deffile)) apf[deffile[hs[hi]]] = 1
            }
        }
        upf[deffile[nm]] = 1
        for (f in apf) printf "%s %s\n", nm, f > out_apath
        for (f in upf) printf "%s %s\n", nm, f > out_upath
    }

    # (1) a prototyped service with no definition NAMES what vanished
    for (nm in protoname)
        if (!(nm in universe))
            errors[++nerr] = "PROTOTYPE WITH NO DEFINITION: " nm " is declared in " protofile[nm] \
                             " but nothing in the product compile set defines it.\n" \
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
    # OVMX-EXECUTIVE line naming a proof that exists, forks, CALLS the service,
    # and that an injected defect editing the service answer path reddens.
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
    printf "universe %d\nexec %d\nstate %d\nexec+state %d\nfd-only %d\ndeclared %d\nuserspace %d\npartial %d\nexecutive %d\nprotos %d\nexported %d\nsymonly %d\nitems %d\nerrors %d\n",
           nuni, nexec, nstate, nmix, nfdonly, ndecl, nkind_u, nkind_p, nkind_e, nproto, nexported, nsymonly, nitems, nerr > out_universe
}' "$WORK/facts"

# ------------------------------------------------- the price of exemption --
# An OVMX-EXECUTIVE line is the ONLY full exemption this gate grants, so it is
# the only place worth attacking, and it is priced accordingly. SIX checks:
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
#      which is exactly how the known facades survived;
#   4. it CALLS the service, in code, with comments stripped by the same
#      scanner that reads the product. Not "mentions": CALLS;
#   5. some defect in tests/qemu/facility_defects.sh MUTATES A FILE IN THE
#      EXECUTIVE HALF of the service's answer path and names an assertion that
#      appears verbatim in that proof;
#   6. and NO defect in that manifest mutates the service's USERSPACE
#      REMAINDER. Check 6 is a DISQUALIFIER, not another thing to buy, and it
#      is check 5's mirror image -- see below.
#
# WHY 6 EXISTS AND WHY IT IS A DISQUALIFIER (vms-ecf round 4).
#
# The three declaration kinds say different things and the difference is the
# only reason the register is worth reading:
#
#   OVMX-PARTIAL   -- SOME of the answer is the executive's, and the paired
#                     OVMX-LOCAL half names the rest.
#   OVMX-EXECUTIVE -- ALL of it is. There is no remainder.
#
# Checks 1-5 are all evidence that the executive is REACHED and that mutating
# it is observable. None of that separates the two kinds: a PARTIAL service
# reaches the executive too, by definition, and its proof calls it too. So a
# service that was already OVMX-PARTIAL satisfied every one of checks 1-5
# BEFORE ANY EDIT, and the upgrade to a full exemption cost exactly one thing:
# DELETING ITS OWN OVMX-LOCAL HALF -- the half that says a remainder exists.
# Measured, on this branch, by an adversary: flipping sys$creprc's declaration
# block and nothing else printed PASS with an extra full exemption. Worse than
# free: the defect that paid for it, creprc-handshake-eintr, edits
# src/libvms/syssvc/sys_process.c, which is PURE USERSPACE -- so the evidence
# admitted in payment was evidence that the deleted half was TRUE.
#
# Hence the rule this file now enforces: EVIDENCE ABOUT THE USERSPACE
# REMAINDER RUNS THE OTHER WAY. A defect that changes an observable public-API
# answer by mutating code that runs in the CALLING PROCESS is evidence that
# part of the answer is computed there, which is the exact negation of "all of
# it is the executive's". It cannot pay for the claim; it refutes it. The
# remedy the gate prints is OVMX-PARTIAL + OVMX-LOCAL, never deletion.
#
# WHAT CHECK 6 IS AND IS NOT WORTH, said here rather than left to be assumed:
#
#   - It is EVIDENCE-BASED AND ASYMMETRIC. It can find a proven remainder; it
#     can never establish that there is none. A service whose userspace half
#     nobody has written a defect for passes check 6 because nothing refutes
#     it, not because anything confirms it. "No proven userspace remainder" is
#     the strongest thing this gate says, and the printed report says exactly
#     that rather than "wholly executive-resident".
#   - It is FILE-GRANULAR, in both directions, which is the same coarseness
#     check 5 already had (a defect in the executive event-flag file pays for
#     every service whose answer path includes it, and does not separate
#     sys$readef from sys$setef). A defect entered against one service in a
#     shared translation unit disqualifies its neighbours' full exemptions too.
#     That is conservative in the safe direction -- it can only ever weaken a
#     claim, never strengthen one -- and it is not silently coarse: the run
#     prints which defect and which file did it.
#   - It is NOT SCOPED TO THE NAMED PROOF, deliberately. Scoping it there --
#     "the proof you cite is itself reddened by mutating your userspace code"
#     -- is a tighter argument and it would be PROOF-SHOPPABLE: name a
#     different qemu suite that calls the service and the disqualifier goes
#     quiet while the remainder stays exactly where it was.
#
# WHY 4 AND 5 ARE WORDED THAT WAY, MEASURED TWICE (vms-ecf).
#
# ROUND 1: checks 1-4 were all source greps and check 4 was
# `grep -qF "$service" "$proof"` -- WHICH A COMMENT SATISFIES. The exemption
# was buyable for two lines: one ignored `(void)vms_kif_readef(...)` in
# sys$gettim (whose answer is still 100% clock_gettime) plus the single comment
# line `/* also covers sys$gettim */` appended to an otherwise untouched proof.
#
# ROUND 2 answered that with "the proof must contain an assertion that NAMES
# THE SERVICE and that facility_defects.sh has proven reddenable" -- AND THAT
# WAS THE SAME BUG IN PROSE. The assertion text is a string living in exactly
# two places, the proof and the manifest, both editable in one commit:
# appending ", clock via sys$gettim" to an already-proven assertion in both
# files paid the price for sys$gettim in full, and the gate printed it as a
# paid claim. IF EDITING AN ASSERTION'S WORDS IS HOW A SERVICE BECOMES
# COVERED, COVERAGE IS A NAMING CONVENTION AND NOT A MEASUREMENT. Worse, that
# is the instrument round 2 used on its own sys$readef gap.
#
# So the service is no longer bound to the mutation by anything anyone types.
# It is bound by WHICH CODE THE MUTATION EDITS. The executive half of the
# answer path (computed above, printed below) is derived in four hops that are
# all code: the call graph from the service; the VMS_IOCTL_* constants those
# functions reference; the executive's own `case VMS_IOCTL_X:` dispatch arms;
# and the file each handler is defined in. A defect pays for a claim when its
# `targets` -- the files it edits, which the manifest already carries for its
# own injection -- land inside that set, and when one of its
# require_fail/knock_on_fail texts is in the named proof. The assertion never
# has to mention the service, and the round-2 settling command -- re-wording a
# proven assertion in the proof and the manifest together so that it does -- is
# a RED control next door.
#
# THE PRICE IS STILL PURCHASABLE AND THIS FILE DOES NOT CLAIM OTHERWISE. What
# it deliberately does NOT carry is a number: FOUR consecutive revisions of
# this comment stated a measured per-site cost ("one ignored line", "a comment",
# "a re-worded assertion", "three edits") and every one of them was broken by
# execution within days, because a price measured against one service is not
# the price of the class. The residuals that are open, and where each is
# tracked, are named instead of costed:
#
#   - "the proof CALLS the service" is a syntactic proxy for "the answer came
#     from there", and every syntactic proxy is purchasable. Closing it needs
#     per-assertion service attribution measured AT RUNTIME by
#     tests/qemu/run_facility_negctl.sh, the only instrument in this area that
#     executes anything (vms-d89's residual, tracked as vms-38c).
#   - `targets` in the manifest is written by hand, so BOTH check 5 and check 6
#     read a human-written file list. Check 5's exposure is a fabricated entry
#     (answered by run_facility_negctl.sh, which injects each defect in QEMU
#     and requires the red set to EQUAL require_fail + knock_on_fail exactly).
#     Check 6's exposure is the mirror: DELETING a defect entry removes the
#     disqualifying evidence, and nothing in this tree floors the manifest
#     against deletion. That is a real, open residual and it is recorded on
#     vms-38c rather than costed here.
FDMANIFEST="$SRC_ROOT/tests/qemu/facility_defects.sh"

# The manifest, read once: one line per defect, "<name>|<targets>", with each
# defect's assertion texts (require_fail + knock_on_fail) in its own file.
: > "$WORK/defects"
mkdir -p "$WORK/dtext"
if [ -f "$FDMANIFEST" ]; then
    for _d in $(sh "$FDMANIFEST" list 2>/dev/null); do
        case "$_d" in *[!A-Za-z0-9_-]*) continue ;; esac
        _tg=$(sh "$FDMANIFEST" field "$_d" targets 2>/dev/null | tr '\n' ' ')
        {
            sh "$FDMANIFEST" field "$_d" require_fail 2>/dev/null
            sh "$FDMANIFEST" field "$_d" knock_on_fail 2>/dev/null
        } | grep -v '^[[:space:]]*$' > "$WORK/dtext/$_d"
        [ -s "$WORK/dtext/$_d" ] || continue
        [ -n "$(printf '%s' "$_tg" | tr -d ' ')" ] || continue
        printf '%s|%s\n' "$_d" "$_tg" >> "$WORK/defects"
    done
fi

: > "$WORK/backed"
mkdir -p "$WORK/callcache"
while IFS="$(printf '\t')" read -r pfile pkind pname pitem pproof; do
    [ "$pkind" = "EXECUTIVE" ] || continue
    if [ ! -s "$WORK/defects" ]; then
        printf 'BROKEN PRICE CHECK: %s (%s) cannot be priced\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> tests/qemu/facility_defects.sh named no defect with both a target\n' >> "$WORK/errors"
        printf '     file and a proven-reddenable assertion, so every OVMX-EXECUTIVE claim\n' >> "$WORK/errors"
        printf '     here would be exempt for want of a check. A price that cannot be\n' >> "$WORK/errors"
        printf '     computed is not a price.\n' >> "$WORK/errors"
        continue
    fi
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
    # (4) THE PROOF CALLS THE SERVICE. Read with the scanner used on the
    # product, so comments are gone before anything is matched: the ROUND-1
    # buy-off was `/* also covers sys$gettim */`, and a mention in a comment is
    # not a proof about anything. Cached per proof file -- the same proof backs
    # every service of a facility.
    _pk=$(printf '%s' "$pproof" | tr -c 'A-Za-z0-9' '_')
    if [ ! -f "$WORK/callcache/$_pk" ]; then
        awk -f "$WORK/strip.awk" "$SRC_ROOT/$pproof" \
            | awk -v SRC="$pproof" -f "$WORK/scan.awk" \
            | awk -F'\t' '$1 == "C" { print $4 }' | sort -u > "$WORK/callcache/$_pk"
    fi
    if ! grep -qxF "$pname" "$WORK/callcache/$_pk"; then
        printf 'EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> proof=%s never calls %s in code (comments stripped), so it is not a\n' "$pproof" "$pname" >> "$WORK/errors"
        printf '     proof about it. A mention can be a comment; this gate measured that\n' >> "$WORK/errors"
        printf '     exact buy-off (vms-ecf).\n' >> "$WORK/errors"
        continue
    fi
    # (6) NO DEFECT MUTATES THE USERSPACE REMAINDER. Checked BEFORE the payment
    # check and reported separately, because it is not a missing payment: it is
    # evidence against the claim itself. A defect here is a mutation of code
    # running in the calling process that the manifest says changes an
    # observable assertion -- i.e. a demonstration that part of the answer is
    # computed outside the executive. No proof, however good, makes that go
    # away, so this does NOT consult the named proof (see the header: scoping
    # it there would be proof-shoppable).
    _upath=$(awk -v n="$pname" '$1 == n { printf " %s", $2 }' "$WORK/usremainder" 2>/dev/null)
    _refuters=""
    while IFS='|' read -r _d _tg; do
        for _t in $_tg; do
            case "$_upath " in
                *" src/$_t "*) _refuters="$_refuters $_d($_t)"; break ;;
            esac
        done
    done < "$WORK/defects"
    if [ -n "$_refuters" ]; then
        printf 'EXECUTIVE DECLARATION REFUTED BY A DEFECT IN ITS USERSPACE REMAINDER: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> OVMX-EXECUTIVE claims the WHOLE answer comes from the executive, but\n' >> "$WORK/errors"
        printf '     tests/qemu/facility_defects.sh carries a mutation of code that runs in\n' >> "$WORK/errors"
        printf '     the CALLING PROCESS which changes an assertion it names:\n' >> "$WORK/errors"
        for _r in $_refuters; do
            printf '       %s\n' "$_r" >> "$WORK/errors"
        done
        printf '     The userspace remainder is:%s\n' "${_upath:- (empty)}" >> "$WORK/errors"
        printf '     That is evidence FOR a remainder, so it cannot pay for a claim that\n' >> "$WORK/errors"
        printf '     there is none. Say OVMX-PARTIAL and name the local half with an\n' >> "$WORK/errors"
        printf '     OVMX-LOCAL line beside it -- do NOT delete either. Deleting the local\n' >> "$WORK/errors"
        printf '     half to reach a full exemption is the buy-off this check exists for.\n' >> "$WORK/errors"
        printf '%s\t%s\t%s\n' "$pname" "$pproof" "REFUTED:$(printf '%s' "$_refuters" | sed 's/^ //')" >> "$WORK/backed"
        continue
    fi
    # (5) A DEFECT THAT MUTATES THE EXECUTIVE HALF AND REDDENS THIS PROOF.
    _apath=$(awk -v n="$pname" '$1 == n { printf " %s", $2 }' "$WORK/answerpath" 2>/dev/null)
    # COMMENT-STRIPPED, for the same reason check 4 is: the manifest claims
    # these texts go RED when the defect is injected, and a text sitting in a
    # comment cannot go red. Round 1 of this item was bought with a comment.
    _pcontent=$(awk -f "$WORK/strip.awk" "$SRC_ROOT/$pproof" 2>/dev/null)
    _payers=""
    while IFS='|' read -r _d _tg; do
        _hit=0
        for _t in $_tg; do
            case "$_apath " in *" src/$_t "*) _hit=1; break ;; esac
        done
        [ "$_hit" -eq 1 ] || continue
        while IFS= read -r _a; do
            [ -n "$_a" ] || continue
            case "$_pcontent" in
                *"$_a"*) _payers="$_payers $_d"; break ;;
            esac
        done < "$WORK/dtext/$_d"
    done < "$WORK/defects"
    if [ -z "$_payers" ]; then
        printf 'EXECUTIVE DECLARATION NO INJECTED DEFECT IN ITS ANSWER PATH REDDENS: %s (%s)\n' "$pname" "$pfile" >> "$WORK/errors"
        printf '  -> no defect in tests/qemu/facility_defects.sh both EDITS a file in %s\n' "$pname" >> "$WORK/errors"
        printf '     EXECUTIVE answer path and names an assertion that appears in\n' >> "$WORK/errors"
        printf '     proof=%s.\n' "$pproof" >> "$WORK/errors"
        printf '     The executive answer path is:%s\n' "${_apath:- (empty)}" >> "$WORK/errors"
        printf '     Claiming the WHOLE answer comes from the executive costs a mutation\n' >> "$WORK/errors"
        printf '     to the executive code that answers, proven to redden that proof --\n' >> "$WORK/errors"
        printf '     not a wording that mentions the service, and not a mutation of the\n' >> "$WORK/errors"
        printf '     service own userspace code, which argues the other way. Add a defect\n' >> "$WORK/errors"
        printf '     entry for the facility, or say OVMX-PARTIAL and name the half that is\n' >> "$WORK/errors"
        printf "     not the executive's.\\n" >> "$WORK/errors"
    fi
    printf '%s\t%s\t%s\n' "$pname" "$pproof" "$(printf '%s' "$_payers" | sed 's/^ //')" >> "$WORK/backed"
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
        universe)   echo "  $v sys\$ services defined in the product compile set"; nuniverse=$v ;;
        protos)     echo "  $v sys\$ prototypes declared in the headers it compiles against" ;;
        exec)       echo "  $v reach the executive (transitive call to a vms_kif_* entry point)" ;;
        state)      echo "  $v touch retained per-process state (file-scope object, transitively)" ;;
        exec+state) echo "  $v do BOTH -- touch a file-scope object AND reach the executive (evidence, not a verdict)" ;;
        fd-only)    echo "    $v of those touch NOTHING but vms_kif.c's /dev/vms descriptor -- i.e. the" ;
                    echo "       'state' they hold is the executive's CONNECTION, not a process-local answer." ;;
        declared)   echo "  $v carry a well-formed declaration of where their answer comes from" ;;
        userspace)  echo "    $v say OVMX-USERSPACE -- no part of the answer is the executive's" ;;
        partial)    echo "    $v say OVMX-PARTIAL   -- a named part is, a named part is not" ;;
        executive)  echo "    $v say OVMX-EXECUTIVE -- all of it is, and name a proof that CALLS them" ;
                    echo "       and that an injected defect in the EXECUTIVE half of their answer" ;
                    echo "       path is known to redden, with no defect in their userspace remainder" ;;
        exported)   echo "  $v sys\$ symbol(s) are EXPORTED by the compiled product (nm, $nobj object file(s))" ;;
        symonly)    echo "    $v of those the source scan never saw -- exported under a name the source" ;
                    echo "       does not spell (asm label / alias). They are in the universe under their" ;
                    echo "       EXPORTED name; their exec/state columns cannot follow the alias." ;;
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

if [ -s "$WORK/symonly" ]; then
    echo
    echo "  exported under a name the source does not spell:"
    sed 's/^/      /' "$WORK/symonly"
fi

# WHAT EACH FULL EXEMPTION IS ACTUALLY PAYING, derived per run. A claim resting
# on ONE mutation-backed assertion is a claim resting on one assertion; the
# number says so without anybody maintaining it.
if [ -s "$WORK/backed" ]; then
    echo
    echo "  the OVMX-EXECUTIVE claims, and the injected defect(s) that pay for each --"
    echo "  a defect that EDITS a file in the EXECUTIVE half of the service's answer"
    echo "  path AND names an assertion that appears in the proof it points at. Read"
    echo "  a paid claim as 'no proven userspace remainder', not as 'proven to have"
    echo "  none': check 6 can refute a full exemption, never confirm one."
    sort "$WORK/backed" | while IFS="$(printf '\t')" read -r bn bp bd; do
        printf '      %-22s %s\n' "$bn" "$bp"
        case "$bd" in
            REFUTED:*)
                for _b in $(printf '%s' "${bd#REFUTED:}"); do
                    printf '          REFUTED by %-31s -- a defect in its USERSPACE remainder\n' "$_b"
                done
                continue ;;
        esac
        for _b in $bd; do
            printf '          paid by %-34s (edits %s)\n' "$_b" \
                "$(sh "$FDMANIFEST" field "$_b" targets 2>/dev/null | tr '\n' ' ')"
        done
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
    echo "  FAIL: THE FLOOR: zero sys\$ services found in the product compile set."
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
