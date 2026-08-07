#!/bin/sh
#
# test_kif_caller_census.sh - standing gate (rd vms-7fb): for every
# kernel-interface entry point, either THE PRODUCT EMITS A CALL TO IT or it is
# DECLARED UNWIRED against an item. Nothing ships a wrapper that nothing calls.
#
# READ THE VERB LITERALLY. "The product emits a call" is what this gate
# measures and the strongest thing it can say: cmake says which translation
# units the product compiles, the preprocessor says which lines survive, the
# compiler says which calls it actually emitted, and a call graph says whether
# the function holding one is reachable from a root. It does NOT say the call
# runs. That is rd vms-d33's question and it belongs to the QEMU suites.
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
#   - REACHABILITY, not mere presence, ON BOTH SIDES OF THE INTERFACE.
#     Inside vms_kif.c, a call counts only when the calling function is itself
#     reachable from a caller outside vms_kif.c. That is how
#     vms_kif_open/register/kerr_to_ss legitimately pass: nothing outside names
#     them, but kif_bind() does, and kif_bind() is reached from every wired
#     wrapper through KIF_CALL -> kif_call. A family that only calls itself is
#     NOT reachable and does not pass.
#     IN THE PRODUCT, the same rule now applies (rd vms-c13, section 2'): a call
#     counts only when the function CONTAINING it is reachable from a root --
#     main(), or something a header the build compiles declares -- following
#     calls and address-taking. Until that landed, a call anywhere in any
#     product translation unit counted, so a function nothing calls was a
#     product path, and TWO edits in two already-built files moved the census
#     from 31/44 reached to 32/44 with rc=0.
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
# declaration to be deleted in the same commit. Negative control 9 is the
# disproof of that half; it is a checked relation, not a guarantee that the
# list cannot rot by some route neither direction looks at.
#
# THE CITED ITEM IS A LABEL, NOT A CHECK (rd vms-dc7, tearing down vms-8cc).
#
# This gate used to resolve every OVMX-UNWIRED id against a committed ledger
# snapshot of rd (tools/gen_rd_citations.py -> tracking/rd-citations.tsv) and
# red on a fabricated or closed id. That checker is gone: it never ran
# anything against a real executive, could not see rd going stale between
# snapshot regenerations, and the property it verified -- whether a comment
# still points at a currently-open tracking ticket -- has no bearing on
# whether the entry point is actually wired, which is what this gate exists to
# measure by other means (the universe/floor machinery below). The id is kept
# as a required but unverified label, same reasoning as the userspace service
# register (tests/integration/test_userspace_service_register.sh).
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
#     out of the namespace. The static helpers this admits -- kif_bind,
#     kif_call, kif_wait_call, getjpi_common -- are all reached from wired
#     wrappers and need no declaration. (kif_wait_call is PR #22's addition,
#     the blocking-call counterpart to kif_call for the three blocking
#     event-flag services; vms_kif_alloc_op is a fifth static but is NOT in
#     this set -- its only two callers, vms_kif_alloc and vms_kif_dalloc, are
#     themselves unwired today (vms-dv1), so it carries its own OVMX-UNWIRED
#     declaration rather than being silently reached.)
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
#     -- by deleting each of the file-scope definitions in vms_kif.c in turn,
#     with its prototype and its declaration, and running this gate. THIS
#     COUNT MOVES WHEN THE FILE DOES -- re-run the brute force, don't recite
#     this number: at 43 file-scope definitions (vms-7fb, after PR #22 /
#     e5cf411 wired the event-flag family and added the kif_wait_call static),
#     37 go RED and SIX are a silent PASS: vms_kif_open, vms_kif_close,
#     vms_kif_kerr_to_ss, the static vms_kif_alloc_op, and the statics
#     kif_call and kif_wait_call.
#
#     FLOOR-EXEMPT, DERIVED NOT RECITED. This gate now PRINTS, at every run,
#     the set of definitions whose own body issues no VMS_IOCTL_* opcode and
#     names no VMS_*_SEL_* selector -- see "floor-exempt" in the output below,
#     computed by opcode_owners() rather than hand-counted here. That is the
#     fix for THIS paragraph's own defect: an earlier revision said "the first
#     four are exactly" that set -- open/close manage the fd, kerr_to_ss maps
#     an errno, alloc_op takes its opcode as a PARAMETER -- and named kif_call
#     and kif_wait_call as fitting "the same shape" without ever counting
#     kif_bind, whose body also never spells an opcode or a selector. "Exactly"
#     was false the moment kif_bind was checked -- read the count from the
#     "floor-exempt" line this gate prints when it runs, not from a number
#     recited here, which is exactly the different, still-hand-measured fact
#     the next paragraph is about and must not be confused with this one.
#     THIS PARAGRAPH WAS DESCRIBING A NECESSARY CONDITION, NOT THE SUFFICIENT
#     ONE, and the two are not the same list. "Issues no opcode, names no
#     selector" is necessary for a definition's deletion to be uncaught by the
#     floor, but kif_bind fails the SUFFICIENT test anyway: deleting its
#     definition deletes the calls inside it, which strands vms_kif_open and
#     vms_kif_register's only path to being REACHED -- a RED at the
#     undeclared-entry-point property (section 5), not at the floor. NOT
#     vms_kif_kerr_to_ss: it is ALSO called directly from inside kif_call and
#     kif_wait_call (both reached independently, through KIF_CALL/KIF_WAIT_CALL
#     in every wired wrapper), so it keeps a second path to REACHED and stays
#     green when kif_bind is deleted -- MEASURED by actually deleting
#     kif_bind's definition and running this gate: 42 entry points, 21
#     reached, RED naming exactly vms_kif_open and vms_kif_register, nothing
#     else -- vms_kif_kerr_to_ss's caller in kif_call/kif_wait_call is a
#     second door, not a stale one, so it does not go undeclared alongside
#     the other two.
#     That is exactly why kif_bind is absent from the SIX-member silent-PASS
#     set two paragraphs up: that set answers the sufficient question and is
#     MEASURED, by actually deleting each definition in turn and running this
#     gate -- there is no cheap static check for "does something else's
#     reachability depend on me", so unlike the floor-exempt set above -- which
#     opcode_owners() derives and prints every run -- this one cannot be
#     reduced to a runtime print without literally running the brute force on
#     every invocation. Re-run it after any change to the static call graph
#     in vms_kif.c; do not assume the silent-PASS set still reads the way the
#     last brute force found it.
#
#     Of the floor-exempt set, kif_call and kif_wait_call additionally fit a
#     second shape (opcode taken as a parameter, like alloc_op): pre-merge kif_call
#     was ALSO the sole path to kif_bind(), so deleting it went RED for a
#     second, unrelated reason -- it broke the bind chain, not the floor.
#     PR #22 added kif_wait_call as a second static that calls kif_bind()
#     directly for the three blocking event-flag services, so the bind chain
#     now survives kif_call's deletion too, and kif_call joined the
#     silent-PASS set. That is a real, tree-shape-dependent fact, not a defect
#     in this gate.
#     The claim this floor can support is therefore the narrow one, and it is
#     the one to quote: NO WRAPPER THAT ISSUES AN OPCODE OR NAMES A SELECTOR
#     can have its definition deleted without a RED. It is NOT "no wrapper".
#     (An earlier revision of this comment said "no wrapper", and the very next
#     paragraph contradicted it. Execution settled it. In the one file whose
#     declared subject is untested assertions, do not write an emphatic claim
#     here you have not run.)
#     The residual risk is low but it is REAL, not zero, AND IT IS UNEVEN
#     across the six -- an earlier revision of this sentence said "all six are
#     called today, so deleting any of them fails to compile", stated as one
#     uniform protection. It is not. FIVE of the six (vms_kif_open,
#     vms_kif_kerr_to_ss, the static vms_kif_alloc_op, and the statics
#     kif_call and kif_wait_call) are called from inside vms_kif.c itself,
#     which the PRODUCT build always compiles, so deleting any of those five
#     fails the PRODUCT build. vms_kif_close is NOT protected that way:
#     `grep -rn vms_kif_close src/ tools/` finds nothing outside vms_kif.c/.h
#     but its own definition, prototype and this comment -- its only real
#     callers anywhere are six files under tests/qemu/. Deleting
#     vms_kif_close's definition compiles the PRODUCT cleanly and breaks only
#     the QEMU test harness, not the product build; re-run that grep if this
#     drifts, do not recite "all six" again. There is deliberately NO escape
#     hatch for an orphaned opcode or selector. If you add an ioctl or a
#     selector to vms.ko, land its wrapper in the same commit.
#
# WHAT THIS GATE DOES NOT SEE, stated so its PASS is never read as more than it
# is. Since vms-c79 it is a CONFIGURE, A PREPROCESS AND A COMPILE -- it asks
# cmake what the product compiles, asks the compiler what survives the
# preprocessor, and asks the object files which calls the compiler actually
# EMITTED -- but it is still not a link and not an execution:
#
#   - AN EMITTED CALL IS NOT AN EXECUTED CALL, and this is the residual that
#     matters. Section 0'' moved the question from "does the source text
#     contain a call" to "did the compiler emit one", which kills the whole
#     class of branches a compiler can PROVE dead without the gate learning a
#     single syntactic form: `if (0)`, `while (0)`, `if (1 == 2)`, a
#     constant-folded flag, and the spellings nobody has written yet all die at
#     the same door. IT DOES NOT KILL A BRANCH THAT IS MERELY FALSE AT RUNTIME,
#     and the price of that is MEASURED, not estimated:
#         src/vmsdcl/dcl_cmd_show.c, in the body of cmd_show():
#             if (cmd->param_count < 0) { (void)vms_kif_chkpriv(0); }
#         src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#     rc=0, "44 entry points -- 32 the product emits a call to, 12 with no
#     product path", PASS. TWO edits across TWO files -- exactly what the
#     `if (0)` form used to cost. cmd->param_count is never negative, but the
#     compiler cannot prove it, so it emits the call and this gate credits it.
#     Negative control 46 pins that as INTENDED rather than as an oversight: a
#     reader that guessed at runtime values would red on every defensive branch
#     in the tree. Execution is what closes this, and execution is rd vms-d33's
#     question, not a reader's.
#   - THE ROOT RULE'S "prototyped in a header" CLAUSE IS SCOPED TO A NON-static
#     DECLARATION (rd vms-41b). A declaration in a header the build compiles
#     used to seed a root regardless of storage class, because that is exactly
#     what an exported library entry point looks like and the P-record reading
#     did not carry the static/extern qualifier at all. MEASURED, the recipe
#     that bought it on this tree before the fix: ONE insertion into
#     src/vmsdcl/include/dcl/dcl_cmd.h,
#         #include "vms_kif.h"
#         static void ovmx_dead_helper(void);
#         static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }
#     plus retiring vms_kif_chkpriv's OVMX-UNWIRED token -- TWO edits across TWO
#     files, one of them a public header, `static` chosen so the multi-includer
#     header would still link. A `static` declaration in a header can NEVER be
#     the exported-API shape rule 2 exists for: each includer gets its own
#     private symbol, not a shared entry point. RE-MEASURED against this
#     revision: rc=1, naming vms_kif_chkpriv -- see negative control 48.
#     THE SAME SHAPE THROUGH vms_kif.h ITSELF DOES NOT WORK EITHER, which is
#     worth knowing before anyone tries it as a shortcut: section 0' excludes
#     $KIF_H from the product text, so a declaration written beside the retired
#     token -- two edits in ONE file -- makes no root regardless of storage
#     class. MEASURED: rc=1, naming vms_kif_chkpriv.
#     What would close the class of a genuinely NON-static header declaration
#     with no in-tree caller is execution evidence. It is not a compile.
#   - A FILE-SCOPE REFERENCE THIS READER CANNOT ATTRIBUTE CREDITS NOTHING. An
#     address taken at file scope outside every brace, in a declarator shape
#     neither $pendobj nor $pendarr names, belongs to no node and therefore
#     makes nothing reachable. That is the safe direction for an evasion and
#     the WRONG one for correct code: it under-counts, which shows up as a RED
#     naming an entry point, never as a silent PASS. If one appears, teach the
#     reader that declarator; do not relax the rule.
#   - It says NOTHING about whether the facility behind the call is real. An
#     entry point can be wired to a per-process fake and pass here. That is the
#     A-writes/B-reads question (CLAUDE.md Rule 11) and it belongs to the QEMU
#     suites and the veracity passes, not to a grep.
#   - The kernel floor counts a MENTION of VMS_IOCTL_* / VMS_*_SEL_* in
#     vms_kif.c, not a proof that the value reaches an ioctl. A bare
#     `(void)VMS_IOCTL_DEVSCAN;` would satisfy it. Deciding that a value
#     actually flows to KIF_CALL is data flow, not a scan, and a census that
#     guessed would be inventing an answer. That hole is unchanged by vms-e2b
#     and was USED to close a control: an exfiltrated wrapper plus
#     `enum { kif_floor_ref = (int)VMS_IOCTL_TTSETMODE };` restored the floor to
#     33/33 while the wrapper's body had left the file.
#   - THE FLOOR IS READ FROM RAW vms_kif.c TEXT, DELIBERATELY NOT PREPROCESSED,
#     and this is the one reading vms-e2b left alone. The universe no longer
#     needs the floor to notice exfiltration -- the entry point simply does not
#     leave -- but keeping the floor raw preserves an independent, cheaper red:
#     an opcode-issuing wrapper whose body moves out of vms_kif.c strands its
#     opcode HERE even when the universe survives it. THE PRICE, stated rather
#     than hidden and it is the same shape as the naming-convention price
#     above: legitimately reorganising vms_kif.c into #included pieces is a
#     floor RED, because an opcode must be spelled in the interface source file
#     itself. If that reorganisation is ever wanted, teach the floor the
#     private-origin reading section 0' already computes -- and re-run the
#     negative controls, because at least one of them turns on this rule.
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
#   - A WITHIN-NAMESPACE COLLISION IS NOT CAUGHT WHEN THE SURVIVOR IS
#     UNDECLARED. Renaming a wrapper's prototype AND definition onto the exact
#     name of an existing vms_kif_ SIBLING is a route negative control 20 never
#     tried (20 collides with a name OUTSIDE the namespace). sort -u collapses
#     the two identically-named entries into one line in both $WORK/protos and
#     $WORK/defs_extern, so the renamed-away entry point leaves the universe
#     with no orphan and no floor violation -- the same silent shrink 13-20
#     close by other doors. Whether it is CAUGHT depends entirely on the
#     survivor: if the survivor carries an OVMX-UNWIRED declaration, the
#     transferred caller makes it go STALE (control 9's property, reached by
#     this new route -- see negative control 21). If the survivor has NO
#     declaration, nothing fires. MEASURED, not asserted (vms-7fb r7):
#     renaming vms_kif_kerr_to_ss onto vms_kif_open -- both reached today,
#     neither declared -- drops this gate from 43 entries / 24 reached to 42
#     entries / 23 reached with rc=0, reporting PASS having silently lost an
#     entry point. This residual is recorded here, not fixed here: closing it
#     changes this gate's pass/fail behaviour on trees that pass today, which
#     is outside this round's authorization. Tracked for a follow-up item;
#     see this round's report to the operator.
#   - A FABRICATED CALLER GENUINELY IN THE BUILD IS NO LONGER ENOUGH BY
#     ITSELF, and the change is measured, not argued. This bullet used to
#     record the vms-e2b residual: put the fabricated caller in a file, ADD
#     THAT FILE TO src/libvmssys/CMakeLists.txt, and the census read 44 entry
#     points / 32 reached / 12 unwired with rc=0. RE-MEASURED against this
#     revision, the same three edits -- kif_orphan_test.c containing
#     `void kif_orphan_fn(void) { (void)vms_kif_chkpriv(0); }`, its line in
#     CMakeLists.txt, and the retired OVMX-UNWIRED token -- are now rc=1 at
#     44 / 31 / 13, naming vms_kif_chkpriv, with the call graph line printing
#     "1 of 49 call(s) ... credit NOTHING: kif_orphan_fn". Compiling and
#     linking a function does not make it reachable; a header has to declare
#     it, or something reachable has to call it. THAT IS THE NEW PRICE, NOT A
#     WALL -- see the root-rule bullet above for the two-edit form that still
#     buys it, and note that this gate asks "does the product emit a call to
#     it", not "is that call executed" (rd vms-d33).
#   - THE EMITTED-CALL EVIDENCE SAYS NOTHING ABOUT ASSEMBLY. The three .S files
#     under src/libvmssys/arch/x86_64 are in the compile database and ARE
#     compiled here, but hand-written assembly puts its functions in whatever
#     section it declares, not in `.text.<name>`, so section 0'' has no
#     evidence about them and their edges are credited from the source reading
#     unchanged. That is the "no evidence" door, working as designed, and it is
#     stated here rather than discovered later. If an entry point is ever
#     called from assembly, this gate credits it on the source reading alone.
#   - THE EVIDENCE IS KEYED ON A FUNCTION NAME, NOT ON (object, name). Two
#     statics of one name in two translation units share one evidence entry, so
#     a call emitted from either credits both. The call graph still keeps them
#     as separate nodes -- this only ever ADDS credit, never removes it, so it
#     is an under-fire and not a hole that certifies anything new.
#   - IT IS A COMPILE, SO A TREE THAT DOES NOT COMPILE IS A REFUSAL. That is a
#     behaviour change, and it is deliberate: the previous revision only
#     required each translation unit to PREPROCESS, and three negative controls
#     in the sibling suite were running against sandbox trees that preprocessed
#     and could never have built (two definitions of one name; a `static`
#     colliding with a header's extern declaration; an .inc using another
#     file's privates). All three were broken FIXTURES that nobody could see.
#     A census of a tree that does not build is not a measurement of anything.
#   - THE THREE TOOLCHAIN FACTS SECTION 0'' RESTS ON WERE MEASURED ON gcc 13.3
#     / x86_64 AND ARE NOT PORTABLE BY ASSUMPTION. Per-function sections,
#     intra-TU calls producing a section-symbol relocation, and dead-branch
#     elimination happening at -O0 as well as -O2 are all things this gate
#     REQUIRES. It fails loudly if the first one stops holding (no `.text.<name>`
#     sections at all is an explicit refusal), but the other two would degrade
#     QUIETLY, in the under-firing direction. Re-measure them on any other
#     compiler or target before trusting a green from one.
#   - EXFILTRATION COMPOSED WITH A RENAME STILL LEAVES THE UNIVERSE, and the
#     recipe is written down here because a residual nobody can reproduce is
#     not a disclosure. Move a wrapper's body into a .inc; mark it `static`
#     AND rename it out of the vms_kif_ namespace; #include that .inc from
#     vms_kif.c AND from one other product file, so it is neither
#     interface-private nor namespaced and every one of section 1's three
#     definition readings drops it; delete the prototype; retire the
#     declaration; add `enum { r = (int)VMS_IOCTL_TTSETMODE };` to vms_kif.c so
#     the raw floor stays satisfied. Seven edits across three files, and the
#     product still LINKS because the function is static in both units.
#     MEASURED: 44 entry points -> 43, rc=0, PASS. Uncomposed -- keeping the
#     name -- the same evasion is a RED at 44 entries; the name filter on the
#     third reading is what makes the rename necessary. NOT CLOSED HERE. What
#     would close it is reading the definition side of the universe from the
#     interface TU with no name filter and no private-origin restriction, which
#     needs a way to tell vms_syscall.h's 47 static inlines from an interface
#     wrapper that this round did not find.
#
# If you are here because this failed: do NOT add a declaration to make it pass
# unless the entry point genuinely has no product path yet AND you have an item
# for it. The declaration says "this facility is not wired" out loud. Adding one
# for something you just shipped a caller-less wrapper for is the defect, spelled.
#
# Usage: test_kif_caller_census.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
# CANONICAL, and this is load-bearing rather than tidy: the compile database
# names every file by the absolute, symlink-resolved path cmake used, and the
# build set is matched against it by string equality. A relative or symlinked
# SRC_ROOT would match nothing and the census would read an empty tree.
if ! SRC_ROOT=$(cd "$SRC_ROOT" 2>/dev/null && pwd -P); then
    echo "FAIL: cannot enter the source root given on the command line"
    exit 1
fi
KIF_H="$SRC_ROOT/src/libvmssys/vms_kif.h"
KIF_C="$SRC_ROOT/src/libvmssys/vms_kif.c"
IOCTL_H="$SRC_ROOT/src/kernel/vms_ioctl.h"
status=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms_kif caller census: the product emits a call to every entry point, or it is declared unwired"

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
# call_edges [calls|defs|graph <rel>]: read comment-stripped C on stdin.
#
#   calls (default) - print "ENCLOSING<TAB>CALLEE" for every call expression.
#   defs            - print "static|extern<TAB>NAME" for every function
#                     DEFINITION at file scope. Definitions, not prototypes:
#                     the same depth-0 rule that stops a prototype counting as
#                     a call is what distinguishes them, so both readings of
#                     the tree come from one reader and cannot disagree about
#                     what a definition is.
#   graph <rel>     - all five record kinds section 2 needs to build the
#                     PRODUCT call graph, in ONE pass, each tagged and
#                     attributed to the origin file <rel>:
#                       D<TAB>rel<TAB>static|extern<TAB>NAME  a function
#                                                             DEFINITION
#                       O<TAB>rel<TAB>static|extern<TAB>NAME  a file-scope
#                                                             OBJECT with a
#                                                             braced initialiser
#                       E<TAB>rel<TAB>ENCLOSING<TAB>CALLEE    a call edge
#                       P<TAB>rel<TAB>static|extern<TAB>NAME  a file-scope
#                                                             DECLARATION
#                       R<TAB>rel<TAB>ENCLOSING<TAB>NAME  an identifier NOT
#                                           followed by "(", i.e. a name USED
#                                           without being CALLED -- which is
#                                           what taking a function's address
#                                           looks like.
#                     ENCLOSING is a function name, or "@obj" when the record
#                     comes from inside a file-scope object's initialiser, or
#                     "" at file scope outside any braces. THE INITIALISER CASE
#                     IS THE WHOLE REASON OBJECTS ARE NODES: a callback table
#                     is where a function's address is taken, and whether that
#                     address can be called depends on whether anything reaches
#                     the TABLE.
#                     Records are deduplicated inside the reader: the same
#                     header is expanded into a hundred translation units and
#                     each copy would otherwise emit the same rows again.
#
# It is a character-level reader, not a token search, for five reasons:
#   - a call at brace depth 0 is a PROTOTYPE or a DEFINITION, not a call, so the
#     definition of vms_kif_setef must not count as a caller of itself;
#   - string and character literals are skipped, so a brace inside "{" cannot
#     desynchronise the depth counter;
#   - the enclosing function has to be known for the reachability pass below,
#     and function-like MACROS are nodes too -- KIF_CALL is the only thing that
#     names kif_call(), so a reader blind to macro bodies would conclude the
#     bind path is dead and mark every entry point unwired;
#   - PARENTHESIS DEPTH is tracked, and it is load-bearing rather than tidy. A
#     FUNCTION-POINTER PARAMETER contains an identifier followed by "(" inside
#     the parameter list, so a reader that only looked at brace depth took the
#     LAST such identifier as the name of the function being defined. MEASURED
#     on this tree before the fix: the enclosing function of the vms_kif_enq
#     and vms_kif_convert call sites in src/libvms/syssvc/sys_lock.c read as
#     "void", from `void (*astadr)(void *)` in $ENQ's parameter list, and the
#     same for $GETJPI's and $QIO's AST parameters. The old census never
#     noticed because it threw the enclosing name away on the product side;
#     a call graph cannot;
#   - "$" IS AN IDENTIFIER CHARACTER HERE. OVMX's system services are spelled
#     sys$assign, sys$enq, sys$getjpi -- real C identifiers, through the GNU
#     extension the build already relies on. A reader that split on "$" would
#     call every one of them "assign", "enq", "getjpi", merging unrelated
#     nodes in the call graph and losing the ones that collide.
# ---------------------------------------------------------------------------
call_edges() {
    awk -v want="${1:-calls}" -v rel="${2:-}" '
        function once(k) { if (k in seen) return 0; seen[k] = 1; return 1 }
        function emit_call(node, id) {
            if (want == "calls") print node "\t" id
            else if (want == "graph" && once("E" SUBSEP node SUBSEP id))
                print "E\t" rel "\t" node "\t" id
        }
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
                # Parenthesis depth. A name followed by "(" INSIDE a parameter
                # list is a function-pointer parameter, not the function being
                # declared, so it must not become $pending.
                if (!ismac && c == "(") { pdepth++; i++; continue }
                if (!ismac && c == ")") { if (pdepth > 0) pdepth--; i++; continue }
                if (!ismac && c == "=" && depth == 0 && pdepth == 0) {
                    # Everything after "=" at file scope is the INITIALISER, so
                    # the name of the declared object is whatever was last seen
                    # before it. Freezing here is what stops
                    # `static void (*p)(void) = some_handler;` from naming the
                    # HANDLER as the object being declared.
                    objfrozen = 1; i++; continue
                }
                if (!ismac && c == "{") {
                    if (depth == 0) {
                        # WHAT KIND OF BRACE IS THIS, and the order of the
                        # three tests is load-bearing. An "=" already seen at
                        # file scope settles it: everything after it is an
                        # INITIALISER, and no function body can follow one. That
                        # test has to come FIRST, because a declarator like
                        #     static void (*const tab[])(void) = { handler };
                        # leaves "void" in $pending -- it is an identifier
                        # followed by "(" -- and a reader that trusted $pending
                        # here would read the TABLE as a function definition
                        # named "void" and hand the handler inside it whatever
                        # reachability that bogus node had. MEASURED: it had
                        # enough, and the dead-table form of the buy this
                        # section exists to close went rc=0 / 44 / 32 / 12
                        # through exactly that hole.
                        # Otherwise a name followed by "(" is a DEFINITION (a
                        # prototype never gets here: its ";" clears pending
                        # below), and a name NOT followed by "(" is an
                        # aggregate type body.
                        if (objfrozen) {
                            # $pendobj is the last file-scope name outside any
                            # parentheses; $pendarr is the last one followed by
                            # "[". The second is what names an ARRAY OF FUNCTION
                            # POINTERS -- in
                            #     static uint32_t (*const tab[1])(void) = {...}
                            # the declared name sits INSIDE the declarator
                            # parentheses, so $pendobj never sees it, and
                            # without $pendarr the table has no node, nothing
                            # can reference it, and every handler in it reads
                            # as unreachable. MEASURED before $pendarr existed:
                            # a live table of that form, called from
                            # cmd_show_process(), still reported its handler
                            # under "credit NOTHING" -- a RED on correct code.
                            objnm = (pendobj != "") ? pendobj : pendarr
                            curfn = (objnm != "") ? "@" objnm : ""
                            if (objnm != "" && want == "graph" && once("O" SUBSEP objnm))
                                print "O\t" rel "\t" (sawstatic ? "static" : "extern") "\t" objnm
                        } else if (pending != "") {
                            curfn = pending
                            if (want == "defs")
                                print (sawstatic ? "static" : "extern") "\t" curfn
                            else if (want == "graph" && once("D" SUBSEP curfn))
                                print "D\t" rel "\t" (sawstatic ? "static" : "extern") "\t" curfn
                        } else if (pendobj != "") {
                            curfn = "@" pendobj
                            if (want == "graph" && once("O" SUBSEP pendobj))
                                print "O\t" rel "\t" (sawstatic ? "static" : "extern") "\t" pendobj
                        } else {
                            curfn = ""
                        }
                        pending = ""; pdepth = 0; sawstatic = 0
                    }
                    depth++; i++; continue
                }
                if (!ismac && c == "}") {
                    depth--
                    if (depth <= 0) {
                        depth = 0; curfn = ""; sawstatic = 0; pdepth = 0
                        pendobj = ""; pendarr = ""; objfrozen = 0
                    }
                    i++; continue
                }
                if (!ismac && c == ";" && depth == 0 && pdepth == 0) {
                    # A name followed by "(" at file scope whose statement ends
                    # in ";" is a DECLARATION -- a prototype.
                    if (pending != "" && want == "graph" && once("P" SUBSEP pending))
                        print "P\t" rel "\t" (sawstatic ? "static" : "extern") "\t" pending
                    pending = ""; sawstatic = 0; pendobj = ""; pendarr = ""; objfrozen = 0
                    i++; continue
                }
                if (c ~ /[A-Za-z_$]/) {
                    j = i
                    while (j <= n && substr(s, j, 1) ~ /[A-Za-z0-9_$]/) j++
                    id = substr(s, i, j - i)
                    k = j
                    while (k <= n && (substr(s, k, 1) == " " || substr(s, k, 1) == "\t")) k++
                    if (substr(s, k, 1) == "(") {
                        if (ismac) emit_call(node, id)
                        else if (depth >= 1) emit_call(curfn, id)
                        else if (pdepth == 0) pending = id
                    } else if (!ismac && depth == 0 && id == "static") {
                        sawstatic = 1
                    } else if (want == "graph" && !ismac) {
                        if (depth == 0 && !objfrozen) {
                            if (pdepth == 0) pendobj = id
                            if (substr(s, k, 1) == "[") pendarr = id
                        }
                        if (once("R" SUBSEP curfn SUBSEP id))
                            print "R\t" rel "\t" curfn "\t" id
                    }
                    i = j; continue
                }
                i++
            }
        }
        BEGIN { depth = 0; pending = ""; curfn = ""; inmac = 0; macnode = ""; sawstatic = 0
                pdepth = 0; pendobj = ""; pendarr = ""; objfrozen = 0 }
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
# opcode_owners: read comment-stripped C on stdin, print the enclosing
# file-scope function name for every VMS_IOCTL_* / VMS_*_SEL_* TOKEN found
# inside a function body (not just call targets -- an opcode is passed as a
# macro ARGUMENT, e.g. `KIF_CALL(VMS_IOCTL_SETMODE, &args)`, so a call-target
# reader would miss it).
#
# WHY THIS EXISTS: this gate's own comment used to claim, by hand and by
# name, which definitions "issue no opcode and name no selector" -- and that
# claim was wrong (it omitted kif_bind; see the floor-exempt paragraph
# below). A count derived from the tree at every run cannot go stale the way
# a hand-written sentence can, because there is nothing to keep in sync: this
# is the same fix already applied to the universe itself in section 1,
# applied to a narrower fact that was still being hand-maintained in prose.
#
# Deliberately independent of call_edges(): it shares the same character-
# level depth/curfn tracking (so it agrees on what "the enclosing function"
# means) but is not layered onto call_edges' shared code path, so a change
# made for this informational reader cannot alter what call_edges reports for
# the actual pass/fail universe, reachability or floor computations above.
# ---------------------------------------------------------------------------
opcode_owners() {
    awk '
        function scan(s,    n, i, j, k, c, id, q) {
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
                if (c == "{") {
                    if (depth == 0) { curfn = pending; pending = "" }
                    depth++; i++; continue
                }
                if (c == "}") {
                    depth--
                    if (depth <= 0) { depth = 0; curfn = "" }
                    i++; continue
                }
                if (c == ";" && depth == 0) { pending = ""; i++; continue }
                if (c ~ /[A-Za-z_]/) {
                    j = i
                    while (j <= n && substr(s, j, 1) ~ /[A-Za-z0-9_]/) j++
                    id = substr(s, i, j - i)
                    if (depth >= 1 && curfn != "" &&
                        (id ~ /^VMS_IOCTL_[A-Z0-9_]+$/ ||
                         id ~ /^VMS_[A-Z0-9_]+_SEL_[A-Z0-9_]+$/))
                        print curfn
                    k = j
                    while (k <= n && (substr(s, k, 1) == " " || substr(s, k, 1) == "\t")) k++
                    if (depth == 0 && substr(s, k, 1) == "(") pending = id
                    i = j; continue
                }
                i++
            }
        }
        BEGIN { depth = 0; pending = ""; curfn = "" }
        {
            if ($0 ~ /^[ \t]*#/) next
            scan($0)
        }
    '
}

# ---------------------------------------------------------------------------
# norm_path: canonicalise "a/b/../c" and "a//b" on stdin, one path per line.
#
# Pure awk, so the census adds no dependency on realpath(1)/readlink(1), and
# deterministic, so the SAME header reached through two different -I paths
# collapses to one origin. That matters: vms_kif.c reaches the opcode header as
# src/libvmssys/../kernel/vms_ioctl.h and every other product file reaches it as
# src/kernel/vms_ioctl.h, and an un-normalised comparison would call them two
# different files -- which would put vms_ioctl.h in the interface-private set
# below and drag every macro in it into the universe.
# ---------------------------------------------------------------------------
norm_path() {
    awk '{
        n = split($0, p, "/"); top = 0
        for (i = 1; i <= n; i++) {
            if (p[i] == "" && i > 1) continue
            if (p[i] == ".") continue
            if (p[i] == "..") { if (top > 1) top--; continue }
            st[++top] = p[i]
        }
        out = ""
        for (i = 1; i <= top; i++) out = out (i > 1 ? "/" : "") st[i]
        print out
    }'
}

# ---------------------------------------------------------------------------
# pp_origins: read `cc -E` output on stdin, print the normalised absolute path
# named by every `# <line> "<file>"` linemarker. Unsorted; the caller sorts.
# ---------------------------------------------------------------------------
pp_origins() {
    grep -oE '^# [0-9]+ "[^"]*"' | sed -e 's/^# [0-9]* "//' -e 's/"$//' | norm_path
}

# ---------------------------------------------------------------------------
# pp_regions <mode> <root> <listfile>: read `cc -E` output on stdin and emit
# only the lines whose ORIGIN FILE (the last linemarker seen) is selected.
#
#   mode=keep  - origin must be under <root>, its path relative to <root> must
#                start with src/ or tools/, and must NOT be listed in
#                <listfile>. Emits "REL<TAB>SEQ<TAB>LINE" so the caller can
#                attribute a call site to a file and still reassemble each
#                file's text in order.
#   mode=only  - origin must be listed (verbatim, absolute) in <listfile>.
#                Emits the bare line.
#
# Linemarker lines themselves are dropped, so the `^#` rule in call_edges()
# never has to see them. THE SPLIT IS BRACE-SAFE: what is dropped is always a
# WHOLE included file's expansion, and a C header is brace-balanced at file
# scope, so the retained regions of one origin file concatenate back into
# balanced text and the depth counter cannot desynchronise.
# ---------------------------------------------------------------------------
pp_regions() {
    awk -v mode="$1" -v root="$2" -v listfile="$3" '
        function norm(s,   n, i, p, st, top, out) {
            n = split(s, p, "/"); top = 0
            for (i = 1; i <= n; i++) {
                if (p[i] == "" && i > 1) continue
                if (p[i] == ".") continue
                if (p[i] == "..") { if (top > 1) top--; continue }
                st[++top] = p[i]
            }
            out = ""
            for (i = 1; i <= top; i++) out = out (i > 1 ? "/" : "") st[i]
            return out
        }
        BEGIN {
            while ((getline l < listfile) > 0) if (l != "") listed[l] = 1
            rl = length(root)
            seq = 0
        }
        /^# [0-9]+ "/ {
            f = $0
            sub(/^# [0-9]+ "/, "", f); sub(/".*$/, "", f)
            f = norm(f)
            keep = 0
            if (mode == "only") {
                if (f in listed) { keep = 1; rel = f }
            } else {
                if (substr(f, 1, rl) == root) {
                    r = substr(f, rl + 1)
                    if (r ~ /^(src|tools)\// && !(f in listed)) { keep = 1; rel = r }
                }
            }
            next
        }
        keep {
            seq++
            if (mode == "only") print
            else print rel "\t" seq "\t" $0
        }
    '
}

# ---------------------------------------------------------------------------
# 0. THE BUILD SET. What the product actually COMPILES, and the flags it
#    compiles it with, read from CMake -- not from a path glob over source text.
#
# WHY THIS REPLACED A GLOB (rd vms-e2b). Every reading below used to be a scan
# of raw source TEXT under src/ and tools/, and text cannot tell a product path
# from something merely shaped like one. Two mechanisms were bought cheaply and
# MEASURED BY EXECUTION against the previous revision of this gate:
#
#   - A FABRICATED CALLER THAT CAN NEVER RUN. One new file under
#     src/libvmssys/, in NO CMakeLists and #included by NOTHING, containing
#     callers for 12 of the 13 unwired wrappers, with each declaration retired:
#     rc=0, and the census printed 44 entry points / 43 reached / 1 unwired.
#     The number this gate hands the Phase 2 verdict went 31/44 -> 43/44 with
#     NOTHING WIRED. Cheaper still, with no new file at all: put the call inside
#     an `#if 0` appended to a genuinely built file (src/vmsdcl/dcl_cmd_show.c)
#     and the scan, which is not preprocessor-aware, counted it -- 44/32/12,
#     rc=0. Two lines.
#   - EXFILTRATION: the entry point LEAVES THE UNIVERSE. Move vms_kif_close's
#     body to vms_kif_close.inc and its prototype to a header, #include both
#     back: rc=0 and the universe shrank 44 -> 43, with `gcc -E -P` output
#     BYTE-IDENTICAL to pristine. Doing that to all 13 unwired wrappers printed
#     "31 entry points -- 31 reached, 0 unwired" while all 13 were still
#     compiled into the same translation unit.
#
# Both die at the same root. Reachability is now derived from WHAT THE BUILD
# COMPILES, read AFTER PREPROCESSING:
#
#   - THE SOURCE SET IS THE BUILD SET. cmake is configured here, with
#     CMAKE_EXPORT_COMPILE_COMMANDS, and the census reads compile_commands.json.
#     A file in no CMakeLists contributes nothing, because the compiler is never
#     asked to look at it. A file that moved out of any path glob still counts,
#     because the build still compiles it.
#   - CALL SITES ARE READ FROM `cc -E` OUTPUT, with the build's own flags and
#     include paths. An `#if 0` is gone before the reader sees it. An
#     exfiltrated .inc body reappears exactly where it was, at file scope in the
#     same translation unit -- which is why the same change that kills the
#     fabricated caller kills exfiltration too.
#
# THE SPLIT THAT MAKES THIS WORK, and getting it backwards breaks the gate: the
# OVMX-UNWIRED declarations are COMMENTS, and the preprocessor STRIPS COMMENTS.
# So section 4 still reads the declarations from RAW header text. Preprocessing
# is used for CODE -- call sites, reachability, and the definition side of the
# universe -- and never for the declarations.
#
# NO SILENT FALLBACK. If cmake is missing, the configure fails, the compile
# database is absent or in a shape this reader does not implement, or ANY
# product translation unit fails to preprocess, this gate REFUSES and exits
# non-zero. It does not fall back to the glob. A census that quietly downgraded
# to a weaker reading would report the same PASS from a strictly worse
# measurement, which is the defect class this file exists to kill; the sibling
# service register already refuses when its compile step is incomplete
# ("BROKEN SYMBOL SCAN: 127 of 128 product source file(s) compiled") and this
# follows that precedent.
#
# WHAT THE BUILD SET DOES NOT COVER, stated rather than discovered later:
#   - Only what THIS cmake configuration compiles. The kernel modules under
#     src/kernel/ are built by their own Makefiles and are NOT in the compile
#     database, so a vms_kif call landing there would not be credited. That
#     direction is safe -- it produces a RED (an entry point with no product
#     path), never a silent PASS -- but it is a RED for the wrong reason, so
#     teach this gate to read that build rather than relaxing it.
#   - The configure is pinned to BUILD_TESTS=ON, BUILD_TOOLS=ON below. TOOLS is
#     load-bearing: src/ovmx_init/ovmx_init.c is the only product caller of
#     vms_kif_setterm, and it is behind BUILD_TOOLS. TESTS is on so that tests/
#     translation units are genuinely IN the database and the product-only rule
#     is enforced by an explicit src/|tools/ filter -- a deliberate exclusion,
#     not an accident of how the tree was configured.
#   - An optional component that is not installed drops out of the build set.
#     src/vmsssh/vmssshd.c needs libssh; on a host without it the census loses
#     that file's call sites. MEASURED on this tree: vmssshd.c's only credit is
#     vms_kif_setident, which tools/vms_login.c and src/ovmx_init/ovmx_init.c
#     also credit, so the census verdict is unchanged either way TODAY. Re-run
#     the per-file attribution rather than trusting that sentence after a
#     change; there is no mechanism keeping it true.
# ---------------------------------------------------------------------------
case "$SRC_ROOT$WORK" in
    *[!-a-zA-Z0-9_/.]*)
        echo "FAIL: the source root or the scratch dir contains a character this"
        echo "      reader cannot word-split safely:"
        echo "        SRC_ROOT=$SRC_ROOT"
        echo "        WORK=$WORK"
        echo "  -> the compile database's command lines are split on whitespace."
        echo "     Teach this reader to quote, or run it from a plain path; do"
        echo "     NOT let it guess where one argument ends."
        exit 1 ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
    echo "FAIL: cmake(1) is not available, so the build set cannot be derived."
    echo "  -> this gate REFUSES rather than falling back to a path glob over"
    echo "     source text. The glob is what let a file in no CMakeLists, and a"
    echo "     call inside #if 0, both count as product callers (vms-e2b)."
    exit 1
fi

CCDB_BUILD="$WORK/build"
if ! cmake -S "$SRC_ROOT" -B "$CCDB_BUILD" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DBUILD_TESTS=ON -DBUILD_TOOLS=ON > "$WORK/cmake.log" 2>&1; then
    echo "FAIL: cmake could not configure $SRC_ROOT, so there is no build set."
    sed 's/^/    /' "$WORK/cmake.log" | tail -20
    echo "  -> this gate REFUSES rather than guessing which files the product"
    echo "     compiles. Fix the configure; do not relax the census."
    exit 1
fi

CCJ="$CCDB_BUILD/compile_commands.json"
if [ ! -f "$CCJ" ]; then
    echo "FAIL: cmake configured but produced no compile_commands.json"
    echo "  -> the build set is the census's source of truth for what compiles."
    exit 1
fi

# The reader below is a line reader, not a JSON parser. Both assumptions it
# makes are CHECKED, and a violation is a refusal rather than a guess.
if grep -q '\\' "$CCJ"; then
    echo "FAIL: compile_commands.json contains backslash escapes, which this"
    echo "      reader does not implement"
    echo "  -> teach the reader to unescape; do NOT let it mis-split a command."
    exit 1
fi
if grep -q '"arguments"' "$CCJ"; then
    echo "FAIL: compile_commands.json uses the \"arguments\" array form, which"
    echo "      this reader does not implement"
    echo "  -> teach the reader that form; do NOT let it silently read nothing."
    exit 1
fi

awk '
    /^[ \t]*"directory":/ { v = $0; sub(/^[^:]*:[ \t]*"/, "", v); sub(/",?[ \t]*$/, "", v); dir = v; next }
    /^[ \t]*"command":/   { v = $0; sub(/^[^:]*:[ \t]*"/, "", v); sub(/",?[ \t]*$/, "", v); cmd = v; next }
    /^[ \t]*"file":/ {
        v = $0; sub(/^[^:]*:[ \t]*"/, "", v); sub(/",?[ \t]*$/, "", v)
        if (dir == "" || cmd == "") { print "INCOMPLETE\t" v; bad = 1 }
        else print dir "\t" cmd "\t" v
        dir = ""; cmd = ""; next
    }
' "$CCJ" | sort -u > "$WORK/ccdb"

if grep -q '^INCOMPLETE	' "$WORK/ccdb"; then
    echo "FAIL: compile_commands.json has entries with no directory or command:"
    grep '^INCOMPLETE	' "$WORK/ccdb" | cut -f2 | sed 's/^/    /' | head -5
    echo "  -> the reader would have to invent the flags. It refuses instead."
    exit 1
fi

n_ccdb=$(grep -c . "$WORK/ccdb" || true)
if [ "$n_ccdb" -eq 0 ]; then
    echo "FAIL: compile_commands.json parsed to zero translation units"
    echo "  -> an empty build set is a vacuous PASS, which is the whole defect."
    exit 1
fi

# PRODUCT ONLY: src/ and tools/. tests/ translation units ARE in the database
# -- BUILD_TESTS is on -- and are excluded here, deliberately and by one
# explicit rule. "Kernel facility + wrapper + test suite, and nothing else" is
# the precise shape of the defect, so a test caller must not satisfy the gate.
awk -F'\t' -v root="$SRC_ROOT/" '{
    if (index($3, root) != 1) next
    r = substr($3, length(root) + 1)
    if (r ~ /^(src|tools)\//) print
}' "$WORK/ccdb" > "$WORK/product_tus"

n_product_tus=$(grep -c . "$WORK/product_tus" || true)
if [ "$n_product_tus" -eq 0 ]; then
    echo "FAIL: the build set contains no product translation unit under"
    echo "      $SRC_ROOT/src or $SRC_ROOT/tools"
    echo "  -> with nothing to scan every entry point reads as unwired; the"
    echo "     census refuses rather than reporting that as a measurement."
    exit 1
fi

# THE INTERFACE TU MUST ITSELF BE IN THE BUILD. If vms_kif.c is not compiled by
# the product build, the universe, the reachability pass and the floor are all
# reading a file that ships in nothing.
if ! cut -f3 "$WORK/product_tus" | grep -qx "$KIF_C"; then
    echo "FAIL: $KIF_C is not in the product build set"
    echo "  -> the kernel interface itself is compiled by nothing, so every"
    echo "     reading below would describe dead source. Put it back in a"
    echo "     CMakeLists; do not measure a translation unit that never builds."
    exit 1
fi

# Preprocess every product translation unit once, with its own build flags.
# -o and -c are dropped and -E inserted; everything else the build passes --
# -D, -I, -std, -ffreestanding -- is kept exactly as the build has it, because
# those are what decide which lines survive the preprocessor.
mkdir -p "$WORK/pp"
tu_n=0
tu_ok=0
while IFS='	' read -r ccdir cccmd ccfile; do
    [ -n "$ccfile" ] || continue
    tu_n=$((tu_n + 1))
    ppargs=""
    skipnext=0
    first=1
    for a in $cccmd; do
        if [ "$skipnext" -eq 1 ]; then skipnext=0; continue; fi
        case "$a" in
            -o) skipnext=1; continue ;;
            -c) continue ;;
        esac
        if [ "$first" -eq 1 ]; then ppargs="$a -E"; first=0
        else ppargs="$ppargs $a"; fi
    done
    if ( cd "$ccdir" && $ppargs ) > "$WORK/pp/$tu_n.i" 2> "$WORK/pp/$tu_n.err"; then
        tu_ok=$((tu_ok + 1))
        printf '%s\n' "$ccfile" > "$WORK/pp/$tu_n.f"
    else
        echo "FAIL: could not preprocess a product translation unit:"
        echo "    ${ccfile#$SRC_ROOT/}"
        sed 's/^/      /' "$WORK/pp/$tu_n.err" | tail -10
        echo "  -> the census REFUSES on a partial build set. A reading that"
        echo "     silently skipped a file would report a smaller, weaker"
        echo "     census as though it were the same measurement."
        exit 1
    fi
done < "$WORK/product_tus"

if [ "$tu_ok" -ne "$tu_n" ] || [ "$tu_n" -eq 0 ]; then
    echo "FAIL: preprocessed $tu_ok of $tu_n product translation unit(s)"
    exit 1
fi

# ---------------------------------------------------------------------------
# 0''. EMITTED-CALL EVIDENCE: what the COMPILER put in the object file
#      (rd vms-c79). ASK THE COMPILER, DO NOT PARSE FOR SYNTAX.
#
# WHAT THIS CLOSES, MEASURED FIRST. vms-e2b made the census read the build set
# and read it AFTER PREPROCESSING; vms-c13 made a call count only when its
# ENCLOSING FUNCTION is reachable from a root. Neither sees a branch the
# COMPILER deletes inside a function that genuinely runs. MEASURED against the
# revision before this section, TWO edits in two files the build already
# compiles, no new file, no dead function, no CMakeLists change:
#
#     src/vmsdcl/dcl_cmd_show.c, inside the body of cmd_show():
#         if (0) { (void)vms_kif_chkpriv(0); }
#     src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#
# rc=0, "44 entry points -- 32 reached from the product, 12 with no product
# path", PASS. The preprocessor keeps `if (0)` -- it only kills `#if 0` -- and
# the call graph is satisfied because cmd_show() IS reached. One level up,
# `if (0) { ovmx_dead_helper(); }` in cmd_show() with the helper calling the
# wrapper was the same rc=0 at 44/32/12, so filtering the LEAF call would not
# have been enough: the disqualifier has to apply to CALL EDGES generally.
#
# THE RULE, AND WHY IT IS NOT A REGEX. Enumerating syntactic forms is the
# losing side: after `if (0)` comes `if (never_true_global)`, then
# `if (argc < 0)`. So this section asks no syntactic question at all. Every
# product translation unit is COMPILED, with its OWN flags out of the compile
# database plus -ffunction-sections, and the RELOCATIONS the compiler emitted
# are read back out of the object file. A call the compiler proved dead leaves
# no relocation; a call it could not prove dead does. The gate never learns
# what `if (0)` looks like.
#
# THE THREE FACTS THIS RESTS ON, EACH MEASURED ON THIS TOOLCHAIN
# (gcc 13.3.0, x86_64) rather than assumed -- re-measure them on any other:
#
#   1. -ffunction-sections gives PER-FUNCTION attribution. Each function lands
#      in its own `.text.<name>`, so its relocations land in
#      `.rela.text.<name>` and can be intersected with the call graph at
#      function granularity instead of collapsing to one answer per TU.
#      Without it every function in a TU shares one `.text` and the evidence
#      would be useless: `-O0` alone puts all five probe functions in one
#      section.
#   2. AN INTRA-TU CALL DOES PRODUCE A RELOCATION, which is the fact that
#      could have sunk this: a direct call to a function in the same TU
#      normally needs none. With -ffunction-sections the callee is in a
#      different section, so the call becomes a section-relative relocation --
#      but it names the SECTION SYMBOL, `.text.stat_fn`, not `stat_fn`. The
#      reader below maps `.text.X` back to X for exactly this reason. A reader
#      that only looked at symbol NAMES would silently credit nothing for every
#      static call in the tree and this gate would red on correct code
#      everywhere. (A call to a same-TU function with EXTERNAL linkage keeps
#      the plain symbol name.)
#   3. THE COMPILER-PROVABLY-DEAD BRANCH IS ERASED AT -O0 AND AT -O2 ALIKE,
#      and the not-provably-dead one SURVIVES. Probe: one function containing
#      `if (0) { f(); }` and `if (never_true_global) { f(); }` emits EXACTLY
#      ONE relocation for f at both -O0 and -O2. That asymmetry is the whole
#      mechanism, and it is also the whole residual -- see "WHAT THIS GATE DOES
#      NOT SEE" in the header for the measured price of the surviving form.
#
# THE DIRECTION OF EVERY APPROXIMATION HERE IS "CREDIT IT", i.e. under-fire,
# because a gate that reds on correct code is the one the next person weakens:
#
#   - NO EVIDENCE MEANS KEEP THE EDGE. If no object file carries a
#     `.text.<enclosing>` section, this section says nothing about that
#     function and its edges are taken from the source reading unchanged. That
#     is not hypothetical: the 17 translation units the build compiles -O2 drop
#     unreferenced statics entirely, and the three .S files under
#     src/libvmssys/arch/x86_64 are hand-written assembly whose functions are
#     not in `.text.<name>` sections at all.
#   - EVIDENCE IS KEYED ON NAMES, NOT ON (file, name). Two statics of one name
#     in two translation units share one evidence entry, so a call emitted from
#     either credits both. The call graph still keeps them as separate nodes;
#     this only ever ADDS credit relative to a per-object reading.
#   - ONLY CALL EDGES AND ADDRESS-TAKES FROM INSIDE A FUNCTION BODY are
#     filtered. An edge out of a file-scope object's initialiser (`@obj`) is
#     kept as the source reading has it: an initialiser is a constant, it has
#     no branches for a compiler to delete, and its relocations live in a data
#     section this reader does not attribute.
# ---------------------------------------------------------------------------
if ! command -v readelf >/dev/null 2>&1; then
    echo "FAIL: readelf(1) is not available, so the compiler's own answer about"
    echo "      which calls it emitted cannot be read."
    echo "  -> this gate REFUSES rather than falling back to the source reading."
    echo "     Without it a branch the compiler deletes is a product path again"
    echo "     (vms-c79), at two edits in two already-built files."
    exit 1
fi

# THE EVIDENCE FLAGS, and every one of the four is load-bearing. This is a
# COMPILE FOR EVIDENCE, not the shipping build: the object files are read and
# thrown away, so the right setting is the one that keeps the answer
# ATTRIBUTABLE while leaving the one transformation under study -- elimination
# of a provably-dead branch -- exactly as the shipping build does it.
#
#   -ffunction-sections     per-function attribution; without it every function
#                           in a TU shares one .text and the evidence collapses
#                           to one answer per TU.
#   -fno-inline             inlining MOVES code between functions, and a call
#                           that was inlined leaves no relocation -- which is
#                           indistinguishable from a call the compiler deleted.
#                           MEASURED without it: 62 edges to product functions
#                           vanished at -O2 (vms_fopen -> alloc_file,
#                           vms_cos -> cos_poly, __vms_runtime_init ->
#                           parse_auxv, ...) and 28 functions lost reachability
#                           on a pristine tree. That is reddening correct code.
#   -fkeep-static-functions an unreferenced static is otherwise dropped whole at
#                           -O2, leaving no section -- and "no section" is this
#                           section's no-evidence escape, so the buy this whole
#                           section closes still worked inside the 17
#                           translation units the build compiles -O2. MEASURED
#                           before this flag: `if (0) { dead_static(); }` in
#                           src/libvmssys/vms_string.c was rc=0 at 44/32/12.
#   -fkeep-inline-functions the same hole for an inline definition in a header.
#
# NOTE WHAT IS NOT HERE: no -O override. Each translation unit keeps the
# optimisation level the build gives it, because that is what decides how much
# the compiler can prove.
EVIDENCE_FLAGS="-ffunction-sections -fno-inline -fkeep-static-functions -fkeep-inline-functions"

# RUN IN PARALLEL, IN BATCHES, AND CHECK EVERY OUTCOME. The compiles are
# independent -- each writes one object into its own scratch path -- and
# serially they cost about as much again as the whole rest of this gate.
# MEASURED on workshop (16 cores): 15s per run serial, 12s batched, against a
# 6s baseline before this section existed. That matters because the sibling
# negative-control suite runs this gate once per control.
# A FAILED COMPILE LEAVES A MARKER FILE rather than a status this loop could
# lose: a background job's exit status is not the loop's, so a reader that
# just carried on would silently measure a partial build set.
NJOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
case "$NJOBS" in ''|*[!0-9]*) NJOBS=4 ;; esac
[ "$NJOBS" -ge 1 ] || NJOBS=4

mkdir -p "$WORK/obj"
obj_n=0
obj_ok=0
running=0
while IFS='	' read -r ccdir cccmd ccfile; do
    [ -n "$ccfile" ] || continue
    obj_n=$((obj_n + 1))
    # The build's own command, with its -o replaced and $EVIDENCE_FLAGS added.
    # -c is KEPT: this is a compile, not a preprocess. Everything else -- -D,
    # -I, -std, -O, -ffreestanding -- is exactly what the build passes, because
    # those are what decide what the compiler emits.
    cargs=""
    skipnext=0
    first=1
    for a in $cccmd; do
        if [ "$skipnext" -eq 1 ]; then skipnext=0; continue; fi
        case "$a" in
            -o) skipnext=1; continue ;;
        esac
        if [ "$first" -eq 1 ]; then cargs="$a $EVIDENCE_FLAGS"; first=0
        else cargs="$cargs $a"; fi
    done
    printf '%s\n' "$ccfile" > "$WORK/obj/$obj_n.f"
    (
        if ( cd "$ccdir" && $cargs -o "$WORK/obj/$obj_n.o" ) \
                > "$WORK/obj/$obj_n.err" 2>&1; then
            : > "$WORK/obj/$obj_n.ok"
        fi
    ) &
    running=$((running + 1))
    if [ "$running" -ge "$NJOBS" ]; then wait; running=0; fi
done < "$WORK/product_tus"
wait

i=1
while [ "$i" -le "$obj_n" ]; do
    if [ -f "$WORK/obj/$i.ok" ] && [ -f "$WORK/obj/$i.o" ]; then
        obj_ok=$((obj_ok + 1))
    else
        echo "FAIL: could not compile a product translation unit:"
        echo "    $(sed "s|^$SRC_ROOT/||" "$WORK/obj/$i.f" 2>/dev/null)"
        sed 's/^/      /' "$WORK/obj/$i.err" 2>/dev/null | tail -10
        echo "  -> the census REFUSES on a partial build set, exactly as it does"
        echo "     for the preprocess. A missing object file is missing evidence,"
        echo "     and missing evidence CREDITS the edge -- so a silent skip here"
        echo "     would hand back the vms-c79 buy without saying so."
        exit 1
    fi
    i=$((i + 1))
done

if [ "$obj_ok" -ne "$obj_n" ] || [ "$obj_n" -eq 0 ]; then
    echo "FAIL: compiled $obj_ok of $obj_n product translation unit(s)"
    exit 1
fi

# Read the section table and the relocations out of every object at once.
#   emit_fns   - NAME, once per function that has a `.text.<NAME>` section
#                somewhere. "This section has evidence about NAME."
#   emit_edges - CALLER<TAB>CALLEE, once per relocation the compiler emitted
#                from inside `.text.<CALLER>`. A relocation naming the section
#                symbol `.text.<CALLEE>` is an intra-TU reference and is mapped
#                back to CALLEE; anything else is used as the symbol name it is.
# .rela.eh_frame is NOT read: it names every function in the TU for unwinding,
# which would make every function look referenced from everywhere. Keying on
# `.rela.text.<name>` excludes it by construction.
if ! readelf -SrW "$WORK"/obj/*.o > "$WORK/relo.txt" 2> "$WORK/relo.err"; then
    echo "FAIL: readelf could not read the object files this gate just compiled"
    sed 's/^/    /' "$WORK/relo.err" | tail -10
    exit 1
fi

awk '
    # The function a section name holds, or "" if it holds none. gcc splits
    # cold and startup code into their own prefixes even under
    # -ffunction-sections, so all four spellings map to the same function.
    function defunc(s) {
        if (sub(/^\.text\.unlikely\./, "", s)) return s
        if (sub(/^\.text\.startup\./,  "", s)) return s
        if (sub(/^\.text\.hot\./,      "", s)) return s
        if (sub(/^\.text\./,           "", s)) return s
        return ""
    }
    /^ *\[ *[0-9]+\] +\./ {
        line = $0
        sub(/^ *\[ *[0-9]+\] +/, "", line)
        split(line, f, / +/)
        fn = defunc(f[1])
        if (fn != "") print "S\t" fn
        cur = ""
        next
    }
    /^Relocation section / {
        n = split($0, q, "\047")
        s = (n >= 2) ? q[2] : ""
        if (s ~ /^\.rela\./)     s = substr(s, 6)
        else if (s ~ /^\.rel\./) s = substr(s, 5)
        cur = defunc(s)
        next
    }
    # A relocation entry. The type field always begins R_; that is what
    # separates an entry from the column header and from readelfs "File:"
    # banner when several objects are read at once.
    cur != "" && $3 ~ /^R_/ {
        name = $5
        if (name == "" || name == "+" || name == "-") next
        t = defunc(name)
        if (t != "") name = t
        print "E\t" cur "\t" name
        next
    }
' "$WORK/relo.txt" | sort -u > "$WORK/emit_raw"

grep '^S	' "$WORK/emit_raw" | cut -f2 | sort -u > "$WORK/emit_fns"
grep '^E	' "$WORK/emit_raw" | cut -f2,3 | sort -u > "$WORK/emit_edges"

n_emit_fns=$(grep -c . "$WORK/emit_fns" || true)
n_emit_edges=$(grep -c . "$WORK/emit_edges" || true)

# NO SILENT FALLBACK, the same rule the build set and the call graph follow.
# Zero functions with evidence means every edge falls through the "no evidence"
# door and this section is doing nothing at all -- which is a strictly weaker
# measurement reported under the same PASS.
if [ "$n_emit_fns" -eq 0 ] || [ "$n_emit_edges" -eq 0 ]; then
    echo "FAIL: the object files carry no per-function relocation evidence"
    echo "      ($n_emit_fns function section(s), $n_emit_edges emitted edge(s))"
    echo "  -> either -ffunction-sections did not take effect or this reader does"
    echo "     not understand this toolchain's readelf output. Teach the reader;"
    echo "     do NOT let the census fall back to the source-only reading, which"
    echo "     credits a branch the compiler deletes (vms-c79)."
    exit 1
fi

# 0'. THE INTERFACE-PRIVATE ORIGIN SET: the files that the interface
#     translation unit compiles and NO OTHER product translation unit does.
#
# This is what makes "read the definitions after preprocessing" safe. Naively,
# every file-scope definition in the preprocessed vms_kif.c TU would join the
# universe -- including the 47 static inline functions vms_syscall.h declares,
# which are not the kernel interface and would flood the census with entry
# points nobody can wire. A file compiled into OTHER product TUs is a shared
# header and is dropped; a file compiled ONLY into vms_kif.c's TU is part of the
# interface's own implementation by construction, and that is exactly where an
# exfiltrated .inc body lands. MEASURED on this tree: the private set is exactly
# { src/libvmssys/vms_kif.c }, and the preprocessed reading of it yields the
# same 44 definitions, with the same static/extern split, as the raw reading.
: > "$WORK/origins_other"
: > "$WORK/origins_iface"
i=1
while [ "$i" -le "$tu_n" ]; do
    if [ "$(cat "$WORK/pp/$i.f")" = "$KIF_C" ]; then
        pp_origins < "$WORK/pp/$i.i" >> "$WORK/origins_iface"
    else
        pp_origins < "$WORK/pp/$i.i" >> "$WORK/origins_other"
    fi
    i=$((i + 1))
done
sort -u "$WORK/origins_iface" -o "$WORK/origins_iface"
sort -u "$WORK/origins_other" -o "$WORK/origins_other"
comm -23 "$WORK/origins_iface" "$WORK/origins_other" > "$WORK/origins_private"
n_private=$(grep -c . "$WORK/origins_private" || true)

if ! grep -qx "$KIF_C" "$WORK/origins_private"; then
    echo "FAIL: $KIF_C is not private to its own translation unit"
    echo "  -> some other product file #includes the interface's .c. The"
    echo "     private-origin rule cannot separate the interface from the"
    echo "     shared headers under that condition; fix the include, or teach"
    echo "     this gate a different rule -- do not let it read nothing."
    exit 1
fi

# Everything the product's own call-site scan must NOT credit: the interface's
# own translation unit (a call inside vms_kif.c is not a PRODUCT path -- that is
# what the reachability pass in section 3 is for) and vms_kif.h.
cp "$WORK/origins_private" "$WORK/origins_excluded"
printf '%s\n' "$KIF_H" >> "$WORK/origins_excluded"
sort -u "$WORK/origins_excluded" -o "$WORK/origins_excluded"

# The preprocessed interface: every private-origin region of vms_kif.c's TU,
# and separately EVERY product-origin region of it -- see the third union term
# in section 1 for what the wider reading is for and why it is namespaced.
: > "$WORK/kif_pp"
: > "$WORK/kif_pp_all"
i=1
while [ "$i" -le "$tu_n" ]; do
    if [ "$(cat "$WORK/pp/$i.f")" = "$KIF_C" ]; then
        pp_regions only "$SRC_ROOT/" "$WORK/origins_private" < "$WORK/pp/$i.i" \
            >> "$WORK/kif_pp"
        pp_regions keep "$SRC_ROOT/" /dev/null < "$WORK/pp/$i.i" | cut -f3- \
            >> "$WORK/kif_pp_all"
    fi
    i=$((i + 1))
done

# The preprocessed vms_kif.h include closure, for the prototype reading. It is
# not a translation unit, so it is preprocessed with the interface TU's own
# flags, taken from the build set rather than written down here.
kif_cmd=$(awk -F'\t' -v f="$KIF_C" '$3 == f { print $2; exit }' "$WORK/product_tus")
kif_dir=$(awk -F'\t' -v f="$KIF_C" '$3 == f { print $1; exit }' "$WORK/product_tus")
ppargs=""
skipnext=0
first=1
for a in $kif_cmd; do
    if [ "$skipnext" -eq 1 ]; then skipnext=0; continue; fi
    case "$a" in
        -o) skipnext=1; continue ;;
        -c) continue ;;
        "$KIF_C") continue ;;
    esac
    if [ "$first" -eq 1 ]; then ppargs="$a -E -x c"; first=0
    else ppargs="$ppargs $a"; fi
done
if ! ( cd "$kif_dir" && $ppargs "$KIF_H" ) > "$WORK/kifh.i" 2> "$WORK/kifh.err"; then
    echo "FAIL: could not preprocess $KIF_H with the build's own flags:"
    sed 's/^/    /' "$WORK/kifh.err" | tail -10
    echo "  -> the prototype reading refuses rather than falling back to raw"
    echo "     text, which an exfiltrated prototype walks straight out of."
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. The universe, derived from the tree at check time and PINNED.
#
# Never a hardcoded list: main moves under this gate constantly, and a list is
# what the census exists to replace. But "derived from the header" alone let a
# deleted prototype shrink the universe silently -- so the universe is the UNION
# of independent readings, and their disagreement is itself a RED.
#
# EACH SIDE IS NOW READ TWICE, RAW AND PREPROCESSED, AND UNIONED. The raw
# reading is the one this gate has always had. The preprocessed reading is what
# closes exfiltration: a body moved to a .inc, or a prototype moved to another
# header, is still there after `cc -E`, at file scope in the same translation
# unit. A union can only ADD entry points that must be accounted for, never
# remove one, so adding it cannot turn an existing RED green.
#
# THE PRICE, stated rather than hidden: a definition that exists only in the RAW
# text -- one inside an `#if 0` or a dead `#ifdef` in vms_kif.c -- is in the
# universe but has no compiled call graph, so it is unreachable and must be
# declared or deleted. That is a deliberate lint on dead code in the interface
# file. There is no such block in vms_kif.c today.
# ---------------------------------------------------------------------------
{
    strip_comments < "$KIF_H" \
        | grep -oE 'vms_kif_[A-Za-z0-9_]+[ \t]*\(' \
        | sed -E 's/[ \t]*\($//'
    # The preprocessed closure. Namespaced, like the raw reading it joins:
    # a prototype is a vms_kif_ name here or it is not a prototype of this
    # interface. Comments are gone before this reads, which is why the
    # closure reading needs no strip_comments of its own.
    pp_regions keep "$SRC_ROOT/" /dev/null < "$WORK/kifh.i" | cut -f3- \
        | grep -oE 'vms_kif_[A-Za-z0-9_]+[ \t]*\(' \
        | sed -E 's/[ \t]*\($//'
} | sort -u > "$WORK/protos"

{
    strip_comments < "$KIF_C" | call_edges defs
    # UNFILTERED on the definition side, in both of the first two readings --
    # see the rename paragraph above. The private-origin restriction is what
    # keeps the shared headers out; it is not a name filter.
    call_edges defs < "$WORK/kif_pp"
    # THE THIRD TERM, and it is NAMESPACED where the other two are not, because
    # it reads regions the private-origin rule deliberately drops. MEASURED
    # residual it exists to close (vms-e2b): move a wrapper's body into a .inc,
    # mark it `static` so two translation units can each hold a copy and the
    # product still LINKS, #include that .inc from vms_kif.c AND from one other
    # product file so it is no longer interface-private, delete the prototype,
    # retire the declaration, and add `enum { r = (int)VMS_IOCTL_TTSETMODE };`
    # to keep the raw floor satisfied. Six edits across three files took the
    # census from 44 entry points to 43 with rc=0 -- on the previous gate AND
    # on this one before this term existed. A vms_kif_-named function defined
    # at file scope in the interface translation unit is an entry point
    # wherever its text lives, so it is counted from anywhere in that TU.
    # The name filter is the price of reading regions this gate cannot
    # otherwise distinguish from vms_syscall.h's 47 static inlines; what it
    # leaves open is that same evasion COMPOSED with a rename out of the
    # namespace, which is recorded under "WHAT THIS GATE DOES NOT SEE".
    call_edges defs < "$WORK/kif_pp_all" | grep -E '	vms_kif_' || true
} | sort -u > "$WORK/defs_all"
cut -f2 "$WORK/defs_all" | sort -u > "$WORK/defs"
# A name is externally linked if EITHER reading saw it defined non-static.
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

    # 1d. INFORMATIONAL ONLY -- does not affect status. The floor-exempt set:
    # definitions whose own body issues no VMS_IOCTL_* and names no
    # VMS_*_SEL_* token, so their deletion CANNOT be caught by the floor
    # above. This is a NECESSARY condition for a definition being safe to
    # delete without any RED firing; it is not sufficient (a definition can
    # still be the sole path that keeps some other entry point REACHABLE --
    # see the comment above this gate's floor section for kif_bind, which is
    # in this set but is not safe to delete for exactly that reason). Printed
    # so the count is read from this run, not recited from a comment that
    # cannot detect when it goes stale.
    strip_comments < "$KIF_C" | opcode_owners | sort -u > "$WORK/opcode_owners"
    comm -23 "$WORK/defs" "$WORK/opcode_owners" > "$WORK/floor_exempt"
    n_floor_exempt=$(grep -c . "$WORK/floor_exempt" || true)
fi

# ---------------------------------------------------------------------------
# 2. Direct product callers, outside vms_kif.c: these are the census roots.
#
# READ FROM THE BUILD, AFTER PREPROCESSING (section 0). Every product
# translation unit's `cc -E` output is split back apart by linemarker, so a call
# is credited to the file it was WRITTEN in -- but only if some translation unit
# the build actually compiles pulled that file in, and only if the preprocessor
# left the call standing. A file in no CMakeLists is never preprocessed at all;
# a call inside `#if 0` is gone before this reader runs.
#
# src/ and tools/ only. tests/ is in the build set and is deliberately excluded
# here -- see the header.
# ---------------------------------------------------------------------------
: > "$WORK/prodtext"
i=1
while [ "$i" -le "$tu_n" ]; do
    if [ "$(cat "$WORK/pp/$i.f")" != "$KIF_C" ]; then
        pp_regions keep "$SRC_ROOT/" "$WORK/origins_excluded" < "$WORK/pp/$i.i" \
            >> "$WORK/prodtext"
    fi
    i=$((i + 1))
done

# Group by origin file, preserving the order the lines were emitted in, so each
# file's contribution to each translation unit stays contiguous and balanced.
sort -s -t'	' -k1,1 "$WORK/prodtext" > "$WORK/prodtext_sorted"

: > "$WORK/sites"
mkdir -p "$WORK/byfile"
awk -F'\t' -v dir="$WORK/byfile" '
    { if ($1 != cur) { if (cur != "") close(out); cur = $1; n++; out = dir "/" n
                       print cur > (dir "/" n ".name"); close(dir "/" n ".name") }
      line = $0; sub(/^[^\t]*\t[^\t]*\t/, "", line); print line > out }
' "$WORK/prodtext_sorted"

: > "$WORK/graph"
for chunk in "$WORK/byfile"/[0-9]*; do
    case "$chunk" in *.name) continue ;; esac
    [ -f "$chunk" ] || continue
    rel=$(cat "$chunk.name")
    call_edges graph "$rel" < "$chunk" >> "$WORK/graph"
done
sort -u "$WORK/graph" -o "$WORK/graph"

# The compiled translation units, by their path relative to SRC_ROOT. An origin
# file that is NOT one of these was reached by #include -- it is a header, and
# that is the only thing separating an exported declaration from a static's
# forward declaration inside its own .c.
cut -f3 "$WORK/product_tus" | sed "s|^$SRC_ROOT/||" | sort -u > "$WORK/tu_rel"

# ---------------------------------------------------------------------------
# 2'. THE PRODUCT CALL GRAPH, ITS ROOTS, AND THE CREDIT RULE (rd vms-c13).
#
# WHAT THIS REPLACED, AND WHY. Until this section existed, section 2 kept only
# the CALLEE of each call and threw the enclosing function away: an entry point
# was REACHED if any product translation unit contained a call to it ANYWHERE,
# with no requirement that the calling function be reachable from anything. So
# a function nothing calls was a product path. MEASURED against the revision
# before this one, TWO edits in two files the build already compiles, no new
# file, no CMakeLists change, no `#if 0`:
#
#     src/vmsdcl/dcl_cmd_show.c:
#         static void ovmx_dead_helper(void) { (void)vms_kif_chkpriv(0); }
#     src/libvmssys/vms_kif.h: retire vms_kif_chkpriv's OVMX-UNWIRED token
#
# rc=0, "44 entry points -- 32 reached from the product, 12 with no product
# path", PASS, and `cmake --build --target vmsdcl` clean. Scaled -- one
# 16-line dead static calling all 12 externally-linked unwired wrappers, plus
# `sed s/OVMX-UNWIRED:/NOTE:/` over the header -- the same gate printed
# "44 entry points -- 44 reached, 0 with no product path" and PASSED.
#
# THE RULE NOW. Reachability is seeded from ROOTS and followed along call
# edges, and a call site credits an entry point only if ITS ENCLOSING FUNCTION
# IS REACHED. The graph is built from the same preprocessed, build-set product
# text section 2 already had, so everything vms-e2b bought still holds: a file
# in no CMakeLists contributes no node, and an `#if 0` body is gone before the
# reader sees it.
#
# NODE IDENTITY IS (ORIGIN FILE, NAME) FOR A `static` DEFINED IN A TRANSLATION
# UNIT, AND THE BARE NAME OTHERWISE -- which is what `static` MEANS, and it is
# load-bearing rather than pedantic. With one node per name, defining
#     static void some_existing_api_name(void) { (void)vms_kif_chkpriv(0); }
# in any product .c would merge the dead body into the live function of that
# name and inherit its reachability -- the same two-edit buy wearing a
# different name, and the same shape as the seeding collision section 1'
# closes. Two statics of one name in two .c files stay two nodes; a static and
# an extern of one name stay two nodes.
#
# THE ROOTS, all three derived from the tree, none written down here:
#
#   1. main(). The C runtime's entry point, and the only one that needs
#      naming: nothing in the tree declares it.
#   2. EVERY PRODUCT FUNCTION PROTOTYPED IN A HEADER THE BUILD COMPILES.
#      This is the library's exported API surface, and it is a root ON PURPOSE:
#      OVMX ships as libraries, an exported entry point is reachable by
#      anything that links them, and LIBVMS$SHR's universals are entry points
#      whether or not anything in THIS tree calls them. Demanding an in-tree
#      caller for an exported symbol would make this gate red on correct code.
#      "In a header" is what separates that from a static's forward
#      declaration in its own .c, which declares nothing to anyone.
#   3. EVERY PRODUCT FUNCTION WHOSE NAME IS USED WITHOUT BEING CALLED. That is
#      what taking a function's address looks like, and INDIRECT CALLS ARE THE
#      REASON THIS CLAUSE EXISTS: DCL dispatches its verbs through the
#      builtin_verbs[] table, RMS takes completion routines, $ENQ and $QIO take
#      AST handlers. A call graph blind to those would report the handlers as
#      unreachable and UNDER-COUNT the reached set -- reddening correct code
#      rather than certifying an evasion, but wrong either way. This reader
#      does not resolve which pointer is called where; it treats a function
#      whose address is taken anywhere as reachable, which is the safe
#      direction and is stated rather than implied.
#
# WHAT THIS RULE DOES NOT PROVE, and it is a shorter list than what it does:
#   - NOT that the reached path EXECUTES. An exported API with no in-tree
#     caller is a root by rule 2, and a reachable function that is never
#     called at runtime is still reachable here. This gate remains a CONFIGURE
#     and a PREPROCESS; the execution question is rd vms-d33's and belongs to
#     the QEMU suites.
#   - NOT that an indirect call really happens. Rule 3 over-approximates.
#   - The address-taken reader intersects raw identifiers with the set of
#     product function names, so a VARIABLE that happens to share a function's
#     name makes that function a root. Over-approximation again, same
#     direction.
# ---------------------------------------------------------------------------
: > "$WORK/sites"
: > "$WORK/sites_dead"
: > "$WORK/sites_noemit"
: > "$WORK/sites_unattributed"
: > "$WORK/prod_roots"
: > "$WORK/prod_reached"
: > "$WORK/prod_defs"

awk -F'\t' -v tuf="$WORK/tu_rel" -v seedf="$WORK/seedable" -v w="$WORK" \
    -v emitf="$WORK/emit_edges" -v hasf="$WORK/emit_fns" '
    # The linkage-correct node id for a FUNCTION name n as written in file f,
    # and the same for a file-scope OBJECT. "static" means the name is private
    # to its translation unit, so it is a different node from any same-named
    # function or object elsewhere.
    function res(f, n)  { return ((f SUBSEP n) in statfn)  ? f "\t" n : "\t" n }
    function reso(f, n) { return ((f SUBSEP n) in statobj) ? "@" f "\t" n : "@\t" n }
    # The node a record belongs to: a function, or the initialiser of a
    # file-scope object, or "" -- file scope outside any braces, which is a
    # context this reader cannot attribute and which is therefore never
    # reached. Nothing named only from there becomes a root.
    function ctx(f, e) {
        if (e == "") return ""
        if (substr(e, 1, 1) == "@") return reso(f, substr(e, 2))
        return res(f, e)
    }
    # THE EMITTED-CALL DISQUALIFIER (rd vms-c79). An edge OUT OF A FUNCTION
    # BODY survives only if the compiler actually put a relocation for it in
    # that function s section. Every "return 1" below is a deliberate
    # under-fire -- see section 0 for why each one credits rather than drops.
    function emitted_ok(encl, callee) {
        if (encl == "") return 1                  # file scope: no function to ask about
        if (substr(encl, 1, 1) == "@") return 1   # an object initialiser has no branches
        if (!(encl in hasfn)) return 1            # nothing was emitted for it: no evidence
        if (!(callee in hasfn)) return 1          # the callee was inlined or is not ours
        if (encl == callee) return 1              # a self-call stays inside one section
        return ((encl SUBSEP callee) in emit)
    }
    BEGIN {
        while ((getline l < tuf) > 0)   if (l != "") istu[l] = 1
        while ((getline l < seedf) > 0) if (l != "") seed[l] = 1
        while ((getline l < hasf) > 0)  if (l != "") hasfn[l] = 1
        while ((getline l < emitf) > 0) {
            if (l == "") continue
            i = index(l, "\t")
            if (i > 0) emit[substr(l, 1, i - 1), substr(l, i + 1)] = 1
        }
    }
    # Pass 1: which (file, name) pairs are file-scoped statics -- functions and
    # objects both. Needed before any edge can be resolved, which is why the
    # graph is read twice.
    NR == FNR {
        if ($1 == "D" && $3 == "static" && ($2 in istu)) statfn[$2, $4] = 1
        if ($1 == "O" && $3 == "static" && ($2 in istu)) statobj[$2, $4] = 1
        if ($1 == "D") isfn[$4] = 1
        if ($1 == "O") isobj[$4] = 1
        next
    }
    $1 == "D" { defn[res($2, $4)] = 1; next }
    $1 == "O" { next }
    # A "static" declaration in a header cannot be an exported entry point --
    # each includer gets its own private symbol, which is exactly what a
    # dead helper declared AND defined `static` inside a multi-includer
    # header (e.g. dcl_cmd.h) looks like when it tries to buy a root off
    # rule 2 (rd vms-41b). Only a NON-static declaration in a non-TU file
    # is the exported-API-surface shape rule 2 exists for.
    $1 == "P" { if (!($2 in istu) && $3 != "static") prot[$4] = 1; next }
    $1 == "E" {
        if (!emitted_ok($3, $4)) {
            nnoemit++
            if ($4 in seed) { nx++; xfile[nx] = $2; xencl[nx] = $3; xcall[nx] = $4 }
            next
        }
        e = ctx($2, $3); c = res($2, $4)
        edge[e] = edge[e] SUBSEP c
        if ($4 in seed) { ns++; sfile[ns] = $2; sencl[ns] = $3; scall[ns] = $4 }
        next
    }
    # A name USED WITHOUT BEING CALLED is an edge from the context that uses it
    # -- NOT a root on its own. A function whose address is only ever taken in
    # a table nothing reaches is not reachable, and neither is the function.
    # THE DISQUALIFIER APPLIES TO A FUNCTION S ADDRESS BEING TAKEN TOO, and it
    # has to: `if (0) { p = ovmx_dead_helper; }` is the same buy wearing an
    # address-of instead of a call, and the compiler erases it identically.
    # IT DOES NOT APPLY TO AN OBJECT TARGET. A table read can be constant-folded
    # away -- MEASURED: at -O0 gcc folds `filetab[0](x)` for a `const` table
    # into a direct call, leaving no relocation naming filetab at all -- so
    # requiring evidence for a name-to-OBJECT edge would red on correct code,
    # which is what negative control 40 exists to catch.
    $1 == "R" {
        e = ctx($2, $3)
        if ($4 in isfn) {
            if (!emitted_ok($3, $4)) { nnoemit++; next }
            edge[e] = edge[e] SUBSEP res($2, $4)
        }
        else if ($4 in isobj) edge[e] = edge[e] SUBSEP reso($2, $4)
        next
    }
    END {
        if (("\tmain") in defn) root["\tmain"] = 1
        for (n in prot) { k = "\t" n; if (k in defn) root[k] = 1 }

        nroot = 0
        for (k in root) {
            nroot++
            print k > (w "/prod_roots")
            reach[k] = 1; q[++qn] = k
        }
        for (i = 1; i <= qn; i++) {
            m = split(edge[q[i]], a, SUBSEP)
            for (j = 1; j <= m; j++)
                if (a[j] != "" && !(a[j] in reach)) { reach[a[j]] = 1; q[++qn] = a[j] }
        }

        ndef = 0; nreach = 0
        for (k in defn) { ndef++; print k > (w "/prod_defs"); if (k in reach) nreach++ }
        for (k in reach) print k > (w "/prod_reached")

        ndead = 0
        for (i = 1; i <= ns; i++) {
            if (sencl[i] == "") {
                print sfile[i] "\t(file scope)\t" scall[i] > (w "/sites_unattributed")
                continue
            }
            e = ctx(sfile[i], sencl[i])
            if (!(e in defn)) {
                print sfile[i] "\t" sencl[i] "\t" scall[i] > (w "/sites_unattributed")
                continue
            }
            if (e in reach) print sfile[i] " " sencl[i] " " scall[i] > (w "/sites")
            else { ndead++; print sfile[i] " " sencl[i] " " scall[i] > (w "/sites_dead") }
        }
        for (i = 1; i <= nx; i++)
            print xfile[i] " " xencl[i] " " xcall[i] > (w "/sites_noemit")
        printf "%d %d %d %d %d %d %d\n", ndef, nreach, nroot, ns + nx, ndead, \
            nx, nnoemit > (w "/graph_counts")
    }
' "$WORK/graph" "$WORK/graph"

n_prod_defs=0; n_prod_reached=0; n_prod_roots=0; n_sites_all=0; n_sites_dead=0
n_sites_noemit=0; n_edges_noemit=0
read -r n_prod_defs n_prod_reached n_prod_roots n_sites_all n_sites_dead \
    n_sites_noemit n_edges_noemit < "$WORK/graph_counts" 2>/dev/null || true

# NO SILENT FALLBACK, the same rule the build set follows. Each of these means
# the call graph is not the thing this gate claims to have measured, and a
# census that shrugged and carried on would report the same PASS from a
# strictly worse measurement.
if [ "${n_prod_defs:-0}" -eq 0 ]; then
    echo "FAIL: the product call graph contains no function definitions"
    echo "  -> with no graph every enclosing function reads as unreachable and"
    echo "     every entry point reads as unwired. The census refuses rather"
    echo "     than reporting that as a measurement."
    exit 1
fi
if [ "${n_prod_roots:-0}" -eq 0 ]; then
    echo "FAIL: the product call graph has NO roots"
    echo "  -> no main() and no function declared by any header the build"
    echo "     compiles. Reachability seeded from nothing marks the whole"
    echo "     product dead; the census refuses rather than measuring it."
    exit 1
fi
if [ -s "$WORK/sites_unattributed" ]; then
    echo "FAIL: a call to a kernel-interface entry point whose ENCLOSING function"
    echo "      this reader cannot name, or cannot find a definition for:"
    sed 's/^/    /' "$WORK/sites_unattributed" | head -10
    echo "  -> reachability is decided per enclosing function, so an"
    echo "     unattributable call site can be neither credited nor dismissed."
    echo "     Fix the reader; do NOT let the census guess which it was."
    exit 1
fi

sort -u "$WORK/sites" -o "$WORK/sites"
sort -u "$WORK/sites_dead" -o "$WORK/sites_dead"
n_site_files=$(cut -d' ' -f1 "$WORK/sites" | sort -u | grep -c . || true)
# Seeded from the SEEDABLE set, not from the vms_kif_ prefix and NOT from the
# whole universe. The prefix is wrong because the definition reading is
# unfiltered, so a product caller of an entry point renamed out of the
# namespace must still count. The whole universe is wrong because it now
# contains un-namespaced static helper names, and a same-named product
# function would then certify an unwired wrapper as REACHED. See 1' above.
cut -d' ' -f3 "$WORK/sites" | sort -u > "$WORK/direct"

# ---------------------------------------------------------------------------
# 3. Reachability inside vms_kif.c, seeded by those roots.
#
# An entry point named only inside vms_kif.c is wired only if the function that
# names it can be reached from a root. kif_bind() calls vms_kif_open() and
# vms_kif_register(); KIF_CALL -> kif_call -> kif_bind is what connects it to
# every wired wrapper. A wrapper family that only calls itself never joins.
#
# READ FROM THE PREPROCESSED INTERFACE (section 0'), not the raw file, and this
# side is deliberately NOT unioned with a raw reading. Reachability has to
# describe the call graph that COMPILES: a call inside a dead #ifdef in
# vms_kif.c is not a path to anything, and unioning the raw reading back in
# would reopen inside the interface the exact `#if 0` hole section 0 closes on
# the product side. KIF_CALL is already expanded here, so the macro-as-a-node
# rule in call_edges() has nothing left to do -- each wrapper names kif_call()
# directly, and kif_call() names kif_bind() as it always did.
# ---------------------------------------------------------------------------
call_edges < "$WORK/kif_pp" | sort -u > "$WORK/edges"

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

cat "$WORK/cite_summary" 2>/dev/null || true
echo "  census: $n_entries entry points — $wired the product emits a call to,"
echo "          $unwired with no product path"
echo "  build set: $n_product_tus product translation unit(s) of $n_ccdb in the compile"
echo "          database, all preprocessed AND compiled; call sites read from"
echo "          $n_site_files of them."
echo "          A file in no CMakeLists is not in this set and credits nothing."
echo "  emitted code: $n_emit_fns function(s) have a .text.<name> section in the"
echo "          object files this gate compiled, carrying $n_emit_edges relocation"
echo "          edge(s). $n_edges_noemit source edge(s) had no relocation and were"
echo "          DROPPED — the compiler proved that branch dead. $n_sites_noemit of"
echo "          them were call(s) to an entry point:$(printf ' %s' $(cut -d' ' -f3 "$WORK/sites_noemit" 2>/dev/null | sort -u))"
echo "          A function with NO section anywhere has no evidence here and its"
echo "          edges are credited from the source reading unchanged."
echo "  call graph: $n_prod_defs product function(s), $n_prod_reached of them reached from"
echo "          $n_prod_roots root(s) — main() and every function a header the build"
echo "          compiles declares. Calls and address-taking are followed from there,"
echo "          so a callback is reached exactly when its TABLE is. $n_sites_dead of"
echo "          $n_sites_all call(s) to an entry point sit in a function no root reaches"
echo "          and credit NOTHING:$(printf ' %s' $(cut -d' ' -f2 "$WORK/sites_dead" 2>/dev/null | sort -u))"
echo "  interface: $n_private origin file(s) private to the vms_kif.c translation unit —"
echo "          the definition universe is read from those, after preprocessing, so a"
echo "          body moved to an #included .inc does not leave the census"
echo "  universe pinned: $n_protos prototype(s) + $n_defs definition(s) — the union,"
echo "          so deleting either half, or renaming out of the namespace, is a"
echo "          RED, not a smaller pass"
echo "  floor:  ${n_issued:-0} of ${n_opcodes:-0} kernel opcode(s) issued by a wrapper,"
echo "          ${n_sel_named:-0} of ${n_selectors:-0} selector(s) named by one"
echo "  floor-exempt: ${n_floor_exempt:-0} definition(s) issue no opcode and name no"
echo "          selector in their own body (necessary, not sufficient, for their"
echo "          deletion to be uncaught by the floor):$(printf ' %s' $(cat "$WORK/floor_exempt" 2>/dev/null))"

if [ "$status" -eq 0 ]; then
    echo "vms_kif caller census: PASS"
else
    echo "vms_kif caller census: FAIL"
fi
exit "$status"
