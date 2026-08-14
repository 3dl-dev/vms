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
# WHAT `coverage` AND `selftest` BELOW ACTUALLY CHECK, AND WHAT THEY DO NOT
# (vms-659 -- read this before trusting either command's PASS lines)
#
# Both are STATIC. They check declarations the manifest (DEFECTS below) and
# the tests/qemu suite SOURCES make about THEMSELVES -- names in a list,
# glob patterns matched against other strings, `/* negctl: ... */` comment
# anchors, literal assertion text -- entirely by string and glob matching.
# NEITHER COMPILES NOR RUNS A SINGLE SUITE, AND NEITHER TOUCHES vms.ko OR
# /dev/vms. (`selftest`'s per-defect loop is a partial exception: it does run
# real sed(1) against a throwaway copy of the source tree and diffs the
# result, so ITS claim that an injection "lands" is genuinely executed -- see
# cmd_selftest's own header. That still proves nothing about a suite.)
#
# The one qualification to "both are STATIC", added by vms-d894: `coverage`
# now also READS a record of a past execution
# (tests/qemu/facility_negctl_observed.tsv, emitted by the driver below and
# read through tests/qemu/facility_negctl_record.sh). Reading a record is
# still not executing anything, and the printed output says so every run: it
# proves that A PAST RUN OBSERVED THESE RESULTS, never that they hold now.
#
# The claim that a named suite actually goes red, and that the exact
# assertions this manifest lists are the ones that fire and no others, is
# proven ONLY by tests/qemu/run_facility_negctl.sh: it builds vms.ko and the
# suites, boots a real QEMU guest against /dev/vms, injects each defect one
# at a time, and asserts the observed red set. THAT is the one gate in this
# program that executes anything, and it runs in CI ONLY -- there is no
# host-side equivalent. A "PASS" printed by `coverage` or `selftest` is a
# claim about the manifest's internal consistency, not about the executive.
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
#                            VMS_IOCTL_GETMODE instead of trusting
#                            vms_kif_setmode()'s returned SS$ status; MEASURED
#                            (see require_fail's knock_on_why below): this
#                            defect reddens exactly one assertion in
#                            test_kmod_access.c, the USER-direction one
#                            vms-0e4 added to match it.
#   getmode-buffer-not-written       vms_ioctl_getmode() returning success
#                            without ever writing the caller's buffer (a
#                            no-op-that-reports-success, the same shape as
#                            kif-setmode-always-kernel one layer down). Found
#                            auditing kif-setmode-always-kernel's own sibling
#                            assertion (vms-0e4 round 2): "... and the mode
#                            really changed" checks gm.mode == PSL_C_KERNEL
#                            without checking the ioctl's C return value, and
#                            PSL_C_KERNEL is 0 -- numerically identical to the
#                            test's own memset(0) default -- so a GETMODE that
#                            wrote nothing at all still read as "KERNEL" and
#                            passed. Fixed by checking the ioctl's return
#                            value at every VMS_IOCTL_GETMODE call site in
#                            test_kmod_access.c, not only the one this
#                            mutation targets.
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
#   kstat-cvtungrant-mismapped        the CONDITION VALUES the kernel lock
#                            manager yields (src/kernel/vms_internal.h). Each
#                            mutation gives one of SS__DEADLOCK / SS__IVLOCKID
#                            / SS__CANCELGRANT the value of SS$_NOTQUEUED, so a
#                            caller is told a request was merely "not queued"
#                            when the executive actually rejected it for
#                            deadlock, for an invalid lock ID, or as an
#                            ungrantable conversion. The kernel's DECISION is
#                            untouched by all three; only the value it answers
#                            with moves.
#
#                            THESE THREE USED TO ATTACK USERSPACE, and the
#                            move is the point (vms-82a). They mutated case
#                            arms in kstat_to_ss() in src/libvms/syssvc/
#                            sys_lock.c -- a mapping that ran in the CALLING
#                            PROCESS and turned the executive's private
#                            numbering (40/100/108/...) into public ssdef.h
#                            values. That mapping was the defect vms-2e5
#                            found and vms-82a fixed: the executive now emits
#                            VMS condition values itself and kstat_to_ss() is
#                            deleted. The controls were REPOINTED, not
#                            retired -- same three defects, same suite, same
#                            require_fail assertions, manifest size unchanged
#                            -- because a control that disappears when the
#                            code it attacked improves was measuring the code
#                            rather than the property.
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
# THREE granularities -- translation unit, suite, and individual defect -- so
# neither a new executive source file nor a new suite can arrive uncovered and
# unnoticed, and no existing control can leave without saying so.
#
# THE FLOOR UNDER THIS FILE'S OWN LENGTH (vms-279), AND WHAT IT DOES NOT CLAIM
#
# vms-b33's closing condition is "a deliberately injected defect in each wired
# facility turns CI red", and THIS LIST is the list of those defects. So the
# list's own size is load-bearing, and until vms-279 nothing measured it.
# MEASURED, on the checkout that closed vms-b33: 37 of the 42 entries could be
# deleted one at a time with `selftest` still exiting 0, and an exact set-cover
# over the seven kernel translation units and the twenty-five in-scope suites
# deleted 32 OF 42 IN A SINGLE EDIT -- selftest 0, "every src/kernel/*.c
# translation unit is named" still PASS, and the coverage drop from 25 suites
# to 23 printed as a `NOTE:`. A NOTE IS NOT A GATE.
#
# The obvious repair -- a hand-written expected count in this same file -- is
# bought by editing both halves of one file, and a cardinal that is
# hand-maintained is drift wearing a number. So the floor is DERIVED FROM
# SOMETHING OUTSIDE THIS FILE: the suite sources themselves.
#
#   EVERY DEFECT IS ANCHORED, AT THE ASSERTION IT NAMES, IN THE SUITE SOURCE
#   THAT PRINTS IT.
#
# Each assertion named by a `require_fail` carries, on the line above it:
#
#       /* negctl: <defect> */
#
# and each `knock_on_fail` assertion carries
#
#       /* negctl-knockon: <defect> */
#
# `coverage` then requires the anchor set and the manifest to AGREE BOTH WAYS:
# every defect must have at least one `negctl:` anchor, and every anchor must
# name a live defect. Deleting an entry from DEFECTS therefore orphans its
# anchors and turns the check red -- so cutting coverage now requires editing
# the tests/qemu suite sources too, in the same change, under a reviewer's
# nose. That is the whole of the claim.
#
# WHAT THIS FLOOR DOES **NOT** CLAIM:
#   * It is NOT tamper-proof. An adversary who deletes a defect AND its anchor
#     lines still passes. The claim is only that the edit is no longer confined
#     to this file, and no longer one line per entry.
#   * It does NOT floor the number 42 against a rewrite of the suites. Nothing
#     outside this repository enumerates the properties, so no check here can.
#   * It does NOT say the 42 defects cover the executive. MEASURED (vms-279):
#     of the 33 `vms_ioctl_*` handlers defined in src/kernel/*.c, exactly 9 were
#     inside a hunk any of these mutations edits. The translation-unit check
#     below is satisfied by 7 files; the executive's ioctl surface was nearly
#     five times wider. That gap is a finding, not something this file hides.
#
#     vms-2b2 MEASURED THE GAP BY EXECUTION, not by re-reading the line count.
#     tests/qemu/facility_attribution.sh handlers (vms-38c) joins the manifest
#     against a committed execution record instead of source line overlap, so
#     it answers "which wired handlers have NO mutation that changes what they
#     return" rather than "which handlers no hunk's line range happens to
#     touch". Run against this tree, all 24 handlers vms-279 counted as
#     uncovered come back UNPROBED -- zero of them are covered in effect
#     through a shared-code-path mutation on a sibling handler in the same
#     file; the line-level gap and the behavioural gap are the same gap here.
#     Of those 24, 8 (setprv, chkpriv, dclast, deliverast, getlki, alloc,
#     dalloc, ttsetmode) are OVMX-UNWIRED declarations (src/libvmssys/
#     vms_kif.h) with zero product-tree callers -- exempt under the vms-1e1
#     ruling, not a gap this file can close without inventing a caller. The
#     other 16 have a real product-side caller (a DCL command or a sys$
#     wrapper) and are not exempt. Of those 16, an existing QEMU assertion
#     already checks a real result/side-effect for 14 (ascefc, convert,
#     dacefc, dassgn, deq, devscan, dlcefc, enq, getdvi, getjpi, procscan,
#     readef, register, setef) -- a manifest entry for each is mechanical, no
#     new test-writing required, and `register`'s was added this session as
#     `register-adopt-pid-not-reported` (raising MEASURED from 9/33 to
#     10/33). `wflor` and `wfland` have NO test coverage at all (zero
#     references anywhere under tests/qemu) and need a new assertion written
#     and oracle-pinned before either can get a control. See vms-2b2 for the
#     full split and the follow-up items for the remaining 13.
#   * The anchor placement check is DELIBERATELY LOOSE. It requires the text
#     following an anchor, up to the first `;`, to contain one of that defect's
#     named assertion texts. It exists to catch an anchor parked in a comment
#     block, not to parse C.
#
# vms-d894: THE FIRST BULLET ABOVE WAS THE WHOLE FINDING, AND NOTHING FLOORED
# THE COUNT
#
# The anchor pairing floors CONSISTENCY (a DEFECTS entry and its anchor must
# agree with each other); it was never a floor on the SIZE of DEFECTS, and
# nothing was. MEASURED: deleting `kstat-cvtungrant-mismapped` -- its DEFECTS
# line, its two case arms, and its one anchor, 20 lines across 2 files --
# leaves `coverage` and `selftest` both exit 0, printing "all 41 defect(s)
# anchored by 272 marker(s)" AS A PASS. A full greedy set-cover deletion
# (cover size 12, so 30 of the 42 entries are redundant to it) was DERIVED
# AND THEN EXECUTED, not just predicted: 1179 deleted lines across 13 files,
# `coverage`/`selftest` still green throughout. (An earlier estimate of "~1600
# lines across 26 files" for this same exercise was wrong on both numbers --
# deleted per the standing rule rather than corrected in place; the figure
# above is the one that was actually measured by running the deletion.)
#
# Section 6 of `coverage` (below) adds a derived, printed floor: DEFECTS must
# have at least as many entries as tests/qemu/facility_defects_floor.txt
# records. That file is NOT owned by an in-file deletion the way the anchor
# pairing is -- shrinking DEFECTS without also editing it now fails. IT IS
# STILL NOT TAMPER-PROOF: raising or lowering the floor file's number is one
# more line in one more file, and lowering it to match a shrink still passes.
# What changed is the price and its visibility -- 20 lines/2 files silently
# before, now >=21 lines/3 files, one of which is a file whose only content is
# the number somebody is choosing to lower. A disclosed, priced residual, not
# a claimed closure.
#
# vms-d894 ROUND 2, AND vms-659: A FLOOR SOURCED FROM AN EXECUTION
#
# Every floor above is a relation over declarations this tree makes about
# itself. The one thing in this program that EXECUTES anything is
# tests/qemu/run_facility_negctl.sh -- it boots QEMU against a real /dev/vms
# and injects each of these defects one at a time -- and its results used to
# die with the job log.
#
# It now EMITS what it observed, as tests/qemu/facility_negctl_observed.tsv:
# one row per defect it executed and one row per assertion it actually saw
# FAIL, attributed to the suite that printed it. `coverage` reads that record
# through tests/qemu/facility_negctl_record.sh and derives TWO things from it:
#
#   * section 2b: the suites a past run actually saw an assertion fail in are
#     labelled PROVEN ABLE TO GO RED; every other in-scope suite stays NAMED.
#     Two populations, two cardinals, never summed. That is vms-659: the word
#     "PROVEN" is now spent only where an execution paid for it.
#   * section 6b: the OBSERVED-EXECUTED count -- how many of these defects a
#     past run actually executed. Deleting an entry from DEFECTS today cannot
#     retroactively change what that run observed. Note carefully what the
#     enforcement is there: NOT a numeric comparison. The observed set is
#     intersected with DEFECTS before it is counted, so it can never exceed
#     it and "observed <= declared" would be an assertion that cannot fail.
#     The thing that cannot be gotten past is the REFUSAL -- a record naming
#     a defect DEFECTS no longer has stops the gate certifying anything.
#
# AND THE HALF THAT IS NOT A SNAPSHOT: in CI the driver runs anyway, so it
# compares what it just observed with the committed record and fails on any
# disagreement in either direction. The committed record therefore cannot be
# fabricated upward and cannot go stale quietly.
#
# WHAT THAT STILL DOES NOT BUY, and it is the residual that stays open: a
# deleter who removes a defect from DEFECTS *and* removes that defect's rows
# from the record has told the truth about a smaller manifest -- the live CI
# run agrees with them, and nothing here disagrees. Closing that needs a floor
# from OUTSIDE the commit (the previous commit's copy of the record, or an
# external attestation) and there is not one. What changed is the price again:
# the record's rows are one per OBSERVED ASSERTION, so the deletion is
# proportional to what is being deleted instead of being one integer.
# tests/qemu/facility_record_negctl.sh MEASURES that price on the tree in
# front of it and prints the number, rather than this comment asserting one.
#
# A record that CONTRADICTS this tree -- naming a defect DEFECTS no longer
# has, or an assertion text require_fail no longer names, or emitted by a run
# whose pristine positive control did not pass -- is a REFUSAL, in the shape
# tests/integration/lib/rd_citations.sh already uses: coverage certifies
# nothing from it rather than certifying less.
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
# `coverage` additionally reads, from beside THIS file:
#   facility_defects_floor.txt         the declared count floor (vms-d894 r1)
#   facility_negctl_observed.tsv       the execution record (vms-d894 r2), via
#   facility_negctl_record.sh          its reader
# Its own negative controls are tests/qemu/facility_record_negctl.sh, run as
# the `facility_negctl_record` ctest.
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

# This script's own path. `coverage` reads it back to check that the DEFECTS
# list and the two `case` blocks below agree -- see section 5 there.
SELF="$0"

# The execution-sourced attribution record's reader (rd vms-d894, rd vms-659).
#
# SOURCED CONDITIONALLY, and that is not a fallback. `apply` runs INSIDE the
# harness container, where tests/qemu/Dockerfile copies this file alone to
# /src/tests/qemu/ and the library is not beside it. Failing to source there
# would break the injection path for a facility `coverage` never uses. So the
# library is optional to LOAD and mandatory to HAVE for `coverage`, which
# REFUSES when it is absent rather than quietly checking less -- see section 6.
FNR_LIB="$(dirname "$SELF")/facility_negctl_record.sh"
FNR_LOADED=0
if [ -f "$FNR_LIB" ]; then
    . "$FNR_LIB"
    FNR_LOADED=1
fi

DEFECTS="access-mode-escalation
setprv-grants-unauthorized
kif-setmode-always-kernel
getmode-buffer-not-written
ast-setast-disable
ast-deliver-crossmode-escalation
dclast-super-mode-escalation
eflag-clref-noop
eflag-waitfr-eintr-normal
eflag-setef-status-inverted
eflag-readef-status-inverted
eflag-ascefc-reassoc-status-wrong
eflag-dacefc-status-wrong
eflag-dlcefc-status-wrong
eflag-wflor-status-wrong
eflag-wfland-status-wrong
lock-compat-ex-cr
lock-compat-cr-ex
lock-valblk-grant-not-delivered
lock-enq-immediate-grant-status-wrong
lock-deq-status-wrong
lock-convert-mode-not-updated
resdir-master-csid-not-reported
devtab-owner-not-recorded
devtab-alloc-not-recorded
devtab-dassgn-status-wrong
devtab-getdvi-devnam-status-wrong
devtab-devscan-found-status-wrong
disk-backing-not-resolved
setterm-binding-not-recorded
showterm-width-page-fabricated
showterm-width-page-oracle-shaped
proctab-duplicate-name
proctab-crossgroup-identity
proc-acct-not-sourced
proctab-terminal-redaction-bypassed
proctab-getjpi-nonexpr-status-wrong
proctab-procscan-nonexpr-status-wrong
ident-username-unguarded
executive-not-pinned
pcb-per-thread
bind-client-no-register
creprc-handshake-eintr
run-detached-name-dropped
delprc-privcheck-bypassed
creprc-detach-intermediate-reaped
run-detached-not-detached
run-image-qualifier-refused
run-qualifier-not-abbreviated
lnm-delete-noop
kstat-deadlock-mismapped
kstat-ivlockid-mismapped
kstat-cvtungrant-mismapped
assign-terminal-bypasses-executive
dcl-submit-owner-fabricated
dcl-print-owner-fabricated
dcl-logout-user-fabricated
dcl-reply-operator-fabricated
dcl-accounting-user-fabricated
dcl-fuser-system-fabricated
dcl-fuser-host-login-name
dcl-fident-num2name-host-passwd
dcl-fident-num2name-bracketed-uic
dcl-fident-name2num-host-passwd
getjpi-pidarg-discarded
opcom-header-host-login-name
setuai-sysprv-caller-declared
register-adopt-pid-not-reported
register-continue-identity-dropped
rms-create-filespec-not-translated
scratch-dir-owner-not-system
lnm-manager-delete-noop
lnm-group-scope-collapsed
lnm-privilege-check-bypassed
mbx-not-shared
mbx-wrtattn-not-fired
hiber-ast-not-delivered
dcl-sysinput-mbx-not-read
mmk-drive-command-not-sent
mmk-build-image-not-activated
p0-map-not-recorded
p1-map-not-recorded
p0-unmap-clears-p1
super-mode-escalation
image-rundown-without-entry
image-rundown-leaks-user-lock
imgact-p1-not-protected
consumer-import-not-bound-to-resident
publish-does-not-populate-registry
realimg-auxv-argc-wrong
extern-interp-check-rejects-ours
nonres-producer-not-mapped
tlsdesc-offset-not-biased
devtab-getdvi-userspace-alloc-dropped
lnm-searchlist-equiv-truncated
rightslist-general-hex-as-decimal
sysuaf-uic-radix-decimal
sysuaf-uic-writeback-decimal
bg-recv-length-zeroed
tcpip-ftp-get-length-dropped
bgsock-recv-length-zeroed
vmsfs-mountvis-crossproc-resolve-disabled
initialize-home-magic-not-written"

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
# test_kmod_vmsfs_exepath (rd vms-00e) joins them for the same reason: it
# mounts vmsfs and reads /proc/<pid>/exe and /proc/<pid>/fd/<n>, and never
# opens /dev/vms. Its own red/green control is a vmsfs.ko mutation, not an
# executive one -- it was measured failing 3 phases against the pre-fix
# ->d_revalidate and passing 28/28 against the fixed one.
#
# test_kmod_vmsfs_rename (rd vms-8b3) joins them for the same reason: it
# mounts a block-device vmsfs volume and exercises rename(2), whose own
# red/green control is a vmsfs.ko mutation (vmsfs_blkdev_rename present vs.
# absent), never an executive one -- it opens no /dev/vms and reaches no
# vms_kif entry point.
#
# test_kmod_vmsfs_readdir (rd vms-93a) joins them for the same reason: it
# mounts a BACKING-DIRECTORY (overlay) vmsfs volume and, from a separate
# process, iterates the directory and opens a child by name. Its own
# red/green control is a vmsfs.ko mutation (vmsfs_iterate_shared honouring
# ctx->pos vs. re-scanning the backing dir from 0 every getdents()), never an
# executive one -- it opens no /dev/vms and reaches no vms_kif entry point.
#
# SCOPE_OUT_UNIT_DIRS   directories under src/ whose .c files are NOT executive
#                       translation units. Files there must NOT be named by any
#                       defect (a control there would mean the scope statement
#                       is wrong, not that coverage improved).
# SCOPE_OUT_SUITES      derived tests/qemu suites with no facility control.
# ---------------------------------------------------------------------------
SCOPE_OUT_UNIT_DIRS="kernel/vmsfs"
SCOPE_OUT_SUITES="test_kmod_vmsfs test_kmod_vmsfs_blkdev test_kmod_vmsfs_exepath test_kmod_vmsfs_rename test_kmod_vmsfs_readdir test_kmod_vmsfs_sysgroup"

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
These vmsfs suites are covered by CI job 3c, not by this one.

test_kmod_vmsfs_sysgroup (vms-581, added by #284/#272) is here for exactly
this reason and was added to SCOPE_OUT_SUITES by vms-6c6: the property it
proves -- the System-category SOGW decision (UIC group <= MAXSYSGROUP) that
lets a SYSTEM session create in a system-group directory -- is decided
entirely inside vmsfs.ko's vmsfs_blkdev_permission() (src/kernel/vmsfs/,
SCOPE_OUT_UNIT_DIRS above). It is reached through the VFS open(2) path, not
through /dev/vms; no userspace consumer mirrors the decision (the create is
a raw open(2), never sys$create/RMS), so the only control that could redden
it is a kernel/vmsfs mutation -- which SCOPE_OUT_UNIT_DIRS forbids precisely
because it would be testing vmsfs, not the executive. Its sibling
test_kmod_vmsfs_mountvis (vms-8b6) is NOT scope-out: its cross-process
resolution lives in a userspace library (src/vmsfs/vmsfs_device.c), a
targetable product-half consumer of the kernel mount table, so vms-6c6
anchors it with a real control instead.
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
        targets)      echo "kernel-core/vms_access.c";;
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

    setprv-grants-unauthorized)
        case "$_f" in
        facility)     echo "access modes and privileges (\$SETPRV authorization against the AUTHORIZED mask, VMS_IOCTL_SETPRV)";;
        targets)      echo "kernel-core/vms_access.c";;
        # test_syssvc_setprv drives the PUBLIC sys$setprv (vms-pv1 wired it to
        # vms_kif_setprv -> VMS_IOCTL_SETPRV), and test_kmod_access drives the
        # raw ioctl -- both reach vms_ioctl_setprv's !may_exceed authorization,
        # so this one mutation reddens both, one layer apart. test_kmod_access's
        # two reds are the same self-award seen through the raw ioctl.
        # test_syssvc_setprv_dcl JOINS suites_red WITH vms-e5d7, which wired
        # DCL's SET PROCESS/PRIVILEGES to the same public sys$setprv: an
        # unauthorized process typing the COMMAND reaches the identical
        # !may_exceed authorization, so the mutation reddens its two DCL-surface
        # assertions too (see knock_on_why).
        suites_red)   echo "test_kmod_access test_syssvc_setprv test_syssvc_setprv_dcl";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_setprv() stops intersecting an unprivileged caller's request with its AUTHORIZED mask: for a caller that may NOT exceed its authorization (no SETPRV and not in KERNEL mode) it enables the FULL requested mask instead of the authorized subset, so the process is granted -- and the executive then reports it holding -- any privilege it names. That is the exact self-award \$SETPRV exists to refuse (the vms-b2e privilege LARP, one layer down). One term dropped from one assignment.";;
        require_fail) cat <<'EOF'
a forked child without SETPRV is REFUSED CMKRNL it is not authorized for (SS$_NOTALLPRIV)
the executive still reports CMKRNL NOT held for the unauthorized child
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
an unprivileged process cannot $SETPRV itself CMKRNL
... and still does not hold CMKRNL afterwards
an unauthorized SET PROCESS/PRIVILEGES=CMKRNL prints the oracle-pinned %SYSTEM-W-NOTALLPRIV, not a fabricated success
SHOW PROCESS/PRIVILEGES shows CMKRNL NOT held for the unauthorized process -- the executive refused to widen it
EOF
                      ;;
        knock_on_why) cat <<'EOF'
THE SAME DEFECT, OBSERVED THROUGH THE RAW IOCTL. test_kmod_access.c's
unprivileged child (l.116-122) requests CMKRNL through VMS_IOCTL_SETPRV in USER
mode holding no SETPRV -- the identical !may_exceed authorization path the
public sys$setprv assertions exercise. With the authorized-subset intersection
removed the child is granted CMKRNL, so "an unprivileged process cannot $SETPRV
itself CMKRNL" (its status is now SS$_NORMAL) and "... and still does not hold
CMKRNL afterwards" (chkpriv now reports it held) both go red. These are the two
setprv assertions test_kmod_access already carries as access-mode-escalation
knock-ons; they redden here for the DIFFERENT reason that the authorization
itself was inverted, not that the caller reached KERNEL mode first. No setmode
assertion in either suite moves -- this mutation touches only the enable
!may_exceed branch, which the root parent and every setmode path skip.

THE SAME DEFECT, OBSERVED THROUGH THE DCL COMMAND (vms-e5d7). A DCL.EXE forked
with dropped credentials (test_syssvc_setprv_dcl.c) runs SET
PROCESS/PRIVILEGES=CMKRNL, which now calls the same public sys$setprv. With the
authorized-subset intersection removed the executive grants CMKRNL, so the
command no longer prints %SYSTEM-W-NOTALLPRIV ("... prints the oracle-pinned
%SYSTEM-W-NOTALLPRIV ..." goes red) and its own SHOW PROCESS/PRIVILEGES then
lists CMKRNL as held ("... shows CMKRNL NOT held ..." goes red). Its
AUTHORIZED-process assertions (SET ...=ALL as root) do NOT move: root takes the
may_exceed branch this mutation leaves untouched.
EOF
                      ;;
        esac;;

    kif-setmode-always-kernel)
        case "$_f" in
        facility)     echo "access-mode marshalling in libvmssys (vms_kif_setmode, src/libvmssys/vms_kif.c) -- OVMX-UNWIRED (vms-pv1): this wrapper has NO product caller today, only this test suite";;
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
silently rewritten to PSL_C_KERNEL, and only "... and the mode really
returned to USER" -- the GETMODE re-read added for vms-0e4 -- can see that
the mode never moved; "returning to USER mode is always allowed" (the
vms_kif_setmode() return-status check) cannot, because the callee reports
SS$_NORMAL either way. The knock_on_fail set for THIS defect is MEASURED
EMPTY: a run with this defect injected produces exactly one FAIL line in
test_kmod_access, confirming the mutation is already as fine as it can be.
EOF
                      ;;
        esac;;

    getmode-buffer-not-written)
        case "$_f" in
        facility)     echo "access modes and privileges (VMS_IOCTL_SETMODE/GETMODE/SETPRV/CHKPRIV)";;
        targets)      echo "kernel-core/vms_access.c";;
        suites_red)   echo "test_kmod_access test_kmod_bind";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_getmode() returns before ever calling copy_to_user(), so the ioctl succeeds from the kernel's side (return 0) but the caller's struct vms_getmode_args is left exactly as the caller supplied it -- every call site memset()s that buffer to 0 first, and 0 is PSL_C_KERNEL, so a GETMODE that wrote nothing at all reads back as a process genuinely in KERNEL mode.";;
        require_fail) cat <<'EOF'
a process starts in USER mode
... and the mode really changed
... and the mode really returned to USER
... and the mode is still USER after the denied escalation
EOF
                      ;;
        knock_on_fail) echo "adoption preserved the privilege mask SETIDENT established";;
        knock_on_why) cat <<'EOF'
NOT a blunderbuss: this is ONE mutation to the ONE function (vms_ioctl_getmode)
every one of these five assertions ultimately depends on -- four in
test_kmod_access.c re-read VMS_IOCTL_GETMODE directly, and test_kmod_bind.c's
"adoption preserved the privilege mask SETIDENT established" calls
vms_kif_getmode() (src/libvmssys/vms_kif.c:207/577) to read cur_privs after
SETIDENT/adoption. vms_kif_getmode()'s KIF_CALL macro returns the mapped
error status the instant the ioctl fails, BEFORE the `*cur_privs = args.cur_privs`
assignment runs -- so under this defect the caller's cur/perm variables are
never updated, and the post-adoption comparison against the pre-adoption
snapshot mismatches. Same single function, same single fault, one more
consumer.
MEASURED (vms-0e4 round 2): before this round's fix, "... and the mode really
changed" alone did NOT appear in the test_kmod_access set -- it stayed green
under this exact mutation, because it checked gm.mode == PSL_C_KERNEL (0)
without checking the ioctl's own return value, and 0 is indistinguishable from
the memset(0) default a totally silent GETMODE leaves behind. That was the
audit gap this round closed: the KERNEL-direction sibling test_kmod_access.c's
USER-direction fix (vms-0e4 round 1) was modelled on had the exact defect the
round-1 fix was written to catch, just aimed at the ioctl's return value
instead of the mode's numeric identity with the zeroed default. All four
GETMODE re-reads in test_kmod_access.c now check the ioctl's return value;
this defect is the regression control for that.
EOF
                      ;;
        esac;;

    ast-setast-disable)
        case "$_f" in
        facility)     echo "AST delivery (VMS_IOCTL_DCLAST/SETAST/DELIVERAST)";;
        targets)      echo "kernel-core/vms_ast.c";;
        # test_syssvc_ast JOINS suites_red WITH vms-as1, which wired
        # src/libvms/syssvc/sys_ast.c to the executive: sys$setast is now a
        # translation layer over VMS_IOCTL_SETAST, so a defect injected into
        # kernel/vms_ast.c's SETAST-disable path reaches the PUBLIC sys$ API
        # too. Both suites carry the same two require_fail assertion texts, one
        # layer apart (test_kmod_ast through raw ioctls, test_syssvc_ast through
        # sys$setast), so the mutation reddens both and neither more.
        suites_red)   echo "test_kmod_ast test_syssvc_ast";;
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

    ast-deliver-crossmode-escalation)
        case "$_f" in
        facility)     echo "AST delivery (VMS_IOCTL_DCLAST/SETAST/DELIVERAST), cross-mode delivery scope";;
        targets)      echo "kernel-core/vms_ast.c";;
        # test_syssvc_ast_secmode is the ONLY suite that exercises cross-mode
        # delivery (vms-as1): it declares a KERNEL-mode AST and a USER-mode AST
        # and, at USER mode, drains through the PUBLIC sys$setast. Every other
        # AST suite (test_kmod_ast, test_syssvc_ast) declares and delivers at
        # USER mode only with inner queues empty, so scanning from PSL_C_KERNEL
        # vs from the caller's mode makes NO difference to them -- which is why
        # this mutation reddens exactly one assertion in exactly one suite.
        suites_red)   echo "test_syssvc_ast_secmode";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_deliverast scans from PSL_C_KERNEL again instead of the caller's current access mode, so a user-mode \$SETAST(1) drains (and executes) the KERNEL-mode AST queue -- the access-mode escalation vms-as1's delivery bound closes. Declaring, quota and the per-mode enable flag are untouched; only the delivery SCOPE regresses, so only the cross-mode security assertion reddens (the USER-mode AST is still delivered, and the later at-KERNEL-mode drain still finds g_kfired set, so neither of those reddens).";;
        require_fail) cat <<'EOF'
a user-mode $SETAST(1) did NOT execute the KERNEL-mode AST
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why) echo "";;
        esac;;

    dclast-super-mode-escalation)
        case "$_f" in
        facility)     echo "AST declaration access-mode gate (\$DCLAST / VMS_IOCTL_DCLAST, vms-95a)";;
        targets)      echo "kernel-core/vms_ast.c";;
        # test_syssvc_ast_secmode is the only suite that declares an AST at a
        # more-privileged mode from a genuinely unprivileged caller: PART B
        # forks a child that drops its Linux credentials (so the executive
        # derives no CMEXEC/CMKRNL) and, still at USER mode, calls sys$dclast at
        # SUPER. Every other AST suite declares only at its own mode (or holds
        # CMKRNL when it declares inner), so the SUPER gate makes no difference
        # to them -- this mutation reddens exactly one assertion in one suite.
        suites_red)   echo "test_syssvc_ast_secmode";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_dclast()'s declare-at-more-privileged-mode branch checked KERNEL and EXEC but not SUPER, so an unprivileged USER-mode caller could declare an AST at SUPER (PSL_C_SUPER = 2, MORE privileged than USER = 3) without CMEXEC/CMKRNL -- the same access-mode asymmetry vms_ioctl_setmode's own SUPER gate (super-mode-escalation) already closes, left behind in \$DCLAST. Because image rundown flushes only USER-mode ASTs, such a SUPER AST survives an image and runs when DCL next drains SUPER. The mutation disables the added SUPER gate (0 &&), reverting to the pre-fix fall-through where SUPER declaration is granted outright. Only the cross-mode declaration assertion reddens; queueing, quota and the KERNEL/EXEC checks are untouched.";;
        require_fail) cat <<'EOF'
a USER-mode child without CMEXEC/CMKRNL is REFUSED sys$dclast at SUPER mode
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why) echo "";;
        esac;;

    eflag-clref-noop)
        case "$_f" in
        facility)     echo "event flags (VMS_IOCTL_SETEF/CLREF/READEF/WAITFR/WFLOR/WFLAND/ASCEFC/DACEFC)";;
        targets)      echo "kernel-core/vms_eflag.c";;
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
        targets)      echo "kernel-core/vms_eflag.c";;
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

    eflag-setef-status-inverted)
        case "$_f" in
        facility)     echo "event flags -- \$SETEF's own WASSET/WASCLR discrimination (VMS_IOCTL_SETEF)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-177 (vms-2b2 follow-up). MEASURED at the 9-of-33 audit: no
        # existing mutation hunk sits inside vms_ioctl_setef's own body --
        # eflag-clref-noop mutates \$CLREF, a different handler in the same
        # file. RANGE-ANCHORED to vms_ioctl_setef's own body: the target
        # text "args.status = prev ? SS\$_WASSET : SS\$_WASCLR;" is
        # DUPLICATED verbatim in vms_ioctl_clref, immediately below it in
        # the file.
        #
        # THE REAL BIT-SET IS LEFT UNTOUCHED ON PURPOSE. *flags |= (1U <<
        # bit) still runs and still wakes waiters -- only the discriminating
        # STATUS WORD is inverted. Corrupting the bit-set itself was tried
        # and rejected for the sibling lock-enq defect this session: it can
        # HANG the guest, because blocked WAITFR/WFLOR/WFLAND callers rely
        # on the bit actually flipping, not on what SETEF reports about it.
        # MEASURED (not the entry's first guess): the ORIGINAL require_fail
        # below, "sys$setef(1) set the flag", checks $VMS_STATUS_SUCCESS(),
        # which is true for BOTH WASSET and WASCLR (both are success
        # codes) -- so an inverted discrimination cannot make that specific
        # assertion fail, and a real run confirmed it stayed green. The
        # discriminating assertions are the ones that compare against a
        # SPECIFIC one of the two values, in this suite and in
        # test_kmod_eflag.
        suites_red)   echo "test_syssvc_ef_local test_kmod_eflag";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETEF reports the OPPOSITE of the flag's real previous state -- WASCLR when it was set, WASSET when it was clear. The flag itself is still set correctly (wake_up_interruptible still fires on the real bit), so this is purely a caller-visible lie about history, not a functional break -- but only assertions that check the SPECIFIC previous-state value can see it; \$VMS_STATUS_SUCCESS() alone cannot, since both values are success codes.";;
        require_fail) cat <<'EOF'
sys$setef(1) on an already-set flag reported WASSET
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
setef(5) returns WASCLR
setef(5) again returns WASSET
setef(40) in cluster 1 returns WASCLR
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second through fourth time: every one of these assertions checks SETEF's returned previous-state value against a specific WASSET/WASCLR expectation, and this mutation is the ONLY thing that changed.";;
        esac;;

    eflag-readef-status-inverted)
        case "$_f" in
        facility)     echo "event flags -- \$READEF's own WASSET/WASCLR discrimination (VMS_IOCTL_READEF)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-177. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_readef's own body. The target text is unique in
        # the file (readef's cluster-state-word comparison, not shared with
        # any other handler), so this is a plain text anchor, no range
        # needed.
        # MEASURED (not the entry's first guess): READEF is the standard
        # way every eflag test reads back a flag's state, so a wrong
        # discrimination reaches beyond test_syssvc_ef_local.
        suites_red)   echo "test_syssvc_ef_local test_kmod_bind test_kmod_eflag";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$READEF reports the OPPOSITE of the flag's real current state in args.state's own bit -- WASCLR when the cluster word says set, WASSET when it says clear. args.state itself (the full cluster word) is untouched, so a check that compares the discriminating status against the state word directly catches the lie -- but READEF is the standard way every eflag test reads a flag's state back, so this one status word gates a wide surface.";;
        require_fail) cat <<'EOF'
sys$readef(1) reported WASCLR for the cleared flag
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
$READEF sees the flag this process just set
sibling thread sees the event flag the main thread set
readef(5) returns WASSET
readef(5) after clear returns WASCLR
sys$readef(1) reported WASCLR after the clear
the cluster state word agrees with the status: flag 1's bit is CLEAR
sys$readef(1) reported WASSET after the flag was set
sys$waitfr(1) left the flag SET -- it is not a counting semaphore
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second through ninth time: every one of these assertions reads a flag's state back through READEF's own WASSET/WASCLR discrimination, and this mutation is the ONLY thing that changed.";;
        esac;;

    eflag-ascefc-reassoc-status-wrong)
        case "$_f" in
        facility)     echo "event flags -- \$ASCEFC's own success status, the found-and-reassociate path (VMS_IOCTL_ASCEFC)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-400 (vms-2b2 follow-up). A SECOND attempt at this handler:
        # eflag-ascefc-reassoc-status-wrong was written once already, this
        # session, against test_kmod_eflag_mproc.c AS IT THEN STOOD -- and
        # dropped, not merged, because that file raced its own two
        # processes' first-ever $ASCEFC calls with no ordering between them.
        # vms_ioctl_ascefc has TWO "args.status = SS_NORMAL;" writes: the
        # 4-space one at function scope (the "Create new cluster" branch,
        # shared text with vms_ioctl_waitfr/wflor/wfland/dacefc, all already
        # anchored elsewhere) and a SECOND, uniquely-indented 12-space one
        # inside the "Found it -- associate" branch (list_for_each_entry's
        # if-block) -- the reassociation path this entry targets. Which of
        # the two processes' racing calls landed on which branch was
        # scheduler-dependent, so which of the two "ascefc joined/created"
        # assertions went red was genuinely nondeterministic -- not fixable
        # by choosing a different target line, because the race was in the
        # TEST's own lack of synchronization, not in the mutation.
        # FIXED THIS ROUND: test_kmod_eflag_mproc.c's parent now signals
        # child (token 'P', over the p2c pipe already in the file) only
        # after its OWN $ASCEFC returns, and child now waits for that token
        # before calling its own $ASCEFC -- pipes only, no sleeps, the same
        # synchronisation discipline the file's own header mandates. Parent
        # therefore ALWAYS creates the cluster (4-space branch, already
        # covered); child ALWAYS finds it and re-associates (12-space
        # branch, this entry). UNIQUE TEXT, no range anchor needed:
        # `grep -rn` across src/kernel/*.c finds the 12-space
        # "args.status = SS_NORMAL;" in exactly this one place.
        #
        # test_syssvc_ef_mproc.c JOINS suites_red, MEASURED not predicted:
        # it has the IDENTICAL race (its parent and forked child both call
        # sys$ascefc on "OVMX$F1F_SVC" with no ordering), so it was fixed
        # the same way (a matching 'P' token handshake). Once de-raced, a
        # real run still named 4 MORE assertions here, all genuine and
        # deterministic -- see knock_on_why, not test flakiness (checked by
        # re-running twice; identical both times).
        suites_red)   echo "test_kmod_eflag_mproc test_syssvc_ef_mproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$ASCEFC reports SS\$_UNASEFC -- \"you were never associated with this cluster\" -- for a re-association that actually happened (the cluster's refcount incremented, proc->ef.common[idx] updated to point at it, any prior association released). The caller is told its own real (re)association never took place, the same facade shape as \$DACEFC's and \$DLCEFC's own status-word defects.";;
        require_fail) cat <<'EOF'
child: ascefc joined the named common cluster
child: sys$ascefc joined the named common cluster
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: sys$ascefc re-joined the permanent cluster by name
parent: the waiter was interrupted by a signal repeatedly WHILE blocked in sys$waitfr (the condition under test is reachable, not hypothetical)
parent: sys$waitfr did NOT return until the flag was really set -- an interrupted wait is re-entered, never reported as SS$_NORMAL over a clear flag
parent: wfland child never reported it was ready to block
EOF
                      ;;
        knock_on_why) cat <<'EOF'
"parent: sys$ascefc re-joined the permanent cluster by name" is the SAME
defect observed a second time, no forking involved: sys$dacefc releases the
permanent cluster's last association (it survives, being permanent), and
the very next call, sys$ascefc on that same name, is a genuine
re-association -- the reassociate branch, deterministically, every run.

The other three are ONE STEP REMOVED, through two of this suite's own
forked-child helpers, run_wait_child() and run_wfland_child() -- each
begins with its own sys$ascefc on a cluster its PARENT already created
moments earlier, before the fork. That is ALSO a genuine, deterministic
reassociation (not a race: the parent's own create happens synchronously,
before the fork, so the cluster always exists by the time the child runs).
Both helpers check that call's status and bail out immediately if it is not
success:
    st = sys$ascefc(...);
    if (!(st & 1)) { send_token(c2p_write, 'E'); return 1; }
Under this defect that status lies, so both children take the early-exit
branch WITHOUT EVER REACHING their real test logic -- run_wait_child()
never calls sys$waitfr() at all, so the parent's alarm count stays at 0
("the waiter was interrupted..." fails) and its verdict is 'E' rather than
'S' ("sys$waitfr did NOT return..." fails); run_wfland_child() never sends
the 'R' ready-to-block token, so the parent's read_bounded for it times out
("wfland child never reported..." fails). Confirmed by reading both
helpers, not inferred from the FAIL list alone -- and confirmed
deterministic by re-running this defect twice, identical result both
times, which is what tells this apart from the resource-contention
flakiness this session hit elsewhere on this shared host.
EOF
                      ;;
        esac;;

    eflag-dacefc-status-wrong)
        case "$_f" in
        facility)     echo "event flags -- \$DACEFC's own success status (VMS_IOCTL_DACEFC)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-177. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_dacefc's own body. RANGE-ANCHORED to
        # vms_ioctl_dacefc's own body: "args.status = SS_NORMAL;" at this
        # exact 4-space indentation also appears in vms_ioctl_waitfr,
        # vms_ioctl_wflor, vms_ioctl_wfland (already covered by
        # eflag-waitfr-eintr-normal's sibling scope) and vms_ioctl_ascefc's
        # create path -- vms_ioctl_dacefc is defined between ascefc and
        # dlcefc, so the range excludes all of them.
        suites_red)   echo "test_syssvc_ef_mproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$DACEFC reports SS\$_UNASEFC -- \"you were never associated with this cluster\" -- for a disassociation that actually happened (proc->ef.common[idx] cleared, the cluster's refcount dropped and freed if it hit zero and was not permanent). The caller is told its own real disassociation never took place.";;
        require_fail) cat <<'EOF'
parent: sys$dacefc identifies the cluster from ANY flag number in it, not only the base
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: sys$dacefc released the last association to the permanent cluster
parent: sys$dacefc released the marked cluster
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second and third time: both released-cluster assertions depend on \$DACEFC succeeding on this same call, and this mutation is the ONLY thing that changed.";;
        esac;;

    eflag-dlcefc-status-wrong)
        case "$_f" in
        facility)     echo "event flags -- \$DLCEFC's own success status (VMS_IOCTL_DLCEFC)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-177. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_dlcefc's own body. The target text's 8-space
        # indentation is the only occurrence at that depth in the file, so
        # this is a plain ^-anchored text match, no range needed.
        suites_red)   echo "test_syssvc_ef_mproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$DLCEFC reports SS\$_UNASEFC -- the pre-set default it never overwrites -- for a permanent cluster it actually found and marked for deletion (perm cleared, freed immediately if nothing else is associated). The deletion happens; only the caller-visible confirmation of it does not.";;
        require_fail) cat <<'EOF'
parent: sys$dlcefc accepted the permanent cluster by name
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    eflag-wflor-status-wrong)
        case "$_f" in
        facility)     echo "event flags -- \$WFLOR's own success status (VMS_IOCTL_WFLOR)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-2ed. MEASURED at the 9-of-33 audit: no existing mutation hunk
        # sits inside vms_ioctl_wflor's own body -- eflag-waitfr-eintr-normal's
        # own apply_edit comment explicitly notes its range-anchor excludes
        # WFLOR's copy of the same "args.status = SS_NORMAL;" text. This is
        # the first defect anchored inside \$WFLOR itself, made possible by
        # vms-2ed's new test_syssvc_ef_mproc.c scenario -- before that, no
        # suite in the tree called sys\$wflor at all.
        #
        # THE WAIT PREDICATE ITSELF IS LEFT UNTOUCHED ON PURPOSE.
        # wait_event_interruptible()'s mask comparison still runs and still
        # wakes correctly -- only the post-wait STATUS WORD is corrupted.
        # Corrupting the underlying wait/bit state was tried and rejected
        # for the sibling lock-enq defect this session: it can HANG the
        # guest. RANGE-ANCHORED to vms_ioctl_wflor's own body: this exact
        # "args.status = SS_NORMAL;" also appears in vms_ioctl_waitfr (4-space,
        # earlier in the file) and vms_ioctl_wfland (4-space, later).
        suites_red)   echo "test_syssvc_ef_mproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$WFLOR reports SS\$_ILLEFC instead of SS\$_NORMAL after its wait predicate is genuinely satisfied (at least one mask flag set) -- the wait itself resolved correctly, only the caller-visible confirmation of it did not.";;
        require_fail) cat <<'EOF'
parent: sys$wflor returned with only ONE of the two mask flags set -- OR, not AND
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    eflag-wfland-status-wrong)
        case "$_f" in
        facility)     echo "event flags -- \$WFLAND's own success status (VMS_IOCTL_WFLAND)";;
        targets)      echo "kernel-core/vms_eflag.c";;
        # vms-2ed. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_wfland's own body, for the same reason as
        # eflag-wflor-status-wrong above. Made possible by the same new
        # test_syssvc_ef_mproc.c scenario, which is the only place in the
        # tree that genuinely BLOCKS a process in \$WFLAND (proven via a
        # bounded-silence check with only one of two required flags set)
        # before releasing the second flag and expecting it to unblock --
        # so this is the one suite that can tell "wait resolved, status
        # lied" apart from "wait never resolved at all".
        #
        # THE WAIT PREDICATE ITSELF IS LEFT UNTOUCHED, same reasoning as
        # \$WFLOR's sibling entry. RANGE-ANCHORED to vms_ioctl_wfland's own
        # body: this exact "args.status = SS_NORMAL;" also appears in
        # vms_ioctl_waitfr and vms_ioctl_wflor (both 4-space, earlier in
        # the file).
        suites_red)   echo "test_syssvc_ef_mproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$WFLAND reports SS\$_ILLEFC instead of SS\$_NORMAL after its wait predicate is genuinely satisfied (every mask flag set) -- the blocked process is really released by the second sys\$setef (the child's own read-back at that point still shows both bits set), only the caller-visible confirmation of success does not survive.";;
        require_fail) cat <<'EOF'
parent: sys$wfland unblocked only once BOTH mask flags were set (AND, not OR)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    lock-compat-ex-cr)
        case "$_f" in
        facility)     echo "distributed lock manager (VMS_IOCTL_ENQ/DEQ/CONVERT/GETLKI)";;
        targets)      echo "kernel-core/vms_lock.c";;
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
        targets)      echo "kernel-core/vms_lock.c";;
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

    lock-valblk-grant-not-delivered)
        case "$_f" in
        facility)     echo "distributed lock manager -- value block delivery to a waiter that BLOCKED and was later granted (VMS_IOCTL_ENQ/DEQ/GETLKI), vms-413";;
        targets)      echo "kernel-core/vms_lock.c";;
        # MEASURED. try_grant_waiters() (vms_lock.c:412) is the SOLE call
        # site that runs the mutated copy -- it fires from vms_ioctl_deq and
        # from vms_proc_release_locks, never from the immediate-grant branch
        # of vms_ioctl_enq (vms_lock.c:664-680) or from vms_ioctl_convert's
        # immediate-conversion branch (vms_lock.c:876-899), both of which
        # have their OWN, untouched, valblk copies. test_kmod_lock.c's
        # "value block preserved across DEQ/ENQ" re-acquires UNCONTENDED and
        # never reaches try_grant_waiters() at all; test_kmod_lock_mproc and
        # test_syssvc_lock/test_syssvc_lock_status exercise cross-process
        # contention but never set LCK_M_VALBLK on the request that blocks.
        # test_kmod_lock_sync's scenario 4 (this control's reason for
        # existing) is the only assertion anywhere in the tree that sets
        # LCK_M_VALBLK on a request that is forced to queue and then reads
        # the value back after grant.
        suites_red)   echo "test_kmod_lock_sync";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "try_grant_waiters() stops copying res->valblk into a waiter's own lock->valblk at grant time (vms_lock.c:425-426, the memcpy's guard forced always-false). A process that blocked on a held resource and is later granted it now reads a stale or empty value block with a GOOD status -- the lock succeeded, the data is wrong. Every other valblk assertion in the tree re-acquires uncontended and never reaches this line, so nothing else can see it (vms-413).";;
        require_fail) cat <<'EOF'
child: GETLKI value block equals the PARENT's, not this lock's own pre-grant value (blocked-then-granted valblk delivery)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: child (valblk grant) exited clean
EOF
                      ;;
        knock_on_why) cat <<'EOF'
ONE knock-on, and it is the child's own exit code, not a second property.
child_valblk_grant()'s _exit() at the end of test_kmod_lock_sync.c's scenario
4 is `_exit((fail - fail_at_entry) > 0 ? 1 : 0)` -- a DELTA against the
per-process `fail` counter every CHECK() in that child increments, snapshotted
at function entry precisely so an EARLIER scenario's already-failed state
(inherited across fork(), see that snapshot's own comment) cannot be blamed on
this child. Under this mutation exactly one CHECK in this child goes red (the
valblk-equality assertion named in require_fail above); every other CHECK the
child makes (the async ENQ queuing, the GETLKI granted_mode, its own DEQ) is
untouched by a defect that only deletes a memcpy, so the delta is exactly 1
and the child exits 1. The parent's WIFEXITED(ws) && WEXITSTATUS(ws) == 0
check on that same child process then necessarily goes red too -- the same
single defect, observed a second time through the child's exit status,
exactly as lock-compat-cr-ex's "parent: child (completion AST) exited clean"
knock-on above already establishes the pattern for a sibling scenario in this
same file.
EOF
                      ;;
        esac;;

    lock-enq-immediate-grant-status-wrong)
        case "$_f" in
        facility)     echo "distributed lock manager -- \$ENQ's own success status, immediate-grant path (VMS_IOCTL_ENQ)";;
        targets)      echo "kernel-core/vms_lock.c";;
        # vms-053 (vms-2b2 follow-up). MEASURED at the 9-of-33 audit: no
        # existing mutation hunk sits inside vms_ioctl_enq's own body -- the
        # three lock-* entries above all edit code OUTSIDE it (the
        # lock_compatible() matrix, try_grant_waiters()). This is the first
        # defect anchored inside the handler every lock test calls first.
        #
        # ROUND 1 of this defect corrupted the immediate-grant branch's
        # RETURNED LOCK ID instead (args.lkid forced to 0). A REAL
        # run_facility_negctl.sh run caught, live, why that is unsafe rather
        # than merely wide: every later caller in the SAME boot that reuses
        # a genuine internal lock ID also has it echoed back as 0 to
        # userspace, and something downstream keys blocking-AST completion
        # delivery to the userspace-visible id -- test_kmod_lock_mproc's
        # cross-process AST wait never got its completion, and thirteen
        # suites after it in run order never printed a verdict line at all
        # ('NEVER RAN', harness never reached FINAL RESULTS). A defect that
        # can hang the guest is worse than one that reddens the wrong
        # suite; it burns the whole job's timeout and proves nothing. This
        # replacement corrupts only the STATUS word on the same branch --
        # copy_to_user relays it to userspace and nothing in the kernel
        # branches on it, so it cannot feed back into a later ioctl the way
        # a wrong lock ID does.
        # MEASURED (not the entry's first guess): the immediate-grant path is
        # the SETUP step of nearly every lock test in the tree -- kernel and
        # public-API alike -- so a wrong status on it cascades widely. Real
        # run_facility_negctl.sh output, not a static guess, is what fixed
        # this suites_red/require_fail/knock_on_fail set.
        suites_red)   echo "test_kmod_lock test_kmod_bind test_kmod_lock_mproc test_kmod_lock_sync test_syssvc_lock test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "the immediate-grant branch of \$ENQ reports SS\$_NOTQUEUED instead of SS\$_NORMAL for a lock that WAS granted with no contention -- the request succeeded and the caller is told it was refused. Unlike the lock ID, the status word feeds nothing downstream in the kernel (copy_to_user relays it and returns), so the defect cannot propagate past the assertions that read it -- but nearly every lock test's FIRST step is an uncontended \$ENQ used as setup, so this one status word gates a wide surface.";;
        require_fail) cat <<'EOF'
ENQ NL on TESTRES1
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
ENQ CR on TESTRES2
ENQ second CR on TESTRES2 (compatible)
ENQ EX with value block
$ENQ EX granted through /dev/vms
main thread takes a lock
parent: EX granted on MPROCLOCK1 with blkastadr set
child: CR granted on MPROCLOCK2 (own request)
parent: CR granted on MPROCLOCK2 concurrently with child's CR (CR+CR compatible)
parent: EX granted on SYNCRES1
parent: child (sync block) exited clean
parent: EX granted on DLRES_X
child: EX granted on DLRES_Y
parent: child (deadlock) exited clean
parent: EX granted on ASTRES
parent: child (completion AST) exited clean
parent: EX+VALBLK granted immediately on fresh VALBLKRES (publishes VALBLK_SEED)
parent: sys$enqw EX granted, real lock ID returned (public API)
parent: child's NOQUEUE-denial checks reported via public API
child: sys$enqw EX granted after parent's sys$deq (cross-process release, public API)
parent: child's post-release retry succeeded via public API
parent: child took EX before the CVTUNGRANT probe (setup, not the property under test)
parent: sys$enq CR queues behind the child's EX and still returns a real lock ID (public API)
child: EX granted on SYSSVC_STATUS_DLY (public API)
parent: EX granted on SYSSVC_STATUS_DLX (public API)
parent: child (CVTUNGRANT holder) exited clean
parent: child's report after the deadlock/release sequence has no failures
EOF
                      ;;
        # FOUR MORE, ADDED vms-9b7 -- READ OFF THE DRIVER'S OWN "does NOT name
        # them" LIST (shard 5/6 of the d2d76fc..87d7fda main-red run), NOT
        # PREDICTED. These are test_syssvc_lock_status's deadlock/CVTUNGRANT
        # public-API scenarios, and they arrived here for the same reason the
        # setname/showterm reds arrived in bind-client-no-register: #182
        # (vms-86a) taught test_syssvc_lock_status.c to ignore SIGPIPE, so a
        # scenario whose setup $ENQ is corrupted now runs to a NAMED FAIL
        # instead of dying rc=141 before the assertion prints. Each is the same
        # uncontended-$ENQ-as-setup dependency the entries above have: the two
        # "EX granted on SYSSVC_STATUS_DL[XY] (public API)" are the deadlock
        # scenario's own setup grants reported SS$_NOTQUEUED instead of
        # SS$_NORMAL; "child (CVTUNGRANT holder) exited clean" is the forked
        # child failing that same setup and exiting nonzero; and "child's
        # report after the deadlock/release sequence has no failures" is the
        # parent reading back that the child DID fail. Nothing about deadlock
        # detection, conversion or release was mutated -- the one corrupted
        # status word upstream of every one of these explains all four.
        knock_on_why)
            _n_suites=$(defect_field lock-enq-immediate-grant-status-wrong suites_red | wc -w)
            _n_assert=$(( $(defect_field lock-enq-immediate-grant-status-wrong require_fail | wc -l) + $(defect_field lock-enq-immediate-grant-status-wrong knock_on_fail | wc -l) ))
            echo "the SAME defect, observed ${_n_assert} times in all: every one of these ${_n_assert} assertions across ${_n_suites} suites first depends on an uncontended \$ENQ succeeding as its own setup step, and this mutation is the ONLY thing that changed -- nothing about compatibility, lock IDs, value blocks or AST delivery was touched. A wrong SS\$_NORMAL->SS\$_NOTQUEUED substitution on the one line every one of these calls passes through explains all of them at once; no other hypothesis does.";;
        esac;;

    lock-deq-status-wrong)
        case "$_f" in
        facility)     echo "distributed lock manager -- \$DEQ's own success status (VMS_IOCTL_DEQ)";;
        targets)      echo "kernel-core/vms_lock.c";;
        # vms-053 (vms-2b2 follow-up). MEASURED at the 9-of-33 audit: no
        # existing mutation hunk sits inside vms_ioctl_deq's own body.
        #
        # ROUND 1 of this defect blanked the value-block write-back to the
        # resource (the memcpy guarded by "LCK_M_VALBLK set and not a
        # queued waiter") instead. A REAL run_facility_negctl.sh run showed
        # it INERT -- harness exits 0, nothing reddens. Read the resource
        # lifecycle before trusting a similar target again: DEQ's write-back
        # only matters if a lock's OWN valblk snapshot ever diverges from
        # the resource's before release, and no existing test does that --
        # the one round-trip test_kmod_lock.c has re-acquires the SAME
        # resource with the SAME value it already held, so skipping the
        # write-back changes nothing observable. That is a real gap in
        # existing coverage, not a defect a mutation can expose; it would
        # need new test coverage first (the vms-2ed shape), which is out of
        # scope here. RANGE-ANCHORED to vms_ioctl_deq's own body for the
        # same reason lock-convert-mode-not-updated is: this exact status
        # assignment text also appears in vms_ioctl_convert's own
        # fallthrough path, at the same indentation.
        # MEASURED (not the entry's first guess), same shape as
        # lock-enq-immediate-grant-status-wrong: DEQ is the universal
        # cleanup step, so a wrong status on it cascades to every suite
        # that ever releases a lock. Real run_facility_negctl.sh output,
        # not a static guess, is what fixed this suites_red/require_fail/
        # knock_on_fail set.
        suites_red)   echo "test_kmod_lock test_kmod_bind test_kmod_lock_mproc test_kmod_lock_sync test_syssvc_lock test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$DEQ reports SS\$_IVLOCKID instead of SS\$_NORMAL for a lock it actually released -- the release happens (removed from the resource's list, waiters get their chance, the lock entry is freed), only the status word lies about it -- but nearly every lock test's LAST step is releasing what it acquired, so this one status word gates a wide surface, symmetric with \$ENQ's.";;
        require_fail) cat <<'EOF'
DEQ lock
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
$DEQ released the lock
sibling thread can $DEQ the lock the main thread took
child: DEQ queued CR request
parent: DEQ own EX lock
parent: released EX (should grant child)
parent: child (deadlock) exited clean
parent: released EX on ASTRES (grants child + queues AST)
parent: child (completion AST) exited clean
parent: released EX on VALBLKRES (grants child's queued request)
child: released its own granted EX on VALBLKRES
parent: child (valblk grant) exited clean
parent: sys$deq released EX lock (public API)
child: sys$deq released its own EX lock
parent: child's post-release retry succeeded via public API
parent: dequeued its still-queued CR lock
parent: child (CVTUNGRANT holder) exited clean
parent: released X (should unblock the child)
EOF
                      ;;
        knock_on_why)
            _n_suites=$(defect_field lock-deq-status-wrong suites_red | wc -w)
            _n_assert=$(( $(defect_field lock-deq-status-wrong require_fail | wc -l) + $(defect_field lock-deq-status-wrong knock_on_fail | wc -l) ))
            echo "the SAME defect, observed a second (through seventeenth) time: every one of these ${_n_assert} assertions across ${_n_suites} suites depends on an uncontended \$DEQ succeeding as its own cleanup step, and this mutation is the ONLY thing that changed -- nothing about compatibility, granted mode, or value blocks was touched. A wrong SS\$_NORMAL->SS\$_IVLOCKID substitution on the one line every one of these calls passes through explains all of them at once; no other hypothesis does."
            ;;
        esac;;

    lock-convert-mode-not-updated)
        case "$_f" in
        facility)     echo "distributed lock manager -- \$ENQ/CONVERT's own granted-mode update, immediate-conversion path (VMS_IOCTL_CONVERT)";;
        targets)      echo "kernel-core/vms_lock.c";;
        # MEASURED, same audit. lock->granted_mode = args.lkmode also appears
        # in vms_ioctl_enq's immediate-grant branch (line 666) -- the SAME
        # text -- so this mutation is RANGE-ANCHORED to vms_ioctl_convert's
        # own function body in apply_edit() below, not text-anchored, or it
        # would silently also mutate ENQ's grant path on every apply.
        suites_red)   echo "test_kmod_lock";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$ENQ/CONVERT's immediate-conversion branch stops updating lock->granted_mode to the newly-requested mode (the assignment deleted). The call still reports SS\$_NORMAL -- conversion \"succeeded\" -- but GETLKI on the same lock afterward reads back the OLD granted mode, not the one just requested.";;
        require_fail) cat <<'EOF'
granted mode is CR
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    resdir-master-csid-not-reported)
        case "$_f" in
        facility)     echo "distributed lock manager -- DLM resource-directory + local-mastering readback (VMS_IOCTL_GET_RESMASTER, vms-ci.5 DB)";;
        targets)      echo "kernel-core/vms_lock.c";;
        # vms_kif_get_resmaster()/VMS_IOCTL_GET_RESMASTER is reached by no
        # suite but test_kmod_resdir (grep across tests/qemu and
        # src/libvmssys confirms it) -- the raw-ioctl diagnostic this defect
        # is the only caller of.
        suites_red)   echo "test_kmod_resdir";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_get_resmaster()'s copy-out of the resource's master CSID is pinned to 0, while the INTERNAL res->master_csid state the mastering decision and is_local_master's own compat check are computed from is left untouched -- \$ENQ still masters the resource locally and grants through the existing single-node lock manager exactly as before. This isolates the DIAGNOSTIC READBACK from the ACTUAL mastering state, the same DISPLAY/STATE class devtab-alloc-not-recorded catches on the device table: a resource that IS locally mastered is reported to the one reader that exists (GET_RESMASTER) as though it were not.";;
        require_fail) cat <<'EOF'
resource is mastered on this node
DLMRES2 mastered locally, 1 granted
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    devtab-owner-not-recorded)
        case "$_f" in
        facility)     echo "device table (VMS_IOCTL_ASSIGN/DASSGN/GETDVI/DEVSCAN/TTSETMODE/ALLOC/DALLOC)";;
        targets)      echo "kernel-core/vms_devtab.c";;
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
        targets)      echo "kernel-core/vms_devtab.c";;
        # test_syssvc_showdev.c (vms-fb9) is the derived suite that drives
        # SHOW DEVICE through the real DCL.EXE -- landed after this manifest
        # existed, and facility_negctl_manifest's own coverage check requires
        # every derived suite to be reachable by a control. devinfo_fill() is
        # the one place BOTH $GETDVI and $DEVICE_SCAN copy the flag out of,
        # so this is the narrowest mutation that can redden the user-visible
        # reader without disturbing $ALLOC/$DALLOC's own bookkeeping (which
        # reads dev->allocated directly, never through this copy).
        # test_syssvc_getdvi.c (vms-dv1) is the C-API sibling of the DCL-driven
        # test_syssvc_showdev: its "B sees that A allocated the device" step
        # reads the same devinfo_fill() allocation flag through the PUBLIC
        # sys$getdvi, so pinning that copy to 0 reddens it too.
        suites_red)   echo "test_kmod_devtab test_syssvc_showdev test_syssvc_getdvi";;
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

    devtab-dassgn-status-wrong)
        case "$_f" in
        facility)     echo "device table -- \$DASSGN's own success status (VMS_IOCTL_DASSGN)";;
        targets)      echo "kernel-core/vms_devtab.c";;
        # vms-2e7 (vms-2b2 follow-up). MEASURED at the 9-of-33 audit: no
        # existing mutation hunk sits inside vms_ioctl_dassgn's own body --
        # devtab-owner-not-recorded and devtab-alloc-not-recorded both
        # mutate devinfo_fill()/the \$ASSIGN owner write, neither of which
        # is inside \$DASSGN. RANGE-ANCHORED to vms_ioctl_dassgn's own body:
        # this exact 8-space "args.status = SS_NORMAL;" also appears in
        # \$ALLOC (twice) and \$DALLOC, all defined nearby in the same file.
        # MEASURED (not the entry's first guess): dassgn is called as
        # cleanup by suites outside test_kmod_devtab too. Real
        # run_facility_negctl.sh output, not a static guess, is what fixed
        # this suites_red/require_fail/knock_on_fail set.
        suites_red)   echo "test_kmod_devtab test_kmod_setterm test_syssvc_qio_terminal";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$DASSGN reports SS\$_IVCHAN -- \"no such channel\" -- for a channel it actually found and released (removed from the process's channel list, device_release_channel() run, the entry freed). The deassignment happens; only the caller-visible confirmation of it does not.";;
        require_fail) cat <<'EOF'
channel deassigned
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
last channel deassigned
B returns its channel
parent: sys$dassgn("TT:" channel) succeeded (public API)
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second and third time: every one of these assertions depends on \$DASSGN succeeding as its own cleanup/release step, and this mutation is the ONLY thing that changed -- nothing about the channel table's real bookkeeping was touched.";;
        esac;;

    devtab-getdvi-devnam-status-wrong)
        case "$_f" in
        facility)     echo "device table -- \$GETDVI's own success status on the by-NAME lookup path (VMS_IOCTL_GETDVI)";;
        targets)      echo "kernel-core/vms_devtab.c";;
        # vms-2e7. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_getdvi's own body. Two "args.status =
        # SS_NORMAL;" sites exist in this function -- the by-CHANNEL path
        # and the by-NAME path -- so this is RANGE-ANCHORED to the unique
        # "if (args.select != VMS_DVI_SEL_DEVNAM) {" line (which opens the
        # by-name branch) through the function's own closing brace,
        # excluding the by-channel path entirely.
        # MEASURED (not the entry's first guess): GETDVI-by-name is the
        # standard cross-process read-back verification every device test
        # in the tree uses, so a wrong status on it cascades widely. Real
        # run_facility_negctl.sh output, not a static guess, is what fixed
        # this suites_red/require_fail/knock_on_fail set.
        suites_red)   echo "test_kmod_devtab test_syssvc_qio_terminal test_syssvc_showdev test_syssvc_showterm test_syssvc_getdvi";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$GETDVI by device name reports SS\$_NOSUCHDEV for a device it actually found and filled in (devinfo_fill() still runs, args.info is still the real row) -- the lookup succeeds; only the caller-visible confirmation of it does not. Nearly every device test in the tree uses a by-name GETDVI as its read-back verification step, so this one status word gates a wide surface.";;
        require_fail) cat <<'EOF'
device still exists after its owner dies
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
console OPA0: exists without any process creating it
B sees the ownership A took with a channel alone (A writes, B reads)
this process can still read the device
reference count returns to zero
parent: baseline read of the executive's OPA0: row succeeded
parent: a fresh child process could read the executive's OPA0: row
A-WRITES/B-READS: a fresh child sees the reference sys$assign("TT:") added to OPA0: in the executive (public API, cross-process)
parent: a second fresh child process could read the executive's OPA0: row
A-WRITES/B-READS: a second fresh child sees OPA0:'s reference count back at baseline after sys$dassgn (the release reached the executive, not just local bookkeeping)
SHOW DEVICE OPA0: resolves the name through the executive and prints its row
A-WRITES/B-READS: DCL's SHOW DEVICE reports the console allocated -- a change made by a DIFFERENT process, which a per-process device view could not show
the console is still listed once the other process is gone
SHOW TERMINAL names _OPA0: once the executive holds the binding -- the SAME BINARY that named nothing a moment ago
the characteristics heading is printed (oracle section 2)
grid row 1 is byte-for-byte the V7.3 capture
grid row 2 is byte-for-byte the V7.3 capture
grid row 4 is byte-for-byte the V7.3 capture
grid row 10 is byte-for-byte the V7.3 capture
the last row carries the single remaining characteristic, unpadded
...and the cleared Echo bit, in the grid cell the oracle prints it in
...and the set Pasthru bit, so both directions of one IO$_SETMODE are read back
...and grid row 1 is the oracle's bytes again, so neither is the grid
the console is not allocated before any process allocates it
B sees that A allocated the device
the allocation is gone once the other process exits (not a constant)
EOF
                      ;;
        knock_on_why)
            _n_suites=$(defect_field devtab-getdvi-devnam-status-wrong suites_red | wc -w)
            _n_assert=$(( $(defect_field devtab-getdvi-devnam-status-wrong require_fail | wc -l) + $(defect_field devtab-getdvi-devnam-status-wrong knock_on_fail | wc -l) ))
            echo "the SAME defect, observed a second (through twenty-second) time: every one of these ${_n_assert} assertions across ${_n_suites} suites depends on a by-name \$GETDVI succeeding as its own read-back verification step, and this mutation is the ONLY thing that changed -- nothing about ownership, allocation or channels was touched. A wrong SS\$_NORMAL->SS\$_NOSUCHDEV substitution on the one line every one of these calls passes through explains all of them at once; no other hypothesis does."
            ;;
        esac;;

    devtab-devscan-found-status-wrong)
        case "$_f" in
        facility)     echo "device table -- \$DEVICE_SCAN's own success status when a row IS found (VMS_IOCTL_DEVSCAN)";;
        targets)      echo "kernel-core/vms_devtab.c";;
        # vms-2e7. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_devscan's own body. The only
        # "args.status = SS_NORMAL;" in this function, so a plain
        # range-anchor to vms_ioctl_devscan's own body (for consistency
        # with its siblings, though a bare text anchor would also be
        # unambiguous here) is enough.
        # MEASURED (not the entry's first guess): SHOW DEVICE with no
        # argument drives DEVSCAN too (the "bare listing"), so this
        # reaches test_syssvc_showdev as well as test_kmod_devtab.
        # test_syssvc_getdvi (vms-dv1) drives sys$device_scan directly, whose
        # loop ends the moment DEVSCAN's first row returns the wrong status.
        suites_red)   echo "test_kmod_devtab test_syssvc_showdev test_syssvc_getdvi";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$DEVICE_SCAN reports SS\$_NOSUCHDEV -- a real but wrong refusal -- for the FIRST row it finds, even though it filled in a real device's info first. Every caller here loops \"while status == SS\$_NORMAL\", so a wrong status on the very first hit ends the scan immediately: it never sees a second device and never reaches the real SS\$_NOMOREDEV terminator either.";;
        require_fail) cat <<'EOF'
device scan terminates with SS$_NOMOREDEV
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
device scan lists the console terminal
bare SHOW DEVICE lists OPA0: -- a device DCL has no other way to know about, read from the executive's table
the listing carries the oracle's column header (section 4)
the bare listing shows it too, so both row sources ($DEVICE_SCAN and $GETDVI) read the same shared table
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second through fifth time: every one of these assertions depends on \$DEVICE_SCAN succeeding on its first row, and this mutation is the ONLY thing that changed.";;
        esac;;

    disk-backing-not-resolved)
        case "$_f" in
        facility)     echo "disk unit resolution (VMS_IOCTL_DISK_RESOLVE): the executive hands a process the Linux block device a DK unit was enumerated from";;
        targets)      echo "kernel-core/vms_devtab.c";;
        # vms-3e8. vms_ioctl_disk_resolve() copies the backing device name in
        # exactly one place -- the strscpy from dev->backing into args.backing.
        # Anchoring on that one line zeroes the name the resolve hands back
        # while leaving SS$_NORMAL and the dev_t (major/minor) intact, so the
        # executive REPORTS the unit resolved and hands back an EMPTY backing:
        # the facade shape (reports success, shares nothing) on the one fact
        # this facility exists to carry. test_kmod_disk is the only suite that
        # calls vms_kif_disk_resolve, so this reddens it alone -- its two
        # backing-name assertions (DKA0:->vda, DKA100:->vdb), while the
        # resolve-status, dev_t-match and negative-control assertions stay
        # green because none of them reads the backing NAME.
        suites_red)   echo "test_kmod_disk";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_disk_resolve() still looks the unit up, still checks it is a DISK, still returns SS\$_NORMAL and still fills in the backing dev_t (major/minor) -- it just hands back an EMPTY backing device NAME. So a process is told DKA0: resolved and given no device to open: the INV-6 facade shape (success reported, the fact not shared), isolated to the ONE field this facility exists to carry. The resolve status, the dev_t match against /dev/vda, and the NOSUCHDEV / IVDEVNAM negative controls are all untouched -- none of them reads the backing name -- so only the two backing-name assertions can tell the difference.";;
        require_fail) cat <<'EOF'
DKA0: backing device is vda (the executive's enumeration)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
DKA100: backing device is vdb (the executive's enumeration)
EOF
                      ;;
        knock_on_why)  echo "the SAME empty-name defect, observed a second time on the second disk: the mutation zeroes the ONE strscpy every resolve passes through, so DKA100:'s backing name comes back empty exactly as DKA0:'s does. Both are the one skipped name-copy observed on the two units, not two independent properties -- the dev_t match for both disks stays green because major/minor are still copied, which is what shows this is the NAME write and not the whole resolve.";;
        esac;;

    setterm-binding-not-recorded)
        case "$_f" in
        facility)     echo "job-to-terminal binding (VMS_IOCTL_SETTERM, read back through GETJPI)";;
        targets)      echo "kernel-core/vms_devtab.c";;
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
        targets)      echo "kernel-core/vms_proctab.c";;
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
        # test_syssvc_setname is here as of vms-fbe: SET PROCESS/NAME
        # (src/vmsdcl/dcl_cmd_set.c) did not call vms_kif_setprn AT ALL
        # before that item -- it only wrote a per-DCL-process struct nothing
        # else could read (Rule 11). Now that it calls through, the SAME
        # short-circuited clash is visible at a FOURTH layer: a second
        # DCL.EXE's own SET PROCESS/NAME for a name a live process already
        # holds must be refused with the two-line %SET-E-NOTSET /
        # -SYSTEM-F-DUPLNAM shape. MEASURED (not
        # assumed) with the live mutation: this reddens exactly ONE
        # assertion in test_syssvc_setname, its own name for the same
        # property, listed below in knock_on_fail (a fourth observation of
        # the SAME defect, not a distinct one -- see knock_on_why).
        suites_red)   echo "test_kmod_procnam test_syssvc_procnam test_syssvc_startup_service test_syssvc_setname";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$SETPRN stops rejecting a name already held in the UIC group: the SS\$_DUPLNAM clash test is short-circuited. Name storage, lookup, scan and validation are untouched. The raw-ioctl suite, the public sys\$ suite, and BOTH DCL command suites (RUN/DETACHED's startup service and SET PROCESS/NAME) each name it.";;
        require_fail) cat <<'EOF'
duplicate process name rejected with SS$_DUPLNAM
sys$creprc refuses a duplicate process name with SS$_DUPLNAM
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
starting the same named service twice is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM
the service's name is released when the service dies
SET PROCESS/NAME for a name already held refuses with the oracle-pinned two-line %SET-E-NOTSET / -SYSTEM-F-DUPLNAM shape
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
The third (vms-fbe) is the SAME clash test again, seen through a FOURTH
reader: test_syssvc_setname's holder DCL.EXE (started before the mutated
image even boots the mutation-affected suites) already holds HOLDER_NAME
when this defect's mutated vms_ioctl_setprn() is asked, by a second DCL.EXE
running SET PROCESS/NAME=HOLDER_NAME, whether that name clashes. The
short-circuited check answers "no clash" regardless, so the second DCL.EXE's
SET PROCESS/NAME succeeds where OpenVMS refuses it, and the assertion that
checks for the refusal goes red. It is the identical condition
require_fail's two lines already name, observed a fourth time because a
fourth caller now reaches vms_ioctl_setprn() at all (vms-fbe: before it,
DCL's SET PROCESS/NAME never called it, so this suite could not have existed
to observe anything here).
EOF
                      ;;
        esac;;

    proctab-crossgroup-identity)
        case "$_f" in
        facility)     echo "process table, cross-UIC-group identity read (VMS_IOCTL_GETJPI/PROCSCAN authorisation)";;
        targets)      echo "kernel-core/vms_proctab.c";;
        # Two layers of the same clause, MEASURED not guessed:
        # test_syssvc_showproc names the property through the user-visible
        # command (SHOW PROCESS/ID on an out-of-group process must be
        # refused), and test_kmod_ident sees it at the raw ioctl. The first
        # run of this control listed only the syssvc suite and the driver's
        # equality check rejected it, naming test_kmod_ident's assertions --
        # which is the check doing exactly its job.
        #
        # test_syssvc_procnam WAS a third detector here (its SHOW SYSTEM P12
        # block): before vms-a7e a row that stopped being redacted kept its
        # linux_pid and the DCL layer could then source a CPU figure for it,
        # so this clause governed the row's accounting too. vms-a7e decoupled
        # them -- the executive sources accounting from the task's pid_ref
        # (fill_proc_acct), independent of vms_proc_may_read()'s outcome and
        # of the zeroed linux_pid -- so mutating this clause no longer changes
        # any SHOW SYSTEM accounting. P12 now detects proc-acct-not-sourced
        # instead, and is no longer red for THIS defect.
        suites_red)   echo "test_syssvc_showproc test_kmod_ident";;
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
property. All seven were MEASURED by running this control, not predicted:
the first run named only the two SHOW-PROCESS assertions and the driver's
equality check rejected it and printed the rest. The seventh (the terminal
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

THE ENUMERATION SIDE USED TO BE HERE TOO, and vms-a7e moved it. Before that,
"the UNREADABLE row fabricates NO CPU figure at all" (test_syssvc_procnam
block P12) reddened for this defect: vms_ioctl_procscan() called
proc_fill_info() with vms_proc_may_read()'s outcome as its `full` argument,
so a row that became readable kept its linux_pid and the DCL layer could then
source a CPU figure for it -- one clause governed both the item read and the
row's accounting. vms-a7e sources accounting in the executive from the task's
pid_ref, independent of `full` and of the zeroed linux_pid, so this clause no
longer changes any SHOW SYSTEM accounting; that assertion is now a detector of
proc-acct-not-sourced, not of this defect, and is gone from both lists above.
Identity is still privileged and enumeration still is not (docs/oracle/
vax73-privileges.md Section 5.5); what changed is that accounting is not
identity, so it no longer rides this clause.

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
that condition (vms-d0b) -- the identical `full`-gated redaction the
enumeration-side paragraph above describes, reaching a field added later.
With the WORLD clause deleted, D's row stops being redacted
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

    proc-acct-not-sourced)
        case "$_f" in
        facility)     echo "process table, per-process accounting sourced from the Linux task (fill_proc_acct(), vms-a7e)";;
        targets)      echo "kernel-core/vms_proctab.c";;
        # The executive sources CPU time / page faults / resident pages /
        # login time from the task it pins by pid_ref, in fill_proc_acct(),
        # and vms_ioctl_getjpi()/vms_ioctl_procscan() call it after dropping
        # the hash lock. This mutation removes BOTH calls, so no row carries
        # accounting (every VMS_PI_V_* accounting bit stays clear and every
        # field stays 0) -- the pre-vms-a7e state. Three suites read that
        # accounting: test_kmod_procacct at the raw ioctl (its whole point),
        # test_syssvc_showproc as SHOW SYSTEM's Page-flts/Pages columns, and
        # test_syssvc_procnam block P12 as the CPU figure on a redacted row.
        suites_red)   echo "test_kmod_procacct test_syssvc_showproc test_syssvc_procnam";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "fill_proc_acct() is where the executive reads the real Linux task's CPU time, page faults, resident pages and creation time into the process row (struct vms_procinfo.cputim/pageflts/pages/logintim + the VMS_PI_V_* validity bits). Removing its two call sites is the whole facility gone: rows come back with the accounting bits clear and the fields zero, exactly as they did before vms-a7e. Identity, redaction, naming, scanning and the P0/P1 extents are untouched.";;
        require_fail) cat <<'EOF'
PROCSCAN rows carry the same executive accounting GETJPI does -- this is the exact path SHOW SYSTEM's columns read
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
page faults are sourced and non-zero (a live process has faulted its image in) -- JPI$_PAGEFLTS from the executive
resident pages are sourced and non-zero (a live user process has an address space) -- JPI$_PPGCNT from the executive
CPU time is sourced (valid bit set) -- JPI$_CPUTIM from the executive
login time is sourced (valid bit set) -- JPI$_LOGINTIM from the executive
after burning CPU, JPI$_CPUTIM reports a non-zero figure -- the executive is measuring real consumed CPU, not reporting a stuck 0
A-BURNS/B-READS: B reads A's real page-fault and resident-page counts out of A's executive row -- accounting a per-process fake could not show for another process
B reads A's non-zero CPU time out of A's row (A had burned CPU) -- sourced from the task the executive pins, not from B's own state
the subject's Page flts (42-51) and Pages (53-59) fields carry real digits at VMS geometry -- the executive sourced them
the CPU field starts at column 25, so the Process Name field is 15 columns wide
the readable row carries a CPU figure
the UNREADABLE row carries XGRP's own REAL (non-zero) CPU figure -- accounting rides through identity redaction, matching the oracle, not a fabricated zero
EOF
                      ;;
        knock_on_why) cat <<'EOF'
EVERY RED IS THE SAME FACILITY SEEN FROM A DIFFERENT READER, not a second
property, and the set was MEASURED by injecting this mutation and reading the
harness, not predicted. fill_proc_acct() is the single source of all four
accounting fields, so removing it clears them for every reader at once:

  - test_kmod_procacct is the dedicated raw-ioctl suite; its page-fault,
    resident-page, CPU-time, login-time, burn-registers-CPU, A-BURNS/B-READS
    and PROCSCAN-parity assertions all read the fields this mutation stops
    the executive from setting. (Its "honestly absent" assertions for state/
    priority/I-O/quota STAY GREEN -- those bits are supposed to be clear, and
    this mutation does not touch them; and "the figures B reports for A are
    A's magnitude" stays green because both sides read zero, 0 >= 0.)
  - test_syssvc_showproc reads the same fields as SHOW SYSTEM's CPU, Page
    flts and Pages columns; with nothing sourced they all render blank, so
    BOTH the Page-flts/Pages digit assertion AND the CPU-field-at-column-25
    geometry assertion (which expects digits in the CPU field) fail.
  - test_syssvc_procnam reads the CPU figure on TWO rows: its own readable
    row and a cross-group (redacted) one. The whole point of vms-a7e is that
    accounting rides through identity redaction, so with the source gone
    BOTH rows' CPU fields are blank -- "the readable row carries a CPU
    figure" and the redacted-row assertion (P12) both fail.

The by-name and identity paths are untouched: this mutation is under the
ACCOUNTING source, not the authorisation clause proctab-crossgroup-identity
mutates, so SHOW SYSTEM still lists every process and $GETJPI identity is
still refused/allowed exactly as before.
EOF
                      ;;
        esac;;

    proctab-terminal-redaction-bypassed)
        case "$_f" in
        facility)     echo "process table, terminal field on a redacted row (proc_fill_info(), vms-d0b)";;
        targets)      echo "kernel-core/vms_proctab.c";;
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

    proctab-getjpi-nonexpr-status-wrong)
        case "$_f" in
        facility)     echo "process table -- \$GETJPI's own refusal status when NO target is found (VMS_IOCTL_GETJPI)";;
        targets)      echo "kernel-core/vms_proctab.c";;
        # vms-68e (vms-2b2 follow-up). MEASURED at the 9-of-33 audit: no
        # existing mutation hunk sits inside vms_ioctl_getjpi's own body --
        # proctab-duplicate-name/crossgroup-identity/terminal-redaction-
        # bypassed all mutate proc_fill_info() or the registration/naming
        # path, none of which is GETJPI's own status write.
        #
        # ROUND 1 of this defect corrupted GETJPI's own SUCCESS path (the
        # shared "args.status = SS_NORMAL;" every selector -- SELF, PID,
        # PRCNAM -- converges on after proc_fill_info()) instead of this
        # refusal path. LEARNED THIS SESSION (see vms-68e's own item
        # notes: "a getjpi-adjacent assertion can read identity through
        # vms_kif_getjpi_self() WITHOUT going through this handler's own
        # status write at all" was the WARNING; a real run showed the
        # inverse problem was worse): GETJPI's success path is the shared
        # foundation nearly every identity-reading test in the tree calls
        # through -- SHOW PROCESS, F\$GETJPI, every cross-process identity
        # check -- so corrupting it reddened 9 suites and roughly 90
        # assertions. That is not "the same defect observed widely", it is
        # "the read side of process identity is gone", which stops being a
        # useful discriminator of GETJPI specifically. This replacement
        # targets the NARROWER refusal path instead: the "no such process"
        # branch, reached only by a lookup that legitimately finds nothing.
        # RANGE-ANCHORED to vms_ioctl_getjpi's own body: this exact
        # "args.status = SS_NONEXPR;" also appears in vms_ioctl_procscan's
        # own not-found path (a DIFFERENT defect's target, below).
        # MEASURED (not the entry's first guess): every "name/process not
        # found" check in the tree -- kernel and public-API alike -- routes
        # through this same refusal line, so it reaches beyond
        # test_kmod_procnam.
        suites_red)   echo "test_kmod_procnam test_syssvc_procnam test_syssvc_showproc test_syssvc_startup_service";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$GETJPI reports SS\$_NOPRIV instead of SS\$_NONEXPR when no target row exists at all -- a lookup that found nothing is told it was refused by privilege, not that there was nothing to refuse. args.info is still zeroed (the memset ahead of this line is untouched), so this is purely the wrong flavor of \"no\", not a fabricated \"yes\" -- but every not-found check in the tree depends on this exact status, so it gates a wide surface.";;
        require_fail) cat <<'EOF'
unset name does not resolve (SS$_NONEXPR)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
unknown name does not resolve (SS$_NONEXPR)
name released when the process exits
a name held only in another UIC group does not resolve
sys$getjpi by an unheld name returns SS$_NONEXPR
an absent process name reports %SYSTEM-W-NONEXPR verbatim
SHOW PROCESS <name> on an out-of-group process reports %SYSTEM-W-NONEXPR -- the name search is group-scoped
... and NOT %SYSTEM-F-NOPRIV: the group-scoped name search never reaches a process to be refused
the service's name is released when the service dies
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second through ninth time: every one of these assertions depends on a genuinely-absent name or process resolving through this identical refusal line, and this mutation is the ONLY thing that changed.";;
        esac;;

    proctab-procscan-nonexpr-status-wrong)
        case "$_f" in
        facility)     echo "process table -- \$PROCSCAN's own terminator status when the scan is exhausted (VMS_IOCTL_PROCSCAN)";;
        targets)      echo "kernel-core/vms_proctab.c";;
        # vms-68e. MEASURED, same audit: no existing mutation hunk sits
        # inside vms_ioctl_procscan's own body.
        #
        # ROUND 1 of this defect corrupted PROCSCAN's own SUCCESS path (the
        # one "args.status = SS_NORMAL;" every found row returns through)
        # instead of this terminator path, mirroring the mistake ROUND 1 of
        # proctab-getjpi made and for the identical reason: SHOW SYSTEM
        # (every caller here loops "while status == SS_NORMAL") is as
        # foundational to process-identity tests as \$GETJPI is, so
        # corrupting the shared success status reddened 6 suites and ~21
        # assertions -- not a scoped property of PROCSCAN, the whole
        # enumeration primitive. WORSE: the chosen wrong value (SS_NONEXPR)
        # happened to equal the CORRECT terminator value this same entry's
        # own require_fail checks for, so that assertion passed by
        # COINCIDENCE and never actually exercised the mutation at all --
        # caught by the real run, not by re-reading the code. This
        # replacement targets the NARROWER terminator path instead: the
        # "table exhausted, no more rows" branch, reached only by a scan
        # that runs to completion. RANGE-ANCHORED to vms_ioctl_procscan's
        # own body: this exact "args.status = SS_NONEXPR;" also appears in
        # vms_ioctl_getjpi's own not-found path (a DIFFERENT defect's
        # target, above).
        # MEASURED (not the entry's first guess): test_kmod_ident also runs
        # a full-table scan to its terminator, independent of
        # test_kmod_procnam's own.
        suites_red)   echo "test_kmod_procnam test_kmod_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$PROCSCAN reports SS\$_NOPRIV instead of SS\$_NONEXPR when the table is genuinely exhausted -- a scan that ran off the end is told it was refused by privilege, not that there was nothing left to enumerate. args.info stays whatever the last found row left it (the memset ahead of this line, like GETJPI's, is untouched), so this is purely the wrong flavor of \"no more\", not a fabricated row.";;
        require_fail) cat <<'EOF'
process scan terminates with SS$_NONEXPR
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the unprivileged scan terminates with SS$_NONEXPR
EOF
                      ;;
        knock_on_why)  echo "the SAME defect, observed a second time: test_kmod_ident's own scan also depends on reaching this identical terminator line.";;
        esac;;

    ident-username-unguarded)
        case "$_f" in
        facility)     echo "authenticated identity (VMS_IOCTL_SETIDENT: user name, UIC and authorized mask)";;
        targets)      echo "kernel-core/vms_proctab.c";;
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
        # blind set below. See knock_on_why for what it reddens; its declared
        # set grew from three to eleven when #182 (vms-86a) made the suite
        # ignore SIGPIPE and run to completion instead of dying rc=141.
        #
        # All three (showterm, ident, lock_status) arrived on separate
        # branches; this list is the UNION, re-derived by running the control
        # on the merged tree rather than kept from one side of the merge.
        #
        # test_syssvc_setname is the NINTH, added by vms-fbe round 4, and it
        # arrived the way every one above it did -- NOT PREDICTED, READ OFF A
        # RUN of this control. Rounds 1-3 of that item verified
        # proctab-duplicate-name exactly and never ran this control at all, so
        # CI's attribution job was the first thing to execute it against the
        # new suite: it reported test_syssvc_setname(rc=141) as a suite OUTSIDE
        # this facility plus three unnamed assertions. BOTH halves of that were
        # this suite's own defects, and both are fixed rather than declared
        # away -- see knock_on_why. It does not hand-register (it opens
        # /dev/vms only to decide skip-vs-run, then drives the real DCL.EXE),
        # so it is a genuine detector of the same missing bind, not a widening
        # of blind_suites.
        # test_syssvc_authorize is the TENTH, added by vms-4c2, and it arrived
        # the way every one above it did -- NOT PREDICTED, READ OFF A RUN. It
        # was written to prove AUTHORIZE's SYSPRV check has a positive
        # control (a real /dev/vms, not the dev-host-only refusal PR #76
        # left unproven), and its helper stamps an identity with a BARE
        # vms_kif_setident() -- no vms_kif_register() first, the exact
        # counter-example shape test_syssvc_procnam/showdev/showproc/
        # lock_status establish -- so it was never a blind_suites candidate.
        # The first per-facility CI run after it merged reddened it here:
        # one suite outside the declared set and seven assertions outside
        # the named set.
        # test_syssvc_setuai is the ELEVENTH, added by vms-cb5, and it arrived
        # the way every one above it did -- NOT PREDICTED, READ OFF A RUN --
        # except that the run in question did not happen until rd vms-570
        # (the aggregate-verification rework): vms-cb5 added the suite and
        # this facility's committed execution record was never regenerated
        # against it, so the gap sat undetected until the first REAL full
        # driver run against the merged tree (2026-08-07) reddened it here --
        # one suite outside the declared set and eight assertions outside the
        # named set, all in test_syssvc_setuai. It does not hand-register (it
        # opens /dev/vms only to decide skip-vs-run, then stamps identity
        # through the public sys$ API, same counter-example shape as
        # procnam/showdev/showproc/lock_status/authorize), so it is a genuine
        # detector of the same missing bind, not a widening of blind_suites.
        # test_syssvc_ast is the TWELFTH, added by #170 (vms-as1), which
        # rewrote src/libvms/syssvc/sys_ast.c into a pure translation layer
        # over /dev/vms: sys$setast now reaches the executive through the
        # same kif_bind() -> vms_kif_register() auto-bind path as the suites
        # above, so this defect reaches it too. #170 added it to
        # ast-setast-disable's suites_red but missed this one -- same
        # counter-example shape as procnam/showdev/showproc/lock_status/
        # authorize/setuai (it opens /dev/vms only to decide skip-vs-run,
        # then drives the public sys$ API), so it is a genuine detector, not
        # a widening of blind_suites.
        suites_red)   echo "test_kmod_bind test_syssvc_procnam test_syssvc_showproc test_syssvc_ef_mproc test_syssvc_ef_local test_syssvc_showdev test_syssvc_startup_service test_syssvc_showterm test_syssvc_ident test_syssvc_lock_status test_syssvc_setname test_syssvc_authorize test_syssvc_setuai test_syssvc_ast test_syssvc_getdvi";;
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
parent: sys$ascefc joined the cluster the WFLOR/WFLAND measurement uses
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
parent: sys$setef sets the flag $WFLOR will find already satisfied
parent: sys$waitfr did NOT return until the flag was really set -- an interrupted wait is re-entered, never reported as SS$_NORMAL over a clear flag
parent: sys$wflor returned with only ONE of the two mask flags set -- OR, not AND
parent: the waiter was interrupted by a signal repeatedly WHILE blocked in sys$waitfr (the condition under test is reachable, not hypothetical)
parent: wfland child never reported it was ready to block
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
A: F$GETJPI("","USERNAME") returns the name the EXECUTIVE holds -- the programmatic path reads the same source the display does
A: SHOW PROCESS does NOT report the user name planted in VMS_USERNAME
A: SHOW PROCESS reports the UIC the EXECUTIVE holds
A: SHOW PROCESS reports the user name the EXECUTIVE holds
A: the authorized-privileges AND process-privileges blocks are both EMPTY -- none of A's granted mask (TMPMBX|NETMBX|OPER) is in VMS_PRV_M_ENFORCED
A: the executive accepted the identity a privileged writer established
B: F$GETJPI returns B's name -- two processes with an IDENTICAL environment get DIFFERENT answers, so the answer is not the environment
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
F: F$GETJPI CURPRIV renders SYSTEM/ALL's actual enforced privilege names (CMKRNL,CMEXEC,SYSNAM,GRPNAM,SETPRV,WORLD,MOUNT), not merely completes without rendering anything
G: the session established an authenticated identity
G: the executive HOLDS that name and reads it back -- so the subprocess's blank below is not the executive naming nobody
G/OPCOM+: the named run established its identity through the executive (without this the header check below is about a process that is also unnamed)
G/OPCOM+: EVERY header names SHIPPING -- the executive's row DOES reach sys$sndopr's user field, so the empty field above is this process being unnamed and not the field being dead
parent: child took EX before the CVTUNGRANT probe (setup, not the property under test)
parent: sys$enq CR queues behind the child's EX and still returns a real lock ID (public API)
sys$deq on an unknown lock ID reports SS$_IVLOCKID (public API, real executive)
child: EX granted on SYSSVC_STATUS_DLY (public API)
child: blocked EX on SYSSVC_STATUS_DLX granted after parent releases X (public API)
parent: EX granted on SYSSVC_STATUS_DLX (public API)
parent: child (CVTUNGRANT holder) exited clean
parent: child (deadlock) exited clean
parent: child's report after the deadlock/release sequence has no failures
parent: released X (should unblock the child)
parent: sync sys$enqw closing the cycle rejected SS$_DEADLOCK (public API)
the executive's process table row for the holder became named
a SECOND DCL.EXE's SHOW SYSTEM named the holder, which it did not create
SET PROCESS/NAME for a name already held refuses with the oracle-pinned two-line %SET-E-NOTSET / -SYSTEM-F-DUPLNAM shape
the executive's process table row for the second holder became named
the 15-char boundary-legal name (VMS_PRCNAM_SIZE-1) was accepted
the 16-char oversized name is refused with the oracle's two-line %SET-E-NOTSET / -SYSTEM-F-IVLOGNAM shape, now that upname is sized VMS_PRCNAM_XFER and reaches the executive intact
after the refused rename, SHOW SYSTEM still names this process with its PRE-EXISTING boundary name -- left UNCHANGED by the refusal, exactly as the oracle transcript shows
A: the executive accepted the SYSPRV identity
A: the executive's own row holds exactly the SYSPRV mask this run stamped -- not a claim, a readback
A: AUTHORIZE printed its version banner -- the SYSPRV holder was ADMITTED
A: the SYSPRV holder was never refused
A: AUTHORIZE exited 0 for the admitted session
B: the executive accepted the non-SYSPRV identity
B: the executive's own row holds exactly the OPER|WORLD mask this run stamped, with SYSPRV genuinely absent
1: the executive's row for it does not hold SYSPRV
2: the executive accepted the non-SYSPRV identity
2: the executive's row holds exactly the OPER|WORLD mask, no SYSPRV
3: the executive's row for it still does not hold SYSPRV
4: $SETUAI serves a SYSPRV identity -- the refusals are not blanket
4: the executive accepted the SYSPRV identity
4: the executive's row for it holds SYSPRV
4: the parent process reads the new default directory in the file
sys$dclast reported success declaring an AST
the AST declared through the PUBLIC sys$dclast is in the EXECUTIVE queue (a per-process fake would be invisible to the kernel reader)
the executive round-tripped the astprm the public sys$dclast declared
the executive round-tripped the astadr the public sys$dclast declared
the executive round-tripped the access mode the public sys$dclast declared
sys$dclast queued an AST while delivery was disabled
sys$setast(1) delivered the queued AST through the public API
the delivered AST ran with the astprm sys$dclast declared
disable again: prev state was disabled
SETAST(enable) returns WASCLR (== prev state was disabled)
device scan lists the console terminal
device scan terminates with SS$_NOMOREDEV
this process can still read the device
the console is not allocated before any process allocates it
B sees that A allocated the device
the allocation is gone once the other process exits (not a constant)
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

FOUR MORE IN test_syssvc_ef_mproc, ADDED vms-2ed -- SAME MISSING BIND, READ
OFF A RUN OF THIS CONTROL AGAINST THE NEW $WFLOR/$WFLAND SCENARIO, NOT
PREDICTED. That scenario is a THIRD process pair in the same suite (the
first two are the interrupted-wait pair above and the plain shared-flag
pair before it), so it needs its own $ASCEFC/$SETEF to stand the cluster up
before $WFLOR/$WFLAND can be exercised at all -- an unbound parent fails
that setup exactly as it fails the other two pairs' setup, and the two
assertions that read the setup back ("parent: sys$ascefc joined the cluster
the WFLOR/WFLAND measurement uses", "parent: sys$setef sets the flag
$WFLOR will find already satisfied") go red for the same reason
test_syssvc_ef_mproc's other require_fail/knock_on_fail entries do. The
remaining two are the scenario's own two properties failing to be reached
at all: "parent: sys$wflor returned with only ONE of the two mask flags
set -- OR, not AND" never gets a real $WFLOR call to observe (the setup
that would satisfy its predicate never ran), and "parent: wfland child
never reported it was ready to block" is the forked WFLAND child never
reaching its own readiness marker, one layer down, same shape as suite 3's
forked child above. None of this is the eflag-wflor-status-wrong or
eflag-wfland-status-wrong DEFECT itself -- those mutate vms_ioctl_wflor/
wfland's own status word and are unreachable from an unbound process in
the first place, which is exactly why bind-client-no-register's own red
set is these four setup/reachability assertions and not those two.

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
FOUR MORE ADDED (vms-cb5/vms-f39/vms-f42d) -- READ OFF THE DRIVER'S OWN
"does NOT name them" LIST, TWICE, WHICH IS THE POINT OF THIS PARAGRAPH.
  - Two are scenario G's preconditions: "G: the session established an
    authenticated identity" is a vms_kif_setident() through the deleted
    register step (the same property as the require_fail entry, one scenario
    further on), and "G: the executive HOLDS that name and reads it back" is
    the vms_kif_getjpi_self() that reads it back.
  - Two are OLDER than scenario G and were red on this branch before it:
    scenario A's and scenario B's F$GETJPI assertions, added when this suite
    gained programmatic-reader coverage. They are the same startup read
    failing, seen through the lexical function instead of through SHOW
    PROCESS, and they had simply never been declared -- this control is
    expensive, so nothing had executed it since that coverage landed.
THE MISTAKE WORTH RECORDING: the first re-run named all four and only two were
added, because the extra reds were INFERRED from a scrolled log rather than
read off the driver's explicit list. The second run named the remaining two
and refused to pass, which is exactly what the equality check is for.
TWO MORE, AND THE SAME MISTAKE A THIRD TIME (vms-cb5 r4). Scenario G grew an
OPCOM+ half -- the same command sequence re-run in a process the executive HAS
named, which exists to prove sys$sndopr's user field is populated at all and
not merely always-empty -- and its two executive-dependent assertions were not
declared here. CI's attribution job read them off its own "does NOT name them"
list:
  - "G/OPCOM+: the named run established its identity through the executive" is
    a vms_kif_setident() through the deleted register step. It is the SAME
    property as scenario G's own precondition two lines above, and as the
    require_fail entry, observed once more in the scenario's second run.
  - "G/OPCOM+: EVERY header names SHIPPING ..." reads back, out of the operator
    log, the name that setident never recorded. It is a read of the write the
    line above failed to make -- the identical shape as every other pair in
    this manifest.
This is the third arrival of the same lesson in this one entry, which is why it
is written down rather than quietly appended: an assertion added to a suite
already in suites_red still has to be declared, because the equality check is
at PROPERTY granularity and the suite list is not what it compares.
WHAT STAYED GREEN IN THE OPCOM+ HALF, and it is what keeps this from being a
blunderbuss: the other TWO assertions of the same block pass. "the named
process's REPLY and LOGOUT records reached the operator log" stays green
because writing the record is OVMX-LOCAL -- sys$sndopr appends the line itself,
so an unbound process still logs, and only the user FIELD goes empty. "and it
is not SYSTEM" stays green because the empty field is empty, not fabricated --
the vms-f42d property this round landed is untouched by the missing bind. So
the mutation reddens exactly the two assertions that depend on the executive
and neither of the two that do not, which is the attribution this control
exists to make.
WHAT STAYED GREEN, and it matters: scenario G's assertions about SUBMIT,
PRINT, ACCOUNTING, REPLY, LOGOUT and F$USER all PASS under this mutation --
their property is that DCL prints NO user name when the executive holds none,
and an unbound DCL holds none for a second reason. That is not a weakness of
those assertions; it is why each of them has its own minimal mutation, the
dcl-*-fabricated controls below.
THE EIGHTH SUITE, test_syssvc_lock_status, ADDED vms-2e5 -- READ OFF THE FIRST
FULL RUN OF THIS CONTROL AGAINST THE TREE THAT ADDED IT, not predicted. Its
bootstrap() does not hand-register (see the comment at its definition in
tests/qemu/test_syssvc_lock_status.c) -- it opens /dev/vms only to decide
skip-vs-run, exactly test_syssvc_procnam/showdev's shape, so it is a genuine
new detector of the SAME missing bind, not a widening of blind_suites.
ELEVEN assertions go red, and THE DECLARED SET GREW FROM THREE TO ELEVEN
BECAUSE THE OBSERVATION GOT BETTER, NOT BECAUSE THE DEFECT CHANGED -- the same
shape recorded for test_syssvc_showterm and test_syssvc_setname above. On the
run that first added this suite (vms-2e5), the process DIED BY SIGNAL (rc=141,
SIGPIPE) after only three reds: its writer/child failed the missing bind,
exited, and the parent's next write to a now-readerless pipe raised the default
SIGPIPE, so the whole back half of the suite never ran and could not be
observed. #182 (vms-86a) taught test_syssvc_lock_status.c to ignore SIGPIPE so
a dead IPC peer becomes a NAMED FAIL instead of rc=141 -- exactly the remedy
test_syssvc_showterm and test_syssvc_setname record above -- so the suite now
runs to completion under this mutation and the eight further reds it always
produced become visible and nameable. READ OFF THE DRIVER'S OWN "does NOT name
them" LIST (shard 0/6 of the d2d76fc..87d7fda main-red run), NOT PREDICTED.
The three original reds, still red, and traced not guessed:
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
     own sys$enqw(EX) fails, so "parent: child took EX before the CVTUNGRANT
     probe" reddens (this assertion is explicitly labelled setup-not-property
     in its own text for exactly this reason: a registration failure trips the
     SETUP check, not the CVTUNGRANT mapping the scenario exists to probe).
  3. The parent's own sys$enq(CR) in the same scenario also fails to register,
     so lksb_q.lksb$l_lkid stays 0 and "parent: sys$enq CR queues behind the
     child's EX and still returns a real lock ID" reddens too. Because that
     CHECK's condition is false, the `if ((st & 1) && lkid != 0)` guard around
     the CONVERT probe never executes, so SS$_CVTUNGRANT itself is never
     asked about under THIS mutation -- consistent with vms-2e5's own point:
     the registration wall is upstream of the mapping this suite exists to
     assert, so a registration defect masks the mapping property rather than
     exercising it.
THE EIGHT NOW-VISIBLE reds, every one the same unbound process failing every
lock op it attempts, once SIGPIPE no longer truncates the suite:
  - "parent: child (CVTUNGRANT holder) exited clean" -- the very check step 2
     above noted "never runs" under the old rc=141 crash; it runs now, and the
     child (which failed its setup $ENQ) did NOT exit clean, so it reddens.
  - scenario_deadlock's own setup grants, "child: EX granted on
     SYSSVC_STATUS_DLY (public API)", "parent: EX granted on SYSSVC_STATUS_DLX
     (public API)" and "child: blocked EX on SYSSVC_STATUS_DLX granted after
     parent releases X (public API)", are uncontended $ENQ/$ENQW from an
     unbound process -- they never get their lock, so each reddens.
  - "parent: released X (should unblock the child)" and "parent: sync sys$enqw
     closing the cycle rejected SS$_DEADLOCK (public API)" are the deadlock
     sequence's own release and cycle-closing steps failing the same wall.
  - "parent: child (deadlock) exited clean" and "parent: child's report after
     the deadlock/release sequence has no failures" are the parent reading
     back that the forked child DID fail -- the same one-layer-down shape as
     suite 3's forked-child reds above.
None of these is a new defect: they are the whole of scenario_deadlock and the
tail of scenario_cvtungrant reaching the identical missing-bind wall the three
original reds hit, now that the suite survives long enough to assert about it.

THE NINTH SUITE, test_syssvc_setname, ADDED vms-fbe ROUND 4 -- and it arrived
as TWO defects in the suite itself, not as a discovery about this mutation.
Rounds 1-3 of that item verified proctab-duplicate-name exactly and NEVER RAN
THIS CONTROL, so CI's attribution job executed it first and reported:
  FAIL: suites OUTSIDE this facility failed: test_syssvc_setname(rc=141)
  FAIL: these assertions went red and the manifest does NOT name them: (three)
Both halves were fixed rather than declared away, and the order matters:

  1. rc=141 WAS SIGPIPE IN THE SUITE, NOT A PROPERTY OF THIS DEFECT. The suite
     forks a DCL.EXE and writes a script into a pipe it reads from. With the
     bind deleted, DCL.EXE cannot register at startup and exits at once, so the
     write landed on a pipe with no reader and the DEFAULT SIGPIPE disposition
     killed the suite. Its verdict became a signal number, which attributes
     nothing -- the identical failure, under the identical control, that
     test_syssvc_showterm hit and recorded above. The identical remedy was
     applied (signal(SIGPIPE, SIG_IGN) in main(), see that file's comment), and
     the identical thing happened next: THE DECLARED SET GREW BECAUSE THE
     OBSERVATION GOT BETTER. The suite now runs to completion under this
     mutation -- 8 passed, 7 failed, rc=1 -- and the three reds CI could see
     before it died are seven. Nothing about the defect changed; the truncated
     observation did.

  2. THE SEVEN ARE ALL ONE MISSING BIND, REACHING THE COMMAND LAYER. Every
     assertion in this suite is about what SET PROCESS/NAME put in the
     executive's process table, and DCL.EXE reaches that table only through
     kif_bind(). With the register call deleted, vms_kif_setprn() is rejected
     as an unregistered task, so no name is ever recorded and nothing
     downstream can read one back: the two "process table row ... became named"
     assertions are the write failing, "a SECOND DCL.EXE's SHOW SYSTEM named
     the holder" and the P6 "left UNCHANGED by the refusal" assertion are reads
     of a name that was never written, and the DUPLNAM, IVLOGNAM and 15-char
     assertions are refusals-and-acceptances the executive was never asked to
     make. The suite's own output shows the mechanism directly rather than
     leaving it to be argued: SET PROCESS/NAME prints
     "%OVMX-E-SETPRNFAIL, SET PROCESS/NAME could not reach the executive
     (status %X000002A4)" -- the honest unreachable-executive report from
     src/vmsdcl/dcl_cmd_set.c, the same one the executive-absent negative
     control observes, arriving here because an unbound process is as unable to
     reach the executive as an absent one.
     What keeps this from being a blunderbuss: EIGHT assertions in the suite
     stay GREEN, including "the truncated form of the oversized name never
     appears anywhere in the output" -- the round-1 defect this suite was
     written to catch is still correctly absent under an unrelated mutation --
     and all five "a DCL.EXE ran ..." harness checks, which prove the suite is
     still running the command rather than failing to start it.
     "SET PROCESS/NAME for a name already held refuses ... DUPLNAM shape" is
     named by proctab-duplicate-name's knock_on_fail as well. That is the same
     two-defects-one-assertion-text case the lock_status paragraph above
     records, not a collision: a clash test that is short-circuited and a clash
     test that is never reached both leave the refusal unprinted.
     DECLARED, NOT NARROWED, for the reason the showproc paragraph gives: the
     whole value of this suite is that it drives the real DCL.EXE end to end,
     so it SHOULD be sensitive to the whole stack beneath the command.

EIGHT MORE IN test_syssvc_setuai, THE ELEVENTH SUITE -- SAME MISSING BIND,
READ OFF THE FIRST REAL FULL DRIVER RUN AGAINST THE MERGED TREE (rd vms-570,
2026-08-07), NOT PREDICTED. test_syssvc_setuai.c exists to prove $SETUAI's
SYSPRV test is the EXECUTIVE's, not the caller's own PCB mask (vms-cb5): its
probe stamps an identity, calls $SETUAI, then reads the identity's row back
through the SAME executive path every other suite here depends on. With the
bind deleted the stamped identity never lands in a row the executive holds,
so every "the executive's row for it ..." and "the executive accepted ..."
readback in scenarios 1-4 observes an unbound caller instead of the identity
the test just stamped -- the identical shape as authorize's "A:"/"B:" reds
one suite up, on the same $SETUAI/$GETUAI row. "4: the parent process reads
the new default directory in the file" is scenario 4's own downstream check
(the SYSPRV-served write actually landed), one layer further from the same
unbound row. Two of scenario 3's own assertions stay GREEN ("1: the probe
genuinely had NO PCB" and "3: the probe's OWN PCB really does carry SYSPRV")
because they read the PROBE's own memory, not the executive's row -- the
same distinction that keeps this suite meaningful rather than a duplicate of
the bind check itself.
TEN MORE IN test_syssvc_ast, THE TWELFTH SUITE -- SAME MISSING BIND, READ OFF
THE DRIVER'S OWN "does NOT name them" LIST, NOT PREDICTED. #170 (vms-as1)
rewrote src/libvms/syssvc/sys_ast.c into a pure translation layer over
/dev/vms: sys$dclast and sys$setast now reach the executive through the SAME
kif_bind() -> vms_kif_register() auto-bind path every public-API suite here
depends on, so deleting that register call takes the AST facility away from an
unbound process exactly as it takes $SETEF/$GETJPI/$ENQ away. test_syssvc_ast.c
does not hand-register (it opens /dev/vms only to decide skip-vs-run, then
drives the public sys$ API), so it is a genuine new detector of this defect,
not a widening of blind_suites. #170 correctly added it to ast-setast-disable's
suites_red for its OWN minimal mutation, and #176 added it to THIS defect's
suites_red so the suite is permitted to redden -- but neither declared the
assertion texts, so the equality check (which is at PROPERTY granularity, not
suite granularity) kept failing on an unnamed red set. The ten reds split into
two groups, none a separate property:
  - PART 1/2, executive residency and end-to-end delivery (eight): "sys$dclast
    reported success declaring an AST" is the wall itself -- an unbound process
    cannot reach the executive, so the very first declare fails. Every other
    assertion in these parts reads back what that failed declare never put in
    the executive queue: the residency readback and the three astprm/astadr/
    acmode round-trips are the AST that is not there, "sys$dclast queued an AST
    while delivery was disabled" is the same declare failing a second time, and
    "sys$setast(1) delivered the queued AST" / "the delivered AST ran with the
    astprm" read back a delivery of an AST that was never queued.
  - PART 3, the SETAST previous-state return (two): "disable again: prev state
    was disabled" and "SETAST(enable) returns WASCLR" are sys$setast reaching
    the executive from the same unbound process -- the prev-state word comes
    back wrong because the call never lands. (These are the ast-setast-disable
    property observed a second time from a different angle: that defect mutates
    the kernel's own prev-state word, this one denies the call a PCB at all.)
WHAT STAYS GREEN, and it keeps this from being a blunderbuss: "the AST is NOT
delivered while AST delivery is disabled" passes, because an AST that was never
queued is trivially not delivered (g_fired stays 0) -- so the mutation reddens
the properties that depend on the bind and leaves the one that does not.
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

    delprc-privcheck-bypassed)
        case "$_f" in
        facility)     echo "process deletion -- sys\$delprc's own GROUP/WORLD privilege gate for a process target (the DCL STOP command's process-target forms, src/libvms/syssvc/sys_process.c)";;
        targets)      echo "libvms/syssvc/sys_process.c";;
        # PRODUCT-half defect, the same class as run-detached-name-dropped and
        # creprc-handshake-eintr above: the property lives in sys$delprc
        # (libvms), NOT in vms.ko, so no kernel mutation can reach it.
        # sys$delprc RESOLVES and AUTHORISES the target THROUGH the executive
        # (vms_kif_getjpi_prcnam/_pid), then applies the DCL Dictionary's
        # GROUP/WORLD rule ITSELF before the terminating kill(). This mutation
        # makes that refusal structurally unreachable -- the always-false
        # `if (0 ...)` idiom access-mode-escalation and dclast-super-mode-
        # escalation also use -- so a caller holding NEITHER GROUP nor WORLD is
        # no longer refused a same-group target: sys$delprc falls through to the
        # kill and returns SS$_NORMAL.
        #
        # MEASURED as isolated to the ONE SS$_NOPRIV assertion (P3), NOT the
        # sibling "still ALIVE" check in the same block: the target is the
        # suite's OWN $CREPRC child, so a helper that wrongly kills it only
        # turns it into a ZOMBIE of the main test process, and kill(zombie, 0)
        # still returns 0 -- the suite documents exactly this footgun in
        # wait_and_reap()'s comment. The "still ALIVE" assertion therefore
        # cannot observe the kill and stays green; only the SS$_NOPRIV status
        # the privilege-stripped helper reads back changes. P1/P2 (whose caller
        # holds WORLD, so `authorized` was already true) are untouched.
        suites_red)   echo "test_syssvc_delprc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sys\$delprc stops enforcing the DCL Dictionary's GROUP/WORLD rule for a process target: a caller holding NEITHER GROUP nor WORLD is allowed to delete a process in its own UIC group. Resolution, naming, the cross-process kill and the caller==target self-delete path are all untouched -- only the privilege refusal is made unreachable, so a same-group STOP that must be refused SS\$_NOPRIV is reported SS\$_NORMAL instead.";;
        require_fail) cat <<'EOF'
P3: sys$delprc refuses a same-group target without GROUP privilege (SS$_NOPRIV, DCL Dictionary STOP)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
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
        facility)     echo "the executive's SS__DEADLOCK condition value (src/kernel/vms_internal.h), the value the kernel lock manager yields to a caller it aborted for deadlock (vms-2e5, vms-82a)";;
        targets)      echo "kernel/vms_internal.h";;
        suites_red)   echo "test_kmod_lock_sync test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "SS__DEADLOCK is 2488 (SS\$_NOTQUEUED) instead of 3594 (SS\$_DEADLOCK) -- a request the executive rejected FOR DEADLOCK reports to the caller as merely 'not queued'. Since vms-82a the executive yields the VMS condition value itself, so this attacks the executive rather than a userspace mapping: the kernel's DECISION to abort for deadlock is untouched, only the value it answers with changes.";;
        require_fail) cat <<'EOF'
parent: sync sys$enqw closing the cycle rejected SS$_DEADLOCK (public API)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: sync ENQ closing the cycle rejected SS$_DEADLOCK
parent: child (completion AST) exited clean
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
THE SAME DEFECT SEEN FROM THE OTHER SIDE, and its appearance here is a
consequence of vms-82a rather than a coarse mutation.

Before vms-82a this control mutated kstat_to_ss() in src/libvms/syssvc/
sys_lock.c -- a userspace mapping only the PUBLIC sys$ path went through. So
only test_syssvc_lock_status could see it, and test_kmod_lock_sync, which
reaches the lock manager through raw ioctls, could not.

The executive now yields the VMS condition value itself, so there is exactly
ONE place the deadlock status exists and BOTH paths read it. test_kmod_lock_sync
asserts the raw status is SS$_DEADLOCK; test_syssvc_lock_status asserts the
public API reports SS$_DEADLOCK. One mutated constant, two observations of the
one defect -- which is the thing the change was for.

The second assertion follows from the first in that suite: the parent decides
the child exited clean by checking the deadlock status it got, so once the
status is wrong the parent's own verdict on the child goes with it. There is
no finer mutation available -- the constant is a single #define, and splitting
it would mean inventing a second deadlock value the executive does not have
(the illegal third answer, Rule 10).
EOF
                      ;;
        esac;;

    kstat-ivlockid-mismapped)
        case "$_f" in
        facility)     echo "the executive's SS__IVLOCKID condition value (src/kernel/vms_internal.h), the value the kernel lock manager yields for a lock ID that does not exist (vms-2e5, vms-82a)";;
        targets)      echo "kernel/vms_internal.h";;
        suites_red)   echo "test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "SS__IVLOCKID is 2488 (SS\$_NOTQUEUED) instead of 8484 (SS\$_IVLOCKID) -- a caller given a nonexistent lock ID is told the request was merely not queued rather than that the ID itself is invalid.";;
        require_fail) cat <<'EOF'
sys$deq on an unknown lock ID reports SS$_IVLOCKID (public API, real executive)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    kstat-cvtungrant-mismapped)
        case "$_f" in
        facility)     echo "the executive's SS__CANCELGRANT condition value (src/kernel/vms_internal.h), the value the kernel lock manager yields for a queued conversion it could not grant (vms-2e5, vms-82a)";;
        targets)      echo "kernel/vms_internal.h";;
        suites_red)   echo "test_syssvc_lock_status";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "SS__CANCELGRANT is 2488 (SS\$_NOTQUEUED) instead of 8508 (SS\$_CVTUNGRANT) -- an ungrantable conversion is reported as a plain 'not queued'.";;
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

    # -----------------------------------------------------------------------
    # THE SIX USER-NAME FABRICATIONS (vms-cb5 / vms-f39 / vms-f42d)
    #
    # These are NOT executive facilities. They are the READER half of one
    # (CLAUDE.md Rule 11's corollary: a user-visible VMS command is a reader of
    # an executive facility, never a thing that fabricates its own answer), and
    # each restores ONE deleted fallback verbatim:
    #
    #     const char *user = ctx->username[0] ? ctx->username : "SYSTEM";
    #
    # ctx->username is seeded in src/vmsdcl/dcl_main.c from the executive's
    # process table and from nowhere else, so the `else` branch is taken
    # exactly when the executive holds no name for this process -- the state
    # any SPAWNed subprocess is in (vms_proc_register() zeroes the username of
    # every new task and inherits nothing from the parent). Restored, the
    # command names that process after the most privileged account on the
    # system. Each defect is one line in one command, so each names exactly
    # its own site's assertions.
    # -----------------------------------------------------------------------
    dcl-submit-owner-fabricated)
        case "$_f" in
        facility)     echo "SUBMIT's job OWNER, on the way from the executive's process table to the queue entry (src/vmsdcl/dcl_cmd_process.c cmd_submit)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "SUBMIT goes back to writing the literal \"SYSTEM\" into the queue entry for any process the executive has not named. The submission still succeeds, the entry still exists, SHOW QUEUE still lists it -- only its OWNER is a fabrication, and it is a fabrication that degrades UPWARD: the batch job of a process that holds no identity at all is recorded as belonging to the system account.";;
        require_fail) cat <<'EOF'
G/SUBMIT: SHOW QUEUE shows the batch job with an EMPTY owner
G/SUBMIT: the batch job is NOT owned by SYSTEM
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-print-owner-fabricated)
        case "$_f" in
        facility)     echo "PRINT's job OWNER, the same executive-to-queue-entry path SUBMIT uses (src/vmsdcl/dcl_cmd_process.c cmd_print)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "PRINT goes back to writing the literal \"SYSTEM\" as the print job's owner. It is a SEPARATE control from SUBMIT's, in the same file and the same shape, because they are separate call sites: fixing one and leaving the other is exactly how five of these six survived the round that deleted the sixth.";;
        require_fail) cat <<'EOF'
G/PRINT: SHOW QUEUE shows the print job with an EMPTY owner, in cmd_show_queue's own column format
G/PRINT: the print job is NOT owned by SYSTEM
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-logout-user-fabricated)
        case "$_f" in
        facility)     echo "the user name in LOGOUT's console line and its OPCOM record (src/vmsdcl/dcl_cmd_process.c cmd_logout)";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "LOGOUT goes back to announcing \"SYSTEM logged out\" -- on the console AND in the operator log -- for any process the executive has not named. An operator log that records a nameless process as SYSTEM is worse than one that records it as nothing: it is a false audit record, not a missing one.";;
        require_fail) cat <<'EOF'
G/LOGOUT: the logout line names no user, in cmd_logout's own "  %s      logged out at" format
G/LOGOUT: the session is not logged out as SYSTEM
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-reply-operator-fabricated)
        case "$_f" in
        facility)     echo "the operator name REPLY/ENABLE reports and logs (src/vmsdcl/dcl_cmd_misc.c cmd_reply)";;
        targets)      echo "vmsdcl/dcl_cmd_misc.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "REPLY/ENABLE goes back to naming the enabling operator \"SYSTEM\" when the executive holds no name for the process -- in the %OPCOM-I-OPRENA message and in the OPC record it sends to OPERATOR.LOG.";;
        require_fail) cat <<'EOF'
G/REPLY: the OPCOM enable message names no operator
G/REPLY: the console message does not name SYSTEM as the operator
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-accounting-user-fabricated)
        case "$_f" in
        facility)     echo "the account ACCOUNTING reports on, which also selects the last-login record it reads (src/vmsdcl/dcl_cmd_misc.c cmd_accounting)";;
        targets)      echo "vmsdcl/dcl_cmd_misc.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "ACCOUNTING goes back to reporting on \"SYSTEM\" for an unnamed process. This one is not only a wrong label: the same string picks the FILE the login history is read from (ovmx_accounting_get_lastlogin), so an unnamed process is shown SYSTEM's login history under SYSTEM's name.";;
        require_fail) cat <<'EOF'
G/ACCOUNTING: names no account for an unnamed process
G/ACCOUNTING: does not report SYSTEM's login history to an unnamed process
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-fuser-system-fabricated)
        case "$_f" in
        facility)     echo "F\$USER() / F\$GETJPI(\"\",\"USERNAME\"), the PROGRAMMATIC reader of the executive's user name (src/vmsdcl/dcl_lexical.c lex_user) -- its literal-SYSTEM branch";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "lex_user() goes back to answering the literal \"SYSTEM\" when the executive holds no name -- the vms-cb5 defect verbatim, found because SHOW PROCESS (a display) and F\$GETJPI (a lexical function) read the SAME field of the SAME row and disagreed: the display printed nothing and the programmatic path invented the most privileged name on the system for the one process that had just been REFUSED it.";;
        require_fail) cat <<'EOF'
G/F$USER: reports NO name for a process the executive has not named -- not the host Linux login name, not SYSTEM
G/F$USER: does not answer with the literal SYSTEM
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
C: F$GETJPI("","USERNAME") reports NO name for a process the executive refused to name -- it does not fall back to a Linux account name or to SYSTEM
C: SHOW PROCESS does NOT report SYSTEM for a process that only claimed it -- through the ioctl AND through VMS_USERNAME
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
Scenario C is the SAME function answering the same question one scenario
earlier, and it is where this defect was originally measured: an unprivileged
process whose bid to become SYSTEM the executive had just refused with
SS$_NOPRIV. Its two reds are one property seen twice -- the first is C's own
direct check on lex_user's answer, and the second is C's blanket "SYSTEM
appears nowhere in this scenario's output", which the first one's fabricated
value necessarily also trips. There is nothing finer available: the mutation
is already a single branch of a single function, and C and G both exist
deliberately, because C exercises the EXECUTIVE-REFUSED unnamed process and G
exercises the NEVER-NAMED subprocess -- different routes into the one state.
EOF
                      ;;
        esac;;

    dcl-fuser-host-login-name)
        case "$_f" in
        facility)     echo "F\$USER() / F\$GETJPI(\"\",\"USERNAME\") (src/vmsdcl/dcl_lexical.c lex_user) -- its getpwuid(getuid()) branch, the HOST Linux account name";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "lex_user() goes back to answering with the HOST Linux account name for getuid(), upcased -- the vms-f39 defect verbatim, where F\$USER() answered \"BARON\" because that was the developer's login. A Linux account name is not a VMS user name, and this is the branch that would be taken on any system that HAS an /etc/passwd. It is a SEPARATE control from dcl-fuser-system-fabricated because it is a separate branch that was reachable in a separate population of systems: restoring only the SYSTEM half leaves this one deleted and vice versa.";;
        require_fail) cat <<'EOF'
G/F$USER: reports NO name for a process the executive has not named -- not the host Linux login name, not SYSTEM
G/F$USER: DCL does NOT answer with the Linux account name, upcased or otherwise -- the vms-f39 defect exactly
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    # -----------------------------------------------------------------------
    # THE THREE HOST-IDENTITY LEAKS THE PER-SITE ROUND MISSED (vms-cb5 round 3,
    # vms-f39).
    #
    # The two controls above cover lex_user(). They were written, and the class
    # was declared settled, while the SAME host-passwd derivation was still
    # live in lex_identifier() 1840 lines below it IN THE SAME FILE, and in
    # sys$sndopr's OPCOM header in a different library. Neither was found by
    # reading the five call sites the round was handed; both were found by
    # running the product:
    #
    #   printf 'X = F$IDENTIFIER(1000,"NUMBER_TO_NAME")\nSHOW SYMBOL X\n' \
    #       | ./build/bin/DCL.EXE                    ->  X = "BARON"
    #   printf 'LOGOUT\n' | ./build/bin/DCL.EXE  then read the operator log
    #       ->  %%OPCOM, ..., request 1 from user baron on node OVMX
    #
    # So these three exist to make the CLASS falsifiable rather than the site:
    # one control per host derivation, per direction, per function, each naming
    # only its own assertions.
    # -----------------------------------------------------------------------
    dcl-fident-num2name-host-passwd)
        case "$_f" in
        facility)     echo "F\$IDENTIFIER(n,\"NUMBER_TO_NAME\"), the UIC-to-identifier conversion (src/vmsdcl/dcl_lexical.c lex_identifier) -- its getpwuid(member) branch";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "F\$IDENTIFIER goes back to resolving a VMS UIC through the HOST passwd database and answering with the Linux account name, upcased -- measured as X = \"BARON\" on the build host. On VMS this conversion is a RIGHTSLIST lookup, and a Linux account is not a VMS rights identifier. It is a SEPARATE control from the two lex_user() ones because it is a separate FUNCTION, which the round that fixed lex_user() left untouched while reporting the class settled.";;
        require_fail) cat <<'EOF'
G/F$IDENTIFIER: NUMBER_TO_NAME does NOT answer with the HOST Linux account name for that uid, upcased -- the vms-f39 defect exactly
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
G/F$IDENTIFIER: NUMBER_TO_NAME answers the NULL STRING for a UIC OVMX holds no identifier for -- what real VMS answers, for every input shape the oracle was asked
G/F$USER: DCL does NOT answer with the Linux account name, upcased or otherwise -- the vms-f39 defect exactly
EOF
                      ;;
        knock_on_why)  echo "BOTH MEASURED, not reasoned -- this control's red set was 3, not the 1 round 3 declared, and the second extra was there before round 4 touched anything. (1) The mutation restores the passwd lookup AT the miss, which is the only place it ever was, so the miss stops being the null string and becomes the account name: one behaviour seen from both ends, the require_fail line naming the host leak and this line the oracle-pinned check next to it failing on the same answer. (2) The F\$USER check is a WHOLE-OUTPUT scan for the Linux account name across everything DCL printed after G_SUB_PWNAM -- deliberately, so a leak in any command is caught, not just in lex_user(). F\$IDENTIFIER printing SHIPUSER puts that name in DCL's output, so the scan fires. That is the scan working: the name genuinely IS in the output, from a different function. Making it not fire would mean narrowing it to lex_user()'s line, which is exactly the per-site scoping that let this class survive three rounds.";;
        esac;;

    # THE REFUTED VALUE ITSELF, made falsifiable (vms-cb5 round 4, vms-2f8).
    # The control above restores a leak. This one restores a WRONG ANSWER that
    # leaks nothing: the bracketed UIC round 3 kept because it was "already
    # there". The oracle says real VMS answers the null string for every miss
    # shape tried and emits no bracketed UIC from F$IDENTIFIER at all, so
    # without this control the assertion that OVMX now answers "" would be
    # green against a build that echoes the caller's UIC back, and nothing in
    # the suite would notice.
    dcl-fident-num2name-bracketed-uic)
        case "$_f" in
        facility)     echo "F\$IDENTIFIER(n,\"NUMBER_TO_NAME\") (src/vmsdcl/dcl_lexical.c lex_identifier) -- the value it answers for a UIC no identifier matches";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "The miss goes back to rendering the caller's own UIC in brackets -- \"[210,11]\" for the UIC it was handed. REFUTED against OpenVMS VAX V7.3 on lab node vax3: F\$IDENTIFIER answers the NULL STRING for every miss shape asked (plain, general-identifier range, UIC-format with bit 31 set, and zero), and the public HP/VSI DCL Dictionary says the same. Real VMS never emits a bracketed UIC from this function, so the rendering was a plausible-looking answer to a condition VMS never answers that way -- Rule 10's illegal third answer. This control exists because that value was invented, not leaked: no host database is involved, so none of the passwd controls can see it.";;
        require_fail) cat <<'EOF'
G/F$IDENTIFIER: NUMBER_TO_NAME answers the NULL STRING for a UIC OVMX holds no identifier for -- what real VMS answers, for every input shape the oracle was asked
G/F$IDENTIFIER: NUMBER_TO_NAME does NOT echo the caller's UIC back in brackets -- real VMS emits no bracketed UIC from F$IDENTIFIER for any input, so that was Rule 10's illegal third answer
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    dcl-fident-name2num-host-passwd)
        case "$_f" in
        facility)     echo "F\$IDENTIFIER(name,\"NAME_TO_NUMBER\"), the identifier-to-UIC conversion (src/vmsdcl/dcl_lexical.c lex_identifier) -- its getpwnam() branch";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "The reverse direction goes back to building a VMS UIC out of a passwd entry's uid and gid, so F\$IDENTIFIER(\"baron\",\"NAME_TO_NUMBER\") answers with the developer's Linux account expressed as a UIC. Separate from the NUMBER_TO_NAME control because it is a separate branch: deleting one leaves the other, which is exactly how this class kept surviving rounds that fixed it.";;
        require_fail) cat <<'EOF'
G/F$IDENTIFIER: NAME_TO_NUMBER does NOT build a UIC out of the host passwd entry's uid/gid for that account
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
G/F$IDENTIFIER: NAME_TO_NUMBER answers 0 for a name OVMX holds no identifier for, in SHOW SYMBOL's own integer format
EOF
                      ;;
        knock_on_why)  echo "MEASURED: this control's red set is 2, not the 1 round 3 declared, and it was 2 before round 4 touched anything -- the omission is round 3's, found by running the control rather than reasoning about it. The mutation restores the passwd lookup AT the miss, the only place it ever was, so the miss stops being 0 and becomes the UIC built from the passwd entry. The require_fail line names that fabricated UIC; this line is the oracle-pinned miss value failing on the same answer. One behaviour, both ends -- there is no finer mutation, because the deleted lookup and the miss value are the same branch.";;
        esac;;

    getjpi-pidarg-discarded)
        case "$_f" in
        facility)     echo "F\$GETJPI's pid argument -- the DCL lexical honoring it and reading the executive process row for the NAMED target (src/vmsdcl/dcl_lexical.c lex_getjpi), vms-9e2";;
        targets)      echo "vmsdcl/dcl_lexical.c";;
        # PRODUCT-half defect, the same class as delprc-privcheck-bypassed and
        # the dcl-f* family above: the property lives in the DCL lexical layer
        # (vmsdcl), NOT in vms.ko, so no kernel mutation can reach it.
        # lex_getjpi() RESOLVES its target through the executive
        # (vms_kif_getjpi_pid for a hex pid, vms_kif_getjpi_self for the null
        # "current process" form) and answers every item from THAT row. This
        # mutation forces the resolution down the self branch for EVERY call --
        # the always-true `if (1 ...)` idiom -- so the pid argument is parsed
        # and discarded and F$GETJPI answers about the CALLER again, exactly the
        # vms-9e2 facade (F$GETJPI("OTHERPROC","PID") -> the caller's PID).
        #
        # MEASURED to the FOUR pid-target identity assertions in the suite's
        # executive scenario, NOT the self-form or the liveness checks: with
        # the resolution self-pinned, F$GETJPI(<A's pid>, PRCNAM/USERNAME/PID)
        # returns the DCL caller B's own row instead of target A's, reddening
        # all three "== A's ..." checks plus the paired "did NOT report the
        # caller B's own identity" negative. The self form F$GETJPI("",...)
        # STILL reads the caller's own row (it always did), so
        # "SELF_USERNAME == B" stays green, and A is still alive when B reads
        # (the mutation changes only which row B's F$GETJPI resolves), so the
        # liveness check stays green too.
        suites_red)   echo "test_syssvc_getjpi_pidarg";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "lex_getjpi() stops honoring its pid argument: the target resolution is forced down the vms_kif_getjpi_self() branch for every call (the always-true \`if (1 ...)\` idiom), so a hex pid naming another process is parsed and discarded and F\$GETJPI answers PID/USERNAME/PRCNAM out of the CALLER's own executive row -- the vms-9e2 facade restored. Parsing, the null current-process form, the honest-failure path and every item's read-from-the-resolved-row are otherwise untouched; only WHICH row is resolved for a named pid changes, so a foreign-pid read that must return the target's identity returns the caller's instead.";;
        require_fail) cat <<'EOF'
F$GETJPI(<A pid>,"USERNAME") == A's username (not the caller's)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
F$GETJPI(<A pid>,"PRCNAM") == A's process name (not the caller's)
F$GETJPI(<A pid>,"PID") echoes A's pid (not the caller's)
F$GETJPI(<A pid>,...) did NOT simply report the caller B's own identity
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
All four are the SAME facade seen through different items: with the target
resolution self-pinned, B's F$GETJPI(<A's pid>, X) reads B's own executive row
for every identity item, so A's process name, username and pid each come back
as B's, and the paired negative (the answer IS the caller's identity) fires
with them. They are one property -- "the pid argument selects the row" -- read
off one mutation, not four independent regressions; the self-form and liveness
assertions in the same suite are untouched because the mutation changes only
which row a NAMED pid resolves to.
EOF
                      ;;
        esac;;

    opcom-header-host-login-name)
        case "$_f" in
        facility)     echo "the user field of sys\$sndopr's OPCOM header -- i.e. of every record OVMX writes to OPERATOR.LOG (src/libvms/syssvc/sys_operator.c get_current_username)";;
        targets)      echo "libvms/syssvc/sys_operator.c";;
        suites_red)   echo "test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sys\$sndopr goes back to naming the requester from getpwuid(getuid()) when the executive's row holds no name, so the operator log records the HOST Linux account as the VMS user who made the request -- measured as 'request 1 from user baron on node OVMX' after a plain LOGOUT. This is the AUDIT half of the class and it lives in a different library from the DCL sites, which is what a per-site round cannot reach: LOGOUT's own user name was fixed in dcl_cmd_process.c while this field, in the same record, stayed host-derived.";;
        require_fail) cat <<'EOF'
G/OPCOM: EVERY header's user field is empty for a process the executive has not named -- sys$sndopr reads the executive's row, not the caller's PCB and not the passwd database (goes RED, not vacuous, when vms-afd propagates identity to SPAWN)
G/OPCOM: the operator record does NOT name the HOST Linux account -- the vms-f39 leak that survived in sys_operator.c
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    setuai-sysprv-caller-declared)
        case "$_f" in
        facility)     echo "\$SETUAI's SYSPRV test -- the gate on the one service that rewrites SYSUAF.DAT (src/libvms/syssvc/sys_uai.c sys\$setuai)";;
        targets)      echo "libvms/syssvc/sys_uai.c";;
        suites_red)   echo "test_syssvc_setuai";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "The mask test goes away, which is the state \$SETUAI was in before vms-cb5 round 5 for every caller with no PCB: \`if (pcb && !(pcb->cur_privs & PRV\$M_SYSPRV))\` is false when vms_pcb_get() returns NULL, so the service rewrote SYSUAF.DAT -- UAI\$_PWD included -- with no privilege test having run. This control exists because the ONLY thing standing between an unauthorized caller and an account's password hash is that one comparison: delete it and the suite's three refusals become three grants, with the service still returning SS\$_NORMAL and still looking like an access control.";;
        require_fail) cat <<'EOF'
1: $SETUAI refuses a caller with no PCB (was: no test ran at all)
2: $SETUAI refuses an authenticated identity without SYSPRV
3: $SETUAI refuses it anyway -- the PCB mask does not decide
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
1-3: SYSUAF.DAT is byte-identical after all three refusals
EOF
                      ;;
        knock_on_why)  echo "Each of the three calls the mutation turns from a refusal into a grant WRITES -- they all carry the same UAI\$_DEFDIR item -- so the file-unchanged check is the same single defect observed in the artifact instead of in the returned status. It is listed rather than dropped because it is the assertion that shows the refusals were refusals and not merely a status the service returned after doing the work anyway.";;
        esac;;

    register-adopt-pid-not-reported)
        case "$_f" in
        facility)     echo "process registration / adoption (VMS_IOCTL_REGISTER, src/kernel/vms_module.c vms_ioctl_register)";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_setterm";;
        blind_suites) echo "test_kmod_bind";;
        blind_why)    echo "test_kmod_bind.c's own re-exec/adoption scenario calls vms_kif_register(NULL) -- the output pointer is NULL, so the adopted vms_pid this defect stops writing is never read back through REGISTER at all. That scenario's own identity checks (status, getjpi, prcnam, privs, uic, username) all go through a SEPARATE later vms_kif_getjpi_self() call, which reads proc->vms_pid straight out of the table -- a field this defect never touches, only the register ioctl's own reply copy of it. MEASURED: a run of this defect named only test_kmod_bind's assertion in require_fail and it came back green while an unnamed suite (test_kmod_setterm) went red instead -- the ignored-call trap this file's own header warns about, caught by running it rather than by re-reading the call site.";;
        isolation)    echo "isolated";;
        why)          echo "REGISTER on a process that already has an executive entry (the post-execve() adopt path, vms-9fc) stops copying the adopted process's own vms_pid into the reply. Status is still reported SS\$_NORMAL and the process is still genuinely adopted (the hash-table entry is unchanged) -- only the ANSWER the caller is given about which VMS process ID it now has is missing, the same facade shape as setterm-binding-not-recorded and devtab-owner-not-recorded: the operation happens, only the record of it going back to the caller does not. test_kmod_setterm.c's own re-exec scenario passes a non-NULL output pointer to vms_kif_register() on both the pre-exec (fresh, line 438, untouched) and post-exec (adopt, line 412, mutated) calls, and compares the two -- the one place in the suite set that reads REGISTER's own reply rather than re-deriving the pid through GETJPI.";;
        require_fail) cat <<'EOF'
and it is the same VMS process, so the binding was not re-made
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    register-continue-identity-dropped)
        case "$_f" in
        facility)     echo "image-activation identity continuation (VMS_IOCTL_REGISTER_CONTINUE, src/kernel/vms_module.c vms_proc_continue_identity) -- OVMX's fork-per-image made invisible to VMS (vms-4d7, Option B)";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_syssvc_identcont";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_proc_continue_identity() stops SHARING the parent's VMS PID (\`shared_vms_pid = 0\` instead of \`= parent->vms_pid\`), so REGISTER_CONTINUE falls all the way back to the derive-from-capable() path -- exactly the pre-fix behaviour where an activated image got a fresh PCB and a privilege mask derived from CAP_SYS_ADMIN (never SYSPRV) instead of continuing its DCL. The identity copy above it is now dead: with vms_pid 0 the caller derives, so SYSTEM's RUN AUTHORIZE loses SYSPRV again. The register_continue ioctl still returns SS\$_NORMAL (the registration itself succeeds), which is why CONTINUE_STATUS=1 stays green -- only the IDENTITY it should have carried is gone, the same facade shape as the other *-not-recorded entries.";;
        require_fail) cat <<'EOF'
A: the continued image holds the PARENT's SYSPRV mask -- a readback, not a claim, and it never called setident
A: AUTHORIZE printed its banner -- the continued image was ADMITTED
A: the continued image was never refused
A: AUTHORIZE exited 0 for the admitted session
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
B: the continued image holds the PARENT's REDUCED mask, with SYSPRV genuinely absent -- privileges did not reappear
EOF
                      ;;
        knock_on_why) cat <<'EOF'
ONE PROPERTY -- "an activated image continues its activator's identity" -- read
in BOTH directions. Scenario A (require_fail) reads it upward: a SYSPRV parent's
image must hold SYSPRV it never asked for; with the sharing removed the image
derives an enforced-but-not-SYSPRV mask, so its SELF_PRIVS readback, AUTHORIZE's
admission banner, the "never refused" check and the exit-0 check all go red at
once -- one missing identity, four dependent observations. Scenario B (this
knock-on) reads the SAME property downward: a parent that setident'd DOWN to
FIELD must have its image inherit the REDUCED mask, so B's SELF_PRIVS readback
compares against FIELD_PRIVS and also moves when the mask is derived instead of
continued. B's REFUSAL assertions (NAOFIL/PRV, banner-absent, rc=1) stay GREEN
under this defect -- the derived enforced mask lacks SYSPRV just as FIELD does,
so AUTHORIZE refuses either way -- which is why only B's readback is named here
and not B's refusal. CONTINUE_STATUS=1 stays green in both scenarios: the
REGISTER_CONTINUE ioctl still succeeds, it just no longer continues. MEASURED
red set is exactly these five assertions and no others.
EOF
                      ;;
        esac;;

    rms-create-filespec-not-translated)
        case "$_f" in
        facility)     echo "RMS filespec-to-Linux-path translation on create (src/vmsrms/rms_core.c resolve_filename(), called by both sys\$open() and sys\$create()) -- a userspace consumer of vmsfs's translation, the same class as the dcl-fident-*/dcl-fuser-* entries above, not a vms.ko-dispatched ioctl";;
        targets)      echo "vmsrms/rms_core.c";;
        suites_red)   echo "test_syssvc_rms_scratch_create";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "resolve_filename() checked vmsfs_to_linux_path()'s return value with \`== 0\`, but that function returns a VMS status code (SS\$_NORMAL == 1 on success, odd = success -- \$VMS_STATUS_SUCCESS is the project-wide convention, never 0). The check never matched, so a VMS filespec (SYS\$SCRATCH:FOO.DAT) was never recognized as translated and silently fell through to being treated as a literal, relative Linux path -- resolving against the calling process's cwd instead of /vms/... This is the exact vms-221 regression: PARTS's sys\$create() under SYS\$SCRATCH: returned EACCES for a reason that had nothing to do with directory ownership, because it was never really looking at SYS\$SCRATCH: at all.";;
        require_fail) cat <<'EOF'
sys$create() of the RMS indexed file under SYS$SCRATCH: succeeded (the vms-221 EACCES is gone)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
sys$create() resolved the VMS filespec SYS$SCRATCH:VMS221.DAT to its REAL translated Linux path under /vms/SYSTMP -- pins the vms-221 regression directly: resolve_filename() (src/vmsrms/rms_core.c) used to check vmsfs_to_linux_path()'s return value with `== 0`, but that function returns a VMS status code (SS$_NORMAL == 1 on success, odd = success); the check never matched, so EVERY VMS-spec candidate silently fell through to being treated as a literal (relative) Linux path instead of a translated one
sys$create() did NOT fall back to treating the raw VMS spec string as a literal relative Linux path (the exact vms-221 regression shape)
sys$connect() on the freshly created file succeeded
sys$put() #100 (first periodic index save, fresh .rms_idx create) succeeded
sys$put() #200 (SECOND periodic index save -- reopens the .rms_idx sidecar) succeeded
every sys$put() across two periodic index-save reopens succeeded -- no silent mid-run EACCES
sys$close() (final index flush, a THIRD reopen of .rms_idx) succeeded
EOF
                      ;;
        knock_on_why) cat <<'EOF'
ONE LINE, ONE EARLY RETURN, EVERY LATER STAGE FOLLOWS MECHANICALLY -- not a
second property. With the check reverted, sys$create()'s open() targets an
unwritable relative path and returns non-success; test_syssvc_rms_scratch_
create.c's child_main() checks $VMS_STATUS_SUCCESS(st) immediately after and
_exit(0)s right there (see the file: "if (!$VMS_STATUS_SUCCESS(st)) { _exit(0);
}"), before sys$connect(), any sys$put(), or sys$close() ever run. Each of
those stages' status line is therefore simply ABSENT from the child's
transcript, which field_is_ok() reports as not-OK for every one of them, and
the "ALL_205_PUTS_SUCCEEDED" line is never printed either since the put loop
never executes. The two RESOLVED_PATH knock-ons are the same single symptom
observed a second way -- through the path string sys$create() computed,
rather than through the status it returned -- not an independent defect.
EOF
                      ;;
        esac;;

    scratch-dir-owner-not-system)
        case "$_f" in
        facility)     echo "PID 1 boot-time VMS directory provisioning (SYS\$SCRATCH:/SYS\$LOGIN:, src/ovmx_init/ovmx_init.c provision_writable_dir())";;
        # NOT src/ovmx_init/ovmx_init.c. test_syssvc_scratch_writable.c runs
        # in the kernel-executive QEMU rig (tests/qemu/Dockerfile + init.sh),
        # which insmods vms.ko and drives the public sys$ API directly --
        # PID 1 / STARTUP.EXE never boots there, so ovmx_init.c is never
        # linked into this suite's binary and a mutation to it CANNOT reach
        # this suite (MEASURED: qemu_syssvc_tests links against libvms only,
        # not ovmx_init). The suite therefore carries its OWN duplicate of
        # provision_writable_dir()'s recipe
        # (provision_writable_dir_for_test(), with its own header explaining
        # the duplication) so it can stand up [SYSTMP]/[USERS] itself before
        # probing them -- and THAT is the copy an sed mutation has to reach
        # for "detection" to be a true claim rather than a decorative one.
        # ../ is relative to the src-root cmd_apply is given (src/), which
        # is always a sibling of tests/ in every checkout this runs
        # against -- the real driver's /src/repo/src (inject_and_run.sh,
        # facility_attribution_negctl.sh) and this file's own cmd_selftest
        # (which stages tests/qemu next to its src/ copy for exactly this).
        targets)      echo "../tests/qemu/test_syssvc_scratch_writable.c";;
        # This is NOT an executive/vms.ko facility -- it is PID 1's own
        # userspace boot-time provisioning, outside the "every wired
        # EXECUTIVE facility" scope this manifest otherwise covers (see the
        # SCOPE, DECLARED header). It is admitted here, alongside the
        # kernel-facility defects, only because
        # tests/qemu/test_syssvc_scratch_writable.c is discovered by the same
        # test_syssvc_* glob cmd_coverage floors, and the coverage check
        # draws no distinction between "executive" and "PID 1 boot" for that
        # floor -- so this suite needs a real anchor or the floor is wrong
        # about it, not vice versa (vms-e5c).
        suites_red)   echo "test_syssvc_scratch_writable";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        # MEASURED, not reasoned (vms-e5c, corrected vms-9b7 follow-up).
        # provision_writable_dir_for_test() chowns [SYSTMP]/[USERS] to SYSTEM's
        # FULL UIC (both fields) before this suite's credential-drop probes
        # run. This mutation drops only the MEMBER half of that ONE chown call
        # to root (0), leaving the GROUP half alone -- so the directories end
        # up root-owned but still SYSTEM-group-owned, exactly the "chown the
        # group only" shape an earlier draft of the real fix
        # (src/ovmx_init/ovmx_init.c) shipped and then measured broken.
        #
        # WHAT ACTUALLY GATES THE PROBE, STATED HONESTLY. The A1/A2 assertions
        # are a raw open(2) on the vmsfs_to_linux_path()-resolved BACKING PATH,
        # not a call into vmsfs's own SOGW check: SYSDISK_MOUNT ("/vms") is a
        # plain rootfs directory in THIS rig, not a vmsfs mount (tests/qemu/
        # init.sh mounts vmsfs only at /mnt/vmsfs), so the Linux VFS's ordinary
        # owner/group/other DAC on that path is what admits or refuses the
        # create. That is why test_syssvc_scratch_writable.c provisions these
        # directories at 0755, NOT the product's 0775: at 0775 the group WRITE
        # bit alone would grant the SYSTEM probe (whose credential drop uses
        # SYSTEM_UIC_GROUP, this directory's group) the create no matter who
        # owns it, so dropping the owner would change nothing observable and
        # the suite would pass with the defect injected -- an assertion that
        # cannot fail, which is precisely the LARP this whole file exists to
        # kill (it was live on the tree until vms-9b7). At 0755 the group
        # category is r-x with no write, so the create succeeds for SYSTEM only
        # through the OWNER category -- both UIC fields matching -- and dropping
        # the MEMBER half to root removes exactly that, refusing SYSTEM's create
        # just as an unrelated account is refused. The probes themselves are
        # untouched (SYSTEM_UIC_MEMBER, used by try_create_as(), is a different
        # symbol from the literal this mutation edits), so this defect reddens
        # test_syssvc_scratch_writable's positive (A1/A2) assertions and only
        # those. An EARLIER draft of this entry claimed the gate was vmsfs's
        # SOGW decision (vmsfs_blkdev_permission / VMSFS_PROT_DEFAULT); that was
        # wrong for this rig -- the open never reaches vmsfs -- and believing it
        # is why the 0775 masking went unnoticed.
        why)          echo "provision_writable_dir_for_test() stops making its SYSTEM probe's UIC the actual OWNER of [SYSTMP]/[USERS] (only the UIC GROUP field still matches, the MEMBER field reverts to root/0). The A1/A2 probe is a raw open(2) under Linux DAC on the /vms backing path (not a vmsfs mount in this rig), and the suite provisions these directories 0755 -- no group write -- so SYSTEM's sys\$create-equivalent open() under SYS\$SCRATCH:/SYS\$LOGIN: can succeed only through the OWNER category; with the owner reverted to root it is refused exactly as it was before vms-e5c's fix to the real product code.";;
        require_fail) cat <<'EOF'
SYSTEM's sys$create-equivalent open() succeeds under SYS$SCRATCH:
SYSTEM's sys$create-equivalent open() succeeds under SYS$LOGIN:
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    lnm-delete-noop)
        case "$_f" in
        facility)     echo "executive-resident logical name tables (VMS_IOCTL_LNM_DEFINE/DELETE + the mmap arena, LNM\$SYSTEM), vms-d37";;
        targets)      echo "kernel-core/vms_lnm.c";;
        suites_red)   echo "test_syssvc_lnm_crossproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_lnm_delete stops clearing the entry's in_use flag but still reports SS\$_NORMAL, so DEASSIGN/SYSTEM 'succeeds' while the name stays defined -- a public delete reporting success for a mutation it did not perform. One condition, one process pair.";;
        require_fail) echo "child: a name the parent DEASSIGN/SYSTEM'd is gone here too (delete is cross-process, and it is absence, not SS\$_NOSUCHDEV)";;
        knock_on_fail) echo "";;
        knock_on_why)  cat <<'EOF'
SINGLE-PROPERTY BY CONSTRUCTION. The mutation removes only the `e->in_use = 0;`
store in vms_ioctl_lnm_delete and leaves the returned status SS$_NORMAL, so:
  - every DEFINE and TRANSLATE assertion still passes (the arena is written and
    read exactly as before -- nothing on the define/translate path is touched);
  - "parent: sys$dellnm in LNM$SYSTEM reported success" still passes (the
    handler still finds the entry and still returns SS$_NORMAL);
  - only the child's post-delete TRANSLATE reddens, because the name it should
    no longer find is still present.
That is exactly one assertion, which is why require_fail names it alone and
knock_on_fail is empty. A mutation that instead skipped the store in
vms_ioctl_lnm_define would redden four assertions at once (both child reads,
the parent's read-back of the child's define, and the parent's own delete of a
name that was never stored) -- the blunderbuss this manifest's method rule
forbids -- so the delete path was chosen deliberately for its surgical scope.
EOF
                      ;;
        esac;;

    lnm-manager-delete-noop)
        case "$_f" in
        facility)     echo "vmslnm-manager LNM\$SYSTEM path (src/vmslnm/lnm_client.c + lnm_translate.c), the DCL/vmsfs-facing client vms-96e2 built on top of vms-d37's executive-resident LNM\$SYSTEM, hardened by vms-48ab (removal of its host-tooling fallback)";;
        targets)      echo "vmslnm/lnm_client.c";;
        suites_red)   echo "test_syssvc_lnm_system";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "lnm_delete()'s LNM\$SYSTEM branch stops calling vms_kif_lnm_delete and returns SS\$_NORMAL directly, so DEASSIGN/SYSTEM through the vmslnm MANAGER API (what DCL, SHOW LOGICAL/F\$TRNLNM and vmsfs actually call -- distinct from the already-honest sys\$dellnm in src/libvms/syssvc/sys_logical.c) 'succeeds' without ever reaching vms.ko: the name stays defined in the executive arena. This is the SAME shape lnm-delete-noop reddens at the kernel-ioctl layer, one layer up -- the manager API vms-96e2 added on top of vms-d37's already-honest executive plumbing can independently reintroduce a fabricated delete, and nothing before test_syssvc_lnm_system.c (new with vms-48ab) called the manager API against a real executive to catch it.";;
        require_fail) echo "manager: OVMX48AB\$VOL is gone from LNM\$SYSTEM after the manager's own delete";;
        knock_on_fail) echo "banner: deassigning SYS\$WELCOME restores the built-in banner";;
        knock_on_why)  cat <<'EOF'
SAME CALL SITE, OBSERVED TWICE. Both reddened assertions read back a name
through lnm_translate(LNM$SYSTEM/LNM$FILE_DEV, ...) after a prior
lnm_delete(..., LNM_SYSTEM_TABLE, ...) call that the mutation turns into a
no-op SS$_NORMAL: OVMX48AB$VOL in run_manager_system_and_hierarchy() and
SYS$WELCOME in run_banner_override() (by the time it deassigns, SYS$WELCOME
holds the '@file' equivalence from the preceding step, so the banner shows
that file's content instead of the built-in). Every DEFINE/TRANSLATE
assertion before either delete is untouched (the mutation is inside
lnm_delete only), the "reports success" assertion for BOTH deletes still
passes (the mutated line still returns SS$_NORMAL), and the process-table
delete used to clean up OVMX48AB$VOL's override is a different table --
is_system_table() is false for it -- so it takes the unmutated branch. That
is why exactly these two "did it actually go away" assertions redden and no
others: one property (a public LNM$SYSTEM delete reporting success for a
mutation it did not perform), observed at its only two call sites in this
suite.
EOF
                      ;;
        esac;;

    lnm-group-scope-collapsed)
        case "$_f" in
        facility)     echo "executive-resident LNM\$GROUP / LNM\$JOB (VMS_IOCTL_LNM_DEFINE/DELETE + the mmap arena, derive_scope_key() in vms_lnm.c), vms-aba -- the deferred other half of vms-d37's LNM\$SYSTEM residency";;
        targets)      echo "kernel-core/vms_lnm.c";;
        suites_red)   echo "test_syssvc_lnm_groupjob";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "derive_scope_key()'s LNM\$GROUP case stops returning the caller's UIC group (proc->uic >> 16) and returns 0 instead -- the same scope_key SYSTEM uses -- so a GROUP-scoped name becomes visible to every UIC group on the node instead of only the one that defined it. DEFINE/GROUP and TRANSLATE/GROUP both still succeed (the arena write/read path is untouched); only the ISOLATION between groups is gone.";;
        require_fail) echo "C (different group, same job): does NOT see the GROUP-scoped name A defined";;
        knock_on_fail) echo "";;
        knock_on_why)  cat <<'EOF'
SINGLE-PROPERTY BY CONSTRUCTION. The mutation touches only the LNM$GROUP
case's return value in derive_scope_key(); the LNM$JOB case (proc->job_id)
and the SYSTEM case (0, already correct) are untouched. Effects on every
assertion in test_syssvc_lnm_groupjob.c:
  - A's own DEFINE/GROUP, DEFINE/JOB and search-list reads of its own names
    are unaffected -- A translates under its own (now-wrong-but-consistent)
    scope_key 0, so it still finds what it just wrote.
  - B (same group, same job) still passes both checks: B's real scope_key
    (0, since B never setgid()s) also collapses to 0, which is what A wrote
    under, so B still finds the GROUP name -- for the wrong reason, but the
    assertion cannot tell, which is exactly why C's setgid() is the assertion
    that MUST catch this and not B's.
  - C (different group via setgid(ALT_UIC_GROUP), same job) is where the
    defect surfaces: its JOB-name check is untouched (proc->job_id is not
    mutated) and keeps passing, but its GROUP-name check now WRONGLY passes
    too -- C's derived scope_key collapses to 0 same as A's, so the group
    boundary that check exists to prove is gone. require_fail names exactly
    that one assertion.
  - D (same group, different job) is unaffected: D never setgid()s either,
    so its GROUP read still resolves the same way it did before the
    mutation (scope_key 0 either way for a process that never left the
    default group), and its JOB check does not touch this code path at all.
That is exactly one assertion turning from FAIL-if-correct-scoping to
PASS-when-broken, which is why require_fail names it alone and knock_on_fail
is empty -- a mutation to the JOB case would be the single-property sibling
this one is not, and does not exist as a separate defect because the JOB
case's return line has no reason to fail differently from GROUP's.
EOF
                      ;;
        esac;;

    lnm-privilege-check-bypassed)
        case "$_f" in
        facility)     echo "SYSNAM/GRPNAM privilege enforcement on LNM\$SYSTEM/LNM\$GROUP mutation (lnm_priv_check() in vms_lnm.c, consulted by both VMS_IOCTL_LNM_DEFINE and VMS_IOCTL_LNM_DELETE), vms-5b7";;
        targets)      echo "kernel-core/vms_lnm.c";;
        suites_red)   echo "test_syssvc_lnm_privilege";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "lnm_priv_check() stops consulting cur_privs and returns true unconditionally, so DEFINE/SYSTEM, DEASSIGN/SYSTEM, DEFINE/GROUP and DEASSIGN/GROUP all succeed for a caller holding NONE of SYSNAM/GRPNAM/GRPPRV/SYSPRV -- the exact privilege check vms-5b7 adds, bypassed in one shot because both ioctl handlers call this ONE function rather than each carrying its own copy of the switch.";;
        require_fail) cat <<'EOF'
SYSTEM: a process holding neither SYSNAM nor SYSPRV is refused SS$_NOPRIV
SYSTEM: a process holding neither SYSNAM nor SYSPRV is refused DEASSIGN/SYSTEM SS$_NOPRIV
GROUP: a process holding none of GRPNAM/GRPPRV/SYSPRV is refused SS$_NOPRIV
GROUP: a process holding none of GRPNAM/GRPPRV/SYSPRV is refused DEASSIGN/GROUP SS$_NOPRIV
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
SYSTEM: the refused DEFINE never created the name (table unchanged)
SYSTEM: the refused DELETE left the name and its value unchanged
GROUP: the refused DEFINE never created the name (table unchanged)
GROUP: the refused DELETE left the name and its value unchanged
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED A SECOND TIME PER TABLE, NOT A SECOND PROPERTY. Once
lnm_priv_check() always returns true, the four "refused" DEFINE/DELETE
calls above do not just report the wrong status -- they actually reach
lnm_find()/lnm_alloc_slot() and mutate the arena, because the mutation
removed the ONLY gate standing between an unprivileged caller and the
table. So the "table unchanged" companion assertion for each of the four
also flips: the DEFINE case's name now DOES appear where it should still
be absent, and the DELETE case's name is now GONE (or its value replaced)
where it should still be exactly what a privileged process originally
wrote. Both members of each pair trace to the exact same missing check,
not to two independent bugs -- which is why they are paired here as
knock-on rather than each given a separate defect.

WHAT STAYS GREEN, AND WHY. Every positive-privilege assertion in this
suite (SYSNAM/SYSPRV/GRPNAM/GRPPRV each succeeding on their own) is
UNCHANGED by this mutation: those callers already satisfied the real
check and lnm_priv_check() always returning true changes nothing for
them. The LNM$JOB assertions are untouched for the same reason: the
default case already returned true before this mutation reached it.
And no OTHER suite in this manifest sets up an unprivileged identity
before writing LNM$SYSTEM/LNM$GROUP -- test_syssvc_lnm_system.c,
test_syssvc_lnm_groupjob.c and test_syssvc_lnm_crossproc.c all act under
the default registered-process identity, which already carries
SYSNAM/GRPNAM (VMS_PRV_M_ENFORCED, vms_ioctl.h) for a process the
executive derives as CAP_SYS_ADMIN-capable -- true for every process in
this harness's initramfs. Those suites would pass identically whether or
not the gate exists, so this mutation cannot redden them.
EOF
                      ;;
        esac;;

    mbx-not-shared)
        case "$_f" in
        facility)     echo "mailboxes (VMS_IOCTL_MBX_CREATE/ASSIGN/WRITE/READ/DELMBX, executive-resident MBAn:), vms-d44 + named-mailbox rendezvous by logical name, vms-mb1 + bidirectional command/response streaming to a spawned child, vms-e0b";;
        targets)      echo "kernel-core/vms_mbx.c";;
        suites_red)   echo "test_kmod_mbx test_syssvc_mbx_crossproc test_syssvc_mbx_cmdresp";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "\$ASSIGN's mailbox lookup (vms_ioctl_mbx_assign()) gains a check that the caller's Linux pid matches the mailbox's CREATOR, so a mailbox is only ever reachable by the process that \$CREMBX'd it. That is precisely the AF_UNIX-socketpair shape this item replaced (see vms_mbx.h's header): every single-process assertion -- A's own \$CREMBX, its own write, the malformed-name and never-created-unit negative controls -- stays green, because A always IS its own mailbox's creator. Only a SECOND, unrelated process's \$ASSIGN can see the difference, which is exactly the A-writes/B-reads shape CLAUDE.md rule 11 exists to catch.";;
        require_fail) cat <<'EOF'
B: $ASSIGN to A's mailbox by device name succeeds (cross-process rendezvous)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
B: mailbox read succeeds on A's mailbox
B reads the EXACT message A wrote (byte-exact, right length)
IO$M_NOW read of an EMPTY mailbox returns SS$_ENDOFFILE immediately (a blocking read would hang -> the suite timeout catches a regression)
B: write a message to drain with IO$M_NOW
IO$M_NOW read WITH a queued message returns that message immediately (byte-exact), not SS$_ENDOFFILE
B: a second $ASSIGN to the same mailbox succeeds
temporary mailbox SURVIVES its creator's $DASSGN while another process (B) still holds a channel to it
B: $DELMBX succeeds
oracle: $DELMBX alone does not deassign or delete -- the mailbox is still assignable immediately afterward
B: $DASSGN channel 1
B: $DASSGN channel 2
B: $DASSGN channel 3 (the last one)
reader: $ASSIGN by LOGICAL NAME reaches the creator's mailbox cross-process (sys$assign translates the name through LNM$SYSTEM to MBAn:)
reader: $QIO READVBLK returns the EXACT message the creator wrote (byte-exact, right length) -- named-mailbox message delivery through the executive
child: $ASSIGN of the command mailbox BY NAME reaches the parent's mailbox cross-process
child: $ASSIGN of the result mailbox BY NAME reaches the parent's mailbox cross-process
parent: all 8 streamed commands come back TRANSFORMED and byte-exact from the spawned child -- NOT REACHED because the child could not assign the mailboxes cross-process
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED DOWNSTREAM OF ITS ONE ROOT, NOT NINE SEPARATE
PROPERTIES. test_kmod_mbx.c's process B never obtains a valid mailbox
channel from its first $ASSIGN (chan_b1 stays its zero-initialized value),
and every knock-on assertion above is B doing something with a channel it
was never actually given: reading and comparing a message on chan_b1,
the IO$M_NOW non-blocking reads and the interposed write that all issue on
that same phantom chan_b1 (each returning SS$_IVCHAN instead of the
SS$_ENDOFFILE / SS$_NORMAL the assertion expects -- they fail FAST, never
hang, because a zero channel is rejected before any wait), taking a
"second" channel (chan_b2, equally refused since B is still not the
creator), surviving-dassgn and $DELMBX checks against the same
never-obtained channels, and finally three $DASSGN calls on channel
numbers B never held. MEASURED against a real /dev/vms (rebuilt vms.ko,
reloaded, re-run): this mutation reddens exactly these thirteen "  FAIL:"
lines and no others -- 9 passed, 13 failed, out of 22 total assertions.

WHAT STAYS GREEN, AND WHY. A's own four assertions ($CREMBX temporary,
the PRMMBX-privilege refusal, A's write, A's device-name shape) are
untouched: A is always its own mailbox's creator, so the added check
never fires for anything A does to its own mailbox, including A's own
$DASSGN of its own channel. The three negative controls at the bottom
(malformed device name, a never-created MBA999999: unit, and $DASSGN of a
channel never held) are also untouched: none of them names a real
mailbox another process created, so the mutation has nothing to
discriminate on and the executive's existing SS$_IVDEVNAM/SS$_NOSUCHDEV/
SS$_IVCHAN answers are unchanged. The final "mailbox is GONE" assertion
also stays green -- B's phantom channels were never real references, so
the mailbox's refcnt reached zero and it was freed exactly as it would be
if B had genuinely never touched it at all, which is a coincidentally
correct answer to a differently-broken question, not evidence the defect
is narrower than claimed.

THE SAME ONE ROOT, OBSERVED IN test_syssvc_mbx_crossproc TOO (vms-mb1).
That suite proves the by-NAME half: the creator (process A) $CREMBXes a
mailbox WITH a logical name -- published in the executive-resident
LNM$SYSTEM (vms-d37) -- and an unrelated reader (process B) reaches it with
sys$assign(NAME), which translates the name through LNM$SYSTEM to the
"MBAn:" unit. The name resolves fine (LNM is a different facility); it is
the executive's mailbox $ASSIGN of that unit, by a caller that is not the
creator, that this same creator-pid check refuses. So the reader's by-name
$ASSIGN goes red, and the delivery read that depends on the channel it
never got goes red with it (the read is issued unconditionally on the
would-be channel precisely so it reddens BY NAME under this defect rather
than being skipped). The creator's own $CREMBX and $QIO WRITEVBLK to its
OWN mailbox stay green (A is always its own mailbox's creator), and the
never-defined-name control stays green (it names no mailbox, so the check
has nothing to discriminate on). Two suites, one root: a mailbox reachable
only by its creator is the exact socketpair facade the executive replaced,
whether the second process reaches for it by unit (test_kmod_mbx) or by
name (test_syssvc_mbx_crossproc).

THE SAME ONE ROOT, OBSERVED IN test_syssvc_mbx_cmdresp TOO (vms-e0b). That
suite proves the MMK command/response shape: a parent $CREMBXes two named
mailboxes and streams commands to a spawned child over one while reading the
child's transformed results back over the other. The child -- which created
neither mailbox -- reaches both only by sys$assign(NAME), so this same
creator-pid check refuses BOTH of its $ASSIGNs; the child then reports two
failed assigns and the parent's whole command/response loop cannot run, so
the two "child: $ASSIGN ... reaches the parent's mailbox cross-process"
assertions and the "NOT REACHED because the child could not assign the
mailboxes cross-process" round-trip assertion all redden. The parent's own
two $CREMBX assertions stay green (the parent is always its own mailboxes'
creator). Three suites, one root: a mailbox reachable only by its creator is
the exact socketpair facade the executive replaced, whether the second
process reaches for it by unit (test_kmod_mbx), by name for a one-shot read
(test_syssvc_mbx_crossproc), or by name for a streamed command/response
exchange (test_syssvc_mbx_cmdresp).
EOF
                      ;;
        esac;;

    mbx-wrtattn-not-fired)
        case "$_f" in
        facility)     echo "mailbox WRITE-ATTENTION ASTs (VMS_IOCTL_MBX_SET_WRTATTN / IO\$_SETMODE|IO\$M_WRTATTN, executive-resident MBAn:), vms-9003 -- the notification MMK's send_cmd_and_wait waits on (spine #4)";;
        targets)      echo "kernel-core/vms_mbx.c";;
        suites_red)   echo "test_syssvc_mbx_wrtattn";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_mbx_write()'s write-attention firing block allocates the AST entry it queues into the reader's executive AST queue; the mutation makes that allocation always yield NULL, so the \`if (ast)\` guard skips the enqueue and NO write-attention AST is ever delivered -- while every registration is still drained (one-shot bookkeeping intact) and the write itself still succeeds. The arm (\$QIO IO\$_SETMODE|IO\$M_WRTATTN registers fine), the cross-process rendezvous (\$CREMBX/\$ASSIGN by name) and the write are all untouched; only the CROSS-PROCESS DELIVERY of the AST -- the one thing a per-process fake could never do and the whole point of vms-9003 -- stops happening.";;
        require_fail) cat <<'EOF'
A's write-attention AST FIRES when the UNRELATED process B writes the mailbox (cross-process delivery through the executive)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
A's AST ran with the exact parameter A passed in P2 (astprm round-tripped through the executive)
the RE-ARMED write-attention AST fires on the next cross-process write
the re-armed AST ran with the NEW parameter, not round 1's stale value
one-shot: a write with NO re-arm does NOT fire the AST again (VSI mailbox driver semantics)
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME ONE ROOT (the write-attention firing never runs), OBSERVED AT EVERY
POINT THE SUITE LOOKS FOR A DELIVERY. With no AST ever queued, A's recorder
never runs, so g_fired stays 0 and g_param stays 0 through all three rounds:
the round-1 parameter check, the round-2 re-armed fire and its parameter
check, and the round-3 one-shot check (which asserts g_fired == 2 and so also
reads 0) all go red. They are not four more properties -- they are the single
"no AST was delivered" fact seen at each place the suite samples it. What
STAYS GREEN pins the mutation to delivery alone and nothing else: A's own
$CREMBX and IO$_SETMODE|IO$M_WRTATTN arm both return SS$_NORMAL (the ioctl
still registers the AST -- only the write path's firing is broken), B's
$ASSIGN-by-name still reaches the mailbox, and A's round-2 re-arm still
returns SS$_NORMAL. The one-shot NON-fire property is a POSITIVE control in
the healthy tree (round 3 writes without re-arming and g_fired must stay 2);
under this mutation it reddens only as collateral of nothing ever firing, not
because one-shot bookkeeping regressed -- the registrations are still drained
exactly once per write either way.
EOF
                      ;;
        esac;;

    hiber-ast-not-delivered)
        case "$_f" in
        facility)     echo "asynchronous AST delivery interrupting \$HIBER (VMS_IOCTL_HIBER/WAKE + vms_ast_notify_arrival, executive-resident), vms-feb -- the \$HIBER/\$WAKE + write-attention-AST rendezvous MMK's send_cmd_and_wait needs (spine #4, vms-b23)";;
        targets)      echo "kernel-core/vms_ast.c";;
        suites_red)   echo "test_syssvc_hiber_ast";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ast_notify_arrival() is the ONE step that turns the existing AST QUEUE into async DELIVERY: after any path (a mailbox write-attention write, a \$DCLAST, a lock AST) queues an entry into a process's AST queue, it broadcasts that process's hiber_wq so a \$HIBER waiter wakes, drains and RUNS the routine. The mutation removes that broadcast (the arrival wake), so the write-attention AST still lands in the hibernating process's executive queue but the process is NEVER woken to run it -- its sys\$hiber() deadlocks. Everything the arm/rendezvous/write needs is untouched (\$CREMBX/\$ASSIGN-by-name/\$QIO all succeed, the AST is still enqueued); ONLY the wakeup that makes \$HIBER interruptible by that queued AST -- the whole point of vms-feb -- stops happening. The \$WAKE path (vms_ioctl_wake's own broadcast, in a DIFFERENT file, kernel-core/vms_proctab.c, which the sed on vms_ast.c never touches) is untouched, so the sticky-wake scenario still passes -- pinning the mutation to AST-arrival delivery alone.";;
        require_fail) cat <<'EOF'
A's $HIBER was INTERRUPTED by the cross-process write-attention AST: A ran the AST and returned from $HIBER
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the AST that woke A ran with the exact parameter A armed (astprm round-tripped through the executive)
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME ONE ROOT (A's $HIBER is never woken by the queued AST), seen at both
places the scenario samples the delivery. With the arrival broadcast gone, B's
write enqueues the write-attention AST into A's executive queue but A's
sys$hiber() is never released to drain it, so A's recorder never runs: the
coordinator's bounded wait for A's post-$HIBER report times out, failing both
the "$HIBER was interrupted / AST ran and returned" assertion and the paired
astprm-round-trip check (A never reports a parameter it never recorded). They
are not two properties -- they are the single "A stayed asleep, the AST never
ran" fact seen twice. What STAYS GREEN pins the mutation to AST-arrival delivery
alone: B's $ASSIGN-by-name still reaches the mailbox, and the SEPARATE
sticky-wake scenario (a $WAKE issued BEFORE $HIBER falls straight through) still
passes because vms_ioctl_wake's own broadcast is untouched. test_syssvc_mbx_
wrtattn stays fully green because it drains its write-attention AST with an
explicit sys$setast(1) and never hibernates -- it does not depend on the arrival
wake at all.
EOF
                      ;;
        esac;;

    dcl-sysinput-mbx-not-read)
        case "$_f" in
        facility)     echo "DCL reads SYS\$INPUT from / writes SYS\$OUTPUT to a mailbox device (src/vmsdcl/dcl_mbx.c, vms-786) -- the persistent-DCL-over-mailboxes drive MMK's send_cmd_and_wait stands on (spine #4, vms-b23)";;
        targets)      echo "vmsdcl/dcl_mbx.c";;
        suites_red)   echo "test_syssvc_mbx_dcldrv";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "dcl_mbx.c's reader thread does the IO\$_READVBLK that pulls each command record out of the SYS\$INPUT mailbox and forwards it (mlen bytes) into the pipe DCL's fgets() loop reads. The mutation forces that forwarded length to zero (mlen = 0), so the reader still drains the mailbox but delivers NOTHING to DCL's command loop -- the child never sees `NUM = 6 * 7` or the WRITE, computes nothing, and delivers no result over SYS\$OUTPUT. The suite's driven-result assertion reddens. What STAYS GREEN pins the mutation to the SYS\$INPUT read path alone: the parent's own \$CREMBX of both mailboxes and its \$QIO WRITEVBLK of both command lines into the input mailbox all still return SS\$_NORMAL -- the child's failure to READ is the only thing broken, exactly the A-writes/B-reads shape INV-6/Rule 11 exists to catch.";;
        require_fail) cat <<'EOF'
the spawned DCL read its commands from the SYS$INPUT mailbox, executed them, and delivered the computed result over the SYS$OUTPUT mailbox
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    mmk-drive-command-not-sent)
        case "$_f" in
        facility)     echo "the MMK exec-drive: the shipped MMK.EXE drives a real build through its persistent mailbox-driven DCL subprocess (tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c sp_open/sp_send/sp_receive, vms-b23) -- composing lib\$spawn + mailbox IPC + write-attention AST + \$HIBER + IO\$M_NOW into MMK's send_cmd_and_wait, self-host spine #4";;
        targets)      echo "../tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c";;
        suites_red)   echo "test_syssvc_mmk_drive";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sp_send is the ONE step that copies each resolved command line (its action command plus the three end-of-command-marker commands) into the SYS\$INPUT mailbox the spawned DCL reads. The mutation forces the record length it writes to zero (len = 0), so sp_send drains MMK's command stream but delivers EMPTY records -- the DCL receives no `ANSWER = 6 * 7`, no WRITE, and no marker command, so it computes nothing, delivers no OVMXB23:42 over SYS\$OUTPUT, and never writes the MMK____status= marker MMK's \$HIBER waits on. MMK's own \$CREMBX of both mailboxes, the write-attention-AST arm, and the lib\$spawn of DCL all still succeed -- ONLY the command CONTENT is dropped, the A-writes-nothing / B-reads-nothing shape INV-6/Rule 11 exists to catch.";;
        require_fail) cat <<'EOF'
MMK.EXE drove a real build: its spawned DCL computed 6*7 and delivered OVMXB23:42 back over the mailbox drive (spawn + mailbox + write-attention AST + $HIBER + IO$M_NOW + $STATUS marker)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    mmk-build-image-not-activated)
        case "$_f" in
        facility)     echo "the MMK-driven native BUILD (self-host spine #6 vms-d1b + spine #7 vms-6be/vms-725): the shipped MMK.EXE drives the static TCC.EXE + LIBRARIAN.EXE + LINK.EXE over its persistent mailbox DCL to compile REAL src/libvmssys runtime TUs, archive them into OVMXRT.OLB, and LINK a runnable OVMXRT.EXE -- the DCL foreign-command activation (src/vmsdcl/dcl_cmd_process.c dcl_exec_foreign_command -> dcl_activate_image fork+execve) that actually runs the staged toolchain in the guest";;
        targets)      echo "vmsdcl/dcl_cmd_process.c";;
        suites_red)   echo "test_syssvc_mmk_build";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        # WHY A DEDICATED CONTROL (not a second suite on mmk-drive-command-not-sent).
        # sp_send=0 (the drive control) reddens BOTH mmk suites by DEADLOCKING
        # MMK in $HIBER -- two ~50s wedges do not fit run_tests.sh's 120s QEMU
        # budget in one boot. This control reddens test_syssvc_mmk_build ALONE and
        # FAST, with NO wedge: dcl_exec_foreign_command reports SS$_NORMAL WITHOUT
        # activating the image, so the driven DCL "runs" each toolchain command
        # instantly, echoes the marker, and MMK completes -- but the toolchain
        # never runs, so NO object, NO archive and NO image are produced. The
        # completion assertion stays GREEN (the drive did complete); the compile
        # object/byte-identity assertions, the archive assertions AND the spine-#7
        # LINK+activate assertions redden.
        # ISOLATION: among every suite init.sh runs, ONLY test_syssvc_mmk_build
        # activates an image through a DCL foreign command. mmk_drive's action is
        # a WRITE builtin; test_syssvc_startup_service uses RUN (dcl_cmd_run, a
        # different path); test_syssvc_imgact_* activate through sys$imgact, not
        # DCL. So this mutation is attributable to the build drive alone. (The
        # harness's OWN activation of the produced OVMXRT.EXE -- the exit-216
        # oracle -- is a plain exec of a file that only exists if LINK ran, so it
        # too reddens under this root, not independently.)
        why)          echo "dcl_exec_foreign_command stops ACTIVATING the image: it returns SS\$_NORMAL without calling dcl_activate_image, so a foreign command REPORTS success while running nothing -- the INV-6 facade shape (report success without doing the work). For the MMK build drive, the driven DCL's TCC, LIBRARIAN and LINK commands 'succeed' instantly and MMK's drive completes, but none of TCC.EXE/LIBRARIAN.EXE/LINK.EXE is ever fork+execve'd, so no object, no OVMXRT.OLB and no OVMXRT.EXE is produced and the byte-identity + activation oracles have nothing to inspect. Parsing, argv construction, the image resolution and every non-activation path are untouched; only whether the resolved image actually runs changes -- the exact question 'did MMK really drive a real toolchain in the guest' the suite exists to answer.";;
        require_fail) cat <<'EOF'
the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #1)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the driven object is a valid ELF relocatable (ET_REL) -- TCC really compiled it in QEMU
the driven object carries the runtime symbol vms_strlen -- it is a compile of the REAL src/libvmssys/vms_string.c, not a stand-in
the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #2)
VMS_STRING.OBJ is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic, zero bash)
the MMK-driven LIBRARIAN.EXE produced OVMXRT.OLB in the guest (build #1)
OVMXRT.OLB is a valid ar-format object library (!<arch> magic) -- LIBRARIAN really archived it in QEMU
OVMXRT.OLB carries the archived member VMS_STRING -- LIBRARIAN inserted the driven object
OVMXRT.OLB embeds the runtime symbol vms_strlen -- its member is a compile of the REAL src/libvmssys/vms_string.c
the MMK-driven LIBRARIAN.EXE produced OVMXRT.OLB in the guest (build #2)
OVMXRT.OLB is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic archive, zero bash)
the MMK-driven LINK.EXE produced OVMXRT.EXE in the guest
OVMXRT.EXE is a valid OVMX image (ELF ET_DYN) -- LINK really linked it in QEMU
OVMXRT.EXE carries PT_INTERP=IMGACT.EXE -- it is an image the kernel activates through IMGACT, not a bare ELF
IMGACT activated the MMK-driven OVMXRT.EXE and it RAN to exit 216 (vms_strlen("OVMXRT")*36) -- the LINK pulled VMS_STRING from the .OLB and the image really runs
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME ONE ROOT (the foreign command reports success but activates nothing), seen
as every downstream absence of the object, the archive built from it, AND the
image linked from that. With dcl_activate_image never called, the driven TCC,
LIBRARIAN and LINK commands complete instantly with a success status, so drive #1
runs to the end and the spawned DCL still echoes OVMXD1B:COMPILED -- the single
marker "completion" assertion stays GREEN (this is a fast failure, not a $HIBER
wedge). But no compiler ran, so drive #1 produces no object; LIBRARIAN then has
no input objects to archive, so no OVMXRT.OLB; the suite's OLB-keyed short-circuit
then skips drive #2 (the compile->archive->LINK drive), so no OVMXRT.EXE is ever
produced. Build #1's object is absent -- the "produced VMS_STRING.OBJ (build #1)"
require_fail -- and its downstream reads redden with it: the COMPILE knock_ons
(valid-ELF and vms_strlen oracles with no object to inspect, the build-#2 object
with drive #2 skipped, and the object byte-identity check); the ARCHIVE knock_ons
(no OVMXRT.OLB build #1, so the ar-magic, both-members and embedded-symbols
oracles have no archive; no build #2 archive; the archive byte-identity check);
and the LINK+ACTIVATE knock_ons (drive #2 skipped -> no OVMXRT.EXE, so the
valid-ELF and PT_INTERP oracles have no image and the IMGACT activation yields no
exit 216 because there is no image to run). Every one is an observation of the
single fact "the driven toolchain never ran". Nothing outside
test_syssvc_mmk_build is touched: it is the only suite whose (spawned) DCL
activates an image through a foreign command, and with no executive it
honest-skips before ever spawning that DCL.
EOF
                      ;;
        esac;;

    p0-map-not-recorded)
        case "$_f" in
        facility)     echo "P0 program-region bookkeeping (VMS_IOCTL_P0_MAP/P0_UNMAP, vms-68f.i -- foundation increment of the Option A in-process image activation design, docs/design-in-process-activation.md Part II)";;
        targets)      echo "kernel/vms_p0.c";;
        suites_red)   echo "test_kmod_p0";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_p0_map() validates the extent, then SKIPS the two stores under proc->p0_lock (proc->p0_base = args.base; proc->p0_limit = args.limit;) while still returning SS\$_NORMAL -- exactly the shape INV-6 exists to catch: the ioctl REPORTS success without the executive actually holding the fact. Every validation-only assertion (the three degenerate-extent refusals, and the post-refusal GETJPI check) is untouched, because none of them ever reaches the deleted stores; only the assertions that read the extent BACK after a claimed-successful MAP can tell the difference.";;
        require_fail) cat <<'EOF'
GETJPI reflects the mapped P0 extent: p0_base/p0_limit match what VMS_IOCTL_P0_MAP just registered
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
A-WRITES/B-READS: B reads A's mapped P0 extent out of A's row -- a fact a per-process notion of P0 could not show
P0_UNMAP reports the exact extent it just freed
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED DOWNSTREAM OF ITS ONE ROOT. proc->p0_base/p0_limit
never leave zero after the deleted stores, so every later read of them --
not only the self-read named as require_fail -- reports the same absence:
process B's cross-process GETJPI(PID=A) sees no extent (the p0_base/
p0_limit copied into struct vms_procinfo by proc_fill_info() are the same
zeroed fields the mutation left behind), and VMS_IOCTL_P0_UNMAP's
"report what you freed" copy (args.base = proc->p0_base; before the
clear) has nothing but zero to copy out, so old_base/old_limit read back
as 0 instead of the window's real addresses. One deleted pair of stores,
three assertions naming the same missing fact from three different call
sites -- not three independent properties.

WHAT STAYS GREEN, AND WHY. The three degenerate-extent refusals and the
post-refusal GETJPI check never reach vms_ioctl_p0_map's success path at
all, so the deleted stores are never in their control flow. B's own row
(no extent) is unaffected -- B never calls P0_MAP on itself in this
suite. VMS_IOCTL_P0_UNMAP's status and its "GETJPI shows nothing after
unmap" follow-on both stay green for an unfortunate reason worth stating
plainly: unmapping an already-empty extent is defined as success (see
vms_p0.c's own header), so clearing a p0_base/p0_limit that the mutation
had already left at zero is indistinguishable from clearing a real one --
a coincidentally correct answer to a differently-broken question, the
same caveat mbx-not-shared's own knock_on_why records for its final
mailbox-is-gone assertion. The idempotent-second-unmap assertion is
unaffected for the identical reason.
EOF
                      ;;
        esac;;

    p1-map-not-recorded)
        case "$_f" in
        facility)     echo "P1 control-region bookkeeping (VMS_IOCTL_P1_MAP, vms-68f.ii -- increment (ii) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II)";;
        targets)      echo "kernel/vms_p1.c";;
        suites_red)   echo "test_kmod_p1";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_p1_map() validates the extent, then SKIPS the two stores under proc->p1_lock (proc->p1_base = args.base; proc->p1_limit = args.limit;) while still returning SS\$_NORMAL -- the same INV-6 shape p0-map-not-recorded catches on the P0 side: the ioctl REPORTS success without the executive actually holding the fact. Every validation-only assertion (the three degenerate-extent refusals, and the post-refusal GETJPI check) is untouched, because none of them ever reaches the deleted stores; only the assertions that read the extent BACK after a claimed-successful MAP can tell the difference -- and because this suite's whole P0/P1-persistence section compares AGAINST that never-stored extent, the miss propagates through every later P1 check in the suite, not just the first one.";;
        require_fail) cat <<'EOF'
GETJPI reflects the registered P1 extent: p1_base/p1_limit match what VMS_IOCTL_P1_MAP just registered
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
A-WRITES/B-READS: B reads A's registered P1 extent out of A's row -- a fact a per-process notion of P1 could not show
after P0_MAP: P1 extent still matches what was registered before any P0 activity, and P0 is now also present
P1-SURVIVES: GETJPI still reflects the SAME P1 extent after this P0 teardown -- the property this increment exists to prove, not just once but across a repeated P0 map/unmap cycle
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED EVERYWHERE ELSE IN THE SUITE THAT READS P1. proc->
p1_base/p1_limit never leave zero after the deleted stores, so every LATER
comparison against the real (non-zero) window this suite registered comes
back false: process B's cross-process GETJPI(PID=A) sees no extent (the
p1_base/p1_limit copied into struct vms_procinfo by proc_fill_info() are
the same zeroed fields the mutation left behind); the post-P0_MAP check in
the P0/P1 cycle loop, which compares P1 against the extent registered
BEFORE any P0 activity, has nothing but zero on the executive's side to
compare against; and the "P1-SURVIVES" check, which is really asking
"is P1 still what VMS_IOCTL_P1_MAP registered", is asking about a fact
that was never true even once. One deleted pair of stores, four
assertions naming the same missing fact from four different call sites
across the whole suite -- not four independent properties, and not the
same set p0-unmap-clears-p1 reddens: that defect leaves the FIRST cycle's
post-P0_MAP check green (P1 was genuinely registered, and P0_UNMAP had not
yet run), where this defect fails it on every cycle from the very first,
because P1 was never registered at all.

WHAT STAYS GREEN, AND WHY. The three degenerate-extent refusals and the
post-refusal GETJPI check never reach vms_ioctl_p1_map's success path at
all. VMS_IOCTL_P1_MAP's own reported status stays SS$_NORMAL (the mutation
never touches args.status), so "VMS_IOCTL_P1_MAP registers the window"
stays green -- it is exactly the fabricated-success half of INV-6 this
entry exists to catch. B's own row (no P1 extent) is unaffected -- B never
calls P1_MAP on itself. Every P0-only assertion (VMS_IOCTL_P0_MAP's own
status, VMS_IOCTL_P0_UNMAP's status and its "P0-DELETED-ON-RUNDOWN" GETJPI
check) is unaffected: this mutation never touches proc->p0_base/p0_limit
or vms_p0.c at all.
EOF
                      ;;
        esac;;

    p0-unmap-clears-p1)
        case "$_f" in
        facility)     echo "P1 control-region persistence -- P0 deleted on rundown, P1 survives (vms-68f.ii -- increment (ii) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II)";;
        targets)      echo "kernel/vms_p0.c";;
        suites_red)   echo "test_kmod_p1";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_p0_unmap() (kernel/vms_p0.c) -- the executive's half of image rundown -- ADDITIONALLY clears proc->p1_base/p1_limit right after it clears proc->p0_base/p0_limit, as if P0 teardown and P1 teardown were the same event. This is exactly the defect the P0/P1 SEPARATION exists to make structurally impossible: on the real design (§A.1.1) P1 is process-permanent and has no rundown counterpart at all, so nothing may clear it from image rundown's code path. The mutation puts that missing distinction back in, sharing P0_UNMAP's clear path with fields P0_UNMAP has no business touching.";;
        require_fail) cat <<'EOF'
P1-SURVIVES: GETJPI still reflects the SAME P1 extent after this P0 teardown -- the property this increment exists to prove, not just once but across a repeated P0 map/unmap cycle
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
after P0_MAP: P1 extent still matches what was registered before any P0 activity, and P0 is now also present
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED ON THE NEXT CYCLE. test_kmod_p1.c registers one P1
extent, then cycles VMS_IOCTL_P0_MAP/P0_UNMAP three times, checking after
EVERY unmap that P1 is unchanged (this is require_fail, and it goes red on
the FIRST cycle's unmap: the mutation zeroes p1_base/p1_limit right then).
Once p1_base/p1_limit are zero, the NEXT cycle's post-P0_MAP assertion --
which checks P1 still matches the extent registered before any P0 activity
at all, alongside P0's own now-mapped extent -- has nothing left to match
against, so it goes red too on cycles 2 and 3. One deleted-boundary mutation
in vms_ioctl_p0_unmap, observed as two distinct assertion texts because one
fires on unmap and the other on the following map, not two independent
properties.

WHAT STAYS GREEN, AND WHY. Every assertion before the P0/P1 cycle loop
(the fresh-process check, the three degenerate-extent refusals, the
post-refusal GETJPI check, the initial P1_MAP registration and its GETJPI
readback, and the A-WRITES/B-READS cross-process pair) completes before
vms_ioctl_p0_unmap is ever called in this suite, so none of them can reach
the mutated code path. VMS_IOCTL_P0_MAP's own status and the
"P0-DELETED-ON-RUNDOWN" GETJPI-shows-no-P0-extent check are unaffected in
every cycle: the mutation adds clears, it does not remove the ones already
there, so P0's own bookkeeping is exactly as correct as it was in
p0-map-not-recorded's absence. test_kmod_p0.c -- the OTHER suite exercising
VMS_IOCTL_P0_MAP/P0_UNMAP -- never registers or reads a P1 extent at all,
so it has nothing in its own assertions this mutation's added clears could
ever touch; it stays fully green.
EOF
                      ;;
        esac;;

    super-mode-escalation)
        case "$_f" in
        facility)     echo "access-mode transition + boundary enforcement (VMS_IOCTL_SETMODE/ENTER_IMAGE/IMAGE_RUNDOWN, vms-68f.iii -- increment (iii) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.3)";;
        targets)      echo "kernel-core/vms_access.c";;
        suites_red)   echo "test_kmod_modeswitch";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_setmode()'s raise-to-SUPER branch stops requiring CMEXEC/CMKRNL, reverting to this item's pre-fix behaviour (\"to super mode: from user only, no special priv\") where SUPER (2) is a MORE privileged mode than USER (3) by the function's own numbering, so an unprivileged caller could self-promote into the mode the design reserves for DCL's command loop. One disjunct removed from one condition, mirroring exactly how access-mode-escalation inverts the KERNEL-mode check.";;
        require_fail) cat <<'EOF'
SUPER mode DENIED without CMEXEC/CMKRNL (SS$_NOPRIV)
... and the mode is still USER after the denied SUPER escalation
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    image-rundown-without-entry)
        case "$_f" in
        facility)     echo "access-mode transition + boundary enforcement (VMS_IOCTL_ENTER_IMAGE/IMAGE_RUNDOWN, vms-68f.iii -- increment (iii) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.3)";;
        targets)      echo "kernel-core/vms_access.c";;
        suites_red)   echo "test_kmod_modeswitch";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_image_rundown() stops requiring proc->image_active before restoring current_mode, so a caller that never went through a matching VMS_IOCTL_ENTER_IMAGE -- current_mode is PSL_C_USER by default at registration, image_active is 0, pre_image_mode is whatever kmem_cache_zalloc() left it (PSL_C_KERNEL, 0) -- can call the RETURN leg of the controlled transition cold and have the executive move it to pre_image_mode anyway. This is the exact 'cannot raise its own mode except via the controlled transition' property the design's negative control (§A.5 item 5) and this item's own task both name: the guard, not current_mode, is what makes RUNDOWN a paired return instead of a second SETMODE.";;
        require_fail) cat <<'EOF'
IMAGE_RUNDOWN refused with no active image entry
... and the mode is still USER after the refused IMAGE_RUNDOWN -- no door reaches Supervisor without the controlled transition
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    image-rundown-leaks-user-lock)
        case "$_f" in
        facility)     echo "SYS\$RUNDWN image-scoped resource release (VMS_IOCTL_IMAGE_RUNDOWN, vms-68f.v -- increment (v) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.1 step 2 / §A.6.1; class grounding docs/design-image-rundown-resource-classes.md)";;
        targets)      echo "kernel-core/vms_access.c";;
        suites_red)   echo "test_kmod_rundown";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_image_rundown() stops calling vms_proc_rundown_locks(), so a lock the activated image \$ENQed at USER access mode is NOT dequeued when the image runs down -- it survives into DCL with a still-valid lock ID, leaking image-scoped executive state across the activation. This is exactly the 'rundown completeness' the design (§A.6.1) names as the hard part: get the image-scoped-vs-process-permanent split wrong and resources leak across images in one process (a bug the fork model never had because the child's death was a sledgehammer). The mutation removes ONLY the lock release: the Supervisor-mode (process-permanent) lock was never in the release set so it is unaffected, and the sibling channel and AST releases in the same handler are untouched -- the image still runs down and returns to Supervisor. Only the one assertion that reads the image lock back can tell the difference. One minimal mutation, one property.";;
        require_fail) cat <<'EOF'
image rundown DEQUEUED the User-mode lock (its lkid is now invalid)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    imgact-p1-not-protected)
        case "$_f" in
        facility)     echo "in-process image activation, critical-P1 protection (imgact_activate() -> vms_kif_p1_protect, vms-68f.iv -- increment (iv) of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.3(b))";;
        targets)      echo "libvms/syssvc/sys_imgact.c";;
        suites_red)   echo "test_syssvc_imgact_inproc";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_activate() STOPS mprotect'ing the caller's critical-P1 range read-only before it descends to User and enters the in-process image: the vms_kif_p1_protect(critp1->base, critp1->limit, 0) call is replaced by an unconditional SS\$_NORMAL, so the image runs with DCL's crown-jewel P1 pages still writable. This is the exact enforcement §A.2.3(b) names -- the ENFORCED half of the access-mode model -- and the defect is the LARP shape it exists to kill: activation reports the image ran (and it did) while the protection that makes a User-mode image unable to corrupt DCL's P1 was never applied. A hostile image's wild write then SUCCEEDS instead of faulting, so it neither takes an access violation nor leaves the sentinel intact. The benign path is untouched (it never writes the page), and the ENTER_IMAGE/rundown/P0 mechanics are untouched (the image still runs in-process and returns), so only the two assertions that read the protection back can tell the difference -- one minimal mutation, one property.";;
        require_fail) cat <<'EOF'
the critical-P1 sentinel is INTACT -- the User-mode image could not corrupt DCL's P1 (the protection held at the MMU)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the image's wild write to the protected P1 page was an access violation
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED ON THE OTHER SIDE OF THE SAME WRITE. With the page left writable the hostile image's store completes instead of faulting, so imgact_activate() returns SS\$_NORMAL rather than SS\$_ACCVIO -- the missing fault (this assertion) and the corrupted sentinel (require_fail) are the two visible faces of the one skipped mprotect, not two independent properties.";;
        esac;;

    consumer-import-not-bound-to-resident)
        case "$_f" in
        facility)     echo "in-process image activation, .vms\$imp import binding to an ALREADY-RESIDENT shareable (imgact_bind_imports_resident(), vms-db2 -- the §A.8-remainder import-binding-to-resident-shareable sub-step of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.2)";;
        targets)      echo "libvms/syssvc/imgact_prodreg.c";;
        suites_red)   echo "test_imgact_bind";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_bind_imports_resident() STOPS writing the resolved RESIDENT producer address into the consumer's GOT cell (the one \`*cell = addr;\` store replaced by a no-op that keeps cell/addr referenced), so binding still resolves the universal and still returns SS\$_NORMAL -- it just never points the consumer at the resident producer. This is the exact LARP shape the authenticity invariants exist to kill: activation reports the import bound (and the status says so) while the image shares NOTHING with the resident LIBVMS\$SHR/DECC\$SHR the process already holds -- its call lands on its pre-bind stub, not the one instance DCL uses, so a \$CRELNM/DEFINE it made would never flow back. The registry lookup, GSMATCH resolve and the malformed/not-resident/mismatch refusals are all untouched (they run before this store or on other paths), so only the assertions that read the bound cell back and observe the SHARED counter can tell the difference -- one minimal mutation, one property.";;
        require_fail) cat <<'EOF'
the consumer's imported call reached the RESIDENT producer -- it returned the resident counter value 1, not the unbound stub
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the test sees the consumer's increment to the SAME resident counter (genuine sharing, not a private copy)
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED FROM THE OTHER PARTY TO THE SHARED STATE. With the GOT cell left at its stub the consumer never increments the resident counter, so when the test (as DCL) calls the same producer directly it sees 1, not 2 -- the consumer's missing effect (this assertion) and the consumer's call landing on the stub (require_fail) are the two visible faces of the one skipped store, not two independent properties: both say the image bound to nothing resident.";;
        esac;;

    publish-does-not-populate-registry)
        case "$_f" in
        facility)     echo "in-process image activation, PUBLISHING the resident-producer registry at runtime (imgact_publish_producers(), vms-db2 -- the §A.8-remainder gap 1 of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.8: IMGACT hands the LIBVMS\$SHR/DECC\$SHR bases it mapped across to the registry so a later in-process RUN can bind to them)";;
        targets)      echo "libvms/syssvc/imgact_prodreg.c";;
        suites_red)   echo "test_imgact_publish";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_publish_producers() STOPS calling imgact_register_producer for each producer in the list (the one \`imgact_register_producer(...)\` call replaced by a no-op that keeps prods referenced and leaves st = SS\$_NORMAL), so publish still returns SS\$_NORMAL -- it just records NOTHING. This is the LARP shape the authenticity invariants exist to kill: publish reports success (and the status says so) while the registry stays EMPTY, exactly the runtime state IMGACT left it in before this gap was closed -- so a later in-process RUN's .vms\$imp import finds no resident producer, imgact_bind_imports_resident refuses it (SS\$_UNSUPPORTED) and the caller forks, sharing nothing with the resident LIBVMS\$SHR/DECC\$SHR. The malformed-argument refusals (NULL list, negative count) return before this loop and are untouched, so only the assertions that publish a producer and then observe a consumer reach its SHARED counter can tell the difference -- one minimal mutation, one property.";;
        require_fail) cat <<'EOF'
the consumer's imported call reached the PUBLISHED resident producer -- it returned the resident counter value 1, not the unbound stub
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the test sees the consumer's increment to the SAME resident counter publish made reachable (genuine sharing, not a private copy)
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED FROM THE OTHER PARTY TO THE SHARED STATE. With the registry left empty by the skipped publish the consumer's import is refused and its GOT cell stays at its stub, so it never increments the resident counter; when the test (as DCL) calls the same producer directly it sees 1, not 2. The consumer's missing effect (this assertion) and the consumer's call never reaching the resident producer (require_fail) are the two visible faces of the one skipped registry population, not two independent properties: both say publish recorded nothing, so nothing could bind.";;
        esac;;

    realimg-auxv-argc-wrong)
        case "$_f" in
        facility)     echo "in-process image activation, SysV auxv _start entry for a REAL image (imgact_activate() -> imgact_build_auxv_stack + imgact_enter_auxv, vms-db2 -- the §A.8-remainder gap 2 + FLIP of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.1: a real image is entered through the constructed argc/argv/envp/auxv stack, and its SYS\$EXIT returns to DCL)";;
        targets)      echo "libvms/syssvc/sys_imgact.c";;
        suites_red)   echo "test_syssvc_imgact_realimg";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_build_auxv_stack() lays argc = 0 instead of 1 onto the initial process stack (the one \`w[k++] = 1;\` for argc replaced by \`w[k++] = 0;\`), so the real image is still ENTERED through the SysV _start ABI and still runs, but the auxv stack it reads is WRONG. This is the LARP shape the flip must not have: activation reports SS\$_NORMAL (the image ran, control returned) while the constructed stack -- the whole point of the real-image entry ABI -- carried a wrong argc. TESTREAL reads argc off that stack; seeing it != 1 it takes its bad-ABI SYS\$EXIT code and touches NEITHER the executive event flag NOR the resident TLS, so the image's exit code comes back wrong and its process-permanent flows-back never happens. The mapping, mode descent, rundown and P0 teardown are untouched (the image still runs in-process at the same PID and returns), so only the assertions that read the image's SYS\$EXIT code and its flows-back can tell the difference -- one minimal mutation, one property (the auxv stack is load-bearing, not decorative).";;
        require_fail) cat <<'EOF'
SYS$EXIT returned control to DCL with the image's exit code -- the command loop resumed in the same process (not a fault, not a process exit)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
a process-permanent event flag SET by the in-process real image is visible to DCL after rundown -- executive flows-back the fork (Option B, per-child flags) never could
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED THROUGH THE EXECUTIVE FLOWS-BACK. With argc laid wrong the in-process real image never reaches its resident vms_kif_setef call (it exits early on the bad-ABI branch), so the event flag DCL reads back afterwards is clear. The wrong exit code (require_fail) and the missing event flag (this assertion) are two faces of the one mis-built auxv stack, not two independent properties: both say the image was handed a stack it could not trust, so it did none of its resident-service work.";;
        esac;;

    extern-interp-check-rejects-ours)
        case "$_f" in
        facility)     echo "in-process image activation, ACCEPTING a PT_INTERP that names the OVMX loader for a genuinely EXTERNAL image (imgact_activate() -> imgact_interp_is_ours, vms-db2 -- the §A.8-remainder item 2 / external-image flip of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.8: a real LINK.EXE executable names IMGACT.EXE in PT_INTERP, and in-process activation IS that loader, so the image runs in DCL's process instead of forking)";;
        targets)      echo "libvms/syssvc/sys_imgact.c";;
        suites_red)   echo "test_syssvc_imgact_extern";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_interp_is_ours() returns 0 for EVERY interpreter (the final \`return want[j] == '\\0' && bn[j] == '\\0';\` replaced by \`return 0;\`), so an image that names the OVMX loader (IMGACT.EXE) in its PT_INTERP -- a genuinely external LINK.EXE executable -- is now treated as FOREIGN-interpreted and REFUSED the in-process path (SS\$_UNSUPPORTED -> the caller forks). This is exactly the class the external-image flip exists to run in-process: the PT_INTERP acceptance is what makes a real linker-produced executable eligible. TESTEXTERN.EXE carries such a PT_INTERP; with the check mutated to reject it, imgact_activate refuses TESTEXTERN and it never runs in DCL's process, so the suite's SS\$_NORMAL / same-PID / SYS\$EXIT-returned / flows-back / shared-TLS assertions all fail. The foreign-interp control (TESTEXTERN_FOREIGN) still returns SS\$_UNSUPPORTED under the mutation, and the no-PT_INTERP TESTREAL image (interp_sz == 0) never reaches this check, so ONLY the external-image flip assertions can tell the difference -- one minimal mutation, one property (accepting the OVMX loader's own PT_INTERP is load-bearing, not decorative).";;
        require_fail) cat <<'EOF'
the EXTERNAL image (with a PT_INTERP naming the OVMX loader) ran in-process and returned SS$_NORMAL
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the VMS PID is UNCHANGED across the RUN of an EXTERNAL image -- no fork (the crux)
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED THROUGH THE NO-FORK CRUX. With imgact_interp_is_ours rejecting the OVMX loader's own name, TESTEXTERN is refused the in-process path entirely -- it never activates in DCL's process, so the VMS PID recorded after the (failed) activation is not compared against a run that happened in-process. The refused SS\$_NORMAL (require_fail) and the unproven same-PID (this assertion) are two faces of the one over-strict interp check: both say the external image was denied the in-process path its PT_INTERP naming the OVMX loader should have granted it.";;
        esac;;

    nonres-producer-not-mapped)
        case "$_f" in
        facility)     echo "in-process image activation, MAPPING a NON-RESIDENT producer shareable so a real image's .vms\$imp import can bind to it (imgact_activate() -> imgact_map_producer, vms-db2 -- the §A.8-remainder item 1 non-resident-producer sub-step of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.2: an image may import a universal from a shareable the process does NOT hold; the activator maps it in-process, once/process-permanent, and binds to that one instance)";;
        targets)      echo "libvms/syssvc/sys_imgact.c";;
        suites_red)   echo "test_syssvc_imgact_nonres";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_map_producer() STOPS registering the producer it just mapped (the one \`uint32_t status = imgact_register_producer(soname, (uint64_t)base, sv);\` replaced by \`status = SS\$_UNSUPPORTED;\`), so the producer is genuinely mapped into the process but never entered into the resident-producer registry -- imgact_find_producer therefore still does not find it, imgact_bind_imports_mapped leaves the consumer's prod_bump import unbound, and imgact_activate returns SS\$_UNSUPPORTED so dcl_activate_image FORKS the image instead of running it in-process. This is exactly the class the non-resident-producer flip exists to run in-process: TESTNONRES imports prod_bump from TESTPROD, a producer DCL does not hold; with the registration skipped the mapping does nothing usable, so the suite's SS\$_NORMAL / same-PID / shared-counter assertions all fail (the image forks). The resident-producer path (realimg/extern, whose producers ARE published) never calls imgact_map_producer, and the import-binding store (consumer-import-not-bound-to-resident) is a different line in a different file, so ONLY the non-resident flip's assertions can tell the difference -- one minimal mutation, one property (registering the mapped producer is load-bearing, not decorative).";;
        require_fail) cat <<'EOF'
the second run's bump returned 101 -- it reached the SAME mapped producer instance the suite set to 100 (a per-consumer private copy would have returned 1) -- genuine single-instance sharing
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
TESTNONRES ran in-process and returned SS$_NORMAL
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED THROUGH THE FIRST RUN. With the mapped producer never registered, the consumer's prod_bump import cannot bind on EITHER run, so imgact_activate refuses the image (SS\$_UNSUPPORTED) and the first run never activates in-process -- there is no SS\$_NORMAL, no banner, no counter to share. The refused first run (this assertion) and the unshared second-run counter (require_fail) are two faces of the one skipped registration: both say the mapped producer was invisible to the binder, so nothing could bind to it.";;
        esac;;

    tlsdesc-offset-not-biased)
        case "$_f" in
        facility)     echo "in-process image activation, GIVING an own-PT_TLS image its OWN TLS block biased against DCL's thread pointer (imgact_activate() -> imgact_setup_own_tls, vms-db2 -- the §A.8-remainder item 4 DTV-append / TLS-absorb sub-step of the Option A in-process image activation design, docs/design-in-process-activation.md Part II §A.2.2 + §A.8: an image with its OWN PT_TLS gets a distinct per-thread block whose .vms\$tls TLSDESC entries resolve to the image's block via DCL's UNCHANGED thread pointer, so its __thread data is its own while DCL's TP/TLS are never reprogrammed)";;
        targets)      echo "libvms/syssvc/sys_imgact.c";;
        suites_red)   echo "test_syssvc_imgact_tls";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "imgact_setup_own_tls() STOPS adding the block's TP-relative bias to each .vms\$tls TLSDESC entry (the one \`desc[1] += delta;\` replaced by \`desc[1] += 0;\`), so the descriptor keeps only the variable's MODULE-relative offset (0 for the first datum) instead of (block - TP) + module_offset. The resident resolver then returns 0 and the image's TLSDESC access computes TP + 0 -- landing in DCL's OWN TLS block, NOT the image's. This is the exact clobber the own-PT_TLS increment exists to avoid: the image is still entered in-process (mapping, mode descent, rundown untouched), but its __thread access reads DCL's TLS instead of its own private block. TESTTLS reads its own datum first and, seeing it is not the init magic the activator copied into the image's block, takes its distinct bad-TLS-init SYS\$EXIT code and touches NOTHING else (no resident bump, no event flag, no own-TLS write -- so no actual corruption of DCL's TLS, only a wrong read). Only the assertions that read the image's SYS\$EXIT success code and its flows-back can tell the difference -- one minimal mutation, one property (biasing the TLSDESC offset against DCL's TP is load-bearing, not decorative).";;
        require_fail) cat <<'EOF'
SYS$EXIT returned control to DCL with the image's success code -- the image read its OWN __thread init magic, wrote and re-read its own TLS, and bumped the resident TLS, all before exiting (own TLS correct)
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
a process-permanent event flag SET by the in-process own-PT_TLS image is visible to DCL after rundown (executive flows-back)
EOF
                      ;;
        knock_on_why)  echo "SAME DEFECT, OBSERVED THROUGH THE EXECUTIVE FLOWS-BACK. With the TP bias dropped, TESTTLS's very first own-TLS read returns DCL's TLS (not its init magic), so it bails on the bad-TLS-init branch BEFORE reaching its resident vms_kif_setef call -- the event flag DCL reads back afterwards is clear. The wrong exit code (require_fail) and the missing event flag (this assertion) are two faces of the one dropped bias: both say the image's own __thread access did not reach the image's own block, so it did none of its later resident-service work.";;
        esac;;

    devtab-getdvi-userspace-alloc-dropped)
        case "$_f" in
        facility)     echo "device table -- sys\$getdvi's USERSPACE translation of the executive's allocation bit (DVI\$_DEVCHAR)";;
        targets)      echo "libvms/syssvc/sys_device.c";;
        # vms-vx1 (executive R3 veracity gate). THE ONLY device-table entry
        # whose target is the USERSPACE C-API layer, not the executive.
        #
        # WHY THIS EXISTS, distinct from the five kernel devtab entries above.
        # devtab-alloc-not-recorded reddens the SAME assertion ("B sees that A
        # allocated the device"), but it mutates the EXECUTIVE (devinfo_fill()
        # in kernel-core/vms_devtab.c), so it reddens that assertion in ALL THREE of
        # test_kmod_devtab, test_syssvc_showdev AND test_syssvc_getdvi at once.
        # The gating teeth of a negctl are check 4 (a suite in suites_red went
        # red), which the raw test_kmod_devtab satisfies regardless of what the
        # userspace sys_device.c translation does -- so a devtab reader that
        # silently reverted to a per-process view (the vms-dv1 fake class:
        # classify_device()+statvfs()+a compiled-in scan_devices[] that showed
        # the same devices on every system) would NOT be caught by any devtab
        # entry above. THIS entry closes that: its target is sys_device.c's own
        # DVI$_DEVCHAR translation, and its suites_red is test_syssvc_getdvi
        # ALONE -- because that is the only suite that reaches the executive's
        # allocation state THROUGH the shipped public sys$getdvi (SHOW DEVICE
        # drives DCL.EXE, which calls vms_kif_* directly and never enters
        # sys_device.c; test_kmod_devtab uses the raw ioctls). So this control
        # fires IFF the userspace C API stops reflecting the executive's shared
        # allocation -- the exact "cannot silently revert to a per-process
        # fake" property vms-vx1 (INV-6) exists to enforce, made a first-class
        # firing negctl at the C-API layer rather than left implicit in the
        # kernel-executive-negative-control honest-skip-77 job.
        #
        # test_syssvc_getdvi step 4 ("B sees that A allocated the device") is a
        # genuine A-writes/B-reads: a forked child $ALLOCates OPA0: through the
        # executive and THIS process, which touched nothing, must observe the
        # DEV$M_ALL bit via sys$getdvi. Masking the one line that OR's
        # DEV$M_ALL in from info->allocated makes the C API report the console
        # unallocated even while a DIFFERENT process holds it -- a per-process
        # view. Steps 2 and 5 ("not allocated before"/"...after") still pass:
        # a bit that is NEVER set is correctly not-set in those windows, so the
        # red set is exactly this one assertion, in this one suite.
        suites_red)   echo "test_syssvc_getdvi";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sys_device.c's DVI\$_DEVCHAR handler stops OR-ing DEV\$M_ALL in from the executive's info->allocated flag (\`if (info->allocated) chars |= DEV\$M_ALL;\` guarded to \`if (0 && ...)\`), so sys\$getdvi -- the shipped public C API -- reports a device as unallocated even when the executive says another process holds it. The row's identity, class and existence still come from the executive (nothing else in the translation changes); only the allocation bit is dropped on the read side, which is precisely the per-process device view vms-dv1 converted away from. SHOW DEVICE (DCL -> vms_kif directly) and the raw-ioctl test_kmod_devtab do not pass through this line and stay green, isolating the failure to the C-API translation layer.";;
        require_fail) cat <<'EOF'
B sees that A allocated the device
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    # -------------------------------------------------------------------
    # vms-6d6: the three suites #284/#272 (vms-420, vms-d37) left with NO
    # per-facility negative control at all -- the same coverage-gate hole
    # vms-6bdf/#293 just closed for test_kmod_resdir. Baselined before this
    # change: `coverage` named all three in both "NAMED BY NO defect's
    # suites_red" and "in-scope suite(s) with NO anchor at all".
    # -------------------------------------------------------------------

    lnm-searchlist-equiv-truncated)
        case "$_f" in
        facility)     echo "executive-resident multi-value (search-list) logical names -- the stored equivalence COUNT (VMS_IOCTL_LNM_DEFINE's num_equiv field, kernel-core/vms_lnm.c), vms-420";;
        targets)      echo "kernel-core/vms_lnm.c";;
        # No other suite in this manifest's known set calls lnm_create_multi
        # or reads a logical at any equivalence index above 0 (grep across
        # tests/qemu confirms it: test_syssvc_lnm_privilege is the only other
        # vms_kif_lnm_translate() caller with an explicit index argument, and
        # it always passes 0). A single-value DEFINE always carries
        # a->num_equiv == 1, so pinning the stored count to 1 is a no-op for
        # every one of them.
        suites_red)   echo "test_syssvc_lnm_searchlist";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_lnm_define()'s NEW-ENTRY branch (the fresh-arena-slot path; the SUPERSEDE branch a few lines above it, taken only when the name is already defined, is untouched) pins the stored e->num_equiv to 1 regardless of how many equivalence strings the caller's DEFINE/SYSTEM actually supplied. The full equiv[] array is still memcpy'd into the entry whole -- every value the caller wrote is genuinely IN the arena -- but the entry's own recorded COUNT says there is only one, so every reader that trusts that count (the mmap TRANSLATE-by-index path, lnm_translate_values(), SHOW LOGICAL's enumerate) sees a one-value logical no matter how many were defined. This is the vms-420 defect itself (DEFINE FOO BAR,BAZ silently dropping BAZ) reintroduced at the one call site that stores the count -- test_syssvc_lnm_searchlist.c exists specifically to catch a regression here, and its TEST_NAME is namespaced so this suite's DEFINE always lands on the NEW-ENTRY branch, never SUPERSEDE.";;
        require_fail) cat <<'EOF'
child: equivalence index 1 (previously dropped, vms-420) matches the parent's define
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
parent: reads its own multi-value SYSTEM define back with both values, in order
child: the parent's SYSTEM search-list logical reports BOTH equivalence strings here
child: equivalence index 0 matches the parent's define
child: vms_kif_lnm_translate index 0 matches and reports num_equiv=2
child: vms_kif_lnm_translate index 1 matches and reports num_equiv=2
child: vms_kif_lnm_translate index 2 (past the end) reports "no more", not a fabricated third value
child: SHOW LOGICAL's enumerate path lists BOTH equivalence strings for the parent's SYSTEM search list
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME DEFECT, OBSERVED AT EVERY READER OF THE SAME COUNT. num_equiv is the
single field every consumer of a multi-value logical trusts to know how many
equivalence strings exist: lnm_translate_values() (what SHOW LOGICAL and
F$TRNLNM sit on top of), the raw vms_kif_lnm_translate() index walk, and
lnm_enumerate()'s listing path all read it, and all three stop believing a
second value exists once it is pinned to 1 -- one corrupted count, observed
at every place this suite reads it back, not several independent defects.
That is why they are named here as knock-on rather than split into
per-reader entries.

WHAT STAYS GREEN, AND WHY. The negative control ("a never-defined name is
absent") never touches the mutated branch. "parent: lnm_create_multi ...
reported success" stays green too: the ioctl still returns SS$_NORMAL --
only the stored count is wrong, not the status the caller sees.
EOF
                      ;;
        esac;;

    rightslist-general-hex-as-decimal)
        case "$_f" in
        facility)     echo "the rights database's GENERAL identifiers (RIGHTSLIST.DAT's %Xnnnnnnnn value notation, parse_value() in src/libvms/rtl/rightslist.c), vms-2f8";;
        targets)      echo "libvms/rtl/rightslist.c";;
        # test_syssvc_ident.c's own LOCAL assertion (a GENERAL identifier,
        # same reader, same %X notation) is a genuine, measured knock-on --
        # see knock_on_why. Nothing else in this manifest's known suites
        # reads a general (non-UIC) rights-database identifier: rightslist
        # UIC checks (DEFAULT/SYSTEM/GUEST) and test_syssvc_sysuaf_uic_base
        # go through sysuaf_lookup()'s OCTAL branch of the SAME parse_value(),
        # a completely separate code path this mutation does not touch (see
        # sysuaf-uic-radix-decimal, which targets that branch instead).
        suites_red)   echo "test_syssvc_rightslist test_syssvc_ident";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "parse_value()'s %Xnnnnnnnn branch -- the notation every GENERAL identifier in RIGHTSLIST.DAT (BATCH, DIALUP, INTERACTIVE, LOCAL, NETWORK, REMOTE) is written in -- stops reading the digits after %X as hexadecimal and reads them as decimal instead. The row is still FOUND (the colon-delimited scan, the name match and the case-insensitive compare are all untouched); only the value column is misread, so rightslist_name_to_value()/rightslist_value_to_name() answer a wrong number for every general identifier while the UIC branch ([g,m], a few lines below, OCTAL) is a completely different code path and is not reached by this edit at all.";;
        require_fail) cat <<'EOF'
oracle: DCL prints -2147483644
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
oracle: DCL prints -2147483647
oracle: absent from OVMX entirely before vms-2f8
oracle: DCL prints -2147483645
oracle: DCL prints -2147483643
oracle: DCL prints -2147483642
reverse resolves (oracle round-trips it)
lookup is case-insensitive ("local" resolves like "LOCAL")
holdable by no hardcode and derivable from no account's UIC
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME PARSE, OBSERVED AT EVERY GENERAL-IDENTIFIER READER. All six general
identifiers RIGHTSLIST.DAT carries (BATCH, DIALUP, INTERACTIVE, LOCAL,
NETWORK, REMOTE) are written in the same %Xnnnnnnnn notation, so every one
of test_syssvc_rightslist.c's six check_roundtrip() calls exercises the
mutated branch -- both directions (name->value, then the now-orphaned
value's reverse lookup) -- not LOCAL alone. require_fail names LOCAL's own
value-mismatch text (the anchor's statement); knock_on_fail's first five
lines are the other five identifiers' own oracle notes (each fires the same
value-mismatch shape LOCAL's does), the sixth is the shared reverse-lookup
failure text every one of the six hits once its mutated value stops
matching any row, and the seventh is the case-insensitive "local" lookup,
which resolves through the identical mutated branch. test_syssvc_ident.c's
own F$IDENTIFIER("LOCAL", ...) assertion (eighth line, a substring of its
message) reddens too -- LOCAL is the one general identifier that suite
drives through the real DCL.EXE (see that suite's own comment: "the one
that cannot be faked by the old shape") -- a second suite reached through
the exact same reader, not a second defect. A full run of
run_facility_negctl.sh is what confirms the complete red set, exactly as
vms-6bdf's resdir control was verified.

WHAT STAYS GREEN, AND WHY. rightslist.c's UIC checks (DEFAULT/SYSTEM/GUEST)
and every check_miss_value() call are untouched: they never pass through
the %X branch this edit mutates (misses 1..5, 1000, 0, 77777, 196609 are
not valid %Xnnnnnnnn rows to begin with, and 0x80010004 is asserted absent
either way). test_syssvc_sysuaf_uic_base.c does not call
rightslist_name_to_value()/rightslist_value_to_name() at all.
EOF
                      ;;
        esac;;

    sysuaf-uic-radix-decimal)
        case "$_f" in
        facility)     echo "SYSUAF.DAT's UIC fields -- the shared read radix (SYSUAF_UIC_RADIX, src/libvms/include/sysuaf.h, consulted only by sysuaf_lookup()'s field parse in src/libvms/rtl/sysuaf.c), vms-e60";;
        targets)      echo "libvms/include/sysuaf.h";;
        # test_syssvc_rightslist.c's OWN UIC checks (DEFAULT/GUEST, via
        # sysuaf_lookup()), test_syssvc_ident.c's DEFAULT checks (via the
        # same reader, one layer up through F$IDENTIFIER) and
        # test_syssvc_setuai.c's UIC-write-back readback (scenario 4) are
        # genuine, MEASURED knock-on -- see knock_on_why. This suites_red set
        # was corrected after running run_facility_negctl.sh for real: an
        # earlier draft predicted setuai would be untouched (sysuaf_format_
        # record(), the WRITER, uses a literal %o and does not consult this
        # constant) -- true for the WRITE, but scenario 4 also READS the row
        # back afterward through the same mutated sysuaf_lookup(), and the
        # real run reddened it. Declared here rather than left as an
        # unattributed stray, per this manifest's own method rule.
        suites_red)   echo "test_syssvc_sysuaf_uic_base test_syssvc_rightslist test_syssvc_ident test_syssvc_setuai";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "SYSUAF_UIC_RADIX -- the ONE constant sysuaf_lookup()'s field parse applies to SYSUAF.DAT's UIC_GROUP/UIC_MEMBER columns (vms-e60: every VMS UIC is written in octal) -- is changed from 8 to 10, so a shipped record like DEFAULT's '200|200' is read as decimal 200/200 instead of octal 128/128. SYSTEM's '1|4' is unaffected (identical in both bases, the LIVENESS ANCHOR every one of these suites' own comments name it as -- as is OPERATOR's '1|6'; TWO shipped rows read the same in both bases, not one, both being single-octal-digit rows, measured vms-f57); DEFAULT and GUEST, whose fields actually discriminate the base, are not. sysuaf_format_record() (the writer) uses a literal %o unconnected to this constant, so a WRITE alone is not reached through THIS mutation -- but any suite that reads a record back afterward through sysuaf_lookup() (test_syssvc_setuai's scenario 4) is. The writer's own octal-ness has its own dedicated control, sysuaf-uic-writeback-decimal.";;
        require_fail) cat <<'EOF'
member field is octal as well as group
EOF
                      ;;
        knock_on_fail) cat <<'EOF'
the oracle's 8388736; decimal parsing gives 13107400
DEFAULT is NOT 13107400 (the decimal misreading vms-e60 filed)
oracle %X00800080 = [200,200] OCTAL; decimal gives 13107400
a shipped account that was never in any hardcoded table
reverse resolves (oracle round-trips it)
OVMX answered 13107201, having read VMS's octal UIC as decimal group 200 member 1
the direction an earlier round deliberately left unmapped rather than add on the strength of symmetry
the rewritten UIC GROUP field still reads 200 (octal, vms-e60)
the rewritten UIC MEMBER field still reads 202 (octal, vms-e60)
EOF
                      ;;
        knock_on_why)  cat <<'EOF'
SAME CONSTANT, OBSERVED AT EVERY UIC READER -- FOUR OF THEM, ONE FOUND BY
RUNNING IT. test_syssvc_sysuaf_uic_base.c drives sysuaf_lookup() directly
for DEFAULT and GUEST (require_fail names GUEST's own check; knock_on_fail's
first two lines are DEFAULT's check and its own regression-naming assertion,
which now reads exactly the decimal value it exists to rule out).
test_syssvc_rightslist.c reaches the SAME sysuaf_lookup() one layer up,
through rightslist_name_to_value()'s UIC branch, for the SAME two accounts --
both directions of its check_roundtrip("DEFAULT", ...) and
check_roundtrip("GUEST", ...): each account still RESOLVES (sysuaf_lookup
still finds the record; only the value is wrong), so it is the value-match
and reverse-lookup halves of check_roundtrip that redden, not the "resolved
at all" branch (that one requires resolution to fail outright, which this
mutation does not cause). test_syssvc_ident.c reaches it a THIRD time,
through F$IDENTIFIER's live DCL.EXE path, for DEFAULT only (ident.c never
probes GUEST).

test_syssvc_setuai.c's scenario 4 is the FOURTH, and it was NOT predicted --
an earlier draft of this entry reasoned "the writer uses a literal %o, so
nothing that only writes SYSUAF.DAT is reached" and left setuai out of
suites_red entirely. Running run_facility_negctl.sh for real reddened it
anyway: scenario 4 does not just $SETUAI a new UIC, it reads USER1's rewritten
row BACK afterward (row_field() against the same sysuaf_lookup()-backed
record the read side of this mutation touches) to prove the write landed --
so its readback, not its write, hits the mutated radix. Two assertions
redden ("4: the rewritten UIC GROUP field still reads 200" and its MEMBER
sibling), because USER1 ships 200|202, and decimal-reading those digits
produces a different string than octal does. This is exactly the discipline
this manifest's own header describes: an unpredicted red is declared here,
not narrowed away or left as an unattributed stray.

WHAT STAYS GREEN, AND WHY. SYSTEM's UIC ([1,4]) is identical in both bases
in every suite that checks it -- explicitly labelled LIVENESS ONLY in each
of their own comments, for exactly this reason. rightslist.c's GENERAL
identifiers (BATCH/DIALUP/INTERACTIVE/LOCAL/NETWORK/REMOTE) and its
check_miss_value() calls never reach sysuaf_lookup() at all.
test_syssvc_ident.c's LOCAL checks are the general-identifier path,
untouched here (see rightslist-general-hex-as-decimal, the sibling defect
for that branch). test_syssvc_setuai's OTHER scenarios (1-3, the SYSPRV
refusal/grant checks) never read a UIC field back and stay green.
EOF
                      ;;
        esac;;

    sysuaf-uic-writeback-decimal)
        case "$_f" in
        facility)     echo "SYSUAF.DAT's UIC WRITE-BACK -- sysuaf_format_record()'s octal (%o) formatting of the UIC_GROUP/UIC_MEMBER columns (src/libvms/rtl/sysuaf.c, vms-e60). The WRITER half, the complement of sysuaf-uic-radix-decimal (which mutates the READER): a record's UIC round-trips only if the writer emits the same base the reader parses.";;
        targets)      echo "libvms/rtl/sysuaf.c";;
        suites_red)   echo "test_syssvc_setuai";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "sysuaf_format_record() formats the two UIC fields with %o (octal, from the same SYSUAF_UIC_RADIX the parse uses). The mutation flips both %o to %u, so a rewritten record whose UIC digits differ between octal and decimal is written in DECIMAL. \$SETUAI's scenario 4 (test_syssvc_setuai) rewrites USER1, whose shipped 200|202 parses to struct 128|130 (decimal): with %o the writer emits '200'|'202' (unchanged), with %u it emits '128'|'130'. The suite reads the rewritten row's FIELD TEXT back with row_field() and asserts it still reads 200 / 202 -- so the %u write reddens exactly those two assertions. MEASURED (vms-f57): strtoul(\"200\",8)=128, strtoul(\"202\",8)=130; snprintf %o of 128/130 = '200'/'202', %u = '128'/'130'. This is DISTINCT from sysuaf-uic-radix-decimal: that entry mutates the READER radix (8->10) so the same round-trip corrupts through the read side (read decimal 200, write %o -> '310'); this one holds the reader octal and proves the WRITER's %o is itself load-bearing. No other qemu suite writes a record via sysuaf_format_record() and then reads a UIC field back (test_syssvc_authorize's session only EXITs; scenario 4 is the sole UIC write-back readback in the suite set), so the mutation reddens nothing outside test_syssvc_setuai.";;
        require_fail) cat <<'EOF'
4: the rewritten UIC GROUP field still reads 200 (octal, vms-e60)
4: the rewritten UIC MEMBER field still reads 202 (octal, vms-e60)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    bg-recv-length-zeroed)
        case "$_f" in
        facility)     echo "INET pseudo-device BGn: -- the IO\$_READVBLK (recv) handler of the executive-resident BGn: driver (vms_ioctl_bg_recv, src/kernel/vms_bg.c, vms-527). The first network facility: a VMS program \$ASSIGNs TCPIP\$DEVICE:, \$QIOs connect/send/recv/close to a TCP peer, and the socket lives IN the executive (host in-kernel socket API), not in userspace.";;
        targets)      echo "kernel/vms_bg.c";;
        suites_red)   echo "test_syssvc_bg_echo";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vms_ioctl_bg_recv() reports the received byte count as 0 instead of the count the host kernel's kernel_recvmsg returned, so the echo comes back with a zero IOSB byte count (and the userspace wrapper then copies 0 bytes out). The read still returns SS\$_NORMAL -- a completed recv of nothing is not an error to the driver -- so only the byte-exact echo assertion sees it. One assignment zeroed.";;
        require_fail) cat <<'EOF'
BG $QIO IO$_READVBLK returns the exact bytes the echo peer sent back
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    tcpip-ftp-get-length-dropped)
        case "$_f" in
        facility)     echo "TCP/IP Services client TOOLS -- the FTP RETR data-drain of the shared client engine (tcpip_ftp_get, src/vmstcpip/services/tcpip_client.h, vms-dbb), the header the DCL FTP/TELNET verbs ship and test_syssvc_tcpip_client proves. The tools drive their connections over the executive-resident INET pseudo-device BGn: (vms-527) through the public \$ASSIGN/\$QIO/\$DASSGN services -- no userspace socket stack; \$ASSIGN TCPIP\$DEVICE: fails SS\$_NOSUCHDEV with no executive.";;
        targets)      echo "vmstcpip/services/tcpip_client.h";;
        suites_red)   echo "test_syssvc_tcpip_client";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "tcpip_ftp_get()'s data-drain loop accumulates the received file with 'total += take;'. The mutation flips it to 'total += 0;', so RETR reports zero bytes received while every FTP reply (220/331/230/200/227/150/226) still parses success -- so the RETR STILL COMPLETES (its connect/login/PASV assertion stays green) and only the byte-exact-file assertion reddens (glen != file length, content mismatch). The TELNET assertions and the FTP STOR path never call tcpip_ftp_get and stay green. One assignment zeroed.";;
        require_fail) cat <<'EOF'
FTP RETR receives the file BYTE-EXACT over the PASV data channel
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    bgsock-recv-length-zeroed)
        case "$_f" in
        facility)     echo "BSD-sockets RTL veneer over BGn: -- the ovmx_recv() receive path of the OVMX sockets veneer (src/vmstcpip/sockets/vms_bgsock.c, vms-22a prereq), the DECC\$SOCKET-equivalent middle layer between an application's standard socket()/send()/recv() and the executive-resident BGn: driver. The app speaks ONLY sockets; the veneer translates them into the public \$ASSIGN TCPIP\$DEVICE: + \$QIO ops. \$ASSIGN TCPIP\$DEVICE: fails SS\$_NOSUCHDEV with no executive (ovmx_socket -> ENODEV).";;
        targets)      echo "vmstcpip/sockets/vms_bgsock.c";;
        suites_red)   echo "test_syssvc_bgsock_echo";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "ovmx_recv() does one blocking IO\$_READVBLK and returns the IOSB byte count. The mutation zeroes it ('n = iosb\$w_bcnt;' -> 'n = 0;'), so recv reports EOF (0) and the reader thread receives nothing while the send, connect and socket-open all stay green -- only the byte-exact full-duplex echo assertion reddens. One assignment zeroed.";;
        require_fail) cat <<'EOF'
the reader thread received the echoed message BYTE-EXACT concurrently with the send (full-duplex)
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    # -------------------------------------------------------------------
    # vms-6c6: two suites #284/#272 (vms-8b6, vms-cf62) left with NO
    # per-facility negative control -- the same coverage-gate hole
    # vms-6d6/#293 closed for the lnm search-list suites. Baselined before
    # this change: `coverage` named both in "NAMED BY NO defect's suites_red"
    # and "in-scope suite(s) with NO anchor at all". The THIRD suite in that
    # baseline, test_kmod_vmsfs_sysgroup, is NOT anchored here: its property
    # (the System-category SOGW decision) lives entirely in vmsfs.ko
    # (kernel/vmsfs, SCOPE_OUT_UNIT_DIRS), which this executive gate declares
    # out of scope, so it is moved to SCOPE_OUT_SUITES with its siblings --
    # see the scope_out_why note and the vms-6c6 rd trail.
    # -------------------------------------------------------------------

    vmsfs-mountvis-crossproc-resolve-disabled)
        case "$_f" in
        facility)     echo "cross-process device-mount VISIBILITY -- the executive/system-wide resolution of a MOUNTed unit through the kernel's own mount table (vmsfs_device_resolve_executive, src/vmsfs/vmsfs_device.c, vms-8b6). A VMS mount is EXECUTIVE state: a unit MOUNTed by one process must resolve for a SEPARATE process (an exec'd AUTHORIZE.EXE, SYSGEN.EXE, any RUN'd image). The per-process device_table cannot carry that, so a table miss consults /proc/mounts -- the kernel's own mount table, global and cross-process. This is the userspace consumer of that executive-owned state (the same product-half class as the devtab/rms translation entries, INV-6), not a vms.ko-dispatched ioctl.";;
        targets)      echo "vmsfs/vmsfs_device.c";;
        suites_red)   echo "test_kmod_vmsfs_mountvis";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "vmsfs_device_resolve_executive() stops consulting the kernel mount table on a per-process device_table miss: the guard \`if (!mount_table_has(mp))\` is forced true (\`if (1 || !mount_table_has(mp))\`), so every dynamically-MOUNTed unit resolves SS\$_NOSUCHDEV for a process that did not itself mount it -- the exact per-process view vms-8b6 converted away from. DCL.EXE (a separate process) can no longer resolve DKA100: mounted by the test process and falls back to the system disk, where the volume's distinctive HELLO.TXT does not exist. DKA0: never reaches this path (seeded into device_table at startup), so every suite that only touches the system disk stays green; the unmounted-unit negative control (DKA200:) still returns NOSUCHDEV, so nothing is fabricated. One guard forced true.";;
        require_fail) cat <<'EOF'
cross-process: DCL resolves DKA100: (mounted by another process) and reads HELLO.TXT off it -- the mounted unit is visible and readable
EOF
                      ;;
        knock_on_fail) echo "";;
        knock_on_why)  echo "";;
        esac;;

    initialize-home-magic-not-written)
        case "$_f" in
        facility)     echo "INITIALIZE writing a VALID vmsfs home block to the executive-RESOLVED real backing device (format_volume, tools/vms_initialize.c, vms-cf62). INITIALIZE.EXE resolves DKA100: through the executive (vms_kif_disk_resolve, the same authoritative path MOUNT uses) to its backing block device and formats THAT store -- the anti-facade property (INV-6, CLAUDE.md Rule 9): the real device is actually formatted, not merely reported so. A userspace consumer of the executive disk-resolution facility, the same product-half class as the devtab entries.";;
        targets)      echo "../tools/vms_initialize.c";;
        suites_red)   echo "test_syssvc_initialize";;
        blind_suites) echo "";;
        blind_why)    echo "";;
        isolation)    echo "isolated";;
        why)          echo "format_volume() writes the vmsfs home block to LBN 1 of the resolved backing device with \`hb.hb_magic = VMSFS_HOME_MAGIC;\`. The mutation zeroes it (\`hb.hb_magic = 0;\`), so INITIALIZE still exits 0 (the block is written and its checksum recomputed over it) but the REAL device carries NO valid home block -- it reports success while the store is unformatted, the exact fake-success class vms-cf62 exists to kill. Only the A/B \"after INITIALIZE the backing device carries a home block\" assertion reads hb_magic and reddens; the label field is still written correctly (the volname assertion stays green), the command's own rc stays 0, and the BOGUS999: honest-failure negative control is untouched. One constant zeroed.";;
        require_fail) cat <<'EOF'
after INITIALIZE, the REAL backing device carries a vmsfs home block
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
    setprv-grants-unauthorized)
        # Unique text (vms_ioctl_setprv's authorized-subset intersection); the
        # replacement drops the `& proc->perm_privs` term, so a second apply
        # finds no match and is the no-op selftest requires.
        sed -i 's|uint64_t allowed = args.mask \& proc->perm_privs;|uint64_t allowed = args.mask; /* NEGCTL setprv-grants-unauthorized */|' "$_file";;
    kif-setmode-always-kernel)
        sed -i 's|    args.mode = mode;|    args.mode = 0; /* NEGCTL kif-setmode-always-kernel */|' "$_file";;
    getmode-buffer-not-written)
        sed -i '/^long vms_ioctl_getmode/,/^}$/ s|if (exec_copyout(|if (1 \|\| exec_copyout(|' "$_file";;
    ast-setast-disable)
        sed -i 's|ast_state->enabled = args.enable ? 1 : 0;|ast_state->enabled = 1; /* NEGCTL ast-setast-disable */|' "$_file";;
    ast-deliver-crossmode-escalation)
        sed -i 's|    for (mode = cur_mode; mode <= PSL_C_USER; mode++) {|    for (mode = PSL_C_KERNEL; mode <= PSL_C_USER; mode++) { /* NEGCTL ast-deliver-crossmode-escalation */|' "$_file";;
    dclast-super-mode-escalation)
        # UNIQUE TEXT, no range anchor needed: `if (args.acmode == PSL_C_SUPER &&`
        # occurs once in the file -- vms_ioctl_dclast's own SUPER gate (vms-95a).
        # Prefixing the condition with `0 &&` makes the refusal structurally
        # unreachable (the same always-false idiom image-rundown-without-entry
        # and access-mode-escalation use), so a SUPER declaration falls through to
        # the queue path again while the KERNEL and EXEC checks above are
        # untouched. After substitution the line reads `if (0 && args.acmode ==
        # PSL_C_SUPER &&`, so a second apply finds nothing left to match -- the
        # no-op selftest requires.
        sed -i 's|^        if (args\.acmode == PSL_C_SUPER &&$|        if (0 \&\& args.acmode == PSL_C_SUPER \&\& /* NEGCTL dclast-super-mode-escalation */|' "$_file";;
    eflag-clref-noop)
        sed -i 's|\*flags \&= ~(1U << bit);|/* NEGCTL eflag-clref-noop: the bit is not cleared */|' "$_file";;
    eflag-waitfr-eintr-normal)
        # RANGE-ANCHORED and therefore IDEMPOTENT. The interrupted-wait bail
        # `return -ERESTARTSYS;` appears in all three wait handlers (WAITFR,
        # WFLOR, WFLAND), so the range narrows it to WAITFR's: it opens at
        # `^long vms_ioctl_waitfr` and closes at that function's own `out:`
        # label, and the replaced line is the 12-space `return -ERESTARTSYS;`
        # inside WAITFR's `if (exec_cv_wait(...))` interrupted branch. Injecting
        # the defect makes an interrupted wait FABRICATE SS$_NORMAL (report the
        # flag set) and fall through to `out:` instead of re-entering the wait
        # -- the exact vms-2a8 fiction this facility deletes. A second apply
        # finds no `return -ERESTARTSYS;` left inside the range -- the first
        # replaced it -- so it is the no-op `selftest` requires, instead of
        # walking on to WFLOR's like a first-match address would.
        #
        # RE-ANCHORED in Phase B (rd vms-ec4): the previous anchor still named
        # `wait_event_interruptible(...)`/`return ret;`, which Phase A (rd
        # vms-adb) had already replaced with the exec_cv_wait cv-idiom and
        # `return -ERESTARTSYS;` -- so this fixture was a dead anchor (BROKEN
        # FIXTURE) on main until now.
        sed -i '/^long vms_ioctl_waitfr/,/^out:$/ s|^            return -ERESTARTSYS;$|            { args.status = SS__NORMAL; goto out; } /* NEGCTL eflag-waitfr-eintr-normal */|' "$_file";;
    eflag-setef-status-inverted)
        # `args.status = prev ? SS_WASSET : SS_WASCLR;` is duplicated
        # verbatim in vms_ioctl_clref immediately below. RANGE-ANCHORED to
        # vms_ioctl_setef's own body so the range closes before clref's
        # copy is reached.
        sed -i '/^long vms_ioctl_setef/,/^}$/ s|^    args\.status = prev ? SS__WASSET : SS__WASCLR;$|    args.status = prev ? SS__WASCLR : SS__WASSET; /* NEGCTL eflag-setef-status-inverted */|' "$_file";;
    eflag-readef-status-inverted)
        # Unique text in the file -- readef's own state/bit comparison,
        # shared with no other handler.
        sed -i 's|^    args\.status = (args\.state \& (1U << bit)) ? SS__WASSET : SS__WASCLR;$|    args.status = (args.state \& (1U << bit)) ? SS__WASCLR : SS__WASSET; /* NEGCTL eflag-readef-status-inverted */|' "$_file";;
    eflag-ascefc-reassoc-status-wrong)
        # UNIQUE TEXT, no range anchor needed: the 12-space indentation only
        # occurs inside vms_ioctl_ascefc's own "Found it -- associate"
        # branch (list_for_each_entry's if-block) -- the sibling 4-space
        # "args.status = SS_NORMAL;" at this handler's own Create-new-
        # cluster branch, and every other handler's copy of that same
        # 4-space text, are both a different indentation and untouched.
        sed -i 's|^            args\.status = SS__NORMAL;$|            args.status = SS__UNASEFC; /* NEGCTL eflag-ascefc-reassoc-status-wrong */|' "$_file";;
    eflag-dacefc-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_dacefc's own body. This exact 4-space
        # "args.status = SS_NORMAL;" also appears in vms_ioctl_waitfr,
        # vms_ioctl_wflor, vms_ioctl_wfland and vms_ioctl_ascefc's create
        # path; vms_ioctl_dacefc is defined between ascefc and dlcefc, so
        # the range excludes all of them.
        sed -i '/^long vms_ioctl_dacefc/,/^}$/ s|^    args\.status = SS__NORMAL;$|    args.status = SS__UNASEFC; /* NEGCTL eflag-dacefc-status-wrong */|' "$_file";;
    eflag-dlcefc-status-wrong)
        # 8-space indentation is the only occurrence at that depth in the
        # file -- dlcefc's own found-and-marked success, inside its
        # list_for_each_entry_safe loop.
        sed -i 's|^        args\.status = SS__NORMAL;$|        args.status = SS__UNASEFC; /* NEGCTL eflag-dlcefc-status-wrong */|' "$_file";;
    eflag-wflor-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_wflor's own body. This exact 4-space
        # "args.status = SS_NORMAL;" also appears in vms_ioctl_waitfr
        # (earlier in the file) and vms_ioctl_wfland (later); vms_ioctl_wflor
        # is defined between them, so the range excludes both.
        sed -i '/^long vms_ioctl_wflor/,/^}$/ s|^    args\.status = SS__NORMAL;$|    args.status = SS__ILLEFC; /* NEGCTL eflag-wflor-status-wrong */|' "$_file";;
    eflag-wfland-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_wfland's own body, same reasoning as
        # its WFLOR sibling above -- this exact "args.status = SS_NORMAL;"
        # also appears in vms_ioctl_waitfr and vms_ioctl_wflor, both earlier
        # in the file.
        sed -i '/^long vms_ioctl_wfland/,/^}$/ s|^    args\.status = SS__NORMAL;$|    args.status = SS__ILLEFC; /* NEGCTL eflag-wfland-status-wrong */|' "$_file";;
    lock-compat-ex-cr)
        sed -i 's|/\* EX \*/ {  1,  0,  0,  0,  0,  0 },|/* EX */ {  1,  1,  0,  0,  0,  0 }, /* NEGCTL lock-compat-ex-cr */|' "$_file";;
    lock-compat-cr-ex)
        sed -i 's|/\* CR \*/ {  1,  1,  1,  1,  1,  0 },|/* CR */ {  1,  1,  1,  1,  1,  1 }, /* NEGCTL lock-compat-cr-ex */|' "$_file";;
    lock-valblk-grant-not-delivered)
        # The `if` guard, NOT the memcpy alone. ROUND 1 of this mutation
        # replaced only the memcpy line, leaving
        #     if (waiter->flags & LCK_M_VALBLK)
        #         /* NEGCTL ... */
        # -- a dangling `if` with no statement, so C's single-statement-body
        # rule silently pulled the NEXT line (queue_completion_ast(waiter);)
        # into the condition. That is a WORSE blunderbuss than deleting the
        # whole function: it broke completion-AST delivery for every waiter
        # WITHOUT LCK_M_VALBLK set too, corroborated live -- MEASURED, a run
        # with that first form reddened test_kmod_lock_sync's scenario 3
        # ("child: DELIVERAST returned completion AST", "parent: child
        # (completion AST) exited clean"), a property this defect does not
        # name and scenario 3 never sets LCK_M_VALBLK at all. Anchoring on
        # the `if` line and forcing its condition to always-false with
        # `0 &&` (the same idiom access-mode-escalation and
        # ident-username-unguarded already use in this file) keeps the
        # memcpy as the `if`'s only body statement, structurally inert, so
        # nothing after it is affected. The anchor is the only occurrence of
        # this exact `if` in the file, so a second apply finds no match --
        # the no-op the selftest requires.
        sed -i 's|            if (waiter->flags & LCK_M_VALBLK)|            if (0 \&\& (waiter->flags \& LCK_M_VALBLK)) /* NEGCTL lock-valblk-grant-not-delivered: no delivery */|' "$_file";;
    lock-enq-immediate-grant-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_enq's own body. `args.status =
        # SS__NORMAL;` at this exact 8-space indentation also appears in
        # vms_ioctl_convert's immediate-conversion branch (same text, same
        # indentation -- indentation alone does not disambiguate this pair).
        # vms_ioctl_enq is defined BEFORE vms_ioctl_convert in this file, so
        # the range closes at enq's own `}` and excludes convert's copy.
        sed -i '/^long vms_ioctl_enq/,/^}$/ s|^        args\.status = SS__NORMAL;$|        args.status = SS__NOTQUEUED; /* NEGCTL lock-enq-immediate-grant-status-wrong */|' "$_file";;
    lock-deq-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_deq's own body. `args.status =
        # SS__NORMAL;` at this exact 4-space indentation also appears in
        # vms_ioctl_convert's own fallthrough path (same text, same
        # indentation). vms_ioctl_deq is defined BEFORE vms_ioctl_convert in
        # this file, so the range closes at deq's own `}` and excludes
        # convert's copy.
        sed -i '/^long vms_ioctl_deq/,/^}$/ s|^    args\.status = SS__NORMAL;$|    args.status = SS__IVLOCKID; /* NEGCTL lock-deq-status-wrong */|' "$_file";;
    lock-convert-mode-not-updated)
        # RANGE-ANCHORED to vms_ioctl_convert's own body. `lock->granted_mode
        # = args.lkmode;` at 8-space indent also appears in $ENQ's
        # immediate-grant branch (line ~666, same indentation -- indentation
        # alone does not disambiguate this pair, unlike the enq/deq defects
        # above). vms_ioctl_convert is defined AFTER vms_ioctl_enq in this
        # file, so the range excludes ENQ's copy entirely; only CONVERT's
        # remains inside it. Commented out rather than blanked to a `;`: this
        # assignment sits inside a braced `{ }` block (the immediate-
        # conversion branch), not an unbraced single-statement `if`, so there
        # is no dangling-body hazard here.
        sed -i '/^long vms_ioctl_convert/,/^}$/ s|^        lock->granted_mode = args\.lkmode;$|        /* NEGCTL lock-convert-mode-not-updated: granted_mode left unchanged */|' "$_file";;
    resdir-master-csid-not-reported)
        # `args.master_csid = res->master_csid;` is the ONLY assignment to
        # that field in the file (vms_ioctl_get_resmaster's sole copy-out),
        # so anchoring on it directly is unambiguous. The replacement
        # statement no longer contains the anchor text (`res->master_csid`
        # is dropped, not just reassigned), so a second apply finds no match
        # -- the no-op the selftest requires. res->master_csid itself (the
        # value is_local_master and the mastering decision are computed
        # from) is untouched, so this isolates the READBACK from the STATE.
        sed -i 's|        args.master_csid = res->master_csid;|        args.master_csid = 0; /* NEGCTL resdir-master-csid-not-reported */|' "$_file";;
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
        # Range-end anchor is exec_unlock (not spin_unlock) since vms-31b moved
        # this facility onto the kernel-backend shim in src/kernel-core/.
        sed -i '/if (!dev->shareable && dev->owner_linux_pid == 0) {/,/^    exec_unlock(&dev->lock);$/ s|dev->owner_pid = proc->vms_pid;|/* NEGCTL devtab-owner-not-recorded */|' "$_file";;
    devtab-alloc-not-recorded)
        # devinfo_fill() has exactly one `info->allocated =` write; anchoring
        # to it directly (not a range) is safe because it is the only such
        # assignment in the file, so a second apply finds no match and is the
        # no-op the selftest requires.
        sed -i 's|    info->allocated = dev->allocated;|    info->allocated = 0; /* NEGCTL devtab-alloc-not-recorded */|' "$_file";;
    devtab-dassgn-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_dassgn's own body. This exact 8-space
        # "args.status = SS_NORMAL;" also appears in $ALLOC (twice) and
        # $DALLOC, all defined nearby in the same file; vms_ioctl_dassgn is
        # the first of them, so the range closes at its own `}`.
        sed -i '/^long vms_ioctl_dassgn/,/^}$/ s|^        args\.status = SS__NORMAL;$|        args.status = SS__IVCHAN; /* NEGCTL devtab-dassgn-status-wrong */|' "$_file";;
    devtab-getdvi-devnam-status-wrong)
        # RANGE-ANCHORED to the by-NAME branch only, not the whole
        # function: "if (args.select != VMS_DVI_SEL_DEVNAM) {" is the
        # unique line that opens it, and the range runs to
        # vms_ioctl_getdvi's own closing brace, excluding the by-CHANNEL
        # branch's own "args.status = SS_NORMAL;" earlier in the function.
        sed -i '/if (args\.select != VMS_DVI_SEL_DEVNAM) {/,/^}$/ s|^        args\.status = SS__NORMAL;$|        args.status = SS__NOSUCHDEV; /* NEGCTL devtab-getdvi-devnam-status-wrong */|' "$_file";;
    devtab-devscan-found-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_devscan's own body for consistency
        # with its siblings above -- the only "args.status = SS_NORMAL;" in
        # this function, so the range is not load-bearing for uniqueness,
        # only for the naming convention this file's devtab-* entries share.
        sed -i '/^long vms_ioctl_devscan/,/^}$/ s|^        args\.status = SS__NORMAL;$|        args.status = SS__NOSUCHDEV; /* NEGCTL devtab-devscan-found-status-wrong */|' "$_file";;
    disk-backing-not-resolved)
        # vms_ioctl_disk_resolve() copies the backing device name in exactly
        # one place. Anchoring on that strscpy zeroes the name the resolve
        # hands back while leaving the SS$_NORMAL status and the dev_t
        # (major/minor) intact -- the executive reports "resolved" and hands
        # back an EMPTY backing device, the facade shape (reports success,
        # shares nothing). The replacement statement does not contain the
        # anchor text, so a second apply finds no match and is the no-op the
        # selftest requires.
        sed -i 's|    strscpy(args.backing, dev->backing, sizeof(args.backing));|    args.backing[0] = 0; /* NEGCTL disk-backing-not-resolved: the backing name is not handed back */|' "$_file";;
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
    proc-acct-not-sourced)
        # Both call sites are the identical text "fill_proc_acct(acct_task,
        # &args.info);" (vms_ioctl_getjpi and vms_ioctl_procscan), each on
        # its own line, so this single substitution removes BOTH -- sed
        # applies to every line. The replacement is a bare `;` plus the
        # marker, which leaves the surrounding `if (acct_task) { ...
        # put_task_struct(acct_task); }` valid and makes the anchor vanish
        # after one apply (a second apply finds no match: the idempotence the
        # selftest requires). fill_proc_acct() then goes uncalled -- only a
        # -Wunused-function warning, not an error, for an external module
        # build (verified: vms.ko still links), which is the point: the
        # accounting facility is gone and every reader sees the fields clear.
        sed -i 's|fill_proc_acct(acct_task, &args.info);|/* NEGCTL proc-acct-not-sourced */;|' "$_file";;
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
    proctab-getjpi-nonexpr-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_getjpi's own body. This exact
        # "args.status = SS_NONEXPR;" also appears in vms_ioctl_procscan's
        # own not-found path; vms_ioctl_getjpi is defined first in the
        # file, so the range closes at its own `}` and excludes procscan's.
        sed -i '/^long vms_ioctl_getjpi/,/^}$/ s|^        args\.status = SS__NONEXPR;$|        args.status = SS__NOPRIV; /* NEGCTL proctab-getjpi-nonexpr-status-wrong */|' "$_file";;
    proctab-procscan-nonexpr-status-wrong)
        # RANGE-ANCHORED to vms_ioctl_procscan's own body. This exact
        # "args.status = SS_NONEXPR;" also appears in vms_ioctl_getjpi's
        # own not-found path; vms_ioctl_procscan is defined after
        # vms_ioctl_getjpi in this file, so the range excludes getjpi's.
        sed -i '/^long vms_ioctl_procscan/,/^}$/ s|^        args\.status = SS__NONEXPR;$|        args.status = SS__NOPRIV; /* NEGCTL proctab-procscan-nonexpr-status-wrong */|' "$_file";;
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

    delprc-privcheck-bypassed)
        # The ONE edit, in libvms/syssvc/sys_process.c. `if (!authorized)` is
        # UNIQUE in the file -- it is sys$delprc's own GROUP/WORLD privilege
        # gate, guarding the `return SS$_NOPRIV;` on the next line. Prefixing
        # it into the always-false `if (0 ...)` makes the refusal structurally
        # unreachable, so a privilege-stripped same-group STOP falls through to
        # sys$delprc's terminating kill() and is reported SS$_NORMAL. After
        # substitution the line reads `if (0 /* ... */)`, so a second apply
        # finds no `if (!authorized)` left to match -- the idempotent no-op
        # cmd_selftest requires. Resolution, naming and the kill are untouched.
        sed -i 's|        if (!authorized)|        if (0 /* NEGCTL delprc-privcheck-bypassed */)|' "$_file";;

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
        sed -i 's|#define SS__DEADLOCK    3594|#define SS__DEADLOCK    2488 /* NEGCTL kstat-deadlock-mismapped */|' "$_file";;
    kstat-ivlockid-mismapped)
        sed -i 's|#define SS__IVLOCKID    8484|#define SS__IVLOCKID    2488 /* NEGCTL kstat-ivlockid-mismapped */|' "$_file";;
    kstat-cvtungrant-mismapped)
        sed -i 's|#define SS__CANCELGRANT 8508|#define SS__CANCELGRANT 2488 /* NEGCTL kstat-cvtungrant-mismapped */|' "$_file";;

    assign-terminal-bypasses-executive)
        sed -i 's|        if (devres.is_terminal) {|        if (0 \&\& devres.is_terminal) { /* NEGCTL assign-terminal-bypasses-executive */|' "$_file";;

    # The six user-name fabrications. Each restores ONE deleted fallback.
    # RANGE-ANCHORED to the function that owns the site: `const char *user =
    # ctx->username;` appears in cmd_submit AND cmd_print, and `const char
    # *username = ctx->username;` in cmd_reply AND cmd_accounting, so a
    # first-match address would silently mutate whichever came first and both
    # controls would name the same site. The ranges open on the function's own
    # definition line and close on its closing brace in column 0.
    # IDEMPOTENT BY CONSTRUCTION: the replacement no longer ends in
    # `ctx->username;`, so a second apply matches nothing and cmd_apply
    # reports BROKEN FIXTURE, which is what selftest requires.
    dcl-submit-owner-fabricated)
        sed -i '/^int cmd_submit(/,/^}$/ s|^    const char \*user = ctx->username;$|    const char *user = ctx->username[0] ? ctx->username : "SYSTEM"; /* NEGCTL dcl-submit-owner-fabricated */|' "$_file";;
    dcl-print-owner-fabricated)
        sed -i '/^int cmd_print(/,/^}$/ s|^    const char \*user = ctx->username;$|    const char *user = ctx->username[0] ? ctx->username : "SYSTEM"; /* NEGCTL dcl-print-owner-fabricated */|' "$_file";;
    dcl-logout-user-fabricated)
        sed -i 's|^    const char \*upper_user = ctx->username;$|    const char *upper_user = ctx->username[0] ? ctx->username : "SYSTEM"; /* NEGCTL dcl-logout-user-fabricated */|' "$_file";;
    dcl-reply-operator-fabricated)
        sed -i '/^int cmd_reply(/,/^}$/ s|^    const char \*username = ctx->username;$|    const char *username = ctx->username[0] ? ctx->username : "SYSTEM"; /* NEGCTL dcl-reply-operator-fabricated */|' "$_file";;
    dcl-accounting-user-fabricated)
        sed -i '/^int cmd_accounting(/,/^}$/ s|^    const char \*username = ctx->username;$|    const char *username = ctx->username[0] ? ctx->username : "SYSTEM"; /* NEGCTL dcl-accounting-user-fabricated */|' "$_file";;
    dcl-fuser-system-fabricated)
        sed -i '/^static int lex_user(/,/^}$/ s|^        result\[0\] = .\\0.;$|        strncpy(result, "SYSTEM", result_size - 1); /* NEGCTL dcl-fuser-system-fabricated */|' "$_file";;
    dcl-fuser-host-login-name)
        sed -i '/^static int lex_user(/,/^}$/ s|^        result\[0\] = .\\0.;$|        { struct passwd *pw_ = getpwuid(getuid()); size_t i_ = 0; if (pw_) { for (; i_ < result_size - 1 \&\& pw_->pw_name[i_]; i_++) result[i_] = (char)toupper((unsigned char)pw_->pw_name[i_]); } result[i_] = 0; } /* NEGCTL dcl-fuser-host-login-name */|' "$_file";;
    dcl-fident-num2name-host-passwd)
        sed -i '/^static int lex_identifier(/,/^}$/ s|^            result\[0\] = .\\0.;$|            { struct passwd *pw_ = getpwuid((uid_t)member); size_t i_ = 0; if (pw_) { for (; i_ < result_size - 1 \&\& pw_->pw_name[i_]; i_++) result[i_] = (char)toupper((unsigned char)pw_->pw_name[i_]); } result[i_] = 0; } /* NEGCTL dcl-fident-num2name-host-passwd */|' "$_file";;
    dcl-fident-num2name-bracketed-uic)
        sed -i '/^static int lex_identifier(/,/^}$/ s|^            result\[0\] = .\\0.;$|            snprintf(result, result_size, "[%d,%d]", group, member); /* NEGCTL dcl-fident-num2name-bracketed-uic */|' "$_file";;
    dcl-fident-name2num-host-passwd)
        sed -i '/^static int lex_identifier(/,/^}$/ s|^            snprintf(result, result_size, "0");$|            { struct passwd *pw_ = getpwnam(id_str); if (pw_) { snprintf(result, result_size, "%d", (int)((pw_->pw_gid << 16) \| (pw_->pw_uid \& 0xFFFF))); } else { snprintf(result, result_size, "0"); } } /* NEGCTL dcl-fident-name2num-host-passwd */|' "$_file";;
    opcom-header-host-login-name)
        sed -i '/^static void get_current_username(/,/^}$/ s|^    strncpy(buf, info.username, bufsz - 1);$|    if (!info.username[0]) { struct passwd *pw_ = getpwuid(getuid()); if (pw_) { strncpy(buf, pw_->pw_name, bufsz - 1); buf[bufsz - 1] = 0; return; } } strncpy(buf, info.username, bufsz - 1); /* NEGCTL opcom-header-host-login-name */|' "$_file";;
    getjpi-pidarg-discarded)
        # The ONE edit, in vmsdcl/dcl_lexical.c. `if (target[0] == '\0') {` is
        # UNIQUE with its trailing ` {` -- it opens lex_getjpi's target
        # resolution (the self-vs-pid fork); the MODE branch's `if (target[0]
        # == '\0')` a few lines below has no ` {` and is not matched. Forcing
        # it always-true self-pins the resolution, so the hex pid is discarded
        # and F$GETJPI answers about the caller (the vms-9e2 facade). After
        # substitution the line reads `if (1 /* ... */) {`, so a second apply
        # finds no `if (target[0] == '\0') {` to match -- the idempotent no-op
        # cmd_selftest requires. The else (getjpi_pid) branch is still compiled
        # (so pid/endp stay used, no unused-variable warning), just never taken.
        sed -i "s|    if (target\[0\] == '\\\\0') {|    if (1 /* NEGCTL getjpi-pidarg-discarded */) {|" "$_file";;
    # Deletes the mask test and NOTHING ELSE. The vms_kif_getjpi_self() read
    # above it is left in place on purpose: with it removed as well, the
    # mutation would also delete the Rule 9 refusal-on-failed-read, and the
    # negative-control rig (no /dev/vms) would then differ from the product rig
    # for two reasons instead of one. The replacement is `if (0)`, so a second
    # apply finds no match and is the no-op selftest requires.
    setuai-sysprv-caller-declared)
        sed -i 's|^        if (!(self.cur_privs \& PRV\$M_SYSPRV))$|        if (0) /* NEGCTL setuai-sysprv-caller-declared */|' "$_file";;
    register-adopt-pid-not-reported)
        # Range-anchored to the ADOPT branch only (`if (proc) { ... return 0; }`
        # right after the first vms_proc_find_or_err() call). The FRESH
        # registration path lower in the same function has its own, textually
        # identical `args.vms_pid = proc->vms_pid;` write; a bare match (no
        # range) would hit both and trip two properties (adoption AND fresh
        # registration) at once. The range opens at the first
        # `proc = vms_proc_find_or_err();` in the file and closes at the
        # adopt branch's own `return 0;` -- the first such line after it --
        # so it cannot reach the fresh-registration branch's write, which
        # comes later in the function. The write disappears after one
        # application (replaced by a comment), so a second apply finds no
        # match inside the range and is the no-op selftest requires.
        sed -i '/^    proc = vms_proc_find_or_err();$/,/^        return 0;$/ s|^        args.vms_pid = proc->vms_pid;$|        /* NEGCTL register-adopt-pid-not-reported: vms_pid not copied back on adopt */|' "$_file";;
    register-continue-identity-dropped)
        # UNIQUE TEXT: `shared_vms_pid = parent->vms_pid;` occurs once, at
        # 8-space indentation inside vms_proc_continue_identity()'s hash walk.
        # Forcing it to 0 makes vms_proc_continue_identity() report "no parent
        # to continue", so REGISTER_CONTINUE falls back to derive-from-capable()
        # -- the pre-fix behaviour. After substitution the `= parent->vms_pid;`
        # text is gone, so a second apply finds no match: the no-op selftest
        # requires. This is the whole continuation property in one store; the
        # identity copy above it becomes dead because the caller derives when
        # shared_vms_pid is 0.
        sed -i 's|^        shared_vms_pid = parent->vms_pid;$|        shared_vms_pid = 0; /* NEGCTL register-continue-identity-dropped: image does not continue its activator */|' "$_file";;
    scratch-dir-owner-not-system)
        # Single-line, uniquely-anchored inside test_syssvc_scratch_
        # writable.c's OWN provisioning duplicate (see this defect's
        # `targets`/`why` for why the real product file cannot be the
        # target). The exact call exists once; after substitution the OLD
        # text ("(uid_t)SYSTEM_UIC_MEMBER, (gid_t)SYSTEM_UIC_GROUP) != 0)")
        # is gone, so a second apply finds no match -- the no-op selftest
        # requires. Drops only the MEMBER half of the chown to root (0); the
        # GROUP half is untouched, and SYSTEM_UIC_MEMBER (a different
        # symbol) still drives try_create_as()'s own credential drop
        # unmutated -- reproducing "group matches, owner does not" without
        # also turning the SYSTEM probe into literal root, which would
        # bypass the property being tested instead of failing it.
        sed -i 's|    if (chown(path, (uid_t)SYSTEM_UIC_MEMBER, (gid_t)SYSTEM_UIC_GROUP) != 0)|    if (chown(path, (uid_t)0, (gid_t)SYSTEM_UIC_GROUP) != 0) /* NEGCTL scratch-dir-owner-not-system: SYSTEM no longer owns [SYSTMP]/[USERS], only shares their group */|' "$_file";;

    rms-create-filespec-not-translated)
        # Unique text in the file -- resolve_filename()'s own
        # vmsfs_to_linux_path() success check, the exact line vms-221 fixed.
        # Reverting $VMS_STATUS_SUCCESS(...) back to `== 0` (the original
        # bug: vmsfs_to_linux_path() returns a VMS status code, SS$_NORMAL
        # == 1 on success, never 0) makes the check never match again, so a
        # second apply finds no $VMS_STATUS_SUCCESS(...) text left inside
        # the `if (` and is the no-op selftest requires.
        sed -i 's|if (\$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(spec, linux_path, sizeof(linux_path)))) {|if (vmsfs_to_linux_path(spec, linux_path, sizeof(linux_path)) == 0) { /* NEGCTL rms-create-filespec-not-translated */|' "$_file";;

    lnm-delete-noop)
        # UNIQUE TEXT: `e->in_use = 0;` occurs only in vms_ioctl_lnm_delete.
        # The define path clears the slot with memset() and then sets
        # `e->in_use = 1;`, so this 4-space line is the delete's store alone.
        # Replacing it with a no-op comment leaves the entry live while the
        # handler still returns SS__NORMAL, so DEASSIGN/SYSTEM reports success
        # over a name it did not remove. A second apply finds no
        # `e->in_use = 0;` left and is the no-op selftest requires.
        sed -i 's|^    e->in_use = 0;$|    /* NEGCTL lnm-delete-noop: entry not freed */|' "$_file";;

    lnm-manager-delete-noop)
        # UNIQUE TEXT: this exact call+argument list occurs once in
        # lnm_client.c, inside lnm_delete()'s LNM$SYSTEM branch (the
        # LNM$PROCESS/JOB/GROUP delete path below it never calls vms_kif_*).
        # Replacing the call with a bare `return SS$_NORMAL;` makes
        # DEASSIGN/SYSTEM at the manager layer report success without ever
        # reaching vms.ko, so a name it should have removed stays defined in
        # the executive arena -- the manager-layer analogue of
        # lnm-delete-noop's kernel-layer no-op. A second apply finds no
        # `vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, ...)` call left inside this
        # file and is the no-op selftest requires.
        sed -i 's|        return vms_kif_lnm_delete(VMS_LNM_TBL_SYSTEM, logical_name, acmode);|        return SS$_NORMAL; /* NEGCTL lnm-manager-delete-noop: never reaches vms.ko */|' "$_file";;

    lnm-group-scope-collapsed)
        # UNIQUE TEXT: this exact line occurs once in vms_lnm.c's
        # derive_scope_key(), in the LNM$GROUP case only (the LNM$JOB case
        # right below it returns proc->job_id and is untouched). Collapsing
        # it to the same 0 the SYSTEM case returns makes a GROUP-scoped name
        # visible to every UIC group, not just the one that defined it. A
        # second apply finds no `return proc->uic >> 16;` left and is the
        # no-op selftest requires.
        sed -i 's|        return proc->uic >> 16;   /\* UIC group \*/|        return 0; /* NEGCTL lnm-group-scope-collapsed: was proc->uic >> 16 */|' "$_file";;

    lnm-privilege-check-bypassed)
        # UNIQUE call-site text, occurring TWICE by design: vms_lnm.c's
        # vms_ioctl_lnm_define() and vms_ioctl_lnm_delete() both gate
        # through this exact call (that sharing is the point -- see
        # lnm_priv_check()'s header comment). sed's substitution applies
        # once per LINE with no /g needed, so a single command mutates
        # BOTH call sites in the same pass: `0 &&` short-circuits before
        # lnm_priv_check() is even called, so the `goto out_copy` refusal
        # is unreachable in either handler. A second apply finds no
        # `if (!lnm_priv_check(...))` left in the file and is the no-op
        # selftest requires.
        sed -i 's|    if (!lnm_priv_check(a->table, proc->cur_privs, \&a->status))|    if (0 \&\& !lnm_priv_check(a->table, proc->cur_privs, \&a->status)) /* NEGCTL lnm-privilege-check-bypassed */|' "$_file";;

    mbx-not-shared)
        # RANGE-ANCHORED to vms_ioctl_mbx_assign()'s own body: "if (!mbx) {"
        # (the device-name lookup's not-found branch) is NOT unique in this
        # file -- vms_ioctl_mbx_delmbx/_write/_read each have an identical
        # line testing a per-process CHANNEL lookup's result -- so the
        # address range confines the substitution to $ASSIGN's own
        # not-found check, immediately after the device-name lookup that is
        # the ONLY thing standing between an unrelated process and a
        # mailbox it did not $CREMBX. Widening the refusal to "not found OR
        # not the creator" makes $ASSIGN refuse (SS$_NOSUCHDEV) every
        # mailbox this caller did not itself create -- MEASURED against a
        # real /dev/vms (build, insmod, run, rmmod): reddens exactly the
        # thirteen assertions this defect's require_fail/knock_on_fail name, 9
        # of test_kmod_mbx's 22 assertions staying green. The replacement
        # text does not contain the original "if (!mbx) {" as a substring
        # (it reads "if (!mbx ||" instead), so a second apply finds nothing
        # left to match -- the no-op selftest requires.
        sed -i '/^long vms_ioctl_mbx_assign/,/^}$/ s|    if (!mbx) {|    if (!mbx \|\| mbx->owner_linux_pid != proc->linux_pid) { /* NEGCTL mbx-not-shared */|' "$_file";;

    mbx-wrtattn-not-fired)
        # UNIQUE TEXT, no range anchor needed: exec_zalloc_atomic(sizeof(*ast))
        # occurs exactly once in vms_mbx.c -- the write-attention firing block's
        # AST allocation in vms_ioctl_mbx_write(). Forcing it to NULL makes the
        # `if (ast)` guard skip the enqueue, so no write-attention AST is ever
        # delivered while every registration is still drained (one-shot intact)
        # and the write still succeeds. After substitution the text no longer
        # matches, so a second apply is a no-op (the selftest requires).
        sed -i 's|ast = exec_zalloc_atomic(sizeof(\*ast));|ast = (struct vms_ast_entry *)0; /* NEGCTL mbx-wrtattn-not-fired */|' "$_file";;

    hiber-ast-not-delivered)
        # UNIQUE TEXT, no range anchor needed: exec_cv_broadcast(&proc->hiber_wq)
        # occurs exactly once in vms_ast.c -- vms_ast_notify_arrival()'s wake of a
        # $HIBER waiter after an AST is queued. Removing it leaves the AST on the
        # queue but never wakes the hibernating process to run it, so a $HIBER
        # waiting on a cross-process write-attention AST deadlocks. The $WAKE
        # path (vms_ioctl_wake's own broadcasts live in a DIFFERENT file,
        # kernel-core/vms_proctab.c, which this sed on vms_ast.c never touches)
        # is untouched, so this isolates AST-arrival delivery from the $WAKE path
        # -- the sticky-wake scenario still passes. After substitution the text no
        # longer matches, so a second apply is a no-op (the selftest requires).
        sed -i 's|    exec_cv_broadcast(&proc->hiber_wq);|    /* NEGCTL hiber-ast-not-delivered: arrival wake removed */|' "$_file";;

    dcl-sysinput-mbx-not-read)
        # UNIQUE TEXT, no range anchor needed: `uint32_t mlen = actlen;` occurs
        # exactly once in dcl_mbx.c -- the reader thread's per-message length
        # (the count of bytes IO$_READVBLK pulled out of the SYS$INPUT mailbox).
        # Forcing it to zero makes the reader drain the SYS$INPUT mailbox but
        # forward NOTHING into DCL's command loop, so the driven DCL never
        # receives a command. After substitution the text no longer matches, so a
        # second apply is a no-op (the selftest requires).
        sed -i 's|uint32_t mlen = actlen;|uint32_t mlen = 0; /* NEGCTL dcl-sysinput-mbx-not-read */|' "$_file";;

    mmk-drive-command-not-sent)
        # UNIQUE TEXT, no range anchor needed: `uint16_t len = d->dsc$w_length;`
        # occurs exactly once in ovmx_mmk_sp.c -- sp_send's per-record length (the
        # count of command bytes it writes into the SYS$INPUT mailbox with
        # IO$_WRITEVBLK). Forcing it to zero makes sp_send drain MMK's command
        # stream but deliver EMPTY records, so the driven DCL receives no command
        # and no end-of-command marker. After substitution the text no longer
        # matches, so a second apply is a no-op (the selftest requires).
        sed -i 's|    uint16_t len = d->dsc$w_length;|    uint16_t len = 0; /* NEGCTL mmk-drive-command-not-sent */|' "$_file";;

    mmk-build-image-not-activated)
        # UNIQUE TEXT, no range anchor needed: `int fc_status =
        # dcl_activate_image(ctx, image_spec, linux_path, argv);` occurs exactly
        # once in dcl_cmd_process.c -- it is the ONE point dcl_exec_foreign_command
        # actually runs the resolved image. Replacing the call with SS$_NORMAL
        # makes a foreign command REPORT success while activating nothing (the
        # (void) casts keep linux_path/argv used, so no unused-variable warning;
        # dcl_activate_image itself stays defined and is still called by the RUN
        # path in the same file, so it does not become an unused static). For the
        # MMK build drive, the driven TCC command then 'succeeds' without running
        # TCC.EXE, so no object is produced -- the vms-d1b facade. After
        # substitution the text no longer matches, so a second apply is a no-op
        # (the selftest requires).
        sed -i 's|    int fc_status = dcl_activate_image(ctx, image_spec, linux_path, argv);|    int fc_status = SS$_NORMAL; (void)ctx; (void)image_spec; (void)linux_path; (void)argv; /* NEGCTL mmk-build-image-not-activated */|' "$_file";;

    p0-map-not-recorded)
        # RANGE-ANCHORED to vms_ioctl_p0_map's own body: `proc->p0_base =
        # args.base;` immediately followed by `proc->p0_limit = args.limit;`
        # occurs exactly once in the file, inside the spin_lock(&proc->
        # p0_lock)/spin_unlock() pair that is this ioctl's whole success
        # path -- vms_ioctl_p0_unmap's own body only READS proc->p0_base/
        # p0_limit (into args, before clearing them), never assigns them
        # from args, so there is no second occurrence to disambiguate
        # against. Deleting the two stores and leaving `args.status =
        # SS__NORMAL;` right after them makes P0_MAP report success while
        # the executive keeps recording no extent at all -- INV-6's exact
        # shape: honest failure or a fact the executive genuinely holds,
        # never a claimed success over nothing. After substitution neither
        # deleted assignment's text remains, so a second apply finds
        # nothing left to match -- the no-op selftest requires.
        sed -i '/^long vms_ioctl_p0_map/,/^}$/ {
            s|^    proc->p0_base = args.base;$|    /* NEGCTL p0-map-not-recorded: extent not stored */|
            s|^    proc->p0_limit = args.limit;$||
        }' "$_file";;

    p1-map-not-recorded)
        # RANGE-ANCHORED to vms_ioctl_p1_map's own body: `proc->p1_base =
        # args.base;` immediately followed by `proc->p1_limit = args.limit;`
        # occurs exactly once in the file -- same shape as p0-map-not-
        # recorded's own anchor in vms_p0.c, mirrored here for vms_p1.c.
        # Deleting the two stores and leaving `args.status = SS__NORMAL;`
        # right after them makes P1_MAP report success while the executive
        # keeps recording no extent at all -- INV-6's exact shape. After
        # substitution neither deleted assignment's text remains, so a
        # second apply finds nothing left to match -- the no-op selftest
        # requires.
        sed -i '/^long vms_ioctl_p1_map/,/^}$/ {
            s|^    proc->p1_base = args.base;$|    /* NEGCTL p1-map-not-recorded: extent not stored */|
            s|^    proc->p1_limit = args.limit;$||
        }' "$_file";;

    p0-unmap-clears-p1)
        # RANGE-ANCHORED to vms_ioctl_p0_unmap's own body: `proc->p0_base = 0;`
        # immediately followed by `proc->p0_limit = 0;` occurs exactly once in
        # the file (vms_ioctl_p0_map only ever ASSIGNS p0_base/p0_limit FROM
        # args, never to the literal 0, so there is no second occurrence to
        # disambiguate against). Appending two more clears -- of p1_base and
        # p1_limit -- right after P0_UNMAP's own clears, still inside the same
        # spin_lock(&proc->p0_lock)/spin_unlock() pair that is this ioctl's
        # whole body, makes image rundown's P0 teardown silently wipe the
        # process-permanent P1 extent too: exactly the missing P0/P1
        # separation this increment exists to make structurally impossible.
        # After substitution the matched line itself carries a trailing
        # comment, so it no longer matches "proc->p0_limit = 0;$" (end of
        # line right after the semicolon) -- and the two newly-inserted
        # lines name p1_base/p1_limit, not p0_limit, so neither matches
        # either. A second apply therefore finds nothing left to match --
        # the no-op selftest requires.
        sed -i '/^long vms_ioctl_p0_unmap/,/^}$/ {
            s|^    proc->p0_limit = 0;$|    proc->p0_limit = 0; /* NEGCTL p0-unmap-clears-p1 */\n    proc->p1_base = 0;\n    proc->p1_limit = 0;|
        }' "$_file";;

    super-mode-escalation)
        # UNIQUE TEXT, no range anchor needed: this exact disjunction (EXEC
        # OR SUPER) occurs once in the file -- vms_ioctl_setmode's own
        # raise-mode branch. Dropping "|| args.mode == PSL_C_SUPER" reverts
        # SUPER to this item's pre-fix behaviour while leaving KERNEL's own
        # branch (a few lines above, a different `if`) and EXEC's own
        # privilege check (still reached for args.mode == PSL_C_EXEC)
        # completely untouched -- a caller requesting EXEC is unaffected;
        # only a caller requesting SUPER stops being checked at all.
        sed -i 's|^        } else if (args\.mode == PSL_C_EXEC \|\| args\.mode == PSL_C_SUPER) {$|        } else if (args.mode == PSL_C_EXEC) { /* NEGCTL super-mode-escalation: SUPER no longer requires privilege */|' "$_file";;

    image-rundown-without-entry)
        # UNIQUE TEXT, no range anchor needed: this exact guard occurs once
        # in the file -- vms_ioctl_image_rundown's own image_active check.
        # `0 &&` is the same always-false idiom access-mode-escalation and
        # lock-valblk-grant-not-delivered already use in this file: the
        # guard's THEN-branch (the refusal) becomes structurally
        # unreachable rather than deleted, so nothing after it shifts.
        # vms_ioctl_enter_image's own body (the ONLY other place
        # image_active is touched) is a plain assignment, never an `if`
        # naming it, so this pattern cannot match there.
        sed -i 's|^    if (!proc->image_active) {$|    if (0 \&\& !proc->image_active) { /* NEGCTL image-rundown-without-entry */|' "$_file";;

    image-rundown-leaks-user-lock)
        # UNIQUE TEXT, no range anchor needed: vms_proc_rundown_locks() is
        # called exactly once in the file -- vms_ioctl_image_rundown's release
        # block. Replacing that one call with a comment leaves the sibling
        # vms_proc_rundown_channels()/vms_proc_rundown_asts() calls and the
        # mode restore untouched, so the image still runs down and returns to
        # Supervisor; only the image's USER-mode lock stops being released.
        # The declaration in kernel/vms_internal.h is a different file, never
        # $_file for this defect (targets is kernel/vms_access.c), so it is
        # unaffected.
        sed -i 's|    vms_proc_rundown_locks(proc, rundown_mode);|    /* NEGCTL image-rundown-leaks-user-lock: image locks not released */|' "$_file";;

    imgact-p1-not-protected)
        # UNIQUE TEXT, no range anchor needed: the RO protect is the only
        # vms_kif_p1_protect call in the file whose third argument is 0 (the
        # two restore calls pass 1). Replacing it with an unconditional
        # SS$_NORMAL leaves the `if (!(status & 1))` bail-out that follows
        # structurally satisfied, so activation proceeds to ENTER_IMAGE with
        # the critical-P1 page still writable -- nothing after it shifts, and
        # the paired restore-to-writable calls (`, 1)`) are not matched.
        sed -i 's|status = vms_kif_p1_protect(critp1->base, critp1->limit, 0);|status = SS$_NORMAL; /* NEGCTL imgact-p1-not-protected: critical-P1 left writable */|' "$_file";;

    consumer-import-not-bound-to-resident)
        # UNIQUE TEXT, no range anchor needed: `*cell = addr;` is the only store
        # of a resolved resident address into a consumer GOT cell in the file.
        # Replacing it with a no-op that keeps cell/addr referenced (no unused-
        # variable warning that could confuse a -Werror build) leaves the loop's
        # structure and its SS$_NORMAL return intact -- binding still resolves
        # the universal, it just never points the consumer at the resident
        # producer, so the image shares nothing (the LARP shape). The sibling
        # refusal paths (bad magic, not-resident, GSMATCH) return before or
        # around this line and are unaffected.
        sed -i 's|        \*cell = addr;|        (void)cell; (void)addr; /* NEGCTL consumer-import-not-bound-to-resident: import left unbound */|' "$_file";;

    publish-does-not-populate-registry)
        # UNIQUE TEXT, no range anchor needed: the imgact_register_producer(...)
        # call inside imgact_publish_producers is the only one on a single line
        # (imgact_activate's own callers are elsewhere). Replacing it with a
        # SS$_NORMAL assignment that keeps prods referenced leaves publish's loop
        # and its SS$_NORMAL return structurally intact -- publish still reports
        # success, it just records no producer, so the registry stays empty (the
        # runtime state before this gap closed). The malformed-argument refusals
        # return before the loop and are unaffected.
        sed -i 's|        uint32_t st = imgact_register_producer(prods\[i\].soname, prods\[i\].base, prods\[i\].sv);|        uint32_t st = SS$_NORMAL; (void)prods; /* NEGCTL publish-does-not-populate-registry: producer not recorded */|' "$_file";;

    realimg-auxv-argc-wrong)
        # UNIQUE TEXT: the only `w[k++] = 1;` in imgact_build_auxv_stack is argc
        # (every other auxv word is a type keyword or a value != 1). Laying argc
        # as 0 leaves the whole auxv-entry path structurally intact -- the image
        # is still entered at _start on the constructed stack and still runs --
        # but the stack it reads carries a wrong argc, so TESTREAL takes its
        # bad-ABI SYS$EXIT code and skips its resident-service work. Idempotent:
        # after the edit there is no `w[k++] = 1;` left to match.
        sed -i 's|w\[k++\] = 1;.*argc.*|w[k++] = 0; /* NEGCTL realimg-auxv-argc-wrong: argc mis-built */|' "$_file";;

    extern-interp-check-rejects-ours)
        # UNIQUE TEXT: imgact_interp_is_ours()'s final basename-match return is
        # the only `return want[j] == '\0' && bn[j] == '\0';` in the file.
        # Forcing it to 0 makes EVERY PT_INTERP read as foreign, so an external
        # image naming the OVMX loader is refused the in-process path and forks
        # -- the whole point of the external-image flip. The foreign-interp
        # control still returns SS$_UNSUPPORTED and the no-PT_INTERP TESTREAL
        # never reaches this function, so only the external flip's assertions
        # redden. Idempotent: after the edit there is no such return left to match.
        sed -i "s|    return want\[j\] == '\\\\0' \&\& bn\[j\] == '\\\\0';|    return 0; /* NEGCTL extern-interp-check-rejects-ours: OVMX loader rejected */|" "$_file";;

    nonres-producer-not-mapped)
        # UNIQUE TEXT: imgact_map_producer's registration of the producer it
        # just mapped is the only imgact_register_producer(soname, ...) call in
        # the file (imgact_publish_producers, the other caller, is in
        # imgact_prodreg.c -- a different $_file). Forcing status to
        # SS$_UNSUPPORTED leaves the mapped producer unregistered, so the
        # not-resident bind path never finds it and the consumer forks -- the
        # whole point of the non-resident-producer flip. base/sv are referenced
        # earlier (reprotect, imgact_relocate, the C-RTL fence) so no -Werror
        # unused warning; status is read by the `if (!(status & 1))` that
        # follows. Idempotent: after the edit no such call is left to match.
        sed -i 's|    uint32_t status = imgact_register_producer(soname, (uint64_t)base, sv);|    uint32_t status = SS$_UNSUPPORTED; /* NEGCTL nonres-producer-not-mapped: mapped producer not registered */|' "$_file";;

    tlsdesc-offset-not-biased)
        # UNIQUE TEXT: imgact_setup_own_tls's per-entry TP-relative bias is the
        # only `desc[1] += delta;` in the file. Forcing the addend to 0 leaves the
        # descriptor carrying only the variable's module-relative offset, so the
        # image's TLSDESC access lands in DCL's TLS block instead of the image's
        # own -- the clobber the own-PT_TLS increment exists to avoid. The `(void)
        # delta` keeps delta referenced so the mutated file still compiles under
        # -Wall -Wextra (delta is otherwise used only on this line); the image
        # still runs in-process, only its own __thread read is wrong.
        # Idempotent: after the edit no `desc[1] += delta;` is left to match.
        sed -i 's|        desc\[1\] += delta;.*|        desc[1] += 0; (void)delta; /* NEGCTL tlsdesc-offset-not-biased: TP bias dropped */|' "$_file";;

    devtab-getdvi-userspace-alloc-dropped)
        # UNIQUE TEXT: the DVI$_DEVCHAR handler's `if (info->allocated) chars |=
        # DEV$M_ALL;` is the only place sys_device.c derives the allocation bit
        # from the executive's row. Guarding it with `0 &&` drops that bit from
        # the C API's answer (a per-process view -- the vms-dv1 fake class),
        # while info->allocated stays referenced (short-circuit, no -Werror
        # unused warning) and chars is still written and read below. Same
        # constant-false-guard shape as assign-terminal-bypasses-executive.
        # `@` delimiter because the line contains `|=`. Idempotent: after the
        # edit the line reads `if (0 && info->allocated) ...`, which the pattern
        # `if (info->allocated) ...` no longer matches -- a second apply is a
        # BROKEN FIXTURE, as selftest requires.
        sed -i 's@if (info->allocated) chars |= DEV$M_ALL;@if (0 \&\& info->allocated) chars |= DEV$M_ALL; /* NEGCTL devtab-getdvi-userspace-alloc-dropped */@' "$_file";;

    lnm-searchlist-equiv-truncated)
        # RANGE-ANCHORED: `e->num_equiv = a->num_equiv;` appears twice in
        # vms_ioctl_lnm_define -- once in the SUPERSEDE branch, once in the
        # NEW-ENTRY branch this defect targets. `e->scope_key = scope_key;`
        # is written only in the NEW-ENTRY branch (SUPERSEDE never re-derives
        # it), so it is the unambiguous start of the range; `e->name_length`
        # (also NEW-ENTRY-only) closes it. Idempotent: after the edit the
        # range no longer contains the literal `a->num_equiv` text, so a
        # second apply is a no-op -- BROKEN FIXTURE, as selftest requires.
        sed -i '/^    e->scope_key = scope_key;$/,/^    e->name_length/ s|^    e->num_equiv = a->num_equiv;$|    e->num_equiv = 1; /* NEGCTL lnm-searchlist-equiv-truncated */|' "$_file";;

    rightslist-general-hex-as-decimal)
        # UNIQUE TEXT: this is the only strtoul(..., 16) call in the file --
        # parse_value()'s %Xnnnnnnnn branch. The UIC branch a few lines below
        # uses base 8 twice and is untouched. Idempotent: the edit removes
        # the literal `16)` this pattern matches.
        sed -i 's|strtoul(s + 2, \&end, 16);|strtoul(s + 2, \&end, 10); /* NEGCTL rightslist-general-hex-as-decimal */|' "$_file";;

    sysuaf-uic-radix-decimal)
        # UNIQUE TEXT: the sole #define. Both strtoul() call sites in
        # sysuaf.c reference the symbol, not a literal, so this one edit
        # reaches both. Idempotent: the edit removes the literal `8` this
        # pattern matches.
        sed -i 's|^#define SYSUAF_UIC_RADIX     8$|#define SYSUAF_UIC_RADIX     10  /* NEGCTL sysuaf-uic-radix-decimal */|' "$_file";;

    sysuaf-uic-writeback-decimal)
        # Flip the WRITER's octal UIC formatting to decimal. UNIQUE TEXT: the
        # two sysuaf_format_record() snprintf format strings are the only
        # `%o|%o` in the file (the with-LGICMD and without-LGICMD variants).
        # Idempotent: the edit removes the `%o|%o` each pattern matches, so a
        # second apply finds nothing.
        sed -i 's@"%s|%s|%o|%o|%s|%s|%s|%s"@"%s|%s|%u|%u|%s|%s|%s|%s" /* NEGCTL sysuaf-uic-writeback-decimal */@' "$_file"
        sed -i 's@"%s|%s|%o|%o|%s|%s|%s"@"%s|%s|%u|%u|%s|%s|%s" /* NEGCTL sysuaf-uic-writeback-decimal */@' "$_file";;

    bg-recv-length-zeroed)
        # RANGE-ANCHORED to vms_ioctl_bg_recv's own body: the 8-space
        # "a->len = (uint32_t)n;" also appears in vms_ioctl_bg_send (its
        # send-count assignment, earlier in the file), so the range opens at
        # "^long vms_ioctl_bg_recv" and closes at that function's own column-0
        # "}" to exclude send's copy. Zeroing the received length reports a
        # completed recv of 0 bytes -- the echo comes back with a wrong IOSB
        # byte count. After substitution no "a->len = (uint32_t)n;" is left in
        # the range, so a second apply is the no-op the selftest requires.
        sed -i '/^long vms_ioctl_bg_recv/,/^}$/ s|        a->len = (uint32_t)n;|        a->len = 0; /* NEGCTL bg-recv-length-zeroed */|' "$_file";;

    tcpip-ftp-get-length-dropped)
        # Drop the accumulated received length in tcpip_ftp_get()'s data-drain
        # loop: 'total += take;' -> 'total += 0;'. The string is UNIQUE in the
        # header (only tcpip_ftp_get accumulates a transfer this way), so no
        # range anchor is needed; after substitution no 'total += take;' is left,
        # making a second apply the no-op the selftest requires.
        sed -i 's|total += take;          /\* NEGCTL tcpip-ftp-get-length-dropped \*/|total += 0; /* NEGCTL tcpip-ftp-get-length-dropped */|' "$_file";;

    bgsock-recv-length-zeroed)
        # Zero the received byte count in ovmx_recv(), anchored on its own NEGCTL
        # comment so the text is unique: 'n = iosb$w_bcnt;' -> 'n = 0;'. recv then
        # reports EOF and the reader gets nothing. After substitution no
        # 'n = iosb$w_bcnt;' with that comment is left, so a second apply is the
        # no-op the selftest requires.
        sed -i 's|n = iosb.iosb$w_bcnt;               /\* NEGCTL bgsock-recv-length-zeroed \*/|n = 0; /* NEGCTL bgsock-recv-length-zeroed */|' "$_file";;

    vmsfs-mountvis-crossproc-resolve-disabled)
        # UNIQUE TEXT: vmsfs_device_resolve_executive's own /proc/mounts guard;
        # `if (!mount_table_has(mp))` occurs once in the file. Forcing it true
        # with `1 ||` makes every cross-process resolve of a MOUNTed unit return
        # NOSUCHDEV. After substitution the guard reads
        # `if (1 || !mount_table_has(mp))`, so a second apply finds no
        # `if (!mount_table_has(mp))` left -- the no-op selftest requires.
        sed -i 's|    if (!mount_table_has(mp))|    if (1 \|\| !mount_table_has(mp)) /* NEGCTL vmsfs-mountvis-crossproc-resolve-disabled */|' "$_file";;

    initialize-home-magic-not-written)
        # UNIQUE TEXT: the home-block magic write in format_volume
        # (`hb.hb_magic        = VMSFS_HOME_MAGIC;`) is the only occurrence of
        # that assignment in the file. Zeroing it leaves a written-but-invalid
        # home block on the real device. After substitution no
        # `hb.hb_magic        = VMSFS_HOME_MAGIC;` is left -- the no-op selftest
        # requires.
        sed -i 's|hb.hb_magic        = VMSFS_HOME_MAGIC;|hb.hb_magic        = 0; /* NEGCTL initialize-home-magic-not-written */|' "$_file";;

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
#      either (a) NAMED by some defect's suites_red glob, or (b) in
#      SCOPE_OUT_SUITES with a stated reason. A suite named by nothing has no
#      declared attribution at all, and round 1 shipped two of those (the
#      vmsfs pair) without saying so.
#      A suite that appears ONLY in some defect's blind_suites USED TO satisfy
#      this and print a `NOTE:`. It no longer does. Being declared blind is a
#      record that no glob names the suite -- that is the definition of
#      uncovered, and printing it as a note is how the set-cover deletion
#      measured in the header dropped two suites out of the named set while
#      exiting 0. blind_suites keeps its other job (pinning a named suite GREEN
#      under one defect); it is no longer a substitute for coverage.
#   3. SCOPE CONSISTENCY. Nothing under SCOPE_OUT_UNIT_DIRS may be named by a
#      defect: if it is, the scope statement is wrong and must be corrected
#      rather than quietly outvoted by a declaration.
#   4. ANCHORS. Every defect must be anchored, by a `/* negctl: <defect> */`
#      comment, at an assertion in a suite source it is allowed to redden; and
#      every anchor found in a suite source must name a live defect. This
#      floors the PAIRING between the manifest and the suite sources -- a
#      deletion confined to this one file cannot pass -- and its universe
#      comes from outside this file (the suite sources). It does NOT floor the
#      list's SIZE: deleting a defect's line, its two arms, AND its anchor
#      together still passes, still prints a smaller number as PASS (vms-d894).
#      See the header and section 6 below for the size floor.
#   5. ARM AGREEMENT. Every defect must have exactly two `case` arms in this
#      script (defect_field and apply_edit) and every arm must name a live
#      defect. Deleting one line from DEFECTS and leaving forty lines of
#      documented metadata behind is not a legitimate edit; neither is an arm
#      for a defect nobody runs.
#   6. COUNT FLOOR (vms-d894). DEFECTS must not have fewer entries than the
#      number recorded in tests/qemu/facility_defects_floor.txt, a file this
#      command does not itself write. NOT tamper-proof -- see section 6's own
#      comment in the function body and the header for the price this buys.
#
# None of the six is an execution claim. Read the header before trusting any
# PASS line as more than "these declarations agree with each other". The PASS
# lines below also name the exclusions explicitly, so a reader cannot take
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

    # --- 0. the execution-sourced record, read ONCE ----------------------
    # Loaded before section 2 because two different sections consume it: the
    # suite populations (vms-659) and the observed count floor (vms-d894).
    # _cov_rec_state is one of:
    #   ok       validated, populations derived, usable
    #   refuse   present and contradicts this tree -- already printed, and
    #            the caller's rc is set to 1 below
    #   absent   no record has been committed yet
    #   nolib    the reader itself is missing
    _cov_recw=$(mktemp -d) || { echo "FAIL: coverage: mktemp -d failed" >&2; return 2; }
    _cov_rec=""
    _cov_rec_state=absent
    if [ "$FNR_LOADED" -ne 1 ]; then
        _cov_rec_state=nolib
    else
        _cov_rec=$(fnr_record_path "$(dirname "$SELF")")
        if [ -f "$_cov_rec" ]; then
            printf '%s\n' $DEFECTS >"$_cov_recw/defects"
            # Built only when there is a record to check it against: this runs
            # in the ordinary ctest job, and it is one sed(1) per defect.
            : >"$_cov_recw/texts.raw"
            for _cov_td in $DEFECTS; do
                { defect_field "$_cov_td" require_fail; defect_field "$_cov_td" knock_on_fail; } \
                    | sed "s/^/$_cov_td	/" >>"$_cov_recw/texts.raw"
            done
            # An empty field echoes a blank line, which becomes "<defect><TAB>".
            grep -v '	[ 	]*$' "$_cov_recw/texts.raw" >"$_cov_recw/texts" || true
            if fnr_validate "$_cov_rec" "$_cov_recw/defects" "$_cov_recw/texts" "$_cov_recw"; then
                _cov_rec_state=ok
            else
                _cov_rec_state=refuse
            fi
        fi
    fi

    # --- 1. translation units -------------------------------------------
    # Executive facility .c files live in BOTH src/kernel/ (the Linux glue +
    # the not-yet-extracted facilities) AND src/kernel-core/ (the
    # substrate-agnostic facilities being promoted onto the exec_* shim, rd
    # vms-ec4 / epic vms-8e8 -- event flags is the first). Both dirs are
    # scanned: a facility does not stop needing a negative control when it moves
    # to the shared core. Headers (exec_*.h) are not translation units and are
    # skipped by the *.c glob. The kernel-core glob may legitimately be empty on
    # a tree before any facility has moved (`[ -f ]` guards that).
    _missing=""
    for _cov_c in "$_cov_root"/kernel/*.c "$_cov_root"/kernel-core/*.c; do
        [ -f "$_cov_c" ] || continue
        case "$_cov_c" in
            "$_cov_root"/kernel-core/*) _rel="kernel-core/$(basename "$_cov_c")";;
            *)                          _rel="kernel/$(basename "$_cov_c")";;
        esac
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
        echo "PASS: every src/{kernel,kernel-core}/*.c translation unit is named by some defect's targets declaration"
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
    # ALSO test_imgact_*.c (main-red classification fix): pure-userspace
    # suites (test_imgact_bind, test_imgact_publish -- were test_syssvc_
    # imgact_bind/publish through #225/#226) that never touch /dev/vms, so
    # they are deliberately OUTSIDE ci.yml's test_kmod_*/test_syssvc_*
    # policed set (see tests/qemu/CMakeLists.txt's comment on them) but they
    # ARE covered by negctl anchors and belong in THIS coverage universe --
    # widening it here, unlike ci.yml, does not put them under the
    # test_syssvc_*-must-honest-skip-77 contract.
    _cov_derived=$(ls "$_cov_tests"/test_kmod_*.c "$_cov_tests"/test_syssvc_*.c "$_cov_tests"/test_imgact_*.c 2>/dev/null \
                   | xargs -n1 basename | sed 's/\.c$//' | sort)
    _uncovered=""
    _blind_only=""
    _named_suites=""
    _n_named=0
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
            _n_named=$((_n_named + 1))
            _named_suites="$_named_suites $_cov_s"
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
        echo "FAIL: derived suite(s) NAMED BY NO defect's suites_red:$_uncovered"
        echo "  A suite no glob names is not attributed to anything, so nothing here even"
        echo "  claims it can go red. Give it a facility control, or add it to"
        echo "  SCOPE_OUT_SUITES with a reason -- but do not leave it silent."
        _cov_rc=1
    else
        echo "PASS: $_n_named derived suite(s) are each NAMED by some defect's suites_red glob"
        echo "  (a STRING MATCH against the manifest's own attribution claim -- not an"
        echo "   execution. See the header: only run_facility_negctl.sh, in CI, executes.)"
    fi

    # --- 2b. the two populations, separately counted (rd vms-659) --------
    # "PROVEN able to go red" is restored HERE and only here, and only for the
    # suites a past executed run recorded a failing assertion in. Everything
    # else stays NAMED. Two populations, two cardinals, never summed into one
    # sentence -- the defect this replaces was a single number that read as an
    # execution result and was a glob match.
    _n_proven=0
    _proven_suites=""
    _named_only=""
    if [ "$_cov_rec_state" = ok ]; then
        for _cov_s in $_named_suites; do
            if grep -qx "$_cov_s" "$_cov_recw/fnr_red_suites" 2>/dev/null; then
                _n_proven=$((_n_proven + 1))
                _proven_suites="$_proven_suites $_cov_s"
            else
                _named_only="$_named_only $_cov_s"
            fi
        done
        _n_exec=$(grep -c . "$_cov_recw/fnr_exec" 2>/dev/null || true)
        _n_execpass=$(grep -c . "$_cov_recw/fnr_exec_pass" 2>/dev/null || true)
        _n_complete=$(grep -c . "$_cov_recw/fnr_complete" 2>/dev/null || true)
        _n_incomplete=$(grep -c . "$_cov_recw/fnr_incomplete" 2>/dev/null || true)
        _n_unproven=$(grep -c . "$_cov_recw/fnr_unproven" 2>/dev/null || true)
        _n_redrows=$(grep -c . "$_cov_recw/fnr_red" 2>/dev/null || true)
        : "${_n_exec:=0}" "${_n_execpass:=0}" "${_n_complete:=0}"
        : "${_n_incomplete:=0}" "${_n_unproven:=0}" "${_n_redrows:=0}"
        echo "OBSERVED: of those $_n_named, $_n_proven are PROVEN ABLE TO GO RED and" \
             "$((_n_named - _n_proven)) NAMED ONLY."
        echo "  PROVEN = a past run recorded a failing assertion in the suite:$_proven_suites"
        [ -n "$_named_only" ] && echo "  NAMED ONLY (declared reddenable, never observed red):$_named_only"
        echo "  Source: $(basename "$_cov_rec"), generated" \
             "$(fnr_header "$_cov_rec" generated-at), tree $(fnr_header "$_cov_rec" tree-commit)."
        echo "  It records $_n_exec defect(s) of this manifest as EXECUTED" \
             "($_n_execpass with a passing control, $_n_complete of those carrying every"
        echo "  assertion text the manifest now names for them, $_n_incomplete carrying" \
             "fewer), across $_n_redrows observed failing assertion(s);"
        echo "  $_n_unproven manifest defect(s) are in no run this record covers."
        echo "  WHAT THAT PROVES HERE: a PAST run, on the tree named above, observed"
        echo "  these results. It does NOT say they hold now -- nothing on a host"
        echo "  without a real /dev/vms can say that. The live driver in CI re-emits"
        echo "  this record and reds on any disagreement with the committed copy;"
        echo "  that comparison, not this file, is what keeps it honest."
    else
        _named_only="$_named_suites"
        echo "OBSERVED: of those $_n_named, 0 are PROVEN ABLE TO GO RED and $_n_named are"
        echo "  NAMED ONLY -- NOT MEASURED: no usable execution record was read (see"
        echo "  section 6). Every claim above this line is a string relation over"
        echo "  declarations the tree makes about itself."
    fi
    # A suite that appears ONLY as somebody's blind_suites is declared and
    # tracked, but no defect's suites_red glob names it -- so nothing here
    # even claims its assertions can fire. That is not coverage, and until
    # vms-279 it was printed as a NOTE and passed.
    if [ -n "$_blind_only" ]; then
        echo "FAIL: suite(s) declared blind but named by no defect's suites_red:$_blind_only"
        echo "  A blind declaration records that a suite does not catch one defect. A"
        echo "  suite that no defect catches is UNCOVERED, however many defects declare"
        echo "  it blind. Give it a control that names an assertion in it, or move it to"
        echo "  SCOPE_OUT_SUITES with a reason. Do NOT widen some existing defect's"
        echo "  suites_red to swallow it: that glob is an attribution claim, and it is"
        echo "  supposed to be as narrow as the measurement."
        _cov_rc=1
    fi

    # --- 4. anchors: the floor under the SIZE of the DEFECTS list ---------
    # Universe derived from the suite sources, not from this file. See header.
    _anch_tmp=$(mktemp -d) || { echo "FAIL: coverage: mktemp -d failed" >&2; return 2; }
    : >"$_anch_tmp/fail"; : >"$_anch_tmp/named"; : >"$_anch_tmp/suites"
    _defs_sp=$(echo $DEFECTS)

    _anch_raw=$( (cd "$_cov_tests" && grep -Hno \
        '/\* negctl\(-knockon\)\{0,1\}: [a-z0-9][a-z0-9-]* \*/' \
        test_kmod_*.c test_syssvc_*.c test_imgact_*.c) 2>/dev/null )

    printf '%s\n' "$_anch_raw" | while IFS= read -r _anch; do
        [ -n "$_anch" ] || continue
        _af=${_anch%%:*}; _arest=${_anch#*:}
        _aln=${_arest%%:*}; _atxt=${_arest#*:}
        _akind=${_atxt#/\* }; _akind=${_akind%%:*}
        _aname=${_atxt#*: }; _aname=${_aname% \*/}
        _abase=${_af%.c}

        case " $_defs_sp " in
            *" $_aname "*) ;;
            *) echo "$_af:$_aln anchors '$_aname', which is not in DEFECTS." \
                    "Either the defect was deleted (put it back) or the anchor is a typo." \
                    >>"$_anch_tmp/fail"; continue;;
        esac
        case " $SCOPE_OUT_SUITES " in
            *" $_abase "*) echo "$_af:$_aln anchors '$_aname' in a suite declared OUT OF SCOPE." \
                                "The manifest says two things." >>"$_anch_tmp/fail"; continue;;
        esac

        _ahit=0
        for _ag in $(defect_field "$_aname" suites_red); do
            # shellcheck disable=SC2254
            case "$_abase" in $_ag) _ahit=1; break;; esac
        done
        [ "$_ahit" -eq 1 ] || echo "$_af:$_aln anchors '$_aname', but that defect's suites_red" \
            "does not match $_abase -- the anchor and the attribution claim disagree." \
            >>"$_anch_tmp/fail"

        if [ ! -f "$_anch_tmp/txt.$_aname" ]; then
            { defect_field "$_aname" require_fail; defect_field "$_aname" knock_on_fail; } \
                | tr -d '"\\' | tr -s ' ' | grep -v '^ *$' >"$_anch_tmp/txt.$_aname"
        fi
        _aseg=$(sed -n "$((_aln + 1)),$((_aln + 12))p" "$_cov_tests/$_af" \
                | tr '\n\t' '  ' | sed 's/;.*//' | tr -d '"\\' | tr -s ' ')
        printf '%s\n' "$_aseg" | grep -qF -f "$_anch_tmp/txt.$_aname" \
            || echo "$_af:$_aln anchors '$_aname' but the statement under it names none of" \
                    "that defect's require_fail/knock_on_fail texts." >>"$_anch_tmp/fail"

        echo "$_abase" >>"$_anch_tmp/suites"
        [ "$_akind" = "negctl" ] && echo "$_aname" >>"$_anch_tmp/named"
        true
    done

    _anch_bad=$(cat "$_anch_tmp/fail")
    _unanchored=""
    for _cov_d in $DEFECTS; do
        grep -qx "$_cov_d" "$_anch_tmp/named" 2>/dev/null || _unanchored="$_unanchored $_cov_d"
    done
    _unanch_suites=""
    for _cov_s in $_cov_derived; do
        case " $SCOPE_OUT_SUITES " in *" $_cov_s "*) continue;; esac
        grep -qx "$_cov_s" "$_anch_tmp/suites" 2>/dev/null || _unanch_suites="$_unanch_suites $_cov_s"
    done
    _n_anch=$(printf '%s\n' "$_anch_raw" | grep -c . || true)
    _n_anch_suites=$(sort -u "$_anch_tmp/suites" 2>/dev/null | grep -c . || true)
    rm -rf "$_anch_tmp"

    _anch_rc=0
    if [ -n "$_anch_bad" ]; then
        echo "FAIL: anchor(s) that disagree with the manifest:"
        echo "$_anch_bad" | sed 's/^/      /'
        _cov_rc=1; _anch_rc=1
    fi
    if [ -n "$_unanchored" ]; then
        echo "FAIL: defect(s) with NO /* negctl: ... */ anchor in any suite source:$_unanchored"
        echo "  Every entry has to be findable AT the assertion it names, so that a"
        echo "  deletion here alone (this file only) cannot pass -- the suite source"
        echo "  has to change too. That is a floor on PAIRING, not on the list's SIZE;"
        echo "  see the header (vms-d894) and section 6 below for the size floor."
        echo "  Add the anchor above the CHECK() its require_fail names."
        _cov_rc=1; _anch_rc=1
    fi
    if [ -n "$_unanch_suites" ]; then
        echo "FAIL: in-scope suite(s) with NO anchor at all:$_unanch_suites"
        echo "  No defect names an assertion in these, so nothing here even claims they"
        echo "  can fail -- being matched by somebody's suites_red glob is a permission,"
        echo "  not a measurement."
        _cov_rc=1; _anch_rc=1
    fi
    if [ "$_anch_rc" -eq 0 ]; then
        echo "PASS: all $(echo $DEFECTS | wc -w) defect(s) anchored by $_n_anch marker(s)" \
             "across $_n_anch_suites in-scope suite source(s)"
        echo "  (this floors PAIRING against a ONE-FILE EDIT ONLY -- deleting a defect"
        echo "   and its anchors together still shrinks the list and still passes THIS"
        echo "   check. It does not floor the list's SIZE; see section 6 below, priced"
        echo "   honestly rather than claimed closed -- vms-d894.)"
    fi

    # --- 5. the two case blocks must agree with the list -----------------
    if [ -f "$SELF" ]; then
        _arms=$(grep -E '^    [a-z0-9][a-z0-9-]*\)$' "$SELF" | tr -d ' )')
        _orphan_arm=""
        for _l in $(printf '%s\n' "$_arms" | sort -u); do
            case " $_defs_sp " in *" $_l "*) ;; *) _orphan_arm="$_orphan_arm $_l";; esac
        done
        _short_arm=""
        for _cov_d in $DEFECTS; do
            _n_arm=$(printf '%s\n' "$_arms" | grep -cx "$_cov_d" || true)
            [ "$_n_arm" -eq 2 ] || _short_arm="$_short_arm $_cov_d($_n_arm/2)"
        done
        if [ -n "$_orphan_arm" ]; then
            echo "FAIL: case arm(s) for defect(s) not in DEFECTS:$_orphan_arm"
            echo "  A name was removed from the list and its metadata left behind. Metadata"
            echo "  nothing runs is not a control; put the name back or delete both arms in"
            echo "  the same change, where a reviewer can see the size of it."
            _cov_rc=1
        fi
        if [ -n "$_short_arm" ]; then
            echo "FAIL: defect(s) without exactly two case arms (defect_field, apply_edit):$_short_arm"
            _cov_rc=1
        fi
    else
        echo "FAIL: cannot read \$SELF ($SELF) to check the case arms against DEFECTS."
        _cov_rc=1
    fi

    # --- 6. count floor (vms-d894) ----------------------------------------
    # None of sections 1-5 floors the SIZE of DEFECTS: a defect's line, its
    # two arms, and its one anchor can all be deleted together (20 lines
    # across 2 files, MEASURED) and every check above still passes, printing
    # a smaller number as a PASS. This section adds a floor sourced from a
    # file this function does not itself hold the pen for --
    # tests/qemu/facility_defects_floor.txt -- so a shrink below the recorded
    # floor now also requires a THIRD file to change, in a diff whose only
    # content is the number being lowered.
    #
    # NOT TAMPER-PROOF (see the header, vms-d894): the floor file can be
    # edited in the same commit that shrinks DEFECTS. What this buys is that
    # the edit is no longer silent -- raising the price from 20 lines/2 files
    # to >=21 lines/3 files, one of which has no other job. A disclosed,
    # priced residual, not a claimed closure.
    _floor_file="$(dirname "$SELF")/facility_defects_floor.txt"
    _n_defects=$(echo $DEFECTS | wc -w)
    if [ -f "$_floor_file" ]; then
        _floor=$(grep -Ev '^[[:space:]]*(#|$)' "$_floor_file" | tail -1 | tr -d '[:space:]')
        case "$_floor" in
            ''|*[!0-9]*)
                echo "FAIL: $_floor_file's floor value is not a bare integer: '$_floor'"
                _cov_rc=1;;
            *)
                if [ "$_n_defects" -lt "$_floor" ]; then
                    echo "FAIL: DEFECTS has $_n_defects entries, below the floor of $_floor" \
                         "recorded in $_floor_file."
                    echo "  This floor is a derived count check, not a claim of tamper-proofing"
                    echo "  (vms-d894): raising or lowering it is one edit to one more file. What"
                    echo "  it buys is that a shrink is no longer silent -- lower it only for a"
                    echo "  tracked, intentional removal, in the same change that explains why."
                    _cov_rc=1
                else
                    echo "PASS: $_n_defects defect(s) >= floor $_floor recorded in $_floor_file"
                fi;;
        esac
    else
        echo "FAIL: $_floor_file is missing -- the count floor (vms-d894) has nothing to read."
        _cov_rc=1
    fi

    # --- 6b. the OBSERVED count floor (vms-d894, the execution-sourced half)
    # The floor above is a hand-set integer in a file the deleter can edit in
    # the same commit. This one is the number of defects a PAST RUN OF THE
    # DRIVER ACTUALLY EXECUTED, read out of the record it emitted -- and
    # deleting a manifest entry today cannot retroactively change what that run
    # observed.
    #
    # THE TWO FLOORS ARE BOTH APPLIED AND NEITHER REPLACES THE OTHER. A record
    # covering fewer defects (a partial run) can therefore never LOWER the
    # declared floor; it can only add a second, independently sourced one.
    #
    # WHAT IT IS STILL NOT: tamper-proof. A deleter who removes a defect from
    # DEFECTS and removes that defect's rows from the record has told the truth
    # about a smaller manifest, and the live CI run agrees with them. The price
    # is what changed: the record's rows are one per OBSERVED ASSERTION, so the
    # deletion is proportional and visible instead of being one integer.
    # Closing it entirely needs a floor from outside the commit -- the previous
    # commit's copy of this record, or an external attestation. There is not
    # one, and this is a disclosure, not a claim.
    #
    # WRITTEN AS if/elif AND NOT AS A `case`, deliberately: section 5 above
    # finds this file's defect metadata by scanning $SELF for lines matching
    # `^    [a-z0-9-]*\)$`, so a four-space-indented `ok)` arm here is read as
    # a case arm for a defect named "ok". MEASURED -- the first draft of this
    # section did exactly that and section 5 correctly reported "case arm(s)
    # for defect(s) not in DEFECTS: absent nolib ok refuse". Section 5 is
    # right; this is the code that has to move.
    if [ "$_cov_rec_state" = ok ]; then
        _obs_floor=$(grep -c . "$_cov_recw/fnr_exec" 2>/dev/null || true)
        : "${_obs_floor:=0}"
        echo "PASS: $_obs_floor of this manifest's $_n_defects defect(s) are" \
             "OBSERVED-EXECUTED by the run recorded in $(basename "$_cov_rec")"
        echo "  (derived from that record's RUN rows, not from any integer anybody"
        echo "   wrote down.)"
        echo "  THE ENFORCEMENT HERE IS THE REFUSAL, NOT A COMPARISON, and saying so is"
        echo "  the point: the observed count is intersected with DEFECTS before it is"
        echo "  counted, so it can never EXCEED it and 'observed <= declared' would be"
        echo "  an assertion that cannot fail. What cannot be gotten past is above --"
        echo "  a record naming a defect DEFECTS no longer has stops this gate"
        echo "  certifying anything at all. That is what makes this count"
        echo "  execution-sourced rather than declared."
    elif [ "$_cov_rec_state" = refuse ]; then
        # fnr_validate has already printed the REFUSING lines and why.
        echo "  -> the observed count floor and the PROVEN suite population are BOTH"
        echo "     withheld by that refusal. The declared floor above still applies;"
        echo "     it is the weaker one, and it is now the only one."
        _cov_rc=1
    elif [ "$_cov_rec_state" = absent ]; then
        echo "NOT MEASURED: no execution record at $_cov_rec, so there is no"
        echo "  observed count floor and NO suite is PROVEN able to go red -- only"
        echo "  NAMED. The record is emitted by tests/qemu/run_facility_negctl.sh,"
        echo "  which runs in CI only (rd vms-b1f: it cannot run on a dev host at"
        echo "  all). Take it from that job's output and commit it."
        echo "  This is a stated degradation, not a pass with a smaller claim: the"
        echo "  two cardinals it would have produced are absent rather than guessed."
    else
        echo "FAIL: REFUSING to certify: the execution-record reader"
        echo "      $FNR_LIB is missing, so section 2b's populations and the observed"
        echo "      count floor cannot be computed at all."
        echo "  -> deleting the reader must not be a way to make this gate print fewer"
        echo "     claims and still pass."
        _cov_rc=1
    fi
    rm -rf "$_cov_recw"

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

        rm -rf "$_st_tmp/tree" "$_st_tmp/tests" "$_st_tmp/tools"
        mkdir -p "$_st_tmp/tree"
        # libvms, vmsdcl, vmsrms and vmslnm are copied too: a defect may
        # target the PRODUCT half of an interface (creprc-handshake-eintr and
        # run-detached-name-dropped do, rms-create-filespec-not-translated
        # targets vmsrms/rms_core.c -- a userspace consumer of vmsfs's
        # translation, the same class as the vmsdcl entries -- and
        # lnm-manager-delete-noop targets vmslnm/lnm_client.c, the
        # DCL/vmsfs-facing manager built on top of the executive-resident
        # LNM$SYSTEM), and a target this function cannot see would be
        # reported as a dead anchor on every run.
        # vmsfs is copied too (vms-6c6): vmsfs-mountvis-crossproc-resolve-disabled
        # targets vmsfs/vmsfs_device.c -- the userspace mount-visibility library
        # (a product-half consumer of the kernel mount table, same class as the
        # vmsrms/vmsdcl entries above), which vmsdcl links but is its own
        # translation unit under src/vmsfs. Without it that defect's anchor would
        # be reported as a dead fixture on every run.
        # vmstcpip is copied too (vms-6c6): tcpip-ftp-get-length-dropped targets
        # vmstcpip/services/tcpip_client.h -- the shared TCP/IP client engine
        # header the DCL FTP/TELNET verbs ship (a product-half consumer of the
        # executive-resident BGn: device). It was absent from this copy set, so
        # that defect was reported as a dead fixture ("its sed anchor no longer
        # matches") on every selftest run even though its anchor is fine.
        if ! cp -a "$_st_root/kernel" "$_st_root/kernel-core" \
                   "$_st_root/libvmssys" "$_st_root/libvms" \
                   "$_st_root/vmsdcl" "$_st_root/vmsrms" "$_st_root/vmslnm" \
                   "$_st_root/vmsfs" "$_st_root/vmstcpip" \
                   "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root/{kernel,kernel-core,libvmssys,libvms,vmsdcl,vmsrms,vmslnm,vmsfs,vmstcpip} for the self-test"
            rm -rf "$_st_tmp"
            return 2
        fi
        # tools/ staged as a SIBLING of tree (same convention as tests/qemu and
        # tests/corpus below, vms-6c6): initialize-home-magic-not-written targets
        # "../tools/vms_initialize.c" -- INITIALIZE.EXE's source, relative to a
        # src/ root exactly as the real driver's /src/repo/src resolves it. It is
        # a shipped image (a userspace consumer of the executive disk-resolution
        # facility), not a src/ translation unit, so it lives beside src/ here.
        if [ -d "$_st_repo/tools" ]; then
            if ! cp -a "$_st_repo/tools" "$_st_tmp/tools" 2>/dev/null; then
                echo "FAIL: cannot copy $_st_repo/tools for the self-test"
                rm -rf "$_st_tmp"
                return 2
            fi
        fi
        # tests/qemu is staged as a SIBLING of tree (mirroring tree's own
        # role as $_st_root, i.e. src/, with tests/qemu next to it in every
        # real checkout) so a target like scratch-dir-owner-not-system's
        # "../tests/qemu/test_syssvc_scratch_writable.c" -- relative to a
        # src/ root, same convention the real driver's /src/repo/src uses --
        # resolves here exactly as it does in inject_and_run.sh and
        # facility_attribution_negctl.sh. Without this, that defect's own
        # anchor would report a false BROKEN FIXTURE: not because the sed
        # pattern moved, but because this dry run alone never staged the
        # file it targets.
        mkdir -p "$_st_tmp/tests"
        if ! cp -a "$_st_tests" "$_st_tmp/tests/qemu" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_tests for the self-test"
            rm -rf "$_st_tmp"
            return 2
        fi
        # tests/corpus staged as a SIBLING of tree too (same convention), so a
        # target like mmk-drive-command-not-sent's "../tests/corpus/tier3-mmk/
        # ovmx/ovmx_mmk_sp.c" -- the MMK exec-drive companion compiled into
        # MMK.EXE, relative to a src/ root exactly as inject_and_run.sh's
        # /src/repo/src resolves it -- is present for the dry-run apply.
        if [ -d "$_st_repo/tests/corpus" ]; then
            if ! cp -a "$_st_repo/tests/corpus" "$_st_tmp/tests/corpus" 2>/dev/null; then
                echo "FAIL: cannot copy $_st_repo/tests/corpus for the self-test"
                rm -rf "$_st_tmp"
                return 2
            fi
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
    cat "$_st_tests"/test_kmod_*.c "$_st_tests"/test_syssvc_*.c "$_st_tests"/test_imgact_*.c 2>/dev/null \
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
        echo "PASS: every defect's sed mutation injects into the current tree (executed,"
        echo "      real sed + cmp against a throwaway copy), its injection-landed check"
        echo "      demonstrably fires on a no-op re-apply, and every assertion it names"
        echo "      appears literally in a suite source (a text search, not a run --"
        echo "      only run_facility_negctl.sh, in CI, actually prints it)."
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
