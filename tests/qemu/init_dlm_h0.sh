#!/bin/busybox sh
# init_dlm_h0.sh - PID 1 inside the DLM harness H0 QEMU node (rd vms-4b6).
#
# H0 (DLM epic vms-7fa, harness rung 0) proves ONE thing, on a SINGLE real-
# executive node: the SCSD-carrying image COMPOSES WITH A REAL EXECUTIVE. SCSD's
# own cross-node DLM dispatch path -- scsd_dlm_dispatch_to_executive() in
# src/vmsscs/scsd.c, which open()s /dev/vms and issues VMS_IOCTL_DLM_XNODE --
# reaches a REAL /dev/vms and the executive's rung-1 handler
# (vms_lock_dlm_xnode_dispatch, src/kernel-core/vms_lock.c) returns
# SS$_UNSUPPORTED (2296), NOT SS$_NOSUCHDEV (2680).
#
# That status FLIP is the whole point. In the Docker cluster harness
# (tests/cluster/two-ovmx) there is no /dev/vms (Rule 9: Docker is not a
# runtime), so the SAME code fails HONEST with 2680 -- which is correct, but
# proves nothing about a real cross-node grant. Only a node with a real
# executive can return 2296, and this harness gives it one (vms.ko insmod'd,
# /dev/vms present) exactly as tests/qemu/init.sh does. The two-node harness
# (H1-H4) builds on this single-node foundation.
#
# INV-6 / Rule 9: nothing here fakes a pass. SCSD prints verbatim whatever the
# executive returned; if /dev/vms is absent the verdict is a FAIL, loudly.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

echo ""
echo "=== OVMX DLM Harness H0: SCSD.EXE composes with a real executive (vms-4b6) ==="
echo "Kernel: $(uname -r) ($(uname -m))"
echo ""

echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo ""
    echo "H0-DLM-SCSD-EXEC: rc=none FAIL (no /dev/vms -- cannot prove SCSD reaches a real executive)"
    reboot -f
fi

# Confirm SCSD.EXE actually reached the initramfs (absence must be a loud FAIL,
# never a silent skip -- a missing subject would turn the proof into a no-op).
if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE is not present/executable in the initramfs"
    echo ""
    echo "H0-DLM-SCSD-EXEC: rc=none FAIL (SCSD.EXE missing from the image)"
    reboot -f
fi

echo ""
echo "--- SCSD.EXE --dlm-selftest (its own scsd_dlm_dispatch_to_executive over /dev/vms) ---"
# --dlm-selftest opens no socket and needs no privilege: it drives ONE synthetic
# but well-formed cross-node ENQ through scsd_dlm_dispatch_to_executive() and
# prints "SCSD-I-DLMSELFTEST, executive DLM dispatch status=<n> (0x...)".
SCSD_OUT=$(/bin/SCSD.EXE --dlm-selftest 2>&1)
scsd_rc=$?
echo "$SCSD_OUT"
echo "(SCSD.EXE --dlm-selftest exit code: $scsd_rc)"

# Extract the status the executive returned from SCSD's own report line.
status=$(printf '%s\n' "$SCSD_OUT" | sed -n 's/.*status=\([0-9][0-9]*\).*/\1/p' | head -1)

echo ""
if [ "$status" = "2296" ]; then
    # SS$_UNSUPPORTED: SCSD opened the REAL /dev/vms and the rung-1 handler
    # declined honestly -- the machine-checkable proof H0 exists to produce.
    echo "H0-DLM-SCSD-EXEC: rc=2296 PASS (SCSD reached the real executive; SS\$_UNSUPPORTED, not SS\$_NOSUCHDEV)"
elif [ "$status" = "2680" ]; then
    # SS$_NOSUCHDEV: the fail-honest path -- SCSD did NOT reach a real /dev/vms.
    # In THIS harness that is a real defect (vms.ko was loaded above), not the
    # expected Docker-harness outcome.
    echo "H0-DLM-SCSD-EXEC: rc=2680 FAIL (SS\$_NOSUCHDEV -- SCSD did not reach the real executive though vms.ko is loaded)"
else
    echo "H0-DLM-SCSD-EXEC: rc=${status:-none} FAIL (unexpected status from SCSD's executive dispatch)"
fi
echo ""

echo "--- dmesg (vms) ---"
dmesg | grep -iE 'vms' || true
echo "--- end dmesg ---"

reboot -f
