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
# epic shipped exactly that and had to be redone. So each defect below carries
# BOTH a require_fail list (the assertions that MUST go red) AND a forbid_fail
# list (sibling assertions in the SAME suite that MUST stay green). A mutation
# that reddens its neighbours fails its own control.
#
# WHAT A "FACILITY" IS HERE
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
# `coverage` below turns "every facility has a control" into a mechanical
# check: every src/kernel/*.c translation unit must be named by at least one
# defect. A new facility file added without a control turns CI red.
#
# USAGE
#   facility_defects.sh list
#   facility_defects.sh field <defect> <facility|targets|suites_red|isolation|
#                                       require_fail|forbid_fail|why>
#   facility_defects.sh apply <defect> <src-root> [<src-root>...]
#   facility_defects.sh coverage <src-root>
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
devtab-owner-not-recorded
proctab-duplicate-name
executive-not-pinned
pcb-per-thread
bind-client-no-register"

# ---------------------------------------------------------------------------
# Metadata
#
#   targets      source files, relative to a src/ root, the mutation edits.
#                EVERY existing copy must change or the fixture is broken.
#   suites_red   the suites this defect is ALLOWED to redden, as shell globs.
#                At least one matching suite must go red (detection); no
#                NON-matching suite may go red (attribution).
#   require_fail assertion texts that must appear as "  FAIL: <text>".
#   forbid_fail  assertion texts that must NOT appear as "  FAIL: <text>" --
#                the sibling properties the mutation must leave alone.
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
        isolation)    echo "isolated";;
        why)          echo "\$SETMOD to KERNEL mode stops requiring CMKRNL: the executive lets a process escalate its own access mode. One condition, inverted.";;
        require_fail) cat <<'EOF'
KERNEL mode denied without CMKRNL
mode still USER after denied escalation
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
register process
initial mode is USER
EOF
                      ;;
        esac;;

    ast-setast-disable)
        case "$_f" in
        facility)     echo "AST delivery (VMS_IOCTL_DCLAST/SETAST/DELIVERAST)";;
        targets)      echo "kernel/vms_ast.c";;
        suites_red)   echo "test_kmod_ast";;
        isolation)    echo "isolated";;
        why)          echo "\$SETAST(disable) stops disabling: the enable flag is written as 1 whatever the caller asked for. Queueing, quota and mode checks are untouched.";;
        require_fail) cat <<'EOF'
disable again: prev state was disabled
SETAST(enable) returns WASCLR
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
register
SETAST(disable) returns WASSET
previous state was enabled
DCLAST while disabled: queued
DCLAST second AST queued
EOF
                      ;;
        esac;;

    eflag-clref-noop)
        case "$_f" in
        facility)     echo "event flags (VMS_IOCTL_SETEF/CLREF/READEF/WAITFR/WFLOR/WFLAND/ASCEFC/DACEFC)";;
        targets)      echo "kernel/vms_eflag.c";;
        suites_red)   echo "test_kmod_eflag";;
        isolation)    echo "isolated";;
        why)          echo "\$CLREF stops clearing the bit. It still REPORTS the correct previous state, so only the assertions that read the flag back afterwards can see it -- which is exactly the shape of a facade that reports success while changing nothing.";;
        require_fail) cat <<'EOF'
readef(5) after clear returns WASCLR
cluster has flags 0,3,7,31 set
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
register
setef(5) returns WASCLR
setef(5) again returns WASSET
readef(5) returns WASSET
cluster state has bit 5 set
clref(5) returns WASSET
setef(200) returns ILLEFC
setef(40) in cluster 1 returns WASCLR
cluster 1 has bit 8 (flag 40) set
EOF
                      ;;
        esac;;

    lock-compat-ex-cr)
        case "$_f" in
        facility)     echo "distributed lock manager (VMS_IOCTL_ENQ/DEQ/CONVERT/GETLKI)";;
        targets)      echo "kernel/vms_lock.c";;
        suites_red)   echo "test_kmod_lock test_kmod_lock_mproc test_kmod_lock_sync test_syssvc_lock";;
        isolation)    echo "isolated";;
        why)          echo "compat[EX][CR] flipped 0 -> 1: an exclusive request is granted against a held concurrent-read lock. THE vms-e4d PRECEDENT, mechanised -- one entry of one matrix, nothing else.";;
        require_fail) cat <<'EOF'
ENQ EX+NOQUEUE denied (CR held)
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
register
ENQ NL on TESTRES1
got non-zero lock ID
convert NL -> CR
GETLKI succeeds
granted mode is CR
resource name matches
convert CR -> EX
DEQ lock
ENQ CR on TESTRES2
ENQ second CR on TESTRES2 (compatible)
EOF
                      ;;
        esac;;

    devtab-owner-not-recorded)
        case "$_f" in
        facility)     echo "device table (VMS_IOCTL_ASSIGN/DASSGN/GETDVI/DEVSCAN/TTSETMODE/ALLOC/DALLOC)";;
        targets)      echo "kernel/vms_devtab.c";;
        suites_red)   echo "test_kmod_devtab";;
        isolation)    echo "isolated";;
        why)          echo "\$ASSIGN stops recording the owning process in the shared device entry (the FIRST of the two owner_pid writes; \$ALLOC's is left alone). The channel is still created and still costs a reference, so only the A-writes/B-reads ownership assertions can see it -- Rule 11's decisive test, negatively.";;
        require_fail) cat <<'EOF'
oracle: $ASSIGN to a non-shareable device nobody owns makes the caller its owner
B sees the ownership A took with a channel alone (A writes, B reads)
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
console OPA0: exists without any process creating it
device reports its VMS physical name
console is a terminal-class device (DC$_TERM)
console starts unowned
absent device reports SS$_NOSUCHDEV
another process assigns a channel to OPA0:
oracle: ownership by channel is not an allocation
oracle: ownership by channel costs one reference, not two
B sees that nothing is allocated
EOF
                      ;;
        esac;;

    proctab-duplicate-name)
        case "$_f" in
        facility)     echo "process table (VMS_IOCTL_SETPRN/GETJPI/PROCSCAN)";;
        targets)      echo "kernel/vms_proctab.c";;
        suites_red)   echo "test_kmod_procnam";;
        isolation)    echo "isolated";;
        why)          echo "\$SETPRN stops rejecting a name already held in the UIC group: the SS\$_DUPLNAM clash test is short-circuited. Name storage, lookup, scan and validation are untouched.";;
        require_fail) cat <<'EOF'
duplicate process name rejected with SS$_DUPLNAM
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
unset name does not resolve (SS$_NONEXPR)
unknown name does not resolve (SS$_NONEXPR)
lookup by PID returns the same name
distinct process name accepted
EOF
                      ;;
        esac;;

    executive-not-pinned)
        case "$_f" in
        facility)     echo "executive residency (vms_fops.owner pins vms.ko while /dev/vms is open)";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_pin";;
        isolation)    echo "fatal";;
        why)          echo "vms_fops loses .owner = THIS_MODULE, so an open descriptor no longer holds a module reference and test_kmod_pin's own rmmod succeeds. FATAL, and measured, not assumed: the guest then takes 'Unable to handle kernel paging request' + 'Internal error: Oops' with Comm: test_kmod_pin, and the run never reaches its own accounting. That IS the guarantee -- an unpinned executive is not a degraded system, it is a dead one -- so the control asserts what is checkable (the eight suites before it ran clean, the three pin assertions went red by name) instead of pretending the unload is survivable.";;
        require_fail) cat <<'EOF'
an open /dev/vms descriptor holds a reference on vms.ko
rmmod vms is REFUSED while a descriptor is open (executive pinned)
the refusal is specifically 'module is in use'
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
/sys/module/vms/refcnt is readable (vms.ko is loaded)
EOF
                      ;;
        esac;;

    pcb-per-thread)
        case "$_f" in
        facility)     echo "PCB identity: one executive process per THREAD GROUP, shared by its threads";;
        targets)      echo "kernel/vms_module.c";;
        suites_red)   echo "test_kmod_bind";;
        isolation)    echo "isolated";;
        why)          echo "vms_proc_find_or_err() keys the PCB on current->pid (the Linux TID) instead of current->tgid, so a sibling thread of one image no longer resolves to its process's PCB. Invisible to every single-threaded suite -- tgid == pid there -- which is precisely why it needs a control.";;
        require_fail) cat <<'EOF'
sibling thread resolves in the process table
sibling thread sees the process name the main thread set
sibling thread sees the event flag the main thread set
sibling thread can $DEQ the lock the main thread took
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
main thread names the process
main thread sets a local event flag
main thread takes a lock
main thread holds a lock id
the sibling really is a second Linux task
$SETEF reaches the executive with no explicit register
$GETJPI(self) resolves the auto-bound process
EOF
                      ;;
        esac;;

    bind-client-no-register)
        case "$_f" in
        facility)     echo "kernel-interface binding in the PRODUCT (vms_kif kif_bind -> vms_kif_register)";;
        targets)      echo "libvmssys/vms_kif.c";;
        suites_red)   echo "test_kmod_bind test_kmod_devtab test_kmod_procnam test_syssvc_*";;
        isolation)    echo "isolated";;
        why)          echo "kif_bind() stops calling vms_kif_register() -- THE vms-9fc defect, restored on purpose. MEASURED RESULT: only test_kmod_bind goes red. test_kmod_devtab, test_kmod_procnam and test_syssvc_lock all call vms_kif_open()/vms_kif_register() BY HAND before using a facility, so they supply the very step the product forgets and cannot see the defect -- which is exactly how it survived until vms-9fc. They stay in this list because they are the client-dependent suites: if one of them ever stops hand-registering it SHOULD go red here, and this control will absorb that without an edit.";;
        require_fail) cat <<'EOF'
$SETEF reaches the executive with no explicit register
$GETJPI(self) resolves the auto-bound process
EOF
                      ;;
        forbid_fail)  cat <<'EOF'
$SETEF does not report the caller's parameters as bad
module rejects an unregistered task with -ESRCH
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
# cmd_coverage <src-root>
#
# "Every wired executive facility has a negative control" as a mechanical
# check rather than a claim: every src/kernel/*.c translation unit -- the
# executive's own code -- must be named by at least one defect's targets.
# Adding a facility file without a control turns this red.
# ---------------------------------------------------------------------------
cmd_coverage() {
    [ $# -eq 1 ] || { echo "usage: facility_defects.sh coverage <src-root>" >&2; return 2; }
    _root="$1"
    [ -d "$_root/kernel" ] || { echo "FAIL: $_root/kernel is not a directory" >&2; return 2; }

    _all_targets=""
    for _d in $DEFECTS; do
        _all_targets="$_all_targets $(defect_field "$_d" targets)"
    done

    _missing=""
    for _c in "$_root"/kernel/*.c; do
        [ -f "$_c" ] || continue
        _rel="kernel/$(basename "$_c")"
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
        return 1
    fi
    echo "PASS: every src/kernel/*.c translation unit is named by a negative control"
    return 0
}

# ---------------------------------------------------------------------------
# cmd_selftest <src-root>
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
#   3. every metadata field the driver reads must be non-empty.
# ---------------------------------------------------------------------------
cmd_selftest() {
    [ $# -eq 1 ] || { echo "usage: facility_defects.sh selftest <src-root>" >&2; return 2; }
    # _st_ prefixes throughout: cmd_apply and cmd_coverage use _root/_d/_f in
    # this same (global) variable namespace, and an earlier version of this
    # function lost its own $_root to cmd_apply's on the first iteration.
    _st_root="$1"
    _st_tmp=$(mktemp -d) || return 2
    _st_rc=0

    for _st_d in $DEFECTS; do
        for _st_fld in facility targets suites_red isolation why require_fail forbid_fail; do
            if [ -z "$(defect_field "$_st_d" "$_st_fld")" ]; then
                echo "FAIL: $_st_d: metadata field '$_st_fld' is empty"
                _st_rc=1
            fi
        done
        case "$(defect_field "$_st_d" isolation)" in
            isolated|fatal) ;;
            *) echo "FAIL: $_st_d: unknown isolation '$(defect_field "$_st_d" isolation)'"; _st_rc=1;;
        esac

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
    cmd_coverage "$_st_root" || _st_rc=1

    if [ "$_st_rc" -eq 0 ]; then
        echo "PASS: every negative control injects into the current tree, and its"
        echo "      injection-landed check demonstrably fires when it does not."
    fi
    return $_st_rc
}

case "${1:-}" in
    list)     shift; cmd_list "$@";;
    field)    shift; cmd_field "$@";;
    apply)    shift; cmd_apply "$@";;
    coverage) shift; cmd_coverage "$@";;
    selftest) shift; cmd_selftest "$@";;
    *)  echo "usage: facility_defects.sh {list|field|apply|coverage|selftest} ..." >&2; exit 2;;
esac
