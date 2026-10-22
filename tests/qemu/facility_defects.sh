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
# (vms-165: the vmsfs.ko VFS driver -- a SEPARATE filesystem module, not an
# executive facility -- was retired, its ODS-2 paths folded into the executive
# Files-11 ACP in vms.ko which this gate DOES cover. The scope_* declarations
# below, which used to exclude vmsfs.ko's suites, are now empty -- see them.)
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
#     Of those 24, 7 (setprv, chkpriv, dclast, deliverast, alloc, dalloc,
#     ttsetmode) are OVMX-UNWIRED declarations (src/libvmssys/vms_kif.h)
#     with zero product-tree callers -- exempt under the vms-1e1 ruling, not
#     a gap this file can close without inventing a caller. getlki is NO
#     LONGER in this set: vms-b71 wired sys$getlki/sys$getlkiw onto it
#     (src/libvms/syssvc/sys_lock.c), and getlki-grantcount-not-counted
#     above is its negctl control. The other 17 have a real product-side
#     caller (a DCL command or a sys$ wrapper) and are not exempt. Of the
#     16 that were already covered before vms-b71, an existing QEMU assertion
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

# The defects are tests/qemu/defects.d/<name>.defect, one file each, so two PRs
# adding controls never conflict (the list used to be one block here). The
# order is the sorted file names.
DEFECTS_DIR="$(dirname "$SELF")/defects.d"
DEFECTS=$(cd "$DEFECTS_DIR" 2>/dev/null && LC_ALL=C ls *.defect 2>/dev/null | sed 's/\.defect$//')

# ---------------------------------------------------------------------------
# SCOPE, DECLARED
#
# The item's title is "every wired EXECUTIVE facility". These two declarations
# say exactly what that set is and what it is not, so the coverage PASS line
# cannot be read as a broader claim than it makes.
#
# vms-165: the ONLY members of these scope-out sets were the vmsfs.ko VFS
# driver's own suites (test_kmod_vmsfs*, which drove a SEPARATE filesystem
# module, not an executive facility) and its src/kernel/vmsfs translation-unit
# directory. The vmsfs VFS driver was retired (its ODS-2 code paths moved into
# the executive Files-11 ACP in vms.ko, which THIS gate does cover), so those
# suites and that directory no longer exist and both sets are now empty. There
# is no longer any derived suite that is executive-independent, so nothing needs
# scoping out.
# ---------------------------------------------------------------------------
SCOPE_OUT_UNIT_DIRS=""
SCOPE_OUT_SUITES="test_kmod_cluster_seam"

scope_out_why() {
    cat <<'EOF'
test_kmod_cluster_seam -- the FC-P0.2 SUBSTRATE-CONTRACT conformance test, not a
VMS executive facility. It proves the Linux kbackend BINDINGS satisfy the seam
contract the executive is built ON (exec_kbackend_linux.h §14-18: dev_add_pack /
dev_queue_xmit / dev_mc_add / kthread / timer_list / ktime), driven through a
TEST-ONLY harness (vms_module.c, #if OVMX_KTEST_CLUSTER_SEAM). The only
cluster_seam-exclusive code IS that harness -- not a mutable product facility.
Every primitive it exercises is a SHARED substrate primitive: exec_lan_open/
hwaddr/mtu/link_up/mc_add are used by src/kernel-core/vms_pe.c (product), which
feeds the cluster VC/SCS/membership snapshots that the product suites
test_kmod_cluster_*_diag already anchor (pe-vc-snapshot-fabricates-circuit et
al.); the kthread/timer/time primitives are pervasive. So a defect in any of them
reddens those PRODUCT suites, not the seam in isolation -- there is no isolatable
per-facility negctl to anchor here, and scoping it out opens NO coverage hole:
the substrate's correctness is proven positively by this seam test AND
transitively by every facility suite that would fail if a binding broke. A
per-facility product negctl would be a category error (no VMS facility sits
behind it to fault-inject) -- the exact self-declaration/harness-isolation class
Rule 9 / vms-49f distinguishes from runtime behavioral proof.
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
    case "$_d" in *[!a-z0-9-]*|"") echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2;; esac
    [ -f "$DEFECTS_DIR/$_d.defect" ] || { echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2; }
    . "$DEFECTS_DIR/$_d.defect"
    defect_meta
}

# ---------------------------------------------------------------------------
# apply_edit <file> <defect>
#
# The mutation itself. One sed per defect, anchored to an exact source line.
# ---------------------------------------------------------------------------
apply_edit() {
    _file="$1"; _d="$2"
    case "$_d" in *[!a-z0-9-]*|"") echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2;; esac
    [ -f "$DEFECTS_DIR/$_d.defect" ] || { echo "facility_defects.sh: unknown defect '$_d'" >&2; return 2; }
    . "$DEFECTS_DIR/$_d.defect"
    defect_apply
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

    # --- 0c. derived scope-out: the R1 host ladder's OWNED domain (vms-181)
    # NOT a static SCOPE_OUT_UNITS global -- sourced LIVE, every run, from
    # tests/cluster/host/host_defects.sh's own `owned` subcommand, so the two
    # gates cannot drift apart silently. cmd_coverage is called two ways:
    # normally as `cmd_coverage <src-root> <tests-qemu>` (repo root = the
    # parent of <src-root>), and from cmd_selftest as `cmd_coverage
    # "$_st_repo/src" "$_st_repo/tests/qemu"` -- same shape, same derivation.
    _cov_reporoot=$(dirname "$_cov_root")
    _cov_hostdefects="$_cov_reporoot/tests/cluster/host/host_defects.sh"
    _scope_out_units=""
    if [ -f "$_cov_hostdefects" ]; then
        _scope_out_units=$(sh "$_cov_hostdefects" owned | tr '\n' ' ')
    else
        echo "FAIL: cannot derive the R1 host ladder's scope-out: $_cov_hostdefects is missing"
        _cov_rc=1
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
    #
    # A TU in the derived scope-out above (_scope_out_units, vms-181) is
    # exempted here: it is OWNED and floored by tests/cluster/host/
    # host_defects.sh's own cmd_coverage instead, not by this one.
    _missing=""
    for _cov_c in "$_cov_root"/kernel/*.c "$_cov_root"/kernel-core/*.c; do
        [ -f "$_cov_c" ] || continue
        case "$_cov_c" in
            "$_cov_root"/kernel-core/*) _rel="kernel-core/$(basename "$_cov_c")";;
            *)                          _rel="kernel/$(basename "$_cov_c")";;
        esac
        case " $_all_targets " in
            *" $_rel "*) ;;
            *)
                case " $_scope_out_units " in
                    *" $_rel "*) ;;
                    *) _missing="$_missing $_rel";;
                esac;;
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

    if [ -n "$_scope_out_units" ]; then
        _n_scope_out=$(echo $_scope_out_units | wc -w)
        echo "SCOPE-OUT (DERIVED LIVE, vms-181): $_n_scope_out translation unit(s) OWNED by"
        echo "  the R1 host ladder (tests/cluster/host/host_defects.sh 'owned') -- peer-"
        echo "  requiring cluster FSMs/codecs a single-node VAXCLUSTER=0 QEMU guest cannot"
        echo "  reach, covered-or-still-red by that script's OWN coverage check (vms-181),"
        echo "  re-derived every run so the two gates cannot drift apart:"
        for _cov_su in $_scope_out_units; do echo "    $_cov_su"; done
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
    # Same consistency rule for the derived scope-out (vms-181): a TU the R1
    # host ladder claims to own must not ALSO be named by a defect here --
    # if it is, the two gates disagree about who floors it.
    for _cov_u in $_scope_out_units; do
        case " $_all_targets " in
            *" $_cov_u "*) _bad_scope="$_bad_scope $_cov_u";;
        esac
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

    _anch_raw=$( (cd "$_cov_tests" && grep -a -Hno \
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
        # Bound the window to the anchored STATEMENT: accumulate lines from
        # just below the anchor until one ENDS in ';' (a statement terminator,
        # optionally followed by a trailing block comment), or 12 lines max for
        # a multi-line ct_check(...). vms-b8a2: the old `sed 's/;.*//'` cut at
        # the FIRST ';' anywhere in the joined window, which truncates a
        # require_fail label that itself contains a ';' -- e.g. a VMS version
        # spec "FOO.DAT;1" or ";N" syntax inside the assertion's string -- so
        # the grep below could never find it (this is why rms_workload could not
        # be anchored, vms-ac3). A statement ';' is at end-of-line; a ';' inside
        # a string literal is mid-line, so end-of-line anchoring keeps both the
        # single-statement bound AND the full label text.
        _aseg=$(awk -v s="$((_aln + 1))" '
                NR < s { next }
                { print }
                /;[[:space:]]*(\/\*.*\*\/[[:space:]]*)?$/ { exit }
                NR >= s + 11 { exit }
            ' "$_cov_tests/$_af" \
                | tr '\n\t' '  ' | tr -d '"\\' | tr -s ' ')
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

    # --- 5. every defect file is whole -------------------------------------
    # One file per defect (tests/qemu/defects.d/<name>.defect): it must define
    # BOTH its metadata (defect_meta) and its mutation (defect_apply), and the
    # directory holds nothing else -- a stray file there is a control nothing
    # lists.
    if [ -d "$DEFECTS_DIR" ]; then
        _half=""
        for _cov_d in $DEFECTS; do
            _df="$DEFECTS_DIR/$_cov_d.defect"
            grep -q '^defect_meta() {$' "$_df" && grep -q '^defect_apply() {$' "$_df" \
                || _half="$_half $_cov_d"
        done
        _stray=$(cd "$DEFECTS_DIR" && ls | grep -v '^[a-z0-9][a-z0-9-]*\.defect$' || true)
        if [ -n "$_half" ]; then
            echo "FAIL: defect file(s) without both defect_meta() and defect_apply():$_half"
            _cov_rc=1
        fi
        if [ -n "$_stray" ]; then
            echo "FAIL: file(s) in $DEFECTS_DIR that are not <name>.defect:" $_stray
            _cov_rc=1
        fi
    else
        echo "FAIL: no defect directory $DEFECTS_DIR."
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
        # vmsfs is copied too: several defects target genuine ODS-2 codec
        # translation units under src/vmsfs (e.g. acp-writevb-extend-alloc-offbyone
        # -> vmsfs/ods2/ods2_edit.c, and the vmsfs/vmsfs_translate.c control) --
        # product-half sources the executive ACP + RMS consume. Without this copy
        # those defects' anchors would be reported as dead fixtures on every run.
        # vmstcpip is copied too (vms-6c6): tcpip-ftp-get-length-dropped targets
        # vmstcpip/services/tcpip_client.h -- the shared TCP/IP client engine
        # header the DCL FTP/TELNET verbs ship (a product-half consumer of the
        # executive-resident BGn: device). It was absent from this copy set, so
        # that defect was reported as a dead fixture ("its sed anchor no longer
        # matches") on every selftest run even though its anchor is fine.
        # THE WHOLE src/ TREE, not a list of directories (vms-dda): a list goes
        # stale the moment a defect targets a directory nobody added -- it was
        # vmstcpip once (vms-6c6), then vmsdecnet (net-client-takes-qio-service-
        # status reported as a dead fixture though its anchor matched) -- and
        # every such miss reads as a broken anchor rather than a broken copy.
        if ! cp -a "$_st_root/." "$_st_tmp/tree/" 2>/dev/null; then
            echo "FAIL: cannot copy $_st_root for the self-test"
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
