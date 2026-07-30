#!/bin/sh
#
# facility_defects.sh - the per-facility negative-control manifest (vms-e7d)
#
# WHY THIS EXISTS
#
# A gate that cannot fail is not a gate. The kernel-executive job asserts that
# every tests/qemu suite passes against a real /dev/vms; the existing negative
# control (tests/qemu/Dockerfile's NEGATIVE_CONTROL=1) proves that job can go
# red -- but it proves it by removing the ENTIRE executive. That is a
# blunderbuss: it shows "something failed", not "the lock manager failed".
# ATTRIBUTION IS THE POINT. If the event-flag code silently stopped clearing a
# flag, nothing in CI before this file would have said so; the suite tally
# would just have been one smaller.
#
# The one control that DID have teeth was vms-e4d's: an adversary flipped
# compat[EX][CR] in vms.ko's lock compatibility matrix and CI went red. That
# proof existed for exactly one facility, was performed by hand, and was never
# checked in. This file generalises it: one MINIMAL injected defect per
# executive facility, checked in, applied mechanically, run in CI.
#
# THE METHOD RULE THIS FILE ENFORCES ON ITSELF
#
#   EVERY PROPERTY GETS ITS OWN MINIMAL MUTATION THAT TRIPS THAT PROPERTY AND
#   NO OTHER.
#
# A mutation that deletes a whole function body trips every property at once
# and therefore proves nothing about any of them -- five separate items in this
# epic shipped exactly that and had to be redone.
#
# HOW THAT RULE IS CHECKED, AND HOW IT USED TO BE FAKED
#
# Round 1 of this item checked minimality with a `forbid_fail` list: sibling
# assertions that must stay green. THAT WAS ITSELF THE DEFECT CLASS THIS ITEM
# EXISTS TO KILL -- a hand-maintained PARTIAL allowlist standing in for the
# property you actually want, which is
#
#     NO ASSERTION OTHER THAN THE ONES NAMED HERE WENT RED.
#
# An adversary measured it: four of the nine mutations reddened assertions
# named in NEITHER list, and the driver still printed "the sibling properties
# in the same suite stayed green (the mutation is minimal)" and "PASS: ...
# names <facility>, and nothing else." Coverage was 9 of test_kmod_devtab's 61
# assertions, 4 of test_kmod_procnam's 31, 7 of test_kmod_bind's 43 -- the rest
# silently unasserted. An allowlist someone has to remember to extend is not a
# check; it is a promise.
#
# So `forbid_fail` is DELETED and the check is INVERTED. The driver
# (tests/qemu/run_facility_negctl.sh) captures the COMPLETE set of "  FAIL:"
# assertion texts the run produced, and requires it to EQUAL
#
#     require_fail  +  knock_on_fail
#
# EXACTLY -- no missing member, no extra member. A mutation that trips a
# property nobody listed cannot pass. That leaves exactly two honest outcomes
# for an over-broad mutation, and the manifest has to pick one:
#
#   (a) make the mutation finer, so it trips only the property it names; or
#   (b) admit it is multi-property, list every assertion it reddens in
#       `knock_on_fail`, and say in `knock_on_why` WHY each one is a genuine
#       consequence of the same single defect rather than evidence of a
#       blunderbuss.
#
# `require_fail` stays the assertion(s) that NAME the property under test;
# `knock_on_fail` is everything else the same defect legitimately drags down.
# Splitting them keeps the attribution claim readable while making the full
# red set an asserted fact. `selftest` refuses a non-empty `knock_on_fail` with
# an empty `knock_on_why`, so "extra reds" can never be recorded without a
# stated reason.
#
# WHAT A "FACILITY" IS HERE -- AND WHAT IS DELIBERATELY OUT OF SCOPE
#
# The executive is vms.ko, reached through /dev/vms (CLAUDE.md Rule 9). Its
# facilities are the ioctl groups src/kernel/vms_module.c dispatches -- access
# modes, ASTs, event flags, the lock manager, the device table, the process
# table -- plus two properties of the binding itself: that a caller's PCB is
# per-PROCESS (not per-thread), and that an open descriptor pins the module.
# The ninth defect is the only one outside vms.ko: it is the vms-9fc defect
# itself (kif_bind() not calling vms_kif_register()), i.e. the product half of
# the interface, which no kernel-side mutation can reach.
#
# vmsfs.ko (src/kernel/vmsfs/, and the two suites that drive it) is NOT an
# executive facility and is NOT covered here. See scope_* below: that exclusion
# is now DECLARED and CHECKED rather than falling out of a glob, because an
# implicit narrowing is how a gate quietly stops meaning what its title says.
#
# `coverage` turns "every facility has a control" into a mechanical check at
# BOTH granularities -- translation unit and suite -- so neither a new
# executive source file nor a new suite can arrive uncovered and unnoticed.
#
# USAGE
#   facility_defects.sh list
#   facility_defects.sh scope
#   facility_defects.sh field <defect> <facility|targets|suites_red|
#                                       blind_suites|blind_why|isolation|
#                                       require_fail|knock_on_fail|
#                                       knock_on_why|why>
#   facility_defects.sh apply <defect> <src-root> [<src-root>...]
#   facility_defects.sh coverage <src-root> <tests-qemu-dir>
#   facility_defects.sh selftest <repo-root>
#
# `apply` takes SRC ROOTS -- directories that look like the repo's src/ -- so
# it can patch every copy of a file that exists in the build image (the kernel
# build reads /src/kernel, the CMake build reads /src/repo/src/...). It VERIFIES
# ITS OWN INJECTION: a sed anchor that no longer matches is a BROKEN FIXTURE,
# not a passing gate, and it exits nonzero saying so. That check is not
# theoretical -- tests/integration/test_runtime_target_negctl.sh went 13/20
# against a perfectly healthy gate when a function it was anchored to got
# renamed, and reported the evasions as CERTIFIED.

set -u

DEFECTS="access-mode-escalation
ast-setast-disable
eflag-clref-noop
lock-compat-ex-cr
lock-compat-cr-ex
devtab-owner-not-recorded
proctab-duplicate-name
executive-not-pinned
pcb-per-thread
bind-client-no-register"

# ---------------------------------------------------------------------------
# SCOPE, DECLARED
#
# The item's title is "every wired EXECUTIVE facility". These two declarations
# say exactly what that set is and what it is not, so the coverage PASS line
# cannot be read as a broader claim than it makes. Round 1 got this wrong by
# omission: `coverage` globbed src/kernel/*.c only, which silently left
# test_kmod_vmsfs and test_kmod_vmsfs_blkdev -- 2 of the 13 derived suites --
# never proven capable of going red, while printing a PASS a reader would
# reasonably take as covering the whole harness.
#
# SCOPE_OUT_UNIT_DIRS   directories under src/ whose .c files are NOT executive
#                       translation units. Files there must NOT be named by any
#                       defect (a control there would mean the scope statement
#                       is wrong, not that coverage improved).
# SCOPE_OUT_SUITES      derived tests/qemu suites with no facility control.
# ---------------------------------------------------------------------------
SCOPE_OUT_UNIT_DIRS="kernel/vmsfs"
SCOPE_OUT_SUITES="test_kmod_vmsfs test_kmod_vmsfs_blkdev"

scope_out_why() {
    cat <<'EOF'
vmsfs.ko is a SEPARATE kernel module with its own Makefile, its own
insmod in init.sh and its own /proc/filesystems registration. It is a
filesystem driver, not a facility of the executive: nothing in it is
reached through /dev/vms, it exports no vms_kif entry point, and
src/kernel/vms_module.c dispatches no ioctl to it. CI job 3c already
treats it as independent -- in the executive-absent negative control
vmsfs's suites are the ones REQUIRED to keep passing, precisely because
they do not depend on the executive. Injecting an executive defect
therefore cannot turn them red, and a control that could would be
testing vmsfs, not the executive.
CONSEQUENCE, STATED PLAINLY: this gate proves nothing about vmsfs.ko.
The two vmsfs suites are covered by CI job 3c, not by this one.
EOF
}

# ---------------------------------------------------------------------------
# Metadata
#
#   targets      source files, relative to a src/ root, the mutation edits.
#                EVERY existing copy must change or the fixture is broken.
#   suites_red   the suites this defect is ALLOWED to redden, as shell globs.
#                At least one matching suite must go red (detection); no
#                NON-matching suite may go red (attribution). KEEP THIS AS
#                NARROW AS THE MEASUREMENT: listing a suite that cannot
#                actually detect the defect weakens the printed attribution
#                claim without weakening anything mechanical.
#   blind_suites suites that exercise this facility and OUGHT to detect this
#                defect, but measurably do not. They are asserted GREEN, so
#                the gap is a pinned FACT in CI output rather than something a
#                reader has to infer from suites_red being over-broad. If one
#                ever goes red the control fails and says the gap CLOSED --
#                good news that requires a manifest edit, not a silent pass.
#   blind_why    why they are blind, and the rd item tracking the fix.
#                Mandatory whenever blind_suites is non-empty.
#   require_fail assertion texts that must appear as "  FAIL: <text>" -- the
#                property this defect exists to name.
#   knock_on_fail additional assertion texts that ALSO go red. The driver
#                requires the observed red set to EQUAL require_fail +
#                knock_on_fail exactly, so this is not an allowlist that can be
#                left short: an unlisted extra fails the control.
#   knock_on_why why each knock-on is the same single defect observed again,
#                not a second property. Mandatory whenever knock_on_fail is
#                non-empty.
#   isolation    isolated - every suite must produce a verdict, no suite
#                           outside suites_red may fail.
#                fatal    - the defect takes the GUEST down at the point the
#                           facility is exercised, so there are no verdicts
#                           after it. Only executive-not-pinned uses it, and
#                           its `why` records the measured evidence: unloading
#                           an unpinned vms.ko under an open descriptor Oopses
#                           the guest kernel. The control then asserts what is
#                           actually true and checkable -- every suite BEFORE
#                           it ran clean, and the named pin assertions went red
#                           -- rather than pretending the unload is survivable.
# ---------------------------------------------------------------------------
defect_field() {
    _d="$1"; _f="$2"
    case "$_d" in

    access-mode-escalation)
        case "$_f" in
        facility)     echo "access modes and privileges (VMS_IOCTL_SETMODE/GETMODE/SETPRV/CHKPRIV)";;
        targets)      echo "kernel/vms_access.c";;
        suites_red)   echo "test_kmod_access";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETMOD to KERNEL mode stops requiring CMKRNL: the executive lets a process escalate its own access mode. One condition, inverted.";;
        require_fail) cat <<'EOF'
KERNEL mode denied without CMKRNL
mode still USER after denied escalation
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    ast-setast-disable)
        case "$_f" in
        facility)     echo "AST delivery (VMS_IOCTL_DCLAST/SETAST/DELIVERAST)";;
        targets)      echo "kernel/vms_ast.c";;
        suites_red)   echo "test_kmod_ast";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETAST(disable) stops disabling: the enable flag is written as 1 whatever the caller asked for. Queueing, quota and mode checks are untouched.";;
        require_fail) cat <<'EOF'
disable again: prev state was disabled
SETAST(enable) returns WASCLR
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
prev state was disabled
EOF
                      ;;
        knock_on_why) cat <<'EOF'
test_kmod_ast.c:86-87 reads ONE ioctl result two ways: line 86 checks the
returned status (SS$_WASCLR) and line 87 checks the prev_state field of the
same struct. Both are the executive's answer to the same question -- "was AST
delivery disabled before this call?" -- so a defect in the disable path
necessarily shows up at both. Listing only the status assertion and calling
the field assertion a stray would be arithmetic, not minimality.
EOF
                      ;;
        esac;;

    eflag-clref-noop)
        case "$_f" in
        facility)     echo "event flags (VMS_IOCTL_SETEF/CLREF/READEF/WAITFR/WFLOR/WFLAND/ASCEFC/DACEFC)";;
        targets)      echo "kernel/vms_eflag.c";;
        suites_red)   echo "test_kmod_eflag";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$CLREF stops clearing the bit. It still REPORTS the correct previous state, so only the assertions that read the flag back afterwards can see it -- which is exactly the shape of a facade that reports success while changing nothing.";;
        require_fail) cat <<'EOF'
readef(5) after clear returns WASCLR
cluster has flags 0,3,7,31 set
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    lock-compat-ex-cr)
        case "$_f" in
        facility)     echo "distributed lock manager (VMS_IOCTL_ENQ/DEQ/CONVERT/GETLKI)";;
        targets)      echo "kernel/vms_lock.c";;
        # MEASURED, and deliberately narrower than "the lock suites". The
        # matrix is indexed compat[requested][granted] (vms_lock.c:288), so
        # flipping compat[EX][CR] changes exactly one direction: EX requested
        # while CR is held. test_kmod_lock_mproc and test_syssvc_lock assert
        # the OTHER direction (CR or EX requested while EX is held ->
        # compat[*][EX]), and test_kmod_lock_sync waits on an EX->EX conflict.
        # None of them touch the flipped entry, so none of them can detect it
        # and none of them is "blind" -- they test a different property. Round
        # 1 listed all four here, which let the printed attribution claim cover
        # three suites that could not have contributed to it.
        suites_red)   echo "test_kmod_lock";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "compat[EX][CR] flipped 0 -> 1: an exclusive request is granted against a held concurrent-read lock. THE vms-e4d PRECEDENT, mechanised -- one entry of one matrix, nothing else.";;
        require_fail) cat <<'EOF'
ENQ EX+NOQUEUE denied (CR held)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    lock-compat-cr-ex)
        case "$_f" in
        facility)     echo "distributed lock manager, the OTHER direction of the matrix, reached through the PUBLIC sys\$ API as well as raw ioctls";;
        targets)      echo "kernel/vms_lock.c";;
        suites_red)   echo "test_kmod_lock_mproc test_kmod_lock_sync test_syssvc_lock";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "compat[CR][EX] flipped 0 -> 1: a concurrent-read request is granted against a held EXCLUSIVE lock. The mirror of lock-compat-ex-cr, and it exists because the matrix is indexed compat[requested][granted] (vms_lock.c:288) -- so the EX-over-CR flip cannot reach the cross-process suites, which all assert the EX-held direction. WITHOUT THIS, test_kmod_lock_mproc, test_kmod_lock_sync and test_syssvc_lock were never proven capable of going red by ANYTHING in this manifest, and test_syssvc_lock is the ONLY suite that drives the executive through the public sys\$ entry points.";;
        require_fail) cat <<'EOF'
child: CR+NOQUEUE denied while parent holds EX (EX blocks CR)
child: sys$enq CR+NOQUEUE denied while parent holds EX (public API)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
child: queued CR request still ungranted (NL) per GETLKI
parent: cross-process GETLKI sees child's queued CR request
parent: received blocking AST (astadr=sentinel, astprm=own lkid)
child: DELIVERAST returned completion AST (astadr/astprm sentinels)
parent: child (completion AST) exited clean
child: sys$enqw EX granted after parent's sys$deq (cross-process release, public API)
parent: child's NOQUEUE-denial checks reported via public API
parent: child's post-release retry succeeded via public API
EOF
                      ;;
        knock_on_why) cat <<'EOF'
ONE bit, three suites, ten assertions -- and every one of the eight extras is
the same granted-instead-of-queued request seen further downstream. A CR that
the executive should have QUEUED behind a held EX is instead GRANTED
immediately, so everything that depends on it having waited stops happening:
  mproc  the queue is empty, so GETLKI reports no queued CR from either side
         and the parent's blocking AST is never fired (there is no conflict to
         notify about);
  sync   the child's async CR is granted on the spot, so the completion AST it
         waits for never arrives and the child exits nonzero -- which is what
         reddens the parent's "child exited clean";
  syssvc the child holds a CR it should not; compat[EX][CR] is UNTOUCHED, so
         the child's own CR now blocks its later EX request, and the parent's
         two assertions are reads of the child's report.
No finer mutation exists: this is a single entry of a single matrix, the same
shape as the vms-e4d precedent. Making it finer would mean not flipping it.
NOTE, and it is a finding rather than a defect in this control:
test_kmod_lock_sync.c:285 "child: async CR queued behind parent EX" STAYS GREEN
under this mutation, because it checks only that the $ENQ returned SS$_NORMAL
with a lock id -- which an immediate grant also satisfies. The assertion's text
claims queueing; its condition does not test it. The three assertions that DO
catch it are the ones above.
EOF
                      ;;
        esac;;

    devtab-owner-not-recorded)
        case "$_f" in
        facility)     echo "device table (VMS_IOCTL_ASSIGN/DASSGN/GETDVI/DEVSCAN/TTSETMODE/ALLOC/DALLOC)";;
        targets)      echo "kernel/vms_devtab.c";;
        suites_red)   echo "test_kmod_devtab";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$ASSIGN stops recording the owning process in the shared device entry (the FIRST of the two owner_pid writes; \$ALLOC's is left alone). The channel is still created and still costs a reference, so only the A-writes/B-reads ownership assertions can see it -- Rule 11's decisive test, negatively.";;
        require_fail) cat <<'EOF'
oracle: $ASSIGN to a non-shareable device nobody owns makes the caller its owner
B sees the ownership A took with a channel alone (A writes, B reads)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the refused $ALLOC neither took ownership nor allocated anything
oracle: a channel to the free console makes us its owner, unallocated (TTA0: l.1136-1138)
EOF
                      ;;
        knock_on_why) cat <<'EOF'
All four reds are the SAME missing write observed at four points in
test_kmod_devtab.c, and every one of them is an owner_pid == <pid> read taken
after an $ASSIGN:
  l.300  process A reads its own ownership straight after assigning;
  l.309  process B reads A's ownership (the A-writes/B-reads check);
  l.329  the owner is still recorded after a $ALLOC that was correctly
         REFUSED -- the refusal itself is unaffected and stays green
         ("oracle: $ALLOC is refused while another process owns the device by
         channel alone"), which is what shows this is the ownership write and
         not the allocation logic;
  l.477  a later, fresh $ASSIGN to the now-free console records the owner.
There is no finer mutation available: the executive records ownership on
$ASSIGN in exactly one place, so any defect in it is visible everywhere the
suite reads owner_pid back. The alternative -- naming two of the four and
calling the other two strays -- is the allowlist that was deleted.
EOF
                      ;;
        esac;;

    proctab-duplicate-name)
        case "$_f" in
        facility)     echo "process table (VMS_IOCTL_SETPRN/GETJPI/PROCSCAN)";;
        targets)      echo "kernel/vms_proctab.c";;
        suites_red)   echo "test_kmod_procnam";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETPRN stops rejecting a name already held in the UIC group: the SS\$_DUPLNAM clash test is short-circuited. Name storage, lookup, scan and validation are untouched.";;
        require_fail) cat <<'EOF'
duplicate process name rejected with SS$_DUPLNAM
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    executive-not-pinned)
        case "$_f" in
        facility)     echo "executive residency (vms_fops.owner pins vms.ko while /dev/vms is open)";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_pin";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "fatal";;
        why)          echo "vms_fops loses .owner = THIS_MODULE, so an open descriptor no longer holds a module reference and test_kmod_pin's own rmmod succeeds. FATAL, and measured, not assumed: the guest then takes 'Unable to handle kernel paging request' + 'Internal error: Oops' with Comm: test_kmod_pin, and the run never reaches its own accounting. That IS the guarantee -- an unpinned executive is not a degraded system, it is a dead one -- so the control asserts what is checkable (the eight suites before it ran clean, the three pin assertions went red by name) instead of pretending the unload is survivable.";;
        require_fail) cat <<'EOF'
an open /dev/vms descriptor holds a reference on vms.ko
rmmod vms is REFUSED while a descriptor is open (executive pinned)
the refusal is specifically 'module is in use'
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    pcb-per-thread)
        case "$_f" in
        facility)     echo "PCB identity: one executive process per THREAD GROUP, shared by its threads";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_bind";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_proc_find_or_err() keys the PCB on current->pid (the Linux TID) instead of current->tgid, so a sibling thread of one image no longer resolves to its process's PCB. Invisible to every single-threaded suite -- tgid == pid there -- which is precisely why it needs a control.";;
        require_fail) cat <<'EOF'
sibling thread resolves in the process table
sibling thread sees the process name the main thread set
sibling thread sees the event flag the main thread set
sibling thread can $DEQ the lock the main thread took
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
sibling thread got the PROCESS's entry, not one of its own
sibling thread's cluster state carries the flag bit
EOF
                      ;;
        knock_on_why) cat <<'EOF'
Both are DOWNSTREAM READS of a resolution that already failed, in the same
report struct, at test_kmod_bind.c:611 and :619. The sibling thread fills
trep.linux_pid from its $GETJPI and trep.readef_state from its $READEF; when
the PCB lookup misses, neither call writes its field, so the two assertions
that inspect those fields go red as a consequence of the one that already did.
This is the honest reading of the defect, not a coarse mutation: a thread that
cannot resolve its process's PCB legitimately fails everything downstream of
the lookup, and there is no finer edit -- vms_module.c:86 is the single
tgid-vs-pid site (the register path at :317 is untouched). What DOES stay
green is the guard "the sibling really is a second Linux task", which is what
proves the six reds are about PCB identity and not about the thread never
having run.
EOF
                      ;;
        esac;;

    bind-client-no-register)
        case "$_f" in
        facility)     echo "kernel-interface binding in the PRODUCT (vms_kif kif_bind -> vms_kif_register)";;
        targets)      echo "libvmssys/vms_kif.c";;
        # MEASURED: only test_kmod_bind. Round 1 also listed the three suites
        # below, which permitted four suite groups to redden while only one
        # can -- an attribution claim weaker than it read. They are now
        # declared as what they are: BLIND.
        suites_red)   echo "test_kmod_bind";;
        blind_suites) echo "test_kmod_devtab test_kmod_procnam test_syssvc_lock";;
        blind_why)    cat <<'EOF'
These three drive the product's own vms_kif client, so restoring the vms-9fc
defect (kif_bind() no longer calling vms_kif_register()) SHOULD turn them red.
It does not: each calls vms_kif_open() and vms_kif_register() BY HAND before
using a facility (test_syssvc_lock.c:136-140), supplying the exact product step
kif_bind() exists to perform. MEASURED, not argued -- with the defect injected
all three stay rc=0. They are therefore structurally blind to the entire
auto-bind defect class, which is how vms-9fc survived to be found by
inspection rather than by CI.
Tracked as rd item vms-f27. Do NOT fix it by widening suites_red: that would
re-hide the gap behind a set the control merely permits to redden.
EOF
                      ;;
        isolation)    echo "isolated";;
        why)          echo "kif_bind() stops calling vms_kif_register() -- THE vms-9fc defect, restored on purpose. Only test_kmod_bind detects it; the three client suites that ought to are declared blind_suites and asserted GREEN, so the gap is a fact this job prints rather than one a reader has to infer.";;
        require_fail) cat <<'EOF'
$SETEF reaches the executive with no explicit register
$GETJPI(self) resolves the auto-bound process
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
$READEF sees the flag this process just set
cluster state carries the flag bit
the executive entry is this process's own
$ENQ EX granted through /dev/vms
$ENQ returned a lock id
$DEQ released the lock
forked child's $SETEF reaches the executive
forked child is not rejected as an unregistered task
forked child resolves itself in the process table
child's executive entry is the CHILD's, not the parent's
EOF
                      ;;
        knock_on_why) cat <<'EOF'
TWELVE assertions go red, and that IS the defect rather than evidence against
it: the mutation deletes the ONE call that binds a process to the executive,
and a process with no PCB can use no facility. Round 1 named two of the twelve
and framed the result as narrow ("only test_kmod_bind goes red"), which is
true at suite granularity and misleading at property granularity -- the exact
thing the equality check exists to stop. The twelve are three groups, all the
same missing bind:
  suite 1, auto-bind (5)  -- $SETEF/$READEF/$GETJPI from a process that never
                             registered; the two named in require_fail are the
                             property, the other three read back what those
                             calls failed to do;
  suite 2, lock manager (3) -- $ENQ/$DEQ through the same unbound process;
  suite 3, fork (4)       -- the child re-binds through the same deleted call,
                             so it is unbound too.
Two assertions in the SAME suite stay green and are what keep this from being
a blunderbuss: "$SETEF does not report the caller's parameters as bad" (the
failure is SS$_BUGCHECK, not the SS$_BADPARAM mistranslation vms-9fc also
fixed) and "module rejects an unregistered task with -ESRCH" (the executive
side is untouched and still refuses correctly). Suites 4-6 stay green too --
they register explicitly, which is precisely why the three blind_suites below
cannot see this defect at all.
EOF
                      ;;
        esac;;

    *)  echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2;;
    esac
}

# ---------------------------------------------------------------------------
# apply_edit <file> <defect>
#
# The mutation itself. One sed per defect, anchored to an exact source line.
# ---------------------------------------------------------------------------
apply_edit() {
    _file="$1"; _d="$2"
    case "$_d" in
    access-mode-escalation)
        sed -i 's|if (!(proc->cur_privs \& PRV_M_CMKRNL)) {|if (0 /* NEGCTL access-mode-escalation */) {|' "$_file";;
    ast-setast-disable)
        sed -i 's|ast_state->enabled = args.enable ? 1 : 0;|ast_state->enabled = 1; /* NEGCTL ast-setast-disable */|' "$_file";;
    eflag-clref-noop)
        sed -i 's|\*flags \&= ~(1U << bit);|/* NEGCTL eflag-clref-noop: the bit is not cleared */|' "$_file";;
    lock-compat-ex-cr)
        sed -i 's|/\* EX \*/ {  1,  0,  0,  0,  0,  0 },|/* EX */ {  1,  1,  0,  0,  0,  0 }, /* NEGCTL lock-compat-ex-cr */|' "$_file";;
    lock-compat-cr-ex)
        sed -i 's|/\* CR \*/ {  1,  1,  1,  1,  1,  0 },|/* CR */ {  1,  1,  1,  1,  1,  1 }, /* NEGCTL lock-compat-cr-ex */|' "$_file";;
    devtab-owner-not-recorded)
        # Range-anchored, NOT `0,/re/` (first-match). There are two
        # `dev->owner_pid =` writes -- $ASSIGN's implicit ownership and
        # $ALLOC's -- and mutating both would trip two properties at once.
        # First-match addressing picks the right one but is IDEMPOTENT-UNSAFE:
        # applied twice it silently moves on to $ALLOC's write, which would
        # defeat the `selftest` double-apply check below. Anchoring to the
        # unique `if (!dev->shareable ...)` line that opens the implicit-
        # ownership block makes the edit unrepeatable, so a second apply is
        # the no-op the self-test requires it to be.
        sed -i '/if (!dev->shareable && dev->owner_linux_pid == 0) {/,/^    spin_unlock(&dev->lock);$/ s|dev->owner_pid = proc->vms_pid;|/* NEGCTL devtab-owner-not-recorded */|' "$_file";;
    proctab-duplicate-name)
        sed -i 's|if (clash \&\& clash != proc) {|if (0 \&\& clash != proc) { /* NEGCTL proctab-duplicate-name */|' "$_file";;
    executive-not-pinned)
        sed -i 's|\.owner          = THIS_MODULE,|/* NEGCTL executive-not-pinned: no .owner, so nothing pins vms.ko */|' "$_file";;
    pcb-per-thread)
        sed -i 's|vms_proc_find(current->tgid)|vms_proc_find(current->pid) /* NEGCTL pcb-per-thread */|' "$_file";;
    bind-client-no-register)
        sed -i 's|(void)vms_kif_register((uint32_t)vms_sys_getpid(), 0);|/* NEGCTL bind-client-no-register: the vms-9fc defect, restored */|' "$_file";;
    *)  echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2;;
    esac
}

cmd_list() { echo "$DEFECTS"; }

cmd_scope() {
    echo "executive translation units:  src/kernel/*.c  (every one needs a control)"
    echo "OUT OF SCOPE, translation units: $(for d in $SCOPE_OUT_UNIT_DIRS; do printf 'src/%s/*.c ' "$d"; done)"
    echo "OUT OF SCOPE, suites:            $SCOPE_OUT_SUITES"
    scope_out_why | sed 's/^/  /'
}

cmd_field() {
    [ $# -eq 2 ] || { echo "usage: facility_defects.sh field <defect> <field>" >&2; return 2; }
    defect_field "$1" "$2"
}

# ---------------------------------------------------------------------------
# cmd_apply <defect> <src-root>...
#
# Applies the defect to every copy of every target file that exists under the
# given src roots, and PROVES the edit landed by comparing each file against a
# pristine copy taken immediately before the edit. An anchor that no longer
# matches is a BROKEN FIXTURE: the build must fail loudly rather than produce
# an unmutated image that then "proves" the gate caught nothing.
# ---------------------------------------------------------------------------
cmd_apply() {
    [ $# -ge 2 ] || { echo "usage: facility_defects.sh apply <defect> <src-root>..." >&2; return 2; }
    _d="$1"; shift

    defect_field "$_d" targets >/dev/null || return 2
    _targets=$(defect_field "$_d" targets)

    command -v cmp >/dev/null 2>&1 || {
        echo "FATAL: cmp(1) unavailable -- cannot verify the injection landed" >&2
        return 3
    }

    _touched=0
    for _root in "$@"; do
        [ -d "$_root" ] || continue
        for _t in $_targets; do
            _f="$_root/$_t"
            [ -f "$_f" ] || continue
            cp "$_f" "$_f.negctl-pristine" || return 3
            apply_edit "$_f" "$_d" || return 3
            if cmp -s "$_f" "$_f.negctl-pristine"; then
                echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
                echo "  defect '$_d' did not change $_f -- its sed anchor no longer matches." >&2
                echo "  The source moved; re-anchor the mutation in tests/qemu/facility_defects.sh." >&2
                echo "  Continuing would build an UNMUTATED image and report the gate as having" >&2
                echo "  caught a defect that was never injected." >&2
                rm -f "$_f.negctl-pristine"
                return 3
            fi
            rm -f "$_f.negctl-pristine"
            echo "  injected '$_d' into $_f"
            _touched=$((_touched + 1))
        done
    done

    if [ "$_touched" -eq 0 ]; then
        echo "FATAL: BROKEN FIXTURE (not a broken gate)." >&2
        echo "  defect '$_d' names target(s) [$_targets] but none exist under: $*" >&2
        return 3
    fi
    return 0
}

# ---------------------------------------------------------------------------
# cmd_coverage <src-root> <tests-qemu-dir>
#
# "Every wired executive facility has a negative control" as a mechanical
# check rather than a claim, at BOTH granularities:
#
#   1. TRANSLATION UNIT. Every src/kernel/*.c -- the executive's own code --
#      must be named by at least one defect's targets. Adding a facility file
#      without a control turns this red.
#   2. SUITE. Every derived tests/qemu/test_{kmod,syssvc}_*.c suite must be
#      either (a) in some defect's suites_red, (b) in some defect's
#      blind_suites -- i.e. declared and pinned as a known gap -- or (c) in
#      SCOPE_OUT_SUITES with a stated reason. A suite covered by nothing is
#      never proven capable of going red, and round 1 shipped two of those
#      (the vmsfs pair) without saying so.
#   3. SCOPE CONSISTENCY. Nothing under SCOPE_OUT_UNIT_DIRS may be named by a
#      defect: if it is, the scope statement is wrong and must be corrected
#      rather than quietly outvoted by a control.
#
# The PASS lines below name the exclusions explicitly, so a reader cannot take
# them as a claim about the whole harness.
# ---------------------------------------------------------------------------
cmd_coverage() {
    [ $# -eq 2 ] || { echo "usage: facility_defects.sh coverage <src-root> <tests-qemu-dir>" >&2; return 2; }
    _cov_root="$1"
    _cov_tests="$2"
    [ -d "$_cov_root/kernel" ] || { echo "FAIL: $_cov_root/kernel is not a directory" >&2; return 2; }
    [ -d "$_cov_tests" ] || { echo "FAIL: $_cov_tests is not a directory" >&2; return 2; }

    _cov_rc=0

    _all_targets=""
    _red_globs=""
    _blind_suites=""
    for _cov_d in $DEFECTS; do
        _all_targets="$_all_targets $(defect_field "$_cov_d" targets)"
        _red_globs="$_red_globs $(defect_field "$_cov_d" suites_red)"
        _blind_suites="$_blind_suites $(defect_field "$_cov_d" blind_suites)"
    done

    # --- 1. translation units -------------------------------------------
    _missing=""
    for _cov_c in "$_cov_root"/kernel/*.c; do
        [ -f "$_cov_c" ] || continue
        _rel="kernel/$(basename "$_cov_c")"
        case " $_all_targets " in
            *" $_rel "*) ;;
            *) _missing="$_missing $_rel";;
        esac
    done
    if [ -n "$_missing" ]; then
        echo "FAIL: executive translation unit(s) with NO negative control:$_missing"
        echo "  Every facility vms.ko implements needs a minimal injected defect that"
        echo "  turns its own suite red and names it (vms-e7d). Add one to"
        echo "  tests/qemu/facility_defects.sh -- do not delete this check."
        _cov_rc=1
    else
        echo "PASS: every src/kernel/*.c translation unit is named by a negative control"
    fi

    # --- 3. scope consistency (checked before printing the exclusion) ----
    _bad_scope=""
    for _cov_dir in $SCOPE_OUT_UNIT_DIRS; do
        for _cov_c in "$_cov_root/$_cov_dir"/*.c; do
            [ -f "$_cov_c" ] || continue
            _rel="$_cov_dir/$(basename "$_cov_c")"
            case " $_all_targets " in
                *" $_rel "*) _bad_scope="$_bad_scope $_rel";;
            esac
        done
    done
    if [ -n "$_bad_scope" ]; then
        echo "FAIL: declared OUT OF SCOPE but named by a defect:$_bad_scope"
        echo "  Either the scope declaration (SCOPE_OUT_UNIT_DIRS) is wrong or the"
        echo "  defect is. Fix one; do not leave the manifest saying two things."
        _cov_rc=1
    fi

    # --- 2. suites -------------------------------------------------------
    _cov_derived=$(ls "$_cov_tests"/test_kmod_*.c "$_cov_tests"/test_syssvc_*.c 2>/dev/null \
                   | xargs -n1 basename | sed 's/\.c$//' | sort)
    _uncovered=""
    _blind_only=""
    _n_proven=0
    for _cov_s in $_cov_derived; do
        _hit=0
        for _cov_g in $_red_globs; do
            # shellcheck disable=SC2254
            case "$_cov_s" in $_cov_g) _hit=1; break;; esac
        done
        if [ "$_hit" -eq 1 ]; then
            # Declared out of scope AND reddened by a control: the scope
            # statement is then a lie in the safe direction, but still a lie.
            case " $SCOPE_OUT_SUITES " in
                *" $_cov_s "*)
                    echo "FAIL: $_cov_s is in SCOPE_OUT_SUITES but a defect's suites_red matches it."
                    echo "  The manifest says two things. Drop it from SCOPE_OUT_SUITES."
                    _cov_rc=1;;
            esac
            _n_proven=$((_n_proven + 1))
            continue
        fi
        case " $SCOPE_OUT_SUITES " in
            *" $_cov_s "*) continue;;
        esac
        case " $_blind_suites " in
            *" $_cov_s "*) _blind_only="$_blind_only $_cov_s"; continue;;
        esac
        _uncovered="$_uncovered $_cov_s"
    done
    if [ -n "$_uncovered" ]; then
        echo "FAIL: derived suite(s) that NO negative control can turn red:$_uncovered"
        echo "  A suite nothing can redden is never proven to assert anything. Give it a"
        echo "  facility control, declare it blind_suites with a tracked item, or add it"
        echo "  to SCOPE_OUT_SUITES with a reason -- but do not leave it silent."
        _cov_rc=1
    else
        echo "PASS: $_n_proven derived suite(s) are PROVEN able to go red by a control"
    fi
    # A suite that appears ONLY as somebody's blind_suites is declared and
    # tracked, but nothing in this manifest has ever turned it red -- so its
    # assertions are still unproven. Say so; do not let it count as coverage.
    if [ -n "$_blind_only" ]; then
        echo "NOTE: suite(s) declared blind but never reddened by any control:$_blind_only"
        echo "  These are tracked gaps, not coverage. Nothing here proves their"
        echo "  assertions can fail."
    fi

    # --- the exclusions, stated, never implied ---------------------------
    _n_excl_units=0
    for _cov_dir in $SCOPE_OUT_UNIT_DIRS; do
        for _cov_c in "$_cov_root/$_cov_dir"/*.c; do
            [ -f "$_cov_c" ] && _n_excl_units=$((_n_excl_units + 1))
        done
    done
    echo "SCOPE: this gate does NOT cover $_n_excl_units translation unit(s) under" \
         "$(for d in $SCOPE_OUT_UNIT_DIRS; do printf 'src/%s/ ' "$d"; done)" \
         "or the suite(s) [$SCOPE_OUT_SUITES]."
    scope_out_why | sed 's/^/  /'

    return $_cov_rc
}

# ---------------------------------------------------------------------------
# cmd_selftest <repo-root>
#
# The controls are themselves a gate, so they get a negative control too.
# This needs no container and no QEMU, so it runs in the ordinary ctest job
# and catches manifest drift within seconds instead of twenty minutes.
#
# For every defect, against a throwaway copy of the tree:
#   1. apply once  -> MUST succeed. This is the drift check: a mutation whose
#      sed anchor no longer matches the current source is a fixture that would
#      silently inject nothing, and the QEMU control would then report the
#      gate as having caught a defect that was never injected.
#   2. apply again -> MUST fail with BROKEN FIXTURE. This is the control ON
#      the control: it proves the "did my injection land?" check actually
#      fires, rather than being a cmp that always passes. Every mutation is
#      therefore required to be non-repeatable -- see the devtab entry, whose
#      first-match addressing had to be re-anchored to satisfy this.
#   3. every metadata field the driver reads must be well-formed, INCLUDING
#      the two conditional obligations that keep the equality check honest:
#      a non-empty knock_on_fail requires a knock_on_why, and a non-empty
#      blind_suites requires a blind_why. Without those, "extra assertions go
#      red too" and "this suite ought to catch it and doesn't" could be
#      recorded with no reason given -- which is how the deleted forbid_fail
#      allowlist got away with being short.
# ---------------------------------------------------------------------------
cmd_selftest() {
    [ $# -eq 1 ] || { echo "usage: facility_defects.sh selftest <repo-root>" >&2; return 2; }
    # _st_ prefixes throughout: cmd_apply and cmd_coverage use _root/_d/_f in
    # this same (global) variable namespace, and an earlier version of this
    # function lost its own $_root to cmd_apply's on the first iteration.
    _st_repo="$1"
    _st_root="$_st_repo/src"
    _st_tests="$_st_repo/tests/qemu"
    _st_tmp=$(mktemp -d) || return 2
    _st_rc=0

    for _st_d in $DEFECTS; do
        for _st_fld in facility targets suites_red isolation why require_fail; do
            if [ -z "$(defect_field "$_st_d" "$_st_fld")" ]; then
                echo "FAIL: $_st_d: metadata field '$_st_fld' is empty"
                _st_rc=1
            fi
        done
        case "$(defect_field "$_st_d" isolation)" in
            isolated|fatal) ;;
            *) echo "FAIL: $_st_d: unknown isolation '$(defect_field "$_st_d" isolation)'"; _st_rc=1;;
        esac

        # Conditional obligations: no unexplained extra reds, no unexplained
        # blind suites.
        if [ -n "$(defect_field "$_st_d" knock_on_fail)" ] \
           && [ -z "$(defect_field "$_st_d" knock_on_why)" ]; then
            echo "FAIL: $_st_d: knock_on_fail lists extra assertions with no knock_on_why."
            echo "      An extra red is either the same defect seen again (say so) or"
            echo "      evidence the mutation is too coarse (make it finer)."
            _st_rc=1
        fi
        if [ -n "$(defect_field "$_st_d" blind_suites)" ] \
           && [ -z "$(defect_field "$_st_d" blind_why)" ]; then
            echo "FAIL: $_st_d: blind_suites with no blind_why. A suite that ought to"
            echo "      catch a defect and does not is a tracked finding, not a footnote."
            _st_rc=1
        fi
        # A suite cannot be both permitted to redden and pinned green.
        for _st_b in $(defect_field "$_st_d" blind_suites); do
            for _st_g in $(defect_field "$_st_d" suites_red); do
                # shellcheck disable=SC2254
                case "$_st_b" in $_st_g)
                    echo "FAIL: $_st_d: '$_st_b' is in blind_suites AND matches suites_red glob '$_st_g'"
                    _st_rc=1;;
                esac
            done
        done

        rm -rf "$_st_tmp/tree"
        mkdir -p "$_st_tmp/tree"
        if ! cp -a "$_st_root/kernel" "$_st_root/libvmssys" "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root/{kernel,libvmssys} for the self-test"
            rm -rf "$_st_tmp"
            return 2
        fi

        if cmd_apply "$_st_d" "$_st_tmp/tree" >/dev/null 2>&1; then
            echo "  ok: $_st_d injects into the current tree"
        else
            echo "FAIL: $_st_d: its sed anchor no longer matches the source tree."
            echo "      The mutation would inject NOTHING and the QEMU control would then"
            echo "      certify a defect that was never applied. Re-anchor it."
            _st_rc=1
            continue
        fi

        if _st_out=$(cmd_apply "$_st_d" "$_st_tmp/tree" 2>&1); then
            echo "FAIL: $_st_d: applying it TWICE succeeded. Either the mutation is repeatable"
            echo "      (so it is not the single minimal edit it claims to be) or the"
            echo "      injection-landed check never fires -- in which case a dead anchor"
            echo "      would be reported as a caught defect."
            _st_rc=1
        elif echo "$_st_out" | grep -qF 'BROKEN FIXTURE'; then
            echo "  ok: $_st_d: a no-op re-apply is reported as BROKEN FIXTURE (the check has teeth)"
        else
            echo "FAIL: $_st_d: re-apply failed, but not with BROKEN FIXTURE: $_st_out"
            _st_rc=1
        fi
    done

    rm -rf "$_st_tmp"

    # Every assertion text the driver will look for must EXIST in a suite
    # source. A require_fail/knock_on_fail entry with a typo can never be
    # observed, so under the equality check it would fail the control for the
    # wrong reason -- and under the deleted forbid_fail it would have been
    # vacuously satisfied forever. Catch it here, in seconds, not in QEMU.
    _st_absent=""
    for _st_d in $DEFECTS; do
        for _st_fld in require_fail knock_on_fail; do
            defect_field "$_st_d" "$_st_fld" | while IFS= read -r _st_txt; do
                [ -n "$_st_txt" ] || continue
                grep -qsF -- "$_st_txt" "$_st_tests"/test_kmod_*.c "$_st_tests"/test_syssvc_*.c \
                    || echo "$_st_d/$_st_fld: [$_st_txt]"
            done
        done
    done >"$_st_tmp.absent" 2>/dev/null
    _st_absent=$(cat "$_st_tmp.absent" 2>/dev/null); rm -f "$_st_tmp.absent"
    if [ -n "$_st_absent" ]; then
        echo "FAIL: assertion text(s) named by the manifest that appear in NO suite source:"
        echo "$_st_absent" | sed 's/^/      /'
        echo "      The driver matches these literally. One that no suite can print is a"
        echo "      typo the QEMU control would blame on the executive."
        _st_rc=1
    else
        echo "  ok: every require_fail/knock_on_fail text exists literally in a suite source"
    fi

    cmd_coverage "$_st_root" "$_st_tests" || _st_rc=1

    if [ "$_st_rc" -eq 0 ]; then
        echo "PASS: every negative control injects into the current tree, its"
        echo "      injection-landed check demonstrably fires when it does not, and"
        echo "      every assertion it names is one a suite can actually print."
    fi
    return $_st_rc
}

case "${1:-}" in
    list)     shift; cmd_list "$@";;
    scope)    shift; cmd_scope "$@";;
    field)    shift; cmd_field "$@";;
    apply)    shift; cmd_apply "$@";;
    coverage) shift; cmd_coverage "$@";;
    selftest) shift; cmd_selftest "$@";;
    *)  echo "usage: facility_defects.sh {list|scope|field|apply|coverage|selftest} ..." >&2; exit 2;;
esac
