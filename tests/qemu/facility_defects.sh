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
# table, authenticated identity -- plus two properties of the binding itself:
# that a caller's PCB is per-PROCESS (not per-thread), and that an open
# descriptor pins the module.
# The defects below are outside vms.ko, all because the property they name
# lives in the PRODUCT half of the interface, where no kernel-side mutation
# can reach it:
#   bind-client-no-register  the vms-9fc defect itself (kif_bind() not calling
#                            vms_kif_register()).
#   kif-setmode-always-kernel        vms_kif_setmode() marshalling the
#                            caller's requested mode as a fixed PSL_C_KERNEL
#                            (0) instead of forwarding it, so a caller asking
#                            to DROP to USER has the ioctl sent asking for
#                            KERNEL instead, and is told SS$_NORMAL (vms-0e4).
#                            The KERNEL-direction sibling assertion ("... and
#                            the mode really changed") already re-reads
#                            VMS_IOCTL_GETMODE instead of trusting the
#                            returned status; MEASURED (see require_fail's
#                            knock_on_why below): this defect reddens exactly
#                            one assertion in test_kmod_access.c, the
#                            USER-direction one vms-0e4 added to match it.
#   creprc-handshake-eintr   $CREPRC's report pipe read not retried on EINTR,
#                            so a signal caught by the CALLER decided what the
#                            service reported about the CHILD (vms-8019).
#   run-detached-name-dropped        DCL's RUN/DETACHED not passing
#                            /PROCESS_NAME on to $CREPRC, so a service is
#                            created NAMELESS while the command still reports
#                            success (vms-47b).
#   creprc-detach-intermediate-reaped  $CREPRC leaving the detach
#                            intermediate unreaped, so the creator of a
#                            "detached" process still has a child to wait for
#                            (vms-47b).
#   run-image-qualifier-refused      RUN's subprocess refusal back to
#                            "any qualifier at all", so a RUN (Image)
#                            qualifier (/NODEBUG) is refused as a subprocess
#                            request OpenVMS says it is not, and the image
#                            does not run (vms-47b). The control against
#                            OVER-refusing: every assertion that measures a
#                            refusal stays green under it.
#   run-qualifier-not-abbreviated    RUN back to matching qualifier names
#                            EXACTLY, so /PRIO is not /PRIORITY and /DETACH
#                            is not /DETACHED (vms-47b). Every full spelling
#                            behaves identically under it, which is precisely
#                            why the suite could not see the defect until the
#                            fixtures were written the way operators -- and
#                            mx_start.com -- actually spell qualifiers.
#   run-detached-not-detached        $CREPRC back to accepting PRC$M_DETACH
#                            and discarding it -- the pre-vms-47b behaviour.
#                            This one exists because an adversary applied it
#                            by hand and found that ONE assertion in the whole
#                            suite caught it: "ppid == 1" survives the
#                            mutation, because Linux reparents any orphan to
#                            init. It is here so the discriminating
#                            assertions are NAMED and stay named.
#   kstat-deadlock-mismapped, kstat-ivlockid-mismapped,
#   kstat-cvtungrant-mismapped        src/libvms/syssvc/sys_lock.c's
#                            kstat_to_ss(), the single point where a raw
#                            kernel lock-manager status crosses into the
#                            public ssdef.h SS$_xxx contract (vms-2e5). Each
#                            of these three mutations changes only the
#                            PUBLIC constant kstat_to_ss() returns for a
#                            fixed kernel-side status -- the kernel's own
#                            decision to deadlock/reject is untouched by
#                            THESE THREE mutations. That is not a claim
#                            about kernel-side mutations in general: a pure
#                            constant drift on the KERNEL side (e.g.
#                            SS__DEADLOCK's numeric value in
#                            src/kernel/vms_internal.h) also reddens
#                            test_syssvc_lock_status, because kstat_to_ss()
#                            switches on that same numeric literal. These
#                            three are here because the translation itself
#                            was UNASSERTED at every layer, not because the
#                            kernel side is somehow unreachable.
# All are edits under src/, not src/kernel/, so cmd_selftest copies libvms,
# libvmssys and vmsdcl alongside kernel/ when it checks that every anchor still
# matches.
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
kif-setmode-always-kernel
ast-setast-disable
eflag-clref-noop
eflag-waitfr-eintr-normal
lock-compat-ex-cr
lock-compat-cr-ex
devtab-owner-not-recorded
devtab-alloc-not-recorded
setterm-binding-not-recorded
showterm-width-page-fabricated
showterm-width-page-oracle-shaped
proctab-duplicate-name
proctab-crossgroup-identity
proctab-terminal-redaction-bypassed
ident-username-unguarded
executive-not-pinned
pcb-per-thread
bind-client-no-register
creprc-handshake-eintr
run-detached-name-dropped
creprc-detach-intermediate-reaped
run-detached-not-detached
run-image-qualifier-refused
run-qualifier-not-abbreviated
kstat-deadlock-mismapped
kstat-ivlockid-mismapped
kstat-cvtungrant-mismapped
assign-terminal-bypasses-executive"

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
KERNEL mode DENIED without CMKRNL (SS$_NOPRIV)
... and the mode is still USER after the denied escalation
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
an unprivileged process cannot $SETPRV itself CMKRNL
... and still does not hold CMKRNL afterwards
EOF
                      ;;
        knock_on_why) cat <<'EOF'
THE TWO EXTRAS ARE THE REASON THE GUARD EXISTS, not evidence the mutation is
coarse. vms_access.c:168 computes
    may_exceed = (proc->current_mode == PSL_C_KERNEL) || (cur_privs & SETPRV)
so KERNEL access mode IS the authority to exceed the authorized mask. The
unprivileged child (test_kmod_access.c:100-121) escalates, then immediately
tries to $SETPRV itself CMKRNL; with the escalation wrongly allowed it is now
in KERNEL mode when it asks, so it gets the privilege and holds it afterwards.
One inverted condition, one process, one straight line: a defect that lets a
process into KERNEL mode does not stop at the mode. Naming only the two
setmode assertions would describe the smaller half of what the mutation
actually proves.
These reds appeared when vms-2b8 landed -- before it, $SETPRV had no
mode-derived authority. The equality check caught the change on the first run
after the rebase and named both extras with their suite; the previous
allowlist would have passed silently, which is precisely the failure this
round was re-dispatched to fix.
EOF
                      ;;
        esac;;

    kif-setmode-always-kernel)
        case "$_f" in
        facility)     echo "access-mode marshalling in the PRODUCT (vms_kif_setmode, src/libvmssys/vms_kif.c)";;
        targets)      echo "libvmssys/vms_kif.c";;
        suites_red)   echo "test_kmod_access";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_kif_setmode() writes args.mode as a fixed PSL_C_KERNEL (0) instead of the caller's requested mode, so a caller asking to drop to USER has the ioctl sent asking for KERNEL instead -- and it still reports SS\$_NORMAL, because a process already in KERNEL mode staying in KERNEL mode is unremarkable to the executive.";;
        require_fail) echo "... and the mode really returned to USER";;
        knock_on_fail) echo "";;
        knock_on_why) cat <<'EOF'
KNOWN NEGATIVE: this mutation is NOT a blunderbuss on top of
access-mode-escalation even though both touch VMS_IOCTL_SETMODE. It forces
args.mode to PSL_C_KERNEL (0) UNCONDITIONALLY, so the parent's first setmode
call (already requesting PSL_C_KERNEL) is byte-for-byte unaffected -- "KERNEL
mode ALLOWED with CMKRNL" and "... and the mode really changed" both stay
green. The unprivileged child's escalation attempt (also requesting
PSL_C_KERNEL) is likewise unaffected by this defect, so "KERNEL mode DENIED
without CMKRNL (SS\$_NOPRIV)" and "... and the mode is still USER after the
denied escalation" stay green too -- the child never calls setmode(USER), so
this defect never reaches it. Only the parent's PSL_C_USER request is
silently rewritten to PSL_C_KERNEL, and only the assertion that re-reads
VMS_IOCTL_GETMODE afterwards -- not the one that reads the returned status --
can see that the mode never moved. MEASURED empty: a run with this defect
injected produces exactly one FAIL line in test_kmod_access, confirming the
mutation is already as fine as it can be.
EOF
                      ;;
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
SETAST(enable) returns WASCLR (== prev state was disabled)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why) echo "";;
        esac;;

    eflag-clref-noop)
        case "$_f" in
        facility)     echo "event flags (VMS_IOCTL_SETEF/CLREF/READEF/WAITFR/WFLOR/WFLAND/ASCEFC/DACEFC)";;
        targets)      echo "kernel/vms_eflag.c";;
        # test_syssvc_ef_mproc MOVED HERE FROM blind_suites BY vms-2a8, which
        # is the change the old blind_why entry named as the condition:
        # "Move this suite into suites_red once sys_event.c calls the
        # executive." src/libvms/syssvc/sys_event.c is now a translation
        # layer over /dev/vms and holds no flag state of its own, so a defect
        # injected into kernel/vms_eflag.c reaches the PUBLIC sys$ API too.
        # This suite is no longer blind to the executive, and the entry below
        # proves it by naming the assertion it loses.
        #
        # test_syssvc_ef_local arrives with vms-2a8's conformance MOVE (it is
        # tests/conformance/vms_programs/test_event_flags.c, relocated off a
        # host that has no /dev/vms). It is DERIVED FROM A RUN, not reasoned
        # into place: with this defect injected it goes rc=1 on exactly one
        # assertion, "sys$readef(1) reported WASCLR after the clear", which is
        # the same read-back-after-clear property as test_kmod_eflag's
        # "readef(5) after clear returns WASCLR" one layer down. Its other
        # fifteen assertions stay green, which is what keeps this mutation
        # minimal rather than a blunderbuss on the new suite.
        suites_red)   echo "test_kmod_eflag test_kmod_eflag_mproc test_syssvc_ef_mproc test_syssvc_ef_local";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$CLREF stops clearing the bit. It still REPORTS the correct previous state, so only the assertions that read the flag back afterwards can see it -- which is exactly the shape of a facade that reports success while changing nothing.";;
        require_fail) cat <<'EOF'
readef(5) after clear returns WASCLR
cluster has flags 0,3,7,31 set
child: a common flag CLEARED BY THE PARENT reads clear here (A clears, B reads)
child: a common flag CLEARED BY THE PARENT via sys$clref reads clear here (A clears, B reads, public API)
sys$readef(1) reported WASCLR after the clear
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    eflag-waitfr-eintr-normal)
        case "$_f" in
        facility)     echo "event flag WAITS (VMS_IOCTL_WAITFR/WFLOR/WFLAND), interrupted-wait path";;
        targets)      echo "kernel/vms_eflag.c";;
        suites_red)   echo "test_syssvc_ef_mproc";;
        blind_suites) echo "test_kmod_eflag test_kmod_eflag_mproc test_syssvc_ef_local";;
        blind_why)    cat <<'EOF'
Neither raw-ioctl event flag suite arranges a signal, so neither can observe
what a wait reports when one arrives. That is not a gap to close by adding a
signal to them: the property is about what the PUBLIC service returns to its
caller, and both halves of the fix live on that path (the executive writing no
status, and libvmssys re-entering the wait). test_syssvc_ef_mproc is where a
caller exists to be lied to.
test_syssvc_ef_local is blind for a second, different reason and is declared
here rather than left silent: its $WAITFR call is on a flag that is ALREADY
SET, so it returns without ever blocking and there is no wait for a signal to
interrupt. It stays green with this defect injected -- MEASURED on this host,
not assumed -- and the honest statement is that it drives $WAITFR without
covering this property at all.
EOF
                      ;;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
WAITFR answers SS$_NORMAL when wait_event_interruptible() returns because a
signal was pending -- "the flag is set" about a flag that is still clear, with
rc=0/errno=0 so the caller cannot detect it. This is the code as it actually
shipped for one round of vms-2a8, restored verbatim.
IT IS THE SAME DEFECT CLASS AS creprc-handshake-eintr BELOW, one layer down: a
signal delivered to the CALLER decides what a system service reports about the
world. VMS has no status for it to report -- HELP $WAITFR says the process
waits "until the event flag is set" and has no Condition Values topic at all,
HELP $HIBER says a wait is interrupted by ASTs and then continues, and a SEARCH
of $SSDEF for WAIT/INTERRUPT/ABORTED finds no such condition
(docs/oracle/vax73-event-flags.md §4). So the correct code makes the condition
unreachable rather than handling it, and this mutation is exactly the illegal
third answer put back.
IT ALSO REDDENS IF ONLY HALF THE FIX IS PRESENT. With the executive corrected
but libvmssys not re-entering the wait, the ioctl surfaces -EINTR, which
vms_kif_kerr_to_ss maps to SS$_BUGCHECK -- an even status -- so the same
assertion fails. One control, both halves.
EOF
                      ;;
        require_fail) cat <<'EOF'
parent: sys$waitfr did NOT return until the flag was really set -- an interrupted wait is re-entered, never reported as SS$_NORMAL over a clear flag
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: the waiter was interrupted by a signal repeatedly WHILE blocked in sys$waitfr (the condition under test is reachable, not hypothetical)
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
Same single defect, seen one step earlier. The waiter's interval timer keeps
firing for as long as it is genuinely waiting, so the parent counts three
interrupts before releasing the flag. A WAITFR that returns on the first
interrupt never gets interrupted a second time, so the interrupt count
collapses to one and this assertion goes red as a direct consequence of the
same return -- it is the cause, and the require_fail line is the effect.
EOF
                      ;;
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
        suites_red)   echo "test_kmod_lock_mproc test_kmod_lock_sync test_syssvc_lock test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "compat[CR][EX] flipped 0 -> 1: a concurrent-read request is granted against a held EXCLUSIVE lock. The mirror of lock-compat-ex-cr, and it exists because the matrix is indexed compat[requested][granted] (vms_lock.c:288) -- so the EX-over-CR flip cannot reach the cross-process suites, which all assert the EX-held direction. WITHOUT THIS, test_kmod_lock_mproc, test_kmod_lock_sync and test_syssvc_lock were never proven capable of going red by ANYTHING in this manifest, and test_syssvc_lock and test_syssvc_lock_status (vms-2e5) are the suites here that drive the executive through the public sys\$ entry points -- test_kmod_lock_mproc and test_kmod_lock_sync go through raw ioctls instead.";;
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
sys$enq(LCK$M_CONVERT) on a lock still queued (waiting) reports SS$_CVTUNGRANT (public API)
EOF
                      ;;
        knock_on_why)
            # _n_suites/_n_assert are DERIVED from suites_red/require_fail/
            # knock_on_fail above, not hand-recited -- this sentence already
            # drifted once (three suites/ten assertions -> four/eleven) when
            # test_syssvc_lock_status was added, the same class of drift the
            # bind-client-no-register `why` field's _n/_list computation
            # above exists to make structurally impossible. $() runs in a
            # subshell, so the recursive defect_field calls below cannot
            # clobber this call's own _d/_f.
            _n_suites=$(set -- $(defect_field "$_d" suites_red); echo $#)
            _n_req=$(defect_field "$_d" require_fail | grep -c .)
            _n_knock=$(defect_field "$_d" knock_on_fail | grep -c .)
            _n_assert=$((_n_req + _n_knock))
            cat <<'EOF' | sed "s/@N_SUITES@/$_n_suites/; s/@N_ASSERT@/$_n_assert/"
ONE bit, @N_SUITES@ suites, @N_ASSERT@ assertions -- and every one of the extras is the
same granted-instead-of-queued request seen further downstream. A CR that the
executive should have QUEUED behind a held EX is instead GRANTED immediately,
so everything that depends on it having waited stops happening:
  mproc  the queue is empty, so GETLKI reports no queued CR from either side
         and the parent's blocking AST is never fired (there is no conflict to
         notify about);
  sync   the child's async CR is granted on the spot, so the completion AST it
         waits for never arrives and the child exits nonzero -- which is what
         reddens the parent's "child exited clean";
  syssvc the child holds a CR it should not; compat[EX][CR] is UNTOUCHED, so
         the child's own CR now blocks its later EX request, and the parent's
         two assertions are reads of the child's report;
  status (test_syssvc_lock_status, vms-2e5) scenario_cvtungrant's own setup
         queues a CR behind a held EX the same way test_syssvc_lock's does --
         under this mutation the CR is granted immediately instead, so the
         follow-up LCK$M_CONVERT lands on an ALREADY-GRANTED lock rather than
         a waiting one and the kernel has no reason to reject it with
         SS__CANCELGRANT, so kstat_to_ss() is never asked to translate
         SS$_CVTUNGRANT at all. This is the SAME defect knocking on the SAME
         setup pattern (CR queued behind EX) that test_syssvc_lock already
         names above -- not a second, independent property of
         test_syssvc_lock_status.
No finer mutation exists: this is a single entry of a single matrix, the same
shape as the vms-e4d precedent. Making it finer would mean not flipping it.
NOTE, and it is a finding rather than a defect in this control:
test_kmod_lock_sync.c "child: async CR queued behind parent EX" STAYS GREEN
under this mutation, because it checks only that the $ENQ returned SS$_NORMAL
with a lock id -- which an immediate grant also satisfies. The assertion's text
claims queueing; its condition does not test it. The assertions that DO catch
it are the ones above.
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

    devtab-alloc-not-recorded)
        case "$_f" in
        facility)     echo "device table (VMS_IOCTL_ASSIGN/DASSGN/GETDVI/DEVSCAN/TTSETMODE/ALLOC/DALLOC)";;
        targets)      echo "kernel/vms_devtab.c";;
        # test_syssvc_showdev.c (vms-fb9) is the derived suite that drives
        # SHOW DEVICE through the real DCL.EXE -- landed after this manifest
        # existed, and facility_negctl_manifest's own coverage check requires
        # every derived suite to be reachable by a control. devinfo_fill() is
        # the one place BOTH $GETDVI and $DEVICE_SCAN copy the flag out of,
        # so this is the narrowest mutation that can redden the user-visible
        # reader without disturbing $ALLOC/$DALLOC's own bookkeeping (which
        # reads dev->allocated directly, never through this copy).
        suites_red)   echo "test_kmod_devtab test_syssvc_showdev";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "devinfo_fill()'s copy-out of the allocation flag is pinned to 0, so a device that IS allocated is reported as though it were not -- to every reader, GETDVI and DEVSCAN alike, which is the shared snapshot function both ioctls funnel through. \$ALLOC/\$DALLOC still work correctly against dev->allocated itself (the DEVALLOC refusal, refcnt bookkeeping and release-on-exit are all untouched), so this isolates the DISPLAY from the STATE -- the same class of defect Rule 11 exists to catch, now on the read side.";;
        require_fail) cat <<'EOF'
device reports itself allocated
B sees that A allocated the device
oracle: allocating what we already own adds the allocation and one reference (OPA0: 2 -> 3, l.682)
A-WRITES/B-READS: DCL's SHOW DEVICE reports the console allocated -- a change made by a DIFFERENT process, which a per-process device view could not show
the bare listing shows it too, so both row sources ($DEVICE_SCAN and $GETDVI) read the same shared table
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    setterm-binding-not-recorded)
        case "$_f" in
        facility)     echo "job-to-terminal binding (VMS_IOCTL_SETTERM, read back through GETJPI)";;
        targets)      echo "kernel/vms_devtab.c";;
        suites_red)   echo "test_kmod_setterm test_syssvc_showterm";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "VMS_IOCTL_SETTERM still resolves the channel, still refuses a channel the caller does not hold, still checks the device class and still returns SS\$_NORMAL -- it just never writes the name into the executive's process row. The caller is told the binding was made and no other process can see it, which is the precise shape of the facade CLAUDE.md rule 11 exists to catch: correct-looking output, nothing shared. Everything else in the device table is untouched (SHOW DEVICE, \$ALLOC, IO\$_SETMODE and their suites all stay green), so this isolates the BINDING from the device table it is built on.";;
        require_fail) cat <<'EOF'
A-WRITES/B-READS: B reads OPA0: out of A's row -- a binding a different process made, which a per-process binding could not show
SHOW TERMINAL names _OPA0: once the executive holds the binding -- the SAME BINARY that named nothing a moment ago
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
A's own row now names the console
the freshly activated image still finds its terminal -- nothing was carried across execve() in userspace
and B's row now names the console
the bound name is the device table's name for that channel
the characteristics heading is printed (oracle section 2)
grid row 1 is byte-for-byte the V7.3 capture
grid row 2 is byte-for-byte the V7.3 capture
grid row 4 is byte-for-byte the V7.3 capture
grid row 10 is byte-for-byte the V7.3 capture
the last row carries the single remaining characteristic, unpadded
...and the cleared Echo bit, in the grid cell the oracle prints it in
...and the set Pasthru bit, so both directions of one IO$_SETMODE are read back
...and grid row 1 is the oracle's bytes again, so neither is the grid
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
Every entry is the SAME missing write, observed further downstream, and the
set was MEASURED by running the mutation rather than predicted.

The first four are test_kmod_setterm's remaining reads of a binding that was
never recorded: the binder's own row, the row the re-execed image reads, the
second binder's row, and the cross-check that the recorded name is the device
table's name for that channel. No finer mutation separates them -- there is
exactly one write, and every one of these is a read of it.

The rest are test_syssvc_showterm's, and they are all one consequence:
cmd_show_terminal reads the binding FIRST and prints nothing at all when there
is none, so with the write gone, the entire SHOW TERMINAL output disappears
and every assertion about its content goes with it -- the header, each pinned
grid row, and both directions of the A-writes/B-reads characteristic check.
That is not a second defect: it is the reader behaving exactly as it must
when the executive reports no terminal.

NOT HERE, AND MEASURED RATHER THAN ASSUMED (vms-d0b): the three Width/Page
assertions this list used to carry are gone, not just renamed. SHOW TERMINAL
stopped printing a Width/Page line at all (this fix deleted the one-line
layout that was never oracle-observed -- see
show_terminal_render() in src/vmsdcl/dcl_cmd_show.c), so
tests/qemu/test_syssvc_showterm.c now asserts Width/Page's ABSENCE in every
state. An absence check is trivially satisfied when the whole binding
disappears -- there being no Width/Page line either way -- so this mutation
cannot make it red, and re-running the control after the round-3 edit
confirmed exactly that: test_syssvc_showterm's contribution to the red set
(require_fail's 1 plus this suite's share of knock_on_fail) shrank from 13 to
10 without this manifest changing, until this entry was corrected to match.

What stays GREEN is what makes this isolated rather than a blunderbuss: the
unbound run still names nothing, the SS$_IVCHAN refusal still fires, the row
still disappears when the job exits, and every other device-table and
process-table suite is untouched.
EOF
                      ;;
        esac;;

    showterm-width-page-fabricated)
        case "$_f" in
        facility)     echo "SHOW TERMINAL renderer, Width/Page absence (show_terminal_render(), vms-d0b)";;
        # DCL, not vms.ko -- the absence is a DISPLAY choice, not an
        # executive fact (Width/Page are already A-writes/B-reads proven
        # at the kernel layer by test_kmod_devtab.c). One of the entries
        # whose property lives in the product half of the interface; see
        # the "outside vms.ko" note near the top of this file for the set
        # (not a position within it -- that note names them, it does not
        # number them, and neither does this one).
        targets)      echo "vmsdcl/dcl_cmd_show.c";;
        suites_red)   echo "test_syssvc_showterm";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "show_terminal_render() prints a fabricated Width/Page line -- exactly the ONE-LINE layout vms-d0b deleted because docs/oracle/vax73-terminal-device.md never shows it (section 2 puts Width and Page on separate lines, each sharing the line with fields OVMX cannot source). Nothing else in the renderer changes: the header, the characteristic grid and every other row print exactly as before, so only the three assertions that require a Width/Page VALUE'S absence -- not one particular line's absence, see showterm-width-page-oracle-shaped below for why that distinction has its own defect -- can see this.";;
        require_fail) cat <<'EOF'
SHOW TERMINAL prints no Width or Page VALUE anywhere in its output, in any layout -- not the one-line form vms-d0b deleted and not the oracle's own two-line Input/Output/LFfill/CRfill/Width/Page/Parity block either, with its unsourceable fields left blank (docs/oracle/vax73-terminal-device.md section 2)
...still no Width or Page value anywhere in the output while the width IS 80 at the executive -- not printing it is a display choice, not a value the reader lost
...and still no Width or Page value anywhere in the output once the width is back to 132 -- the absence does not track the value either
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    showterm-width-page-oracle-shaped)
        case "$_f" in
        facility)     echo "SHOW TERMINAL renderer, Width/Page absence -- SECOND SHAPE (show_terminal_render(), vms-d0b)";;
        # THE DEFECT showterm-width-page-fabricated'S TEETH WERE SPELLING-
        # SPECIFIC, MEASURED. An adversary injected the ORACLE-SHAPED
        # two-line Width/Page block instead of the invented one-line
        # layout -- the exact "pin it, leave the unsourceable fields
        # blank" option show_terminal_render()'s own comment names and
        # rejects -- and the has_line_prefix()-based checks that existed
        # at the time never saw it: "   Width:" and "   Page:" only open
        # the ONE-LINE form; in the oracle's own layout they sit mid-line
        # after "   Input:" / "   Output:", so a line-PREFIX check is
        # blind to them. test_syssvc_showterm went 18/0 with the property
        # it exists to forbid actively happening. The fix was at the
        # ASSERTION (has_substr() over the whole capture, not
        # has_line_prefix()), not here; this entry exists so that fixed
        # assertion is PROVEN against the shape that defeated the old one,
        # the same way its sibling proves it against the first shape.
        targets)      echo "vmsdcl/dcl_cmd_show.c";;
        suites_red)   echo "test_syssvc_showterm";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "show_terminal_render() prints a fabricated Width/Page block in the OTHER shape vms-d0b considered and rejected -- the oracle's own two-line Input:/Output:/LFfill:/CRfill:/Width:/Page:/Parity: layout (docs/oracle/vax73-terminal-device.md section 2), with the fields OVMX cannot source (Input, Output, LFfill, CRfill, Parity) left blank rather than invented. That blank-field pinning was rejected in the renderer's own comment as 'the same fabrication one field further in', and this control is the proof: it must be caught by the same three assertions as its one-line sibling, not by a fourth assertion invented to notice this specific spelling.";;
        require_fail) cat <<'EOF'
SHOW TERMINAL prints no Width or Page VALUE anywhere in its output, in any layout -- not the one-line form vms-d0b deleted and not the oracle's own two-line Input/Output/LFfill/CRfill/Width/Page/Parity block either, with its unsourceable fields left blank (docs/oracle/vax73-terminal-device.md section 2)
...still no Width or Page value anywhere in the output while the width IS 80 at the executive -- not printing it is a display choice, not a value the reader lost
...and still no Width or Page value anywhere in the output once the width is back to 132 -- the absence does not track the value either
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    proctab-duplicate-name)
        case "$_f" in
        facility)     echo "process table (VMS_IOCTL_SETPRN/GETJPI/PROCSCAN)";;
        targets)      echo "kernel/vms_proctab.c";;
        # BOTH layers of the same refusal: the raw-ioctl suite and the
        # public-API suite. vms-8019 made $CREPRC report the executive's own
        # clash status to the CREATOR, so the same single kernel edit is now
        # visible through sys$creprc as well -- and MEASURED to be: with the
        # clash test short-circuited, test_syssvc_procnam reddens exactly one
        # assertion, its own name for the same property.
        # test_syssvc_startup_service is here as of vms-47b: that suite drives
        # the SAME clash through the USER-VISIBLE command (a second
        # RUN/DETACHED under a name already held), so the one kernel edit is
        # now visible at a third layer. It was MEASURED, not assumed -- see
        # knock_on_why. This entry was missing while the suite's DUPLNAM
        # assertions existed, and the whole 17-defect sweep had not been run
        # since they landed, so the control was quietly failing.
        suites_red)   echo "test_kmod_procnam test_syssvc_procnam test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETPRN stops rejecting a name already held in the UIC group: the SS\$_DUPLNAM clash test is short-circuited. Name storage, lookup, scan and validation are untouched. The raw-ioctl suite, the public sys\$ suite and the DCL command suite each name it.";;
        require_fail) cat <<'EOF'
duplicate process name rejected with SS$_DUPLNAM
sys$creprc refuses a duplicate process name with SS$_DUPLNAM
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
starting the same named service twice is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM
the service's name is released when the service dies
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
Both are the SAME short-circuited clash test, seen from the command layer
rather than from the ioctl, and both were measured rather than predicted.
The first IS the property require_fail names, reached through DCL: with the
clash test disabled, a second RUN/DETACHED under a name already held succeeds
where it must print %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM.
The second is the direct consequence of the first WITHIN THE SAME RUN: the
duplicate start that should have been refused leaves a SECOND live process
holding the name, so killing the first cannot release it, and the assertion
that the name belongs to the live process goes red too. It is not a second
defect and no finer mutation could separate them -- the mutation is already
one condition, and the two assertions are the same clash observed before and
after the duplicate exists.
EOF
                      ;;
        esac;;

    proctab-crossgroup-identity)
        case "$_f" in
        facility)     echo "process table, cross-UIC-group identity read (VMS_IOCTL_GETJPI/PROCSCAN authorisation)";;
        targets)      echo "kernel/vms_proctab.c";;
        # All three layers of the same clause, MEASURED not guessed:
        # test_syssvc_showproc names the property through the user-visible
        # command (SHOW PROCESS/ID on an out-of-group process must be
        # refused), test_syssvc_procnam sees it as a row that stops being
        # redacted, and test_kmod_ident sees it at the raw ioctl. The first
        # run of this control listed only the two syssvc suites and the
        # driver's equality check rejected it, naming test_kmod_ident's five
        # assertions -- which is the check doing exactly its job.
        suites_red)   echo "test_syssvc_showproc test_syssvc_procnam test_kmod_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_proc_may_read() stops requiring WORLD for a cross-UIC-group read: any caller may read any process's identity. ONE clause, and it is the one the oracle pinned -- docs/oracle/vax73-privileges.md Section 5.4 measured that GROUP does not lift a cross-group read and WORLD does. Storage, lookup, naming, scanning and the same-group rule are untouched.";;
        require_fail) cat <<'EOF'
SHOW PROCESS/ID on an out-of-group process reports %SYSTEM-F-NOPRIV verbatim
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the by-PID refusal printed no process header
the UNREADABLE row fabricates NO CPU figure at all
WORLD CLAUSE ISOLATED: the same cross-group read, now without WORLD -> SS$_NOPRIV
an unprivileged process is REFUSED a process in another UIC group
... and gets no part of that process's identity
... and the refusal returns no part of the row
... but WITHOUT its user name, UIC or privilege mask
TERMINAL REDACTION: the scan withholds D's terminal too, even though D genuinely bound one -- a caller that may not read D's identity may not read which terminal D is on either
EOF
                      ;;
        knock_on_why) cat <<'EOF'
EVERY EXTRA IS THE SAME REFUSAL SEEN FROM A DIFFERENT SIDE, not a second
property. All eight were MEASURED by running this control, not predicted:
the first run named only the two SHOW-PROCESS assertions and the driver's
equality check rejected it and printed the rest. The eighth (the terminal
one) arrived on a LATER run, the same way -- not predicted, READ OFF A RUN --
after vms-d0b added a terminal field to the same redacted row this control
already opens up (see below).

"the by-PID refusal printed no process header" is the paired negative of the
require_fail assertion above: with the read wrongly ALLOWED, SHOW PROCESS/ID
succeeds and prints a full header for the out-of-group process, so the
same command that stopped being refused necessarily stopped being silent.
Listing only the message assertion would describe half of one event.

The by-NAME half of that block is deliberately absent from both lists, and
that is the control doing its job: find_by_name() is group-scoped in its own
right (src/kernel/vms_proctab.c:237) and never consults vms_proc_may_read(),
so mutating the WORLD clause cannot touch it. SHOW PROCESS <name> keeps
answering %SYSTEM-W-NONEXPR. Before vms-6a7 round 2 the two selectors shared
one DCL session and one combined capture, so this distinction was invisible
here -- the block could only say that both messages appeared somewhere.

"the UNREADABLE row fabricates NO CPU figure at all" (test_syssvc_procnam
block P12) is the enumeration side of the identical decision:
vms_ioctl_procscan() calls proc_fill_info() with vms_proc_may_read()'s
outcome as its `full` argument (src/kernel/vms_proctab.c:609), so a row that
becomes readable stops being redacted, keeps its linux_pid, and SHOW SYSTEM
can then source a CPU figure for it. One clause governs both the item read
and the row redaction -- which is the design, not a coincidence: identity is
privileged and enumeration is not (docs/oracle/vax73-privileges.md Section
5.5), and this clause is where that split is decided.

THE SIX test_kmod_ident REDS are the SAME clause one layer down, at the raw
ioctl rather than through the public sys$ API and DCL. That suite's own
wording says so -- "WORLD CLAUSE ISOLATED: the same cross-group read, now
without WORLD -> SS$_NOPRIV" is vms-2b8's isolation of exactly this
condition. Four more are that assertion's paired negatives (the refusal
must return no part of the row) and the unprivileged-caller form of it.
There is no finer edit available: vms_proc_may_read() IS the clause, and
every suite that exercises a cross-group read reaches it. Splitting the
mutation further would mean mutating a caller instead of the rule, which
would test the caller.

THE SIXTH, the terminal one, is the SAME clause reaching a field that did
not exist when the other five were written. vms_ioctl_procscan() (src/
kernel/vms_proctab.c) calls proc_fill_info() with vms_proc_may_read()'s
outcome as `full`, and proc_fill_info() withholds proc->terminal on exactly
that condition (vms-d0b) -- the identical decision "the UNREADABLE row
fabricates NO CPU figure at all" names two paragraphs up, for a field
added later. With the WORLD clause deleted, D's row stops being redacted
altogether, so test_kmod_ident's D genuinely has a terminal to leak (it
binds one, see process_d() in that file) and this control makes it leak.
The check is proctab-terminal-redaction-bypassed's OWN concern from the
other direction -- that control mutates proc_fill_info() directly and
reddens only this one assertion; this control mutates the authorisation
one layer up and reddens six, one of which happens to be the same
assertion, MEASURED, not designed to overlap.

NOT reddened, and worth stating because it is the attribution: the
SAME-GROUP reads stay green throughout (SHOW PROCESS <name> on the subject,
SHOW PROCESS/ID=<subject>, the self case), as does every by-NAME lookup --
find_by_name() is group-scoped independently of this clause, which is why
"SHOW PROCESS <name> on an out-of-group process reports %SYSTEM-W-NONEXPR"
survives the mutation. That is the measured evidence that this control names
the cross-group AUTHORISATION and nothing else.
EOF
                      ;;
        esac;;

    proctab-terminal-redaction-bypassed)
        case "$_f" in
        facility)     echo "process table, terminal field on a redacted row (proc_fill_info(), vms-d0b)";;
        targets)      echo "kernel/vms_proctab.c";;
        # This is proc_fill_info()'s OWN clause, not vms_proc_may_read()'s:
        # the caller's authorisation is untouched, so vms_ioctl_getjpi()'s
        # direct refusal (SS$_NOPRIV, no row at all) never calls
        # proc_fill_info() with full=false in the first place and cannot
        # observe this defect. Only vms_ioctl_procscan() calls it that way
        # (SHOW SYSTEM / PROCSCAN enumeration), so test_kmod_ident's scan_d
        # check -- the only assertion in the tree that binds a real terminal
        # to a process it then reads back through a redacted PROCSCAN row --
        # is the only place this can go red. MEASURED: the full 25-suite
        # sweep with the mutation applied reddened exactly this one
        # assertion, nothing in test_syssvc_showproc or test_syssvc_procnam
        # (both drive $GETJPI, which never reaches the mutated line).
        suites_red)   echo "test_kmod_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "proc_fill_info() copies proc->terminal into the redacted branch, BEFORE the early return that withholds the rest of the identity (linux_pid, uic, privileges, username). vms_pid and prcnam are legitimately copied before that return too (the oracle shows SHOW SYSTEM naming every process including cross-group ones), but terminal is identity data the oracle refused with everything else (docs/oracle/vax73-privileges.md §5) -- so copying it before the return, rather than after with username, hands an enumerating caller a fact about a process it may not \$GETJPI.";;
        require_fail) cat <<'EOF'
TERMINAL REDACTION: the scan withholds D's terminal too, even though D genuinely bound one -- a caller that may not read D's identity may not read which terminal D is on either
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    ident-username-unguarded)
        case "$_f" in
        facility)     echo "authenticated identity (VMS_IOCTL_SETIDENT: user name, UIC and authorized mask)";;
        targets)      echo "kernel/vms_proctab.c";;
        suites_red)   echo "test_kmod_ident";;
        blind_suites) echo "test_syssvc_ident";;
        blind_why)    cat <<'EOF'
test_syssvc_ident.c (vms-2b8) drives VMS_IOCTL_SETIDENT through DCL.EXE
rather than raw ioctls, but MEASURED, not assumed, to stay green under this
mutation (run_facility_negctl.sh ident-username-unguarded, coverage gap
found and closed vms-2b8 this round): every one of its scenarios that
attempts an identity a caller is NOT entitled to (C: unprivileged claimant,
D: session subprocess) sets BOTH a UIC that is not its own AND a mask that
is not a subset of its authorized one, so the UIC clause or the mask-subset
clause -- both left intact by this mutation, which disables only the name
clause -- still refuses the call and the suite's assertions do not move.
Its two scenarios that DO succeed (A, B) run as a privileged writer holding
SETPRV, which skips the whole guarded block (including the name clause)
regardless of this mutation. So no scenario in this suite isolates the name
clause alone; test_kmod_ident's own USER NAME CLAUSE ISOLATED cases
(same UIC, same mask, different name) are what this defect actually
exercises. Closing this gap means adding a test_syssvc_ident scenario that,
like test_kmod_ident's, holds the UIC and mask clauses constant and varies
only the name -- tracked under vms-2b8, not fixed here.
EOF
                      ;;
        isolation)    echo "isolated";;
        why)          echo "\$SETIDENT stops guarding the USER NAME for a caller without SETPRV: the name clause of the one-way-drop rule is short-circuited, while the UIC clause and the authorized-mask subset clause are left intact. This is the vms-2b8 round-3 defect exactly -- the version that guarded the two fields nobody displays and left the one every reader shows unprotected, so an unprivileged process could stamp itself \"SYSTEM\" and \$GETJPI would report it to everyone.";;
        require_fail) cat <<'EOF'
USER NAME CLAUSE ISOLATED: own UIC, own mask, name "SYSTEM" -> SS$_NOPRIV
USER NAME CLAUSE ISOLATED: same UIC, same mask, different name -> SS$_NOPRIV
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
... and the process is STILL UNNAMED (a process cannot name itself a user)
an UNNAMED process cannot stamp an identity at all -- there is no name it is allowed to hold constant
... and the executive still holds the name it authenticated
the refused climb-back changed nothing
A WRITES the user name, B READS it (identity is executive-resident)
B resolves A BY NAME within the UIC group and sees the same identity
the scan returns a SAME-GROUP row in full (no privilege needed)
EOF
                      ;;
        knock_on_why) cat <<'EOF'
NINE reds, one removed guard, and the size of the blast radius IS the argument
for the guard. test_kmod_ident is sequential and shares one executive row per
process, so the moment a rename that should have been refused succeeds, every
later READ of that row in the same suite is reading a name the process gave
itself:
  the two require_fail entries are the refusals themselves, from both sides
    -- an unnamed process stamping "SYSTEM" (l.815) and a named process
    changing to a different name (l.913);
  "... and the process is STILL UNNAMED" and "an UNNAMED process cannot stamp
    an identity at all" are the same refusal read back on the unnamed caller
    (l.817, l.835) -- with the guard gone it now has a name;
  "... and the executive still holds the name it authenticated" (l.919) and
    "the refused climb-back changed nothing" (l.966) are process A's own row
    after its rename wrongly succeeded;
  "A WRITES the user name, B READS it", "B resolves A BY NAME ..." and "the
    scan returns a SAME-GROUP row in full" (l.992, l.999, l.1033) are Rule 11's
    A-writes/B-reads checks -- a second process, and a PROCSCAN, reading the
    corrupted name out of the executive. Those three are the most valuable reds
    in the set: they show the defect is executive-resident, not local to the
    liar.
There is no finer edit inside this clause: it is a single `if`. A DIFFERENT
clause of the same rule (the UIC test, or the authorized-mask subset test)
would give a smaller red set, and the suite has "CLAUSE ISOLATED" assertions
for each -- but the name clause is the one vms-2b8 round 3 had to add, because
the earlier version guarded the two fields nobody displays and left the one
every reader shows unprotected. Controlling the clause that matters least
would be tidier and worth less.
EOF
                      ;;
        esac;;

    executive-not-pinned)
        case "$_f" in
        facility)     echo "executive residency (vms_fops.owner pins vms.ko while /dev/vms is open)";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_pin";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "fatal";;
        why)          echo "vms_fops loses .owner = THIS_MODULE, so an open descriptor no longer holds a module reference and test_kmod_pin's own rmmod succeeds. FATAL, and measured, not assumed: the guest then takes 'Unable to handle kernel paging request' + 'Internal error: Oops' with Comm: test_kmod_pin, and the run never reaches its own accounting. That IS the guarantee -- an unpinned executive is not a degraded system, it is a dead one -- so the control asserts what is checkable (every suite ordered before test_kmod_pin ran clean -- a count derived from the checkout, never written down -- and the three pin assertions went red by name) instead of pretending the unload is survivable.";;
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
        # MEASURED after vms-2b8: test_kmod_ident grew its own thread section
        # (l.878-887) and detects this too. It was NOT in this list, and the
        # red-set equality check named the three new assertions on the first
        # run after the rebase. Both suites belong here -- two independent
        # suites catching one defect is coverage, not a blunderbuss.
        suites_red)   echo "test_kmod_bind test_kmod_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_proc_find_or_err() keys the PCB on current->pid (the Linux TID) instead of current->tgid, so a sibling thread of one image no longer resolves to its process's PCB. Invisible to every single-threaded suite -- tgid == pid there -- which is precisely why it needs a control.";;
        require_fail) cat <<'EOF'
sibling thread resolves in the process table
sibling thread sees the process name the main thread set
sibling thread sees the event flag the main thread set
sibling thread can $DEQ the lock the main thread took
a second thread of the process is known to the executive
... as the SAME process, not a second one (one PCB per process)
... and sees the identity the other thread stamped
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
        # MEASURED: test_kmod_bind, and -- since vms-8019 -- test_syssvc_procnam,
        # and -- since vms-6a7 -- test_syssvc_showproc, and -- since vms-fb9 --
        # test_syssvc_showdev.
        # Round 1 also listed the suites in blind_suites below, which permitted
        # four suite groups to redden while only one could; those are now
        # declared as what they are: BLIND. test_syssvc_procnam is the
        # opposite case and is here on measurement, not permission: it is the
        # FIRST public-API suite that does NOT hand-register, so it is the
        # first one that can see this defect at all.
        # test_syssvc_showproc is the SECOND such suite, and it was added to
        # this list the way the method requires: not predicted, but read off a
        # run. It landed declaring only proctab-crossgroup-identity, and the
        # first CI run after it merged reddened it here -- one suite outside
        # the declared set, one assertion outside the named set. Both directions
        # of the equality check firing at once is the check working, not a
        # coarse mutation, and the honest fix is to DECLARE the dependency
        # rather than narrow a suite whose whole value is that it drives the
        # user-visible command through the entire stack.
        # test_syssvc_ef_mproc and test_syssvc_ef_local are the THIRD and
        # FOURTH, added by vms-2a8, and they arrive the same way: not
        # predicted, READ OFF A RUN. The first CI run after sys_event.c became
        # a reader of the executive reddened test_syssvc_ef_mproc here, one
        # suite outside the declared set and 27 assertions outside the named
        # set; the local rerun that produced the lists below reddened
        # test_syssvc_ef_local too, for 16 more. That is the same forcing
        # function firing for the same reason as vms-6a7's: wiring a facility
        # to the executive makes its suites depend on the bind, and the
        # manifest has to SAY so.
        # test_syssvc_showdev is the FIFTH, added by vms-fb9 (this item) when
        # SHOW DEVICE became a reader of the executive device table. Same
        # arrival as the others -- NOT predicted, READ OFF A RUN of THIS
        # dispatch's own tests/qemu/run_facility_negctl.sh
        # bind-client-no-register (the full proof-set re-run vms-fb9 r6 was
        # asked to do): it landed on an earlier round declaring only
        # devtab-alloc-not-recorded/devtab-owner-not-recorded, and this run
        # is the first time this facility's negative control was actually
        # executed end-to-end since -- the cheap host-side
        # facility_negctl_manifest ctest checks anchors and literal text, not
        # which suites a mutation reaches, so it could not have caught a
        # missing suite. Reddened here, one suite outside the declared set
        # and 3 assertions outside the named set -- test_syssvc_showdev.c
        # does not hand-register (it opens /dev/vms only to decide
        # skip-vs-run, same counter-example shape as procnam/showproc), so it
        # was never a candidate for the blind_suites set below.
        # WHY THESE THREE ARE NOT A WIDENING OF THE BLIND SET BELOW: none
        # hand-register. Each opens /dev/vms only to decide skip-vs-run and
        # then uses the public sys$ API, which is what a product image does --
        # the counter-example property the blind_why paragraph names.
        # test_syssvc_showdev (vms-fb9) and test_syssvc_startup_service
        # (vms-47b) both join on the same basis: each drives the REAL DCL.EXE,
        # a product image that binds the way a product image binds -- through
        # kif_bind() -- so deleting that call takes the whole command layer
        # away from the executive. The two arrived on separate branches; this
        # list is the UNION, re-derived by running the control on the merged
        # tree rather than kept from either side of the rebase conflict.
        # test_syssvc_showterm is the SIXTH, added by vms-d0b, and it arrived
        # the same way every one of the others did -- NOT PREDICTED, READ OFF
        # A RUN. The full sweep this dispatch required reported it as one
        # suite outside the declared set and 9 assertions outside the named
        # set, which is both directions of the equality check firing at once,
        # which is the check working. It belongs here for the same reason
        # test_syssvc_showdev does: it drives the REAL DCL.EXE, a product
        # image that binds the way a product image binds, so deleting
        # kif_bind()'s registration takes the whole command away from the
        # executive. It is not a candidate for the blind set below -- it
        # hand-registers nothing.
        #
        # test_syssvc_ident is the SEVENTH, added by vms-2b8 when SHOW
        # PROCESS/SHOW PROCESS_PRIVILEGES became readers of the executive's
        # identity row. Same arrival as the others -- NOT predicted, READ OFF
        # A RUN of this dispatch's own run_facility_negctl.sh
        # bind-client-no-register: it landed declaring only
        # ident-username-unguarded (via blind_suites), and this is the first
        # time this facility's control ran end-to-end since. Reddened here,
        # one suite outside the declared set and 14 assertions outside the
        # named set -- test_syssvc_ident.c drives DCL.EXE exactly as
        # test_syssvc_showdev and test_syssvc_startup_service do (real
        # product image, kif_bind()-mediated), so it was never a candidate
        # for the blind_suites set below either.
        #
        # ROUND 9 (vms-2b8): test_syssvc_ident.c's scenario F was rewritten
        # (its old assertion only checked that a marker printed after
        # F$GETJPI CURPRIV, not that CURPRIV rendered content -- see that
        # scenario's own comment) and it drives the same DCL.EXE/kif_bind()
        # path as the rest of this suite, so it was never a blind_suites
        # candidate either. RE-MEASURED against real QEMU rather than
        # assumed: the rewrite adds exactly two new reds here, both
        # scenario F's own assertions ("F: the executive accepted the
        # SYSTEM/ALL identity..." and "F: F\$GETJPI CURPRIV renders...").
        # run_facility_negctl.sh bind-client-no-register named exactly
        # these two as unnamed before this entry was corrected, and zero
        # after -- nothing else in this suite or any other moved.
        #
        # test_syssvc_lock_status is the EIGHTH, added by vms-2e5 when the
        # kstat_to_ss() public-status-mapping suite was written -- and it
        # arrived the SAME way every other addition above did: NOT predicted,
        # READ OFF the first full run of THIS control against the tree that
        # added it. Like test_syssvc_procnam/showdev, it does not hand-register
        # (see its bootstrap()'s own comment) -- it opens /dev/vms only to
        # decide skip-vs-run, then drives sys$enq/enqw/deq, the public API --
        # so it is a genuine detector of this defect, not a widening of the
        # blind set below. See knock_on_why for what it reddens and why the
        # suite EXITS BY SIGNAL (rc=141) rather than completing.
        #
        # All three (showterm, ident, lock_status) arrived on separate
        # branches; this list is the UNION, re-derived by running the control
        # on the merged tree rather than kept from one side of the merge.
        suites_red)   echo "test_kmod_bind test_syssvc_procnam test_syssvc_showproc test_syssvc_ef_mproc test_syssvc_ef_local test_syssvc_showdev test_syssvc_startup_service test_syssvc_showterm test_syssvc_ident test_syssvc_lock_status";;
        # test_kmod_setterm (vms-d0b) joins the blind set, MEASURED in the
        # same run: it stayed rc=0 with the defect injected, because
        # open_and_register() hand-registers exactly like test_kmod_devtab
        # and test_kmod_procnam beside it. That is the pattern the blind_why
        # paragraph below says is still spreading -- and it spread again.
        blind_suites) echo "test_kmod_devtab test_kmod_procnam test_kmod_ident test_syssvc_lock test_kmod_setterm";;
        blind_why)    cat <<'EOF'
The suites named in blind_suites above drive the product's own vms_kif
client, so restoring the vms-9fc defect (kif_bind() no longer calling
vms_kif_register()) SHOULD turn them red. It does not: each calls
vms_kif_open() and vms_kif_register() BY HAND before using a facility
(test_syssvc_lock.c:136-140, test_kmod_ident.c:306/364-367/541-544/588-593),
supplying the exact product step kif_bind() exists to perform. MEASURED, not
argued -- with the defect injected, every suite in blind_suites stays rc=0.
They are therefore structurally blind to the entire auto-bind defect class,
which is how vms-9fc survived to be found by inspection rather than by CI.
test_kmod_ident is the newest of them (vms-2b8), which is the point of pinning
this as an asserted fact rather than a note: the pattern is still SPREADING,
and the gate now says so on every run.
Tracked as rd item vms-f27. Do NOT fix it by widening suites_red: that would
re-hide the gap behind a set the control merely permits to redden.
THE COUNTER-EXAMPLE, added by vms-8019 and worth keeping in view: a further
client suite, test_syssvc_procnam, does NOT hand-register -- it opens
/dev/vms and then uses the public sys$ API, which is what a product image
does -- and it goes red immediately. So the blindness above is not a property
of driving vms_kif; it is a property of hand-registering first.
EOF
                      ;;
        isolation)    echo "isolated";;
        # `why`'s count and enumeration are DERIVED from suites_red, not
        # hand-recited: a rebase (or any future edit) that changes suites_red
        # without touching this line used to leave a sentence that quietly
        # disagreed with the field beside it -- exactly what happened across
        # the vms-47b/vms-6a7/vms-2a8 rebase, where suites_red was correctly
        # re-derived to six suites but this text was kept from one side of
        # the conflict and still said "Five suites" while naming only five.
        # The equality check in run_facility_negctl.sh keys on suites_red, so
        # that drift never weakened the gate -- but the printed evidence was
        # false. Computing _n/_list from suites_red here makes that specific
        # disagreement structurally impossible: change suites_red and this
        # sentence's count and list change with it, in the same run.
        # SAME FIX APPLIED TO THE BLIND-SUITES CLAUSE (vms-47b round 4): the
        # sentence used to hand-recite "The four client suites", read off
        # blind_suites at the time it was written rather than derived from
        # it -- a third instance of the identical drift class, just not yet
        # tripped (blind_suites is still four entries, so it was true by
        # coincidence, not by construction). _blind_n is now computed from
        # defect_field "$_d" blind_suites the same way _n is computed from
        # suites_red, so the count in the sentence and the blind_suites
        # field above it cannot disagree.
        why)
            _suites_red=$(defect_field "$_d" suites_red)
            _n=$(set -- $_suites_red; echo $#)
            _list=$(echo "$_suites_red" | sed 's/ /, /g')
            _blind_suites=$(defect_field "$_d" blind_suites)
            _blind_n=$(set -- $_blind_suites; echo $#)
            echo "kif_bind() stops calling vms_kif_register() -- THE vms-9fc defect, restored on purpose. $_n suites detect it (through the public sys\$ API, or test_kmod_bind directly): $_list. The $_blind_n client suites that ought to and do not are declared blind_suites and asserted GREEN, so the gap is a fact this job prints rather than one a reader has to infer."
            ;;
        require_fail) cat <<'EOF'
vms_kif_open() then a BARE vms_kif_setident() reaches the executive with no explicit register
$SETEF reaches the executive with no explicit register
$GETJPI(self) resolves the auto-bound process
the caller has a row in the executive's process table
parent: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)
child: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)
sys$setef(1) set the flag
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
the executive assigned the caller a VMS process ID
sys$creprc created the subject process
sys$creprc returned the subject's pid
sys$creprc returned the subject's VMS process ID
the startup procedure announced the created process with %RUN-S-PROC_ID
the executive resolves the service BY NAME after its creator exited
SHOW SYSTEM, in a different process, lists the service by its VMS process name
starting the same named service twice is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM
sys$creprc PRC$M_DETACH created the probe's process
RUN/DETACH/PROC= creates a detached process the executive knows by name
the abbreviated form announces the process ID the executive assigned
vms-69e: /DETACHED with /PRIORITY still creates the process and announces success
vms-69e: and says NOTHING about the priority it discarded (this is the defect, asserted so it cannot be fixed silently)
child: a LOCAL flag set by the parent is NOT visible here (local clusters stay per-process)
child: a common flag CLEARED BY THE PARENT via sys$clref reads clear here (A clears, B reads, public API)
child: a common flag SET BY THE PARENT via sys$setef is visible here (A writes, B reads, public API)
child: sys$ascefc joined the named common cluster
child: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')
child: sys$setef on the associated common cluster reported success
parent: a PERMANENT cluster survived losing its last association (its flags are still set)
parent: a common flag SET BY THE CHILD via sys$setef is visible here (B writes, A reads, public API)
parent: sys$ascefc after the deletion created a cluster of that name again
parent: sys$ascefc created a PERMANENT common cluster
parent: sys$ascefc created/joined the named common cluster
parent: sys$ascefc joined the cluster the interrupted-wait measurement uses
parent: sys$ascefc re-joined the permanent cluster by name
parent: sys$clref on the associated common cluster reported success
parent: sys$dacefc identifies the cluster from ANY flag number in it, not only the base
parent: sys$dacefc released the last association to the permanent cluster
parent: sys$dacefc released the marked cluster
parent: sys$dlcefc accepted the permanent cluster by name
parent: sys$dlcefc really deleted the cluster (the re-created one is FRESH, not the old flags)
parent: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')
parent: sys$setef on the associated common cluster reported success
parent: sys$setef on the permanent cluster reported success
parent: sys$setef released the waiter's flag
parent: sys$waitfr did NOT return until the flag was really set -- an interrupted wait is re-entered, never reported as SS$_NORMAL over a clear flag
parent: the waiter was interrupted by a signal repeatedly WHILE blocked in sys$waitfr (the condition under test is reachable, not hypothetical)
sys$clref(1) cleared the flag
sys$clref(1) reported WASSET for the previously-set flag
sys$clref(1) succeeded on the set flag
sys$readef(1) reported WASCLR after the clear
sys$readef(1) reported WASCLR for the cleared flag
sys$readef(1) reported WASSET after the flag was set
sys$readef(1) succeeded after the flag was set
sys$readef(1) succeeded on the cleared flag
sys$setef(1) on an already-set flag reported WASSET
sys$setef(1) reported a documented previous state (WASCLR or WASSET)
sys$setef(1) succeeded on the already-set flag
sys$waitfr(1) left the flag SET -- it is not a counting semaphore
sys$waitfr(1) returned for the already-set flag
the cluster state word agrees with the status: flag 1's bit is CLEAR
the cluster state word agrees with the status: flag 1's bit is SET
the second process allocated OPA0: through the executive ($ALLOC)
A-WRITES/B-READS: DCL's SHOW DEVICE reports the console allocated -- a change made by a DIFFERENT process, which a per-process device view could not show
the bare listing shows it too, so both row sources ($DEVICE_SCAN and $GETDVI) read the same shared table
bare SHOW DEVICE lists OPA0: -- a device DCL has no other way to know about, read from the executive's table
the listing carries the oracle's column header (section 4)
SHOW DEVICE OPA0: resolves the name through the executive and prints its row
the console is still listed once the other process is gone
...and refuses it with the oracle's own message (section 6)
a second process put the console in a known state through the executive (IO$_SETMODE)
SHOW TERMINAL names _OPA0: once the executive holds the binding -- the SAME BINARY that named nothing a moment ago
the characteristics heading is printed (oracle section 2)
grid row 1 is byte-for-byte the V7.3 capture
grid row 2 is byte-for-byte the V7.3 capture
grid row 4 is byte-for-byte the V7.3 capture
grid row 10 is byte-for-byte the V7.3 capture
the last row carries the single remaining characteristic, unpadded
could not tell the second process to change the console
could not tell the second process to restore the console
...and the cleared Echo bit, in the grid cell the oracle prints it in
...and the set Pasthru bit, so both directions of one IO$_SETMODE are read back
...and grid row 1 is the oracle's bytes again, so neither is the grid
this process, which bound nothing, still has no terminal -- the bindings belonged to the DCL jobs, not to the device or to the system
A: SHOW PROCESS does NOT report the user name planted in VMS_USERNAME
A: SHOW PROCESS reports the UIC the EXECUTIVE holds
A: SHOW PROCESS reports the user name the EXECUTIVE holds
A: the authorized-privileges AND process-privileges blocks are both EMPTY -- none of A's granted mask (TMPMBX|NETMBX|OPER) is in VMS_PRV_M_ENFORCED
A: the executive accepted the identity a privileged writer established
B: SHOW PROCESS reports B's UIC
B: SHOW PROCESS reports B's user name
B: SHOW PROCESS/PRIVILEGES lists WORLD's description in the process-privileges block too
B: the authorized-privileges grid shows EXACTLY WORLD -- the one bit of B's mask that is in VMS_PRV_M_ENFORCED -- not the whole mask and not nothing
C: SHOW PROCESS does NOT report SYSTEM for a process that only claimed it -- through the ioctl AND through VMS_USERNAME
C: SHOW PROCESS reports the UIC the executive derived from real credentials
C: the executive refused an unprivileged process's attempt to become SYSTEM (SS$_NOPRIV)
C: the privilege display is EMPTY -- the two privileges the executive granted an unprivileged process (TMPMBX, NETMBX) are both outside VMS_PRV_M_ENFORCED
D: the session established its authenticated identity
F: the executive accepted the SYSTEM/ALL identity this scenario needs (cur_privs = ~0ULL, so every VMS_PRV_M_ENFORCED bit is set)
F: F$GETJPI CURPRIV renders SYSTEM/ALL's actual enforced privilege names (CMKRNL,CMEXEC,SETPRV,WORLD), not merely completes without rendering anything
parent: child took EX before the CVTUNGRANT probe (setup, not the property under test)
parent: sys$enq CR queues behind the child's EX and still returns a real lock ID (public API)
sys$deq on an unknown lock ID reports SS$_IVLOCKID (public API, real executive)
EOF
                      ;;
        knock_on_why) cat <<'EOF'
Assertions across six suites go red, and that IS the defect rather than
evidence against it: the mutation deletes the ONE call that binds a process to the executive,
and a process with no PCB can use no facility. Round 1's own framing of
this ("only test_kmod_bind goes red") was narrow: true at suite
granularity and misleading at property granularity -- the exact thing the
equality check exists to stop. The reddened assertions fall into groups,
all the same missing bind (the exact
set is the require_fail/knock_on_fail arrays above, not a count restated
here -- see METHOD 4: a tally a human must remember to update will drift):
  suite 0, setident (1)   -- (vms-fb9 r6) vms_kif_setident() now reaches
                             kif_bind(), so the
                             same deleted register() call leaves it unbound
                             too; named directly in require_fail because it
                             is its own minimal mutation's property, not a
                             downstream read of suite 1's;
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
they register explicitly, which is precisely why the four blind_suites below
cannot see this defect at all.
The last three, in test_syssvc_procnam, are the same missing bind arriving at
the PUBLIC API (vms-8019). "the caller has a row in the executive's process
table" is $GETJPI(self) from an unbound process -- the same property as the
require_fail entry above, one layer up, which is why it is in require_fail and
not here. "the executive assigned the caller a VMS process ID" reads the pid
out of the row that call failed to return. "sys$creprc created the subject
process" and "sys$creprc returned the subject's pid" are the forked CHILD
failing to bind, exactly as suite 3 does one layer down: with no registration
the child cannot report a process ID, so the creation handshake fails and
$CREPRC reports the child lost. That suite then stops, by design, rather than
asserting about a subject it knows was not created -- which is why exactly
four of its assertions appear here and none of the rest of the suite, however
many it has grown to. (The count is deliberately not written down: it was, and
adding an assertion to the suite silently rotted it. The require_fail set is
the machine-checked statement; this paragraph is the reasoning.)

THE LAST ONE, "sys$creprc returned the subject's VMS process ID", is
test_syssvc_showproc reaching the identical wall one suite later (vms-6a7).
That suite exists to prove SHOW SYSTEM and SHOW PROCESS are READERS of the
executive's process table, so its very first act is $GETJPI(self) and its
second is a $CREPRC of the subject it will then go looking for. With the bind
deleted, both fail for the reason suite 3 above already gives -- the child
cannot register, so it cannot report a process ID, so the handshake fails --
and the suite stops before it ever runs DCL. That is why it contributes only
THREE reds and why two of the three are texts this manifest already names:
"the caller has a row in the executive's process table" is the same assertion
test_syssvc_procnam makes, word for word, and so is "sys$creprc created the
subject process". Only the pid assertion is worded differently ("VMS process
ID" rather than "pid"), which is the whole of the delta this entry gained.

DECLARED, NOT NARROWED, AND THE REASON MATTERS. The alternative fix was to
make test_syssvc_showproc insensitive to anything outside its own subject --
to stop it touching registration, the process table and $CREPRC. That would
mean not driving the real DCL.EXE end to end, which is the only thing that
makes it evidence about a user-visible command at all (CLAUDE.md Rule 11's
corollary: the command is a reader of the facility). A suite that drives the
whole stack SHOULD be sensitive to the whole stack; the defect was that the
manifest did not SAY so, and an undeclared red is indistinguishable from a
non-minimal mutation. Saying so is the fix.

MEASURED ON BOTH ARCHITECTURES, because the red set did not have to agree.
The x86_64 CI runner (run 30598269086, SHA 5ef2b65) and an aarch64 host under
pure TCG produced the SAME stray suite and the SAME single unnamed assertion,
byte for byte. There is no ending-picks-itself race here of the kind
creprc-handshake-eintr has: test_syssvc_showproc.c:574-582 evaluates both
$CREPRC assertions before its early return, so the three reds are the same
three whichever way the scheduler runs.

THE EVENT FLAG SUITES: 43 MORE, AND EVERY ONE OF THEM IS THE SAME MISSING
BIND (vms-2a8). Until this item, src/libvms/syssvc/sys_event.c named no
vms_kif_* symbol at all -- it kept 128 event flags in per-process memory --
so test_syssvc_ef_mproc could not see this defect and there was nothing to
declare. Wiring it to /dev/vms makes every sys$ event-flag call a bound call,
and the equality check said so on the first CI run afterwards (27 unnamed
assertions in test_syssvc_ef_mproc) and again locally once the conformance
move added test_syssvc_ef_local (16 more). The lists above are that run's
output, not a prediction.
  test_syssvc_ef_mproc (27) -- the process never registers, so the FIRST
    sys$setef fails, and every A-writes/B-reads assertion downstream of it is
    reading back a write that never happened. Its one require_fail entry per
    process, "sys$setef on a LOCAL flag succeeds (baseline: ...)", is the
    property: a local flag needs no cluster association and no privilege, so
    if even that fails the facility is not operative in this process AT ALL.
    Every remaining red -- the $ASCEFC joins, the shared-flag reads, the
    permanent-cluster lifetime round trip, the interrupted-wait block -- is
    downstream of that one.
  test_syssvc_ef_local (16) -- the whole suite, and it is the whole suite for
    a structural reason worth stating: this suite has no early exit. It is
    the relocated conformance program, sixteen straight-line checks with no
    branch that skips the rest, so an unbound process fails all sixteen
    rather than stopping at the first. "sys$setef(1) set the flag" is in
    require_fail as its baseline; the other fifteen read back what that call
    failed to do.
NOTE WHICH ASSERTION IS *NOT* SILENT HERE: "sys$setef on an UNASSOCIATED
common flag is refused WHILE a local flag succeeds (not merely 'every call
fails')" goes red, in both processes. That assertion exists precisely to
refuse to be satisfied by a facility that does nothing, and with the bind
deleted the facility does nothing -- so it fails, exactly as designed. A
manifest that named only the "succeeds" assertions and not this one would be
describing a different, kinder defect.

TEST_SYSSVC_SHOWDEV, THE SIXTH SUITE, ADDED vms-fb9 r6 -- READ OFF THE PROOF
RE-RUN THIS ITEM WAS ASKED TO DO, NOT PREDICTED. test_syssvc_showdev.c
exists to prove SHOW DEVICE is a READER of the executive device table
(vms-fb9), so it $CREPRCs a second process and has it $ALLOC the console
while the first process's SHOW DEVICE watches for the change (A-writes,
B-reads, Rule 11). Its reds fall into three groups, none hand-recited as a
count (see the set itself, above `knock_on_fail)`, not a tally here):
  - "the caller has a row in the executive's process table", "sys$creprc
    created the subject process" and "sys$creprc returned the subject's VMS
    process ID" are the SAME missing-bind failure test_syssvc_showproc
    already declares, word for word, so they need no second entry -- the
    equality check compares a SET, not a per-suite tally;
  - "the second process allocated OPA0: ...", "A-WRITES/B-READS: DCL's SHOW
    DEVICE reports the console allocated ..." and "the bare listing shows it
    too, ..." are downstream of that same handshake failure: with the
    subject process unable to register, its $ALLOC of OPA0: never happens,
    so the parent's SHOW DEVICE never observes an allocated row;
  - "bare SHOW DEVICE lists OPA0: ...", "the listing carries the oracle's
    column header ...", "SHOW DEVICE OPA0: resolves the name ...", "the
    console is still listed once the other process is gone" and "...and
    refuses it with the oracle's own message ..." are a THIRD mechanism,
    found this round (vms-d0b r4) -- see below.

WHY THIS WAS RED ON THE TREE BEFORE THIS ROUND, AND WHY THAT IS NOT AN
EXCUSE. test_syssvc_showdev.c does not hand-register -- open /dev/vms only
to decide skip-vs-run, then use the public sys$ API, the same counter-example
shape as procnam/showproc -- so it was a client of this bind from the day it
landed (vms-fb9, before this round). It stayed undeclared because
tests/qemu/run_facility_negctl.sh's bind-client-no-register control is
expensive (a full container rebuild and QEMU boot) and, unlike
facility_negctl_manifest (the cheap host-side ctest that only checks anchors
and literal text), nothing had actually EXECUTED it end to end since
test_syssvc_showdev landed. Declared here the moment that execution happened,
per CLAUDE.md Rule 9 -- a pre-existing gap discovered while re-running the
full proof set is still owned by whoever's push would otherwise carry it red.

THE THIRD GROUP, AND A SEPARATE INFRASTRUCTURE DEFECT THIS ROUND FOUND AND
FIXED (vms-d0b r4). tests/qemu/inject_and_run.sh re-stages the rebuilt
DCL.EXE into the initramfs at ONE path, /initramfs/bin/DCL.EXE -- but
tests/qemu/Dockerfile stages it at TWO, because test_syssvc_showdev.c and
test_syssvc_showterm.c both default DCL_PATH to /tests/DCL.EXE (overridable
via OVMX_DCL), not /bin/DCL.EXE. Every defect whose target is under
src/vmsdcl, or reaches DCL.EXE through a library it links (as this one does,
through libvmssys), was therefore driving a STALE, unmutated DCL.EXE at that
second path -- a facility control with no teeth for exactly the assertions
that read DCL's OWN output, discovered when showterm-width-page-fabricated
(added this round, target src/vmsdcl/dcl_cmd_show.c) rebuilt and linked
correctly (confirmed with `strings` on the fresh build-static/bin/DCL.EXE)
but produced zero observable effect through /tests/DCL.EXE. Fixed by copying
the same freshly-built binary to both paths. The fix makes DCL.EXE's OWN
vms_kif calls (inside cmd_show_device(), reached through libvms) subject to
this mutation for the FIRST time in a full sweep, which is why these five are
new: they are the SHOW DEVICE READER path failing to bind, not the $ALLOC
writer path the second group above already covers. Measured, not predicted --
tests/qemu/run_facility_negctl.sh's own equality check named exactly these
five as extra reds on the first full sweep run after the inject_and_run.sh
fix, and they are added here rather than the harness fix being reverted,
because the fix is a genuine coverage improvement (this control now proves
more than it did before) and reverting it to keep this list short would be
re-hiding a gap the file's own header calls the one thing it is not allowed
to do.
THE LAST NINE, in test_syssvc_startup_service, are the same missing bind
arriving at the USER-VISIBLE COMMAND (vms-47b). DCL.EXE is a product image and
binds like one -- through kif_bind() -- so with that call deleted RUN/DETACHED
cannot create a named process, SHOW SYSTEM cannot read the table, the DUPLNAM
clash never arises because the first creation never happened, and even the
vms-69e pair goes red because RUN reports %RUN-F-CREPRC where the pinned
behaviour is a silent success. Not one of them is a separate defect: they are
what "the command layer has no executive" looks like from the command layer,
which is exactly the property the require_fail entries name one and two layers
down. This entry was ABSENT while that suite existed -- the branch that added
the suite never ran the whole sweep -- so the control was failing on an
unnamed red set rather than passing on a named one.
TEST_SYSSVC_SHOWTERM, THE SEVENTH SUITE (vms-d0b) -- READ OFF THE FULL SWEEP
THAT ITEM WAS REQUIRED TO RUN, NOT PREDICTED, and it arrived on the very first
execution after the suite was written. It drives the real DCL.EXE too, so it
reaches the same wall from a seventh door, and its nine reds split in two:
  - "a second process put the console in a known state through the executive
    (IO$_SETMODE)" is the wall itself. That process cannot register, so its
    $ASSIGN of the console fails and the IO$_SETMODE that follows has no
    channel.
  - the other eight are all one consequence of one line. cmd_show_terminal
    asks the executive which terminal this job is on BEFORE anything else,
    and prints nothing at all when it gets no answer -- so with no bind the
    entire SHOW TERMINAL output disappears and every assertion about its
    content goes with it: the header, the four pinned grid rows, the last
    row, and the width/page line.
The remaining eight are the rest of that same collapse, and they are here
because of a MEASUREMENT MISTAKE WORTH RECORDING RATHER THAN QUIETLY FIXING.
The first sweep saw only nine reds from this suite -- because the suite DIED
at that point. Its writer process could not register, exited, and the parent's
next write to the gate pipe killed the program with SIGPIPE (rc=141), so the
whole second half of the suite never ran and could not be observed. The fix
for that (ignoring SIGPIPE so the writes fail into their checked branches)
made the suite run to completion under the mutation and reveal eight further
reds -- the two "could not tell the second process ..." pipe writes, the three
A-writes/B-reads reads of a change that was never made, the two restore-side
reads, and the final check, which is $GETJPI(self) and fails like every other
bound call. So the DECLARED SET GREW BECAUSE THE OBSERVATION GOT BETTER, not
because the defect changed. This is the shape the equality check exists to
force: a truncated observation produces a truncated declaration, and only
running it again catches that.
What stays GREEN in that suite is what keeps this from looking like a
blunderbuss: its unbound run still correctly names no terminal.
Its sibling test_kmod_setterm stays green entirely and is declared in
blind_suites for the reason the paragraph above gives: it hand-registers.
MERGED (vms-47b round 5, rebase onto main after vms-6a7/vms-2a8): this defect's
declaration forked into two branches that each added a suite without seeing
the other's addition -- main gained test_syssvc_showproc and the two event-flag
suites, this branch gained test_syssvc_startup_service. Both sets are kept;
neither is a substitute for the other, and the combined suites_red/knock_on_fail
lists above were re-verified by running the mutation on the rebased tree (see
the item's progress notes), not by picking a side of the git conflict.

TEST_SYSSVC_IDENT, THE SEVENTH SUITE, ADDED vms-2b8 (this round) -- READ OFF A
RUN, NOT PREDICTED. test_syssvc_ident.c exists to prove SHOW PROCESS and SHOW
PROCESS/PRIVILEGES are READERS of the executive's identity row rather than of
the process's own environment (vms-2b8), so every one of its scenarios execs a
real DCL.EXE and reads its SHOW PROCESS output. DCL.EXE binds the way every
product image binds -- through src/vmsdcl/dcl_main.c's dcl_context_init(),
which now calls vms_kif_getjpi_self() unconditionally at startup (the reader
half of this item) -- so with kif_bind()'s register call deleted, that startup
read fails and every downstream assertion about what it returned fails with
it. The 14 reds this manifest names span scenarios A through D: 5 in A (the
harness's own vms_kif_setident() call, which ALSO binds through the same
deleted register() step, plus the four SHOW PROCESS reads of what it should
have established), 4 in B (SHOW PROCESS's user name/UIC plus the two
privilege-grid assertions this round's VMS_PRV_M_ENFORCED filter added), 4 in
C (the refusal status plus the three identity-not-claimed reads), and D's
first assertion ("the session established its authenticated identity") --
exactly the 14 lines above, observed by running this mutation once and
capturing the delta; nothing past D's first line or in scenario E is claimed
here, named, or reasoned about, because the run this entry is built from did
not print anything further to reason from.
THE EIGHTH SUITE, test_syssvc_lock_status, ADDED vms-2e5 -- READ OFF THE FIRST
FULL RUN OF THIS CONTROL AGAINST THE TREE THAT ADDED IT, not predicted. Its
bootstrap() does not hand-register (see the comment at its definition in
tests/qemu/test_syssvc_lock_status.c) -- it opens /dev/vms only to decide
skip-vs-run, exactly test_syssvc_procnam/showdev's shape, so it is a genuine
new detector of the SAME missing bind, not a widening of blind_suites.
Three assertions go red before the process DIES BY SIGNAL (rc=141, SIGPIPE),
not a hang and not a clean suite failure -- traced, not guessed:
  1. scenario_ivlockid's sys$deq(0xDEADBEEF) reaches kif_call() -> kif_bind(),
     which (with vms_kif_register() deleted from kif_bind()) never registers;
     the kernel's per-call check rejects the unbound task with -ESRCH, which
     vms_kif_kerr_to_ss() maps to SS$_BUGCHECK, not SS$_IVLOCKID -- so "sys$deq
     on an unknown lock ID reports SS$_IVLOCKID" reddens. That exact
     assertion text is named twice in this manifest -- by this defect's
     require_fail and by bind-client-no-register's knock_on_fail, both
     against test_syssvc_lock_status. A DIFFERENT defect reaching the SAME
     assertion text is not a collision; it is two mutations exercising the
     same call from different angles. (Verify with: facility_defects.sh
     field <defect> <list> | grep IVLOCKID -- do not take this from the
     comment.)
  2. scenario_cvtungrant forks a child that also cannot register; the child's
     own sys$enqw(EX) fails, so it _exit(1)s WITHOUT writing to ready_pipe --
     the parent's read_bounded() sees EOF, not the expected byte, so "parent:
     child took EX before the CVTUNGRANT probe" reddens (this assertion is
     explicitly labelled setup-not-property in its own text for exactly this
     reason: a registration failure trips the SETUP check, not the CVTUNGRANT
     mapping the scenario exists to probe).
  3. The parent's own sys$enq(CR) in the same scenario also fails to register,
     so lksb_q.lksb$l_lkid stays 0 and "parent: sys$enq CR queues behind the
     child's EX and still returns a real lock ID" reddens too. Because that
     CHECK's condition is false, the `if ((st & 1) && lkid != 0)` guard around
     the CONVERT probe never executes, so SS$_CVTUNGRANT itself is never
     asked about under THIS mutation -- consistent with vms-2e5's own point:
     the registration wall is upstream of the mapping this suite exists to
     assert, so a registration defect masks the mapping property rather than
     exercising it.
  4. THE CRASH IS DETERMINISTIC, NOT FLAKY, under this specific mutation:
     the child in step 2 always exits before the parent reaches its own
     `write(go_pipe[1], ...)` handshake byte (it dies on its FIRST failed
     call, long before the parent could plausibly still be running), so the
     write always lands on a pipe with no reader and always raises SIGPIPE.
     scenario_deadlock, and the "parent: dequeued its still-queued CR lock"
     / "parent: child (CVTUNGRANT holder) exited clean" checks later in
     scenario_cvtungrant, never run -- there is no assertion text for them
     to redden, and none is claimed.
EOF
                      ;;
        esac;;

    creprc-handshake-eintr)
        case "$_f" in
        facility)     echo "process creation handshake in the PRODUCT (\$CREPRC's report pipe, src/libvms/syssvc/sys_process.c)";;
        targets)      echo "libvms/syssvc/sys_process.c";;
        suites_red)   echo "test_syssvc_procnam";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
$CREPRC's handshake read goes back to a bare read(2), so a signal delivered to
the CALLER through a handler without SA_RESTART returns EINTR and the service
reads that as "the child died before reporting". This is the vms-8019 round-4
defect exactly, and it is not a theoretical one: DCL installs its interactive
SIGINT/SIGQUIT handlers with sa_flags = 0 (src/vmsdcl/dcl_main.c) and DCL is
what calls $CREPRC, so a Ctrl-C in the handshake window is the production
trigger.
THE DEFECT HAS TWO ENDINGS AND THE SCHEDULER PICKS ONE. Either the caller
closes the pipe's read end first, so the child's report takes SIGPIPE, the
child dies, and $CREPRC answers OVMX$_PRCLOST ("nothing was ever entered in
the table") about a process that WAS created and registered; or the child
reports first, activates its image, and $CREPRC's reap blocks for the lifetime
of that image, so the call never comes back at all. BOTH were measured from
this same mutation on this same commit -- the second on an aarch64 host under
TCG, the FIRST on the x86_64 CI runner. They are one property (the caller's
signal decided what $CREPRC said about the child) seen through two channels,
so the suite asserts them as ONE assertion and prints which ending it saw;
naming them separately is what made an earlier revision of this control flaky.
Nothing else in the file is touched: with no signal delivered the mutated code
and the correct code are byte-for-byte equivalent in behaviour, which is why
the ONLY assertion it reddens is the one that arranges the signal.
EOF
                      ;;
        require_fail) cat <<'EOF'
sys$creprc returned, and reported no process lost, while the caller caught signals
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    run-detached-name-dropped)
        case "$_f" in
        facility)     echo "the process NAME on the way from the user-visible command to the executive (DCL RUN/DETACHED -> \$CREPRC, src/vmsdcl/dcl_cmd_process.c)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
RUN/DETACHED stops passing /PROCESS_NAME to $CREPRC. The service is still
created, still detached, still reparented, and the command still prints
%RUN-S-PROC_ID with a real executive-assigned process ID -- it is simply
NAMELESS in the executive's table.
THIS IS THE EXACT FACADE SHAPE vms-47b EXISTS TO PREVENT, and the reason the
mutation is a one-argument edit rather than a deleted feature: everything a
single-process test can see stays true. The command succeeds, the process
exists, the process ID resolves. Only the property that makes the name mean
anything -- that ANOTHER process can find the service by it -- disappears. A
suite that asserted "RUN/DETACHED reported success" would stay green through
this, which is why it does not assert that.
EOF
                      ;;
        require_fail) cat <<'EOF'
the executive resolves the service BY NAME after its creator exited
SHOW SYSTEM, in a different process, lists the service by its VMS process name
starting the same named service twice is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
RUN/DETACH/PROC= creates a detached process the executive knows by name
the abbreviated form announces the process ID the executive assigned
vms-69e: /DETACHED with /PRIORITY still creates the process and announces success
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
All three are P10 cases that create a NAMED detached process and then resolve
it out of the executive by that name. They are the same defect seen again, one
spelling further on: P10 exists to prove that an ABBREVIATED qualifier is the
same qualifier, so it necessarily drives the same name-to-executive path this
mutation cuts. There is nothing finer available -- the mutation is already a
single argument, and the only way these could stay green would be for P10 to
stop checking the executive, which is the property.
EOF
                      ;;
        esac;;

    creprc-detach-intermediate-reaped)
        case "$_f" in
        facility)     echo "detachment in \$CREPRC's PRC\$M_DETACH path (src/libvms/syssvc/sys_process.c)";;
        targets)      echo "libvms/syssvc/sys_process.c";;
        suites_red)   echo "test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
$CREPRC stops reaping the detach intermediate, so the creator of a "detached"
process is left with a child it can wait for. The created process itself is
untouched: it is still setsid'd, still reparented, still named, still visible
to every other process. What is lost is the half of "detached" that only the
CREATOR can observe -- that after $CREPRC returns there is nothing in its job
tree belonging to that call.
This is the finest available edit for that property. Removing the second
fork() instead would be coarser AND unusable: $CREPRC's own reap would then
wait on the service, and the call would not return for the lifetime of the
image -- a hang is not a verdict, and a control that hangs is a flaky gate.
Disabling only the reap leaves every other suite byte-identical in behaviour,
because no other suite creates a process with PRC$M_DETACH at all, so the
mutated block is one nothing else enters.
EOF
                      ;;
        require_fail) cat <<'EOF'
the creator of a detached process has no child to wait for
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    run-detached-not-detached)
        case "$_f" in
        facility)     echo "PRC\$M_DETACH itself in \$CREPRC (src/libvms/syssvc/sys_process.c)";;
        targets)      echo "libvms/syssvc/sys_process.c";;
        suites_red)   echo "test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
$CREPRC goes back to accepting PRC$M_DETACH and discarding it -- literally
the pre-vms-47b line, `(void)stsflg;`. Nothing is created differently: the
process is still created, still named in the executive, still announced with
%RUN-S-PROC_ID, still outlives the DCL that created it, and SHOW SYSTEM in
another process still lists it. It is simply a SUBPROCESS wearing the word
"detached".
THIS CONTROL EXISTS BECAUSE AN ADVERSARY APPLIED IT BY HAND AND THE SUITE
ALMOST MISSED IT. Of thirteen assertions, exactly one went red. In particular
"the service's parent is init, not the DCL that created it" stayed GREEN --
the creating DCL exits before anything is observed, and Linux reparents any
orphan to init whether or not it was ever detached. A reparent check is a
necessary consequence of detachment and not a test of it, and it was being
read as one.
Naming this mutation forces the discriminating assertions to be listed below.
Both are properties a subprocess cannot have: the created process is in a
session of its own (setsid()), and the creator has nothing left to wait for.
EOF
                      ;;
        require_fail) cat <<'EOF'
the service left the session its creator ran in
the creator of a detached process has no child to wait for
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    run-image-qualifier-refused)
        case "$_f" in
        facility)     echo "the scope of RUN's subprocess refusal -- which qualifiers OpenVMS says ask for a subprocess (src/vmsdcl/dcl_cmd_process.c)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
RUN goes back to treating ANY qualifier as a subprocess request -- literally
the shipped-and-reverted test, `cmd->qualifier_count > 0` in place of the
oracle-scoped `run_process_qualifier_count(cmd) > 0`.
THIS IS A CONTROL AGAINST OVER-REFUSING, WHICH IS THE HARDER DIRECTION TO
NOTICE. Every "the qualifier was refused" and "the image did not run"
assertion in P7 and P8 stays GREEN under it, because a wider refusal refuses
those cases too; so does the P8 positive control, because plain RUN carries no
qualifier at all. Nothing that measures the refusal can catch a refusal that
is too big. Only a qualifier VMS scopes to the OTHER topic can, which is why
P9 drives /NODEBUG and /DEBUG: `HELP/NOPROMPT RUN Image Qualifier` on the
reference lab (VAX1, OpenVMS VAX V7.3, 2026-07-31) lists exactly those two and
nothing else, and the RUN (Image) form creates no process at all.
The mutation is one operand. It leaves the RUN (Process) set, the /UIC
refusal, $CREPRC, the executive and every other suite untouched.
EOF
                      ;;
        require_fail) cat <<'EOF'
RUN/NODEBUG runs the image: it is not a subprocess request
RUN/DEBUG is refused naming the debugger, not process creation
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
RUN/NODEB is not refused as a subprocess request: the image runs
parser-wide gap: an ambiguous abbreviation is not resolved, and OVMX has no %DCL-W-ABKEYW to refuse it with
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
Both are P10 cases and both are this same over-refusal reaching one spelling
further. The first is /NODEB, the abbreviation of the RUN (Image) qualifier
require_fail names in full: a refusal keyed on "any qualifier at all" cannot
distinguish them, so it swallows both. The second is /PR, which resolves to no
single qualifier and therefore reaches RUN as a qualifier the command does not
act on; counting qualifiers instead of identifying them refuses it as a
subprocess request and the image does not run. Neither is a second defect --
they are the same operand, and no finer mutation exists: the mutated
expression is one comparison.
EOF
                      ;;
        esac;;

    run-qualifier-not-abbreviated)
        case "$_f" in
        facility)     echo "how RUN resolves a qualifier NAME -- DCL's shortest-unique-prefix rule (src/vmsdcl/dcl_cmd_process.c)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          cat <<'EOF'
RUN goes back to matching qualifier names EXACTLY -- literally the shipped-and-
reverted comparison, strcasecmp() in place of the prefix compare in
run_resolve_qualifier(). Every full spelling still behaves identically; only an
ABBREVIATION changes meaning.
THIS CONTROL EXISTS BECAUSE AN ADVERSARY MEASURED THE DEFECT ON A ROUND THAT
HAD ALREADY "FIXED" THE FULL SPELLING. RUN/PRIORITY=4 was correctly refused
while RUN/PRIO=4 ran the image, exit 0, no diagnostic, priority discarded --
so the refusal was one keystroke wide, and the suite could not see it because
every fixture spelled its qualifiers out in full. On the oracle (VAX1, OpenVMS
VAX V7.3, 2026-07-31, captures/run-qualifier-abbrev-vax1-2026-07-31.txt) /PRIO
uniquely identifies /PRIORITY and VMS acts on it; /DETACH is /DETACHED; and
mx_start.com in this repo's own VMS corpus writes exactly those spellings.
The mutation reddens BOTH halves of the property, which is why both are listed:
the half where RUN must REFUSE (an abbreviated process qualifier is no longer
recognised as one, so the image runs) and the half where RUN must OBEY (/DETACH
is no longer /DETACHED and /PROC= no longer names, so the detached creation the
same phase asks for is refused as a subprocess request instead and no process
exists to find). One comparison, one property: what an abbreviation MEANS.
It leaves P7, P8 and P9 -- which spell every qualifier in full -- untouched,
and it leaves $CREPRC, the executive and every other suite untouched.
EOF
                      ;;
        require_fail) cat <<'EOF'
RUN/PRIO is /PRIORITY: the abbreviation is refused and the image does not run
RUN/PROC is /PROCESS_NAME: refused, image not run, nothing named in the executive
RUN/DETACH/PROC= creates a detached process the executive knows by name
the abbreviated form announces the process ID the executive assigned
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    # getjpi-curpriv-name-coverage EXISTED (rounds 7-8) as a QEMU negative
    # control for a runtime abort() guard in dcl_lexical.c. That guard was
    # deleted round 9 -- the fact it protected (every VMS_PRV_M_ENFORCED bit
    # has a row in vms_priv_names[]) is compile-time-determinable, so it is
    # now a _Static_assert in src/libvms/prv_agreement.c with its own
    # negative control (documented there, run by hand the same way the
    # bit-position asserts above it are). There is no longer a way to inject
    # this defect and boot QEMU to observe it go red: the SAME sed this
    # entry used to apply (OR (1ULL << 40) into VMS_PRV_M_ENFORCED) now
    # fails the BUILD at src/libvms/prv_agreement.c, before the container
    # rebuild step this harness depends on can produce a bootable image --
    # which this control's own driver (run_facility_negctl.sh) treats as a
    # BROKEN HARNESS (RUN_RC=4), not a verdict, for every defect in this
    # file. Keeping the entry would make this control permanently "bad" on
    # every run, which is worse than deleting it: an eternally-red gate
    # trains readers to ignore it. test_syssvc_ident.c's scenario F is now a
    # plain functional proof that CURPRIV renders real content (see its own
    # comment), not a negative control for this manifest.
    kstat-deadlock-mismapped)
        case "$_f" in
        facility)     echo "kstat_to_ss()'s DEADLOCK mapping (src/libvms/syssvc/sys_lock.c), the kernel-status-to-public-VMS-status boundary for the lock manager (vms-2e5)";;
        targets)      echo "libvms/syssvc/sys_lock.c";;
        suites_red)   echo "test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "case 100 (kernel SS__DEADLOCK) returns SS\$_NOTQUEUED instead of SS\$_DEADLOCK -- the EXACT mutation vms-2e5 was found by (a request the executive rejected for deadlock is reported to the caller as merely 'not queued'). The kernel's own decision to abort the request for deadlock is untouched; only the public value crossing the boundary changes.";;
        require_fail) cat <<'EOF'
parent: sync sys$enqw closing the cycle rejected SS$_DEADLOCK (public API)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    kstat-ivlockid-mismapped)
        case "$_f" in
        facility)     echo "kstat_to_ss()'s IVLOCKID mapping (src/libvms/syssvc/sys_lock.c), the kernel-status-to-public-VMS-status boundary for the lock manager (vms-2e5)";;
        targets)      echo "libvms/syssvc/sys_lock.c";;
        suites_red)   echo "test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "case 108 (kernel SS__IVLOCKID) returns SS\$_NOTQUEUED instead of SS\$_IVLOCKID -- a caller given a nonexistent lock ID is told the request was merely not queued rather than that the ID itself is invalid.";;
        require_fail) cat <<'EOF'
sys$deq on an unknown lock ID reports SS$_IVLOCKID (public API, real executive)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    kstat-cvtungrant-mismapped)
        case "$_f" in
        facility)     echo "kstat_to_ss()'s CVTUNGRANT mapping (src/libvms/syssvc/sys_lock.c), the kernel-status-to-public-VMS-status boundary for the lock manager (vms-2e5)";;
        targets)      echo "libvms/syssvc/sys_lock.c";;
        suites_red)   echo "test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "case 116 (kernel SS__CANCELGRANT) returns SS\$_NOTQUEUED instead of SS\$_CVTUNGRANT -- a CONVERT that lands on a lock still queued from an earlier request is told the SAME thing a fresh NOQUEUE request would be told, collapsing two different conditions into one report.";;
        require_fail) cat <<'EOF'
sys$enq(LCK$M_CONVERT) on a lock still queued (waiting) reports SS$_CVTUNGRANT (public API)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    assign-terminal-bypasses-executive)
        case "$_f" in
        facility)     echo "\$ASSIGN to a terminal (src/libvms/syssvc/sys_assign.c), the channel-is-the-identity boundary between the public sys\$ API and the executive's device table (vms-1c57)";;
        targets)      echo "libvms/syssvc/sys_assign.c";;
        suites_red)   echo "test_syssvc_qio_terminal";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sys\$assign(\"TT:\") stops calling vms_kif_assign(): the whole executive-registration block is unconditionally skipped, so a terminal channel is granted locally (the real /dev/tty still opens) with no counterpart in the executive's device table at all. This is the exact facade vms-1c57 exists to kill -- a channel obtained through \$ASSIGN that is not the channel the executive issued -- restored verbatim to prove the control can see it.";;
        require_fail) cat <<'EOF'
A-WRITES/B-READS: a fresh child sees the reference sys$assign("TT:") added to OPA0: in the executive (public API, cross-process)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
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
    kif-setmode-always-kernel)
        sed -i 's|    args.mode = mode;|    args.mode = 0; /* NEGCTL kif-setmode-always-kernel */|' "$_file";;
    ast-setast-disable)
        sed -i 's|ast_state->enabled = args.enable ? 1 : 0;|ast_state->enabled = 1; /* NEGCTL ast-setast-disable */|' "$_file";;
    eflag-clref-noop)
        sed -i 's|\*flags \&= ~(1U << bit);|/* NEGCTL eflag-clref-noop: the bit is not cleared */|' "$_file";;
    eflag-waitfr-eintr-normal)
        # RANGE-ANCHORED and therefore IDEMPOTENT. The `if (ret) return ret;`
        # shape appears in all three wait handlers, so the range narrows it to
        # WAITFR's: it opens at the only wait_event_interruptible() whose
        # predicate is a single bit, and closes at that function's own `out:`
        # label. A second apply finds no `return ret;` left inside the range
        # -- the first one replaced it -- so it is the no-op `selftest`
        # requires, instead of walking on to WFLOR's like a first-match
        # address would.
        sed -i '/wait_event_interruptible(\*waitq, (READ_ONCE(\*flags) \& (1U << bit)))/,/^out:$/ s|^        return ret;$|        { args.status = SS__NORMAL; goto out; } /* NEGCTL eflag-waitfr-eintr-normal */|' "$_file";;
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
    devtab-alloc-not-recorded)
        # devinfo_fill() has exactly one `info->allocated =` write; anchoring
        # to it directly (not a range) is safe because it is the only such
        # assignment in the file, so a second apply finds no match and is the
        # no-op the selftest requires.
        sed -i 's|    info->allocated = dev->allocated;|    info->allocated = 0; /* NEGCTL devtab-alloc-not-recorded */|' "$_file";;
    setterm-binding-not-recorded)
        # vms_ioctl_setterm() has exactly one write to proc->terminal, and it
        # is the only strscpy in the file whose destination is a proc field --
        # so anchoring on it directly is unambiguous, and a second apply finds
        # no match and is the no-op the selftest requires. Everything else in
        # the ioctl still runs: the channel is still resolved, the device
        # class is still checked, and SS$_NORMAL is still returned, so the
        # caller is told the binding was made and only the SHARED RECORD of it
        # is missing. That is the facade shape exactly.
        sed -i 's|    strscpy(proc->terminal, devnam, sizeof(proc->terminal));|    /* NEGCTL setterm-binding-not-recorded: the binding is not recorded */|' "$_file";;
    showterm-width-page-fabricated)
        # The ONE edit: restore a Width/Page line ahead of the characteristic
        # grid, exactly the one-line layout vms-d0b deleted as an invented
        # oracle format. Anchored on the exact original statement
        # (printf("Terminal Characteristics:\n");, the only occurrence in the
        # file) and the replacement swaps that trailing printf() for a
        # behaviourally identical puts() -- same bytes on stdout, since the
        # format string carries no conversion and puts() supplies its own
        # newline -- so the literal anchor text does not survive the edit and
        # a second apply finds no match, which is the no-op the selftest
        # requires.
        sed -i 's|    printf("Terminal Characteristics:\\n");|    printf("   Width:%4u      Page:%5u\\n\\n", info->width, info->page); /* NEGCTL showterm-width-page-fabricated: restores the deleted one-line layout */ puts("Terminal Characteristics:");|' "$_file";;
    showterm-width-page-oracle-shaped)
        # The ONE edit: restore the OTHER rejected layout -- the oracle's own
        # two-line Input:/Output:/LFfill:/CRfill:/Width:/Page:/Parity: block,
        # with the fields OVMX cannot source left blank -- ahead of the
        # characteristic grid. Same anchor as its sibling
        # (printf("Terminal Characteristics:\n");, the only occurrence in the
        # file) and the same trailing printf()->puts() substitution, so the
        # literal anchor text does not survive the edit and a second apply
        # finds no match, which is the no-op the selftest requires.
        sed -i 's|    printf("Terminal Characteristics:\\n");|    printf("   Input:            LFfill:         Width:%4u      Parity:\\n   Output:           CRfill:         Page: %4u\\n\\n", info->width, info->page); /* NEGCTL showterm-width-page-oracle-shaped: restores the oracle-shaped two-line layout, unsourceable fields left blank */ puts("Terminal Characteristics:");|' "$_file";;
    proctab-duplicate-name)
        sed -i 's|if (clash \&\& clash != proc) {|if (0 \&\& clash != proc) { /* NEGCTL proctab-duplicate-name */|' "$_file";;
    proctab-crossgroup-identity)
        # ONE clause of vms_proc_may_read(): the WORLD requirement for a
        # cross-UIC-group read. caller==target and same-group are left
        # exactly as they are, so the mutation cannot be mistaken for
        # "authorisation removed".
        sed -i 's|return (caller->cur_privs \& VMS_PRV_M_WORLD) != 0;|return true; /* NEGCTL proctab-crossgroup-identity */|' "$_file";;
    proctab-terminal-redaction-bypassed)
        # `info->redacted = 1;` is the only such statement in the file (the
        # sole write to that field), so anchoring on it directly is
        # unambiguous. The replacement (note: "1U;", not "1;" -- that is
        # what makes the anchor disappear after one application, so a
        # second apply finds no match and is the no-op the selftest
        # requires) copies proc->terminal into the redacted branch, ahead
        # of the early return -- exactly the hoist this control exists to
        # catch -- while leaving the return itself, the redacted flag's
        # value, and every other withheld field untouched.
        sed -i "s|        info->redacted = 1;|        info->redacted = 1U; memcpy(info->terminal, proc->terminal, VMS_DEVNAM_SIZE); info->terminal[VMS_DEVNAM_SIZE - 1] = '\\\\0'; /* NEGCTL proctab-terminal-redaction-bypassed */|" "$_file";;
    executive-not-pinned)
        sed -i 's|\.owner          = THIS_MODULE,|/* NEGCTL executive-not-pinned: no .owner, so nothing pins vms.ko */|' "$_file";;
    pcb-per-thread)
        sed -i 's|vms_proc_find(current->tgid)|vms_proc_find(current->pid) /* NEGCTL pcb-per-thread */|' "$_file";;
    bind-client-no-register)
        # Re-anchored after vms-2b8 changed the registration ABI: vms_pid is
        # OUTPUT-ONLY now and vms_kif_register() takes a pointer, so the old
        # anchor `vms_kif_register((uint32_t)vms_sys_getpid(), 0)` is gone.
        # The manifest self-test caught this in 0.2s as BROKEN FIXTURE, which
        # is the failure mode it exists for -- a dead anchor injects nothing
        # and the QEMU control then certifies a defect never applied.
        sed -i 's|(void)vms_kif_register(NULL);|/* NEGCTL bind-client-no-register: the vms-9fc defect, restored */|' "$_file";;
    ident-username-unguarded)
        sed -i 's|if (strncmp(proc->username, args.username, VMS_USERNAME_SIZE) != 0) {|if (0 \&\& strncmp(proc->username, args.username, VMS_USERNAME_SIZE) != 0) { /* NEGCTL ident-username-unguarded */|' "$_file";;
    creprc-handshake-eintr)
        # The ONE edit, and it is the code as it actually shipped for one
        # round: the creation handshake's read is a bare read(2) again. It
        # changes nothing except what happens when a signal is delivered to
        # the CALLER while it waits -- which is the whole property.
        sed -i 's|ssize_t r = creprc_read_all(namefd\[0\], \&rep, sizeof(rep));|ssize_t r = read(namefd[0], \&rep, sizeof(rep)); /* NEGCTL creprc-handshake-eintr */|' "$_file";;

    run-detached-name-dropped)
        # The ONE edit: RUN/DETACHED hands $CREPRC no process name. Every
        # other argument, and every other line of the command, is untouched.
        sed -i 's|                                 prc_d.dsc\$a_pointer ? \&prc_d : NULL,|                                 NULL, /* NEGCTL run-detached-name-dropped */|' "$_file";;

    creprc-detach-intermediate-reaped)
        # The ONE edit: the parent-side reap of the detach intermediate is
        # not entered, so the creator keeps a waitable child. Anchored on the
        # 4-space `if (detached) {` -- the child-side occurrences of the same
        # condition are indented 8, inside `if (pid == 0) {`.
        sed -i 's|^    if (detached) {$|    if (0) { /* NEGCTL creprc-detach-intermediate-reaped */|' "$_file";;

    run-detached-not-detached)
        # The ONE edit, and it is the pre-vms-47b source line restored:
        # PRC$M_DETACH is read and thrown away. `(void)stsflg;` keeps the
        # parameter used so the mutation is about behaviour, not warnings.
        sed -i 's|^    const int detached = (stsflg & PRC\$M_DETACH) != 0;$|    const int detached = 0; (void)stsflg; /* NEGCTL run-detached-not-detached */|' "$_file";;

    run-image-qualifier-refused)
        # The ONE edit, and it is the shipped-and-reverted source line
        # restored: the subprocess refusal stops asking WHICH qualifier and
        # goes back to counting all of them, so a RUN (Image) qualifier is
        # refused as a request OpenVMS says it is not.
        sed -i 's|        run_process_qualifier_count(cmd) > 0) {|        cmd->qualifier_count > 0) { /* NEGCTL run-image-qualifier-refused */|' "$_file";;

    run-qualifier-not-abbreviated)
        # The ONE edit, and it is the shipped-and-reverted comparison
        # restored: qualifier names match exactly, so an abbreviation is
        # not the qualifier it abbreviates. Both loops in
        # run_resolve_qualifier() carry the same compare and both are
        # mutated -- they are one comparison written twice, over the two
        # halves of RUN's qualifier table, and mutating one would leave
        # the rule half-applied rather than restored.
        sed -i 's|strncasecmp(given, full, glen) == 0|strcasecmp(given, full) == 0 /* NEGCTL run-qualifier-not-abbreviated */|' "$_file";;
    kstat-deadlock-mismapped)
        sed -i 's|case 100: return SS\$_DEADLOCK;|case 100: return SS$_NOTQUEUED; /* NEGCTL kstat-deadlock-mismapped */|' "$_file";;
    kstat-ivlockid-mismapped)
        sed -i 's|case 108: return SS\$_IVLOCKID;|case 108: return SS$_NOTQUEUED; /* NEGCTL kstat-ivlockid-mismapped */|' "$_file";;
    kstat-cvtungrant-mismapped)
        sed -i 's|case 116: return SS\$_CVTUNGRANT;|case 116: return SS$_NOTQUEUED; /* NEGCTL kstat-cvtungrant-mismapped */|' "$_file";;

    assign-terminal-bypasses-executive)
        sed -i 's|        if (devres.is_terminal) {|        if (0 \&\& devres.is_terminal) { /* NEGCTL assign-terminal-bypasses-executive */|' "$_file";;

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
        # libvms and vmsdcl are copied too: a defect may target the PRODUCT
        # half of an interface (creprc-handshake-eintr and
        # run-detached-name-dropped do), and a target this function cannot see
        # would be reported as a dead anchor on every run.
        if ! cp -a "$_st_root/kernel" "$_st_root/libvmssys" "$_st_root/libvms" \
                   "$_st_root/vmsdcl" \
                   "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root/{kernel,libvmssys,libvms,vmsdcl} for the self-test"
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
    #
    # The manifest holds the PRINTED text; the source holds C. Two differences
    # have to be normalised away or a perfectly good entry is reported absent
    # -- a false red in the fast job, which is how a useful check gets deleted
    # by the next person in a hurry:
    #   * escaped quotes -- test_kmod_ident.c writes  name \"SYSTEM\"  and
    #     prints  name "SYSTEM";
    #   * adjacent-literal concatenation -- the same file splits
    #     "USER NAME CLAUSE ISOLATED: same UIC, same mask, different name "
    #     "-> SS$_NOPRIV"  across two lines, and prints one string.
    # So both sides are reduced to the same shape: quotes and backslashes
    # deleted, every whitespace run collapsed to one space, the whole corpus on
    # one line. That is deliberately loose -- this check exists to catch a
    # TYPO, not to parse C.
    cat "$_st_tests"/test_kmod_*.c "$_st_tests"/test_syssvc_*.c 2>/dev/null \
        | tr -d '"\\' | tr '\n\t' '  ' | tr -s ' ' >"$_st_tmp.src"
    _st_absent=""
    for _st_d in $DEFECTS; do
        for _st_fld in require_fail knock_on_fail; do
            defect_field "$_st_d" "$_st_fld" | while IFS= read -r _st_txt; do
                [ -n "$_st_txt" ] || continue
                _st_needle=$(printf '%s' "$_st_txt" | tr -d '"\\' | tr -s ' ')
                grep -qsF -- "$_st_needle" "$_st_tmp.src" \
                    || echo "$_st_d/$_st_fld: [$_st_txt]"
            done
        done
    done >"$_st_tmp.absent" 2>/dev/null
    rm -f "$_st_tmp.src"
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
