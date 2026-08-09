#!/bin/busybox sh
# init.sh - PID 1 inside QEMU initramfs
# Loads vms.ko, runs kernel module test programs, exits.

# Install busybox applets (mount, insmod, reboot, etc.)
/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo ""
echo "=== OVMX Kernel Module Test Suite ==="
echo "Kernel: $(uname -r) ($(uname -m))"
echo ""

# MODULE_PASS/MODULE_FAIL count the two module-load checks below (vms.ko,
# vmsfs.ko). SUITE_PASS/SUITE_FAIL (declared below the loop) count only
# actual test-suite runs. Keeping them apart is the vms-95f fix: the old
# single TOTAL_PASS/TOTAL_FAIL counter mixed the two, so the "FINAL RESULTS"
# headline printed "28 suites passed" when only 26 suites ever ran -- the
# other 2 were these module-load checks mislabeled as suites. See the
# FINAL RESULTS derivation at the bottom of this file for the reconciliation.
MODULE_PASS=0
MODULE_FAIL=0

# Set up loop device for block-device vmsfs test
# loop may be built-in (CONFIG_BLK_DEV_LOOP=y) or a module
if [ -f /lib/modules/loop.ko ]; then
    echo "--- Loading loop.ko ---"
    insmod /lib/modules/loop.ko
fi
if [ -f /test_data/vmsfs_test.img ]; then
    echo "--- Setting up loop device for blkdev test ---"
    # Create /dev/loop0 if devtmpfs didn't auto-create it
    [ -b /dev/loop0 ] || mknod /dev/loop0 b 7 0
    losetup /dev/loop0 /test_data/vmsfs_test.img
    if [ -b /dev/loop0 ]; then
        echo "  OK: loop0 attached to vmsfs_test.img"
    else
        echo "  WARN: losetup failed or /dev/loop0 not present"
    fi
fi

# Load vms.ko
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present"
    MODULE_PASS=$((MODULE_PASS+1))
else
    echo "  FAIL: vms.ko load or /dev/vms creation failed"
    MODULE_FAIL=$((MODULE_FAIL+1))
    # Show dmesg for diagnostics
    dmesg | tail -20
fi

# Load vmsfs.ko
echo "--- Loading vmsfs.ko ---"
insmod /lib/modules/vmsfs.ko
if grep -q vmsfs /proc/filesystems; then
    echo "  PASS: vmsfs.ko loaded, filesystem registered"
    MODULE_PASS=$((MODULE_PASS+1))
else
    echo "  FAIL: vmsfs.ko load or filesystem registration failed"
    MODULE_FAIL=$((MODULE_FAIL+1))
    dmesg | tail -20
fi

# Create directories needed by vmsfs tests
mkdir -p /tmp/vmsfs_backing /mnt/vmsfs

# ASSERTION CHANNEL (vms-b5b round 2). Round 1 (ef9b99c) made suite text
# stream to the console in real time via `tee`, which fixed "a wedge loses
# everything" but opened a SECOND splice source: kernel printk lands on that
# same console concurrently, so a printk can land INSIDE an assertion line
# before its newline. MEASURED by the round-1 audit against a real /dev/vms
# (executive-not-pinned, run 1 of 2 -- run 2, identical code, did not
# reproduce it, which is why round 1's control was flaky, not fixed):
#   '  FAIL: rmmod vms is REFUSED while a descriptor is open (executive pinned)[    5.922598] BUG: unable to handle page fault for address: ffffffffc03c50a8'
# One line, wrong in both directions at once: the clean assertion text reads
# "named but did NOT go red", and the spliced line reads "went red and the
# manifest does NOT name it". setvbuf() cannot touch this -- the splice
# source is the KERNEL's own printk, not userspace stdio buffering.
#
# The fix is a SEPARATE WIRE, not a smarter one. run_tests.sh gives this
# guest a second serial port, ttyS1 (x86_64 only -- see run_tests.sh for the
# aarch64 gap, tracked as rd vms-c83), that carries ONLY what this loop
# writes to fd 4 below. Kernel printk stays on ttyS0 (console=ttyS0 on the
# kernel command line) and never touches ttyS1, so the two cannot splice
# into each other regardless of timing. QEMU's file-backed chardev transmits
# each byte as the guest writes it, so the property round 1 was actually
# chasing -- surviving a guest wedge -- comes along for free, the same way
# it did for `tee` on the console.
#
# If ttyS1 is not present (e.g. a QEMU invocation that never added it), fd 4
# falls back to fd 1 -- LOUDLY, not silently: that is exactly the
# printk-splice exposure above, so the run says so on the console instead of
# pretending the channel split happened.
[ -c /dev/ttyS1 ] || mknod /dev/ttyS1 c 4 65 2>/dev/null
if [ -c /dev/ttyS1 ] && exec 4>/dev/ttyS1; then
    echo "--- assertion channel: /dev/ttyS1 (separate from console) ---"
else
    exec 4>&1
    echo "WARN: /dev/ttyS1 unavailable -- assertion text is sharing the"
    echo "      console with kernel printk. This reopens the vms-b5b splice"
    echo "      class; it is disclosed here, not silently absorbed."
fi

# Run each test program. test_kmod_* drive /dev/vms with raw ioctls
# (kernel lock manager, ASTs, event flags, access modes, vmsfs). test_syssvc_*
# drive the same /dev/vms through the PUBLIC sys$ API in src/libvms instead
# (vms-1d9) -- exercising the userspace system-service layer the ioctl tests
# cannot see at all. test_imgact_* (main-red classification fix; were
# test_syssvc_imgact_bind/publish through #225/#226) touch NEITHER /dev/vms
# NOR any kernel-executive path -- pure userspace .vms$imp binding / resident-
# producer registry logic, run here only so run_facility_negctl.sh's per-
# facility fault-injection proofs keep seeing them execute inside this
# harness. They are deliberately excluded from .github/workflows/ci.yml's
# test_syssvc_*-derived policed set (which requires rc=77 with no /dev/vms):
# these two legitimately return 0 whether or not the executive is present.
#
# PER-SUITE VERDICT LINE (vms-1d9). After each suite we print
#
#     === SUITE <name> rc=<exit code> ===
#
# and .github/workflows/ci.yml asserts on THAT, per suite, instead of on the
# aggregate "FINAL RESULTS" tally below. Two real defects made this necessary,
# both proven against running artifacts, not argued:
#
#  1. The aggregate tally cannot distinguish an honest skip (rc 77) from a
#     failed assertion (rc 1) -- the two branches below both increment
#     SUITE_FAIL. Injecting a FABRICATED SUCCESS into
#     src/libvms/syssvc/sys_lock.c (do_enq and sys$deq returning SS$_NORMAL
#     with an invented lock ID when the executive was never reached) makes
#     every one of test_syssvc_lock's device-absent assertions FAIL and its
#     exit code change 77 -> 1 -- while the negative-control run's FINAL
#     RESULTS line stays BYTE-IDENTICAL to the clean tree
#     ("3 suites passed, 11 suites failed"). Measured, both ways, on this
#     harness. A per-process fake reporting success is invisible to any
#     gate that pins the tally; the per-suite rc catches it.
#
#  2. Any assertion on the aggregate count is either a pin that turns CI red
#     when a legitimate new suite is ADDED, or a floor that stops protecting
#     every suite added after it was written. Per-suite lines let CI derive
#     the expected set from the checkout (`ls tests/qemu/test_*.c`), which is
#     addition-tolerant AND drop-detecting with nothing maintained by hand.
#
# The SUITE_PASS/SUITE_FAIL tally is kept for human readers and for
# run_tests.sh's exit code; it is no longer the thing CI pins.
#
# WHY SUITE_PASS/SUITE_FAIL ARE SEPARATE FROM MODULE_PASS/MODULE_FAIL
# (vms-95f). This loop is the ONLY place a "=== SUITE <name> rc= ==="
# verdict line is printed, so SUITE_PASS+SUITE_FAIL, by construction, always
# equals the number of those lines. Before this fix there was a single
# TOTAL_PASS/TOTAL_FAIL shared with the two module-load checks above, so
# the "FINAL RESULTS" headline below reported 28 ("suites passed") when
# only 26 suite verdict lines were ever printed -- the other 2 were the
# vms.ko/vmsfs.ko module-load checks, not suites. Derived twice
# independently against a clean tree (see rd vms-215's notes). Keeping the
# counters apart means the headline's suite count reconciles with the
# "=== SUITE ... ===" lines by construction instead of by someone
# remembering to subtract 2.
SUITE_PASS=0
SUITE_FAIL=0
#
# ASSERTION-LEVEL TALLY (vms-215). A per-suite "N passed, M failed" line is a
# SELF-REPORT: a suite that forgets to print one (test_kmod_bind did, until
# this change) silently drops its assertions from any total built by summing
# those lines. So ASSERT_PASS/ASSERT_FAIL below are not summed from the
# suites' own tallies at all -- each suite's actual stdout is captured to a
# file and grepped for the "  PASS:"/"  FAIL:" lines CHECK() really printed,
# the byproduct of running, not the suite's opinion of itself. That total is
# reconciled with the transcript by construction: it IS a count of the lines
# in the transcript, so it cannot drift from them the way a self-report can.
ASSERT_PASS=0
ASSERT_FAIL=0
SUITE_OUT=/tmp/suite_out.$$
SUITE_FIFO=/tmp/suite_fifo.$$
# Bounds a real, measured hazard -- see the loop body for the mechanism, and
# for the FIRST version of this bound, which measured wrong and was replaced
# (round 2 of round 2, found by this item's own FULL_RUN=1 proof: not
# hypothetical, not theoretical, an actual regression caught before it
# shipped). 20s is slack for an ORPHAN to be force-reaped, not for a suite to
# finish; see below for why that distinction is the whole fix.
SUITE_DRAIN_TIMEOUT=${SUITE_DRAIN_TIMEOUT:-20}
for test in /tests/test_kmod_* /tests/test_syssvc_* /tests/test_imgact_*; do
    [ -x "$test" ] || continue
    name=$(basename "$test")
    echo "" >&4
    echo "--- $name ---" >&4
    rm -f "$SUITE_OUT" "$SUITE_FIFO"
    mknod "$SUITE_FIFO" p
    # REAL-TIME FORWARDER, via a FIFO instead of a shell pipe. "$test" writes
    # into the fifo; `tee`, backgrounded, copies each chunk to $SUITE_OUT
    # (this loop's own PASS/FAIL tally) AND fd 4 (the assertion channel) as
    # it arrives -- not after "$test" returns. That immediacy is what makes
    # a `fatal` isolation control (executive-not-pinned) survivable: bytes
    # already reach fd 4 before a guest wedge can lose them.
    tee "$SUITE_OUT" >&4 <"$SUITE_FIFO" &
    TEE_PID=$!
    "$test" >"$SUITE_FIFO" 2>&1
    rc=$?
    # DRAIN, bounded -- and bounded starting HERE, after "$test" has ALREADY
    # exited and rc is already known, not from the top of the iteration.
    # THIS ORDERING IS THE FIX. The first version of this bound wrapped
    # `timeout -s KILL $SUITE_DRAIN_TIMEOUT tee ...` around the WHOLE
    # pipeline, so its clock started when the SUITE started, not when it
    # finished -- indistinguishable, to that timeout, from "this suite is
    # simply slow". MEASURED: this item's own FULL_RUN=1 proof caught it
    # rebounding off bind-client-no-register, whose mutation makes every
    # subsequent syscall in several suites fail, which pushed
    # test_syssvc_lock_status and test_syssvc_setname's own real runtimes
    # past the 20s mark with NO orphan involved -- `tee` was SIGKILLed while
    # those suites were still legitimately running, which SIGPIPE'd
    # test_syssvc_lock_status (observed: rc=141) and silently dropped
    # test_syssvc_setname's last 4 PASS/FAIL lines (observed: present on the
    # unfixed ef9b99c baseline, absent here). Confirmed on the base tree:
    # both suites print their full expected assertion sets when nothing
    # times anything out. Waiting for "$test" to exit FIRST (the line above)
    # and only THEN bounding the wait for tee removes the ambiguity: nothing
    # still holding the fifo's write end open past this point can be "$test"
    # itself (it has already exited) -- it can only be an orphaned
    # grandchild ("$test" forked it and never wait()ed for it). Suites whose
    # fork() count exceeds their wait() count are the exposure this control
    # actually targets (test_kmod_bind 9/4, test_kmod_lock_sync 8/4,
    # test_syssvc_ef_mproc 7/3); a 6-second sleeping orphan demonstrated
    # under the original (pre-fifo) pipeline shape that this class is real,
    # not hypothetical -- it made the pipeline take 6.014s instead of
    # returning immediately.
    _waited=0
    while kill -0 "$TEE_PID" 2>/dev/null; do
        if [ "$_waited" -ge "$SUITE_DRAIN_TIMEOUT" ]; then
            echo "  WARN: $name -- an orphaned child is still holding the assertion fifo open ${SUITE_DRAIN_TIMEOUT}s after $name itself exited -- killing the forwarder so the run can continue" >&4
            kill -KILL "$TEE_PID" 2>/dev/null
            break
        fi
        sleep 1
        _waited=$((_waited + 1))
    done
    wait "$TEE_PID" 2>/dev/null
    rm -f "$SUITE_FIFO"
    spass=$(grep -c "^  PASS:" "$SUITE_OUT" 2>/dev/null); spass=${spass:-0}
    sfail=$(grep -c "^  FAIL:" "$SUITE_OUT" 2>/dev/null); sfail=${sfail:-0}
    ASSERT_PASS=$((ASSERT_PASS+spass))
    ASSERT_FAIL=$((ASSERT_FAIL+sfail))
    if [ "$rc" -eq 0 ]; then
        SUITE_PASS=$((SUITE_PASS+1))
    elif [ "$rc" -eq 77 ]; then
        # Honest skip (e.g. /dev/vms absent) -- should never happen in this
        # job, since vms.ko was just insmod'd above. Count as a FAIL: if it
        # ever fires here, the executive is not actually present, which is
        # exactly what this job exists to catch.
        echo "  SKIP reported inside the kernel-executive job -- treating as FAIL" >&4
        SUITE_FAIL=$((SUITE_FAIL+1))
    else
        SUITE_FAIL=$((SUITE_FAIL+1))
    fi
    echo "=== SUITE $name rc=$rc ===" >&4
done
rm -f "$SUITE_OUT" "$SUITE_FIFO"
exec 4>&-

echo ""
# MODULE LOAD is reported separately from FINAL RESULTS (vms-95f) so that
# "suites passed/failed" below counts only what the loop above actually ran
# -- SUITE_PASS+SUITE_FAIL is exactly the number of "=== SUITE ... ===" lines
# printed, no module-load check folded in under the same name. A module-load
# failure is not silently lost by this split: /dev/vms absent or vmsfs
# unregistered means every test_kmod_*/test_syssvc_* suite above fails or
# honest-skips against a missing device, which SUITE_FAIL already reports,
# and .github/workflows/ci.yml's kernel-executive job additionally asserts
# on the two "PASS: vms.ko loaded" / "PASS: vmsfs.ko loaded" lines directly.
echo "=== MODULE LOAD: $MODULE_PASS passed, $MODULE_FAIL failed ==="
echo "=== FINAL RESULTS: $SUITE_PASS suites passed, $SUITE_FAIL suites failed ==="
echo "=== ASSERTIONS: $ASSERT_PASS passed, $ASSERT_FAIL failed (derived from PASS/FAIL lines actually printed, not summed from suite self-reports) ==="
echo ""

# Show kernel log for debugging
echo "--- dmesg (vms/vmsfs) ---"
dmesg | grep -iE 'vms|vmsfs' || true
echo "--- end dmesg ---"

# Exit QEMU (with -no-reboot, reboot causes QEMU to exit)
reboot -f
