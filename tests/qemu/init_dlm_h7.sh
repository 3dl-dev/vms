#!/bin/busybox sh
# init_dlm_h7.sh - PID 1 inside a DLM harness H7 OVMX node (rd vms-1bba).
#
# H7 (DLM directory + CONSISTENT MASTERING, the "DB" rung) proves that two nodes,
# given the SAME static membership vector, INDEPENDENTLY resolve the SAME master
# for a resource via the hashed directory -- WITHOUT communicating. There is no
# SCS join, no scsd, no shared L2, no sysgen, no pcap here: the directory is a
# pure function of (resource name, membership vector), so the nodes need not talk.
#
# This node:
#   1. maps its NODE label (ovmx.node=A/B on the kernel cmdline) to a CSID
#      (A=1030, B=1031),
#   2. insmods vms.ko with vms_local_csid=<mine> AND the SHARED ordered vector
#      dlm_member_csids=1030,1031 -- the SAME vector on both nodes,
#   3. confirms /dev/vms (the real executive), and
#   4. runs /bin/test_dlm_dir_h7, which for a fixed name set prints, verbatim from
#      the executive, the directory/master (H7DIR) and the LOCAL $ENQ status
#      (H7ENQ) for each name, then H7-DRIVER-DONE.
#
# The driver's stdout is emitted on /dev/ttyS1, wrapped in
# ===H7-NODE-$NODE-BEGIN=== / ===H7-NODE-$NODE-END=== markers (the h5/h6 ttyS1
# marker convention). The host runner (run_dlm_harness_h7.sh) boots BOTH nodes,
# reads both nodes' ttyS1 logs, and asserts: (a) both nodes AGREE on dir + master
# for every name; (b) the directory genuinely distributes (some names 1030, some
# 1031); (c) honest no-regression -- a LOCAL $ENQ for a remote-directoried name
# returns SS$_UNSUPPORTED (2296), and for a local-directoried name it succeeds.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly on ttyS1 and powers off; the driver prints verbatim what its
# own executive returned; the host verdict is computed only from those real lines.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X

# Map NODE -> this node's CSID (both nodes share the SAME membership vector).
case "$NODE" in
    A) MYCSID=1030 ;;
    B) MYCSID=1031 ;;
    *) MYCSID=0 ;;
esac

echo ""
echo "=== OVMX DLM Harness H7: directory consistent mastering (node=$NODE csid=$MYCSID) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ "$MYCSID" = "0" ]; then
    echo "  FAIL: unknown node label '$NODE' (want ovmx.node=A or ovmx.node=B)"
    {
        echo "===H7-NODE-$NODE-BEGIN==="
        echo "node=$NODE FAIL (unknown node label, no CSID mapping)"
        echo "===H7-NODE-$NODE-END==="
    } > /dev/ttyS1 2>&1
    sync
    poweroff -f
fi

# --- the real executive, loaded WITH the static membership vector -------------
echo "--- Loading vms.ko vms_local_csid=$MYCSID dlm_member_csids=1030,1031 ---"
insmod /lib/modules/vms.ko vms_local_csid="$MYCSID" dlm_member_csids=1030,1031
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    {
        echo "===H7-NODE-$NODE-BEGIN==="
        echo "node=$NODE csid=$MYCSID FAIL (no /dev/vms)"
        echo "===H7-NODE-$NODE-END==="
    } > /dev/ttyS1 2>&1
    sync
    poweroff -f
fi

# --- the proof driver --------------------------------------------------------
if [ ! -x /bin/test_dlm_dir_h7 ]; then
    echo "  FAIL: /bin/test_dlm_dir_h7 missing"
    {
        echo "===H7-NODE-$NODE-BEGIN==="
        echo "node=$NODE csid=$MYCSID FAIL (no test_dlm_dir_h7)"
        echo "===H7-NODE-$NODE-END==="
    } > /dev/ttyS1 2>&1
    sync
    poweroff -f
fi

echo "--- Running /bin/test_dlm_dir_h7 (node $NODE) ---"
/bin/test_dlm_dir_h7 > /tmp/h7-$NODE.log 2>&1
drv_rc=$?
echo "(driver exit code: $drv_rc)"
cat /tmp/h7-$NODE.log

# --- emit the machine-checkable node log on ttyS1 (verbatim driver output) ---
{
    echo "===H7-NODE-$NODE-BEGIN==="
    echo "node=$NODE csid=$MYCSID drv_rc=$drv_rc member_vector=1030,1031"
    cat /tmp/h7-$NODE.log
    echo "===H7-NODE-$NODE-END==="
} > /dev/ttyS1 2>&1

sync
poweroff -f
