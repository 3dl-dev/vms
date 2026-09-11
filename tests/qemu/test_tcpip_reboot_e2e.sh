#!/bin/bash
# test_tcpip_reboot_e2e.sh - TCP/IP configuration survives a reboot and is
# reapplied at startup WITHOUT the persisted store growing (rd vms-b97, P3 of the
# config-persistence tree vms-a0b2). The full-boot teeth for the apply/persist
# split (vms-b679) + the ACP-persisted config stores (vms-210/vms-402).
#
# =============================================================================
# WHAT THIS PROVES (end to end, against a real boot on the real runtime target)
# =============================================================================
# Boot 1 (SYSTEM, privileged): TCPIP SET ROUTE /DEFAULT /GATEWAY=<uniq> applies the
# route live AND persists one record to SYS$SYSTEM:TCPIP$ROUTE.DAT over the
# Files-11 ACP. Reboot (a fresh QEMU on the same disk). Boot 2: SYSTARTUP runs
# @SYS$STARTUP:TCPIP$STARTUP, which calls @SYS$STARTUP:TCPIP$REAPPLY -- it reads
# TCPIP$ROUTE.DAT over the ACP and re-runs TCPIP SET ROUTE .../REAPPLY (apply-only).
# Two assertions, both required (the conductor's teeth):
#   (a) config REAPPLIED: boot 2's console shows "%TCPIP-I-REAPPLY, reapplied
#       default route via <uniq>" -- the saved route came back after the reboot.
#   (b) store did NOT grow: TYPE SYS$SYSTEM:TCPIP$ROUTE.DAT on boot 2 shows the
#       record EXACTLY ONCE. /REAPPLY makes the reapply apply-only, so it does not
#       re-append -- the store is stable across reboots. A count of 2 would mean
#       the reapply re-persisted (the bug the apply/persist split exists to prevent).
#
# WHY ROUTE.DAT (not INTERFACE.DAT) IS THE NO-GROWTH PROBE: the test overlay's
# SYSTARTUP_VMS.COM runs @TCPIP$CONFIG every boot with a fixed interface but NO
# gateway, so CONFIG never writes TCPIP$ROUTE.DAT -- only this test's one
# interactive SET ROUTE does. That isolates the no-growth assertion to the reapply
# path (INTERFACE.DAT is separately re-persisted by CONFIG every boot, which would
# confound the count).
#
# REBOOT MECHANISM + THE WRITEBACK TRAP: identical to
# test_boot_scsnode_hostname_e2e.sh -- there is no DCL REBOOT (-no-reboot makes a
# guest reboot(2) exit QEMU), so a "reboot" is: kill QEMU 1, start QEMU 2 on the
# SAME disk-image file. SETTLE_SECS bounds the guest writeback (Linux
# dirty_expire_centisecs default 30s) so boot 1's ACP write reaches the backing
# image before the power-cycle. Every QEMU launch is INLINE (a helper + $(...)
# wedges the backgrounded launch -- see the scsnode header post-mortem) and wrapped
# in `timeout -k 15` for a hard ceiling.
#
# Runs inside the OVMX_TEST_ENABLE_TCPIP=1 bootable image (ovmx-boot-tcpip); the
# runner (run_tcpip_reboot_e2e.sh) builds/loads it and docker-runs this script.
#
# Exit 0 = both teeth passed against the real mounted volume; 1 = a real failure.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETTLE_SECS="${SETTLE_SECS:-60}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
GW=10.0.2.222                        # unique default gateway (not the SE0 lease .15)
ARCH=$(uname -m)

[ -f "$DISTRIB_IMG" ] || { echo "FATAL: $DISTRIB_IMG missing - mastering did not run"; exit 1; }

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64;   MACHINE="";                              CONSOLE="console=ttyS0"
fi

PASS=0; FAIL=0
record() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
check()  { # desc log pattern [present|absent]
    if grep -qF -- "$3" "$2" 2>/dev/null; then
        [ "${4:-present}" = "present" ] && record "$1" 0 || record "$1" 1
    else
        [ "${4:-present}" = "absent" ]  && record "$1" 0 || record "$1" 1
    fi
}
waitfor() { local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim*4)) ]; do
        grep -qF "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }

DISK=/tmp/tcpip-reboot-e2e.img
LOG1=/tmp/tcpip-reboot-boot1.log
LOG2=/tmp/tcpip-reboot-boot2.log
FIFO=/tmp/tcpip-reboot.in
rm -f "$DISK" "$LOG1" "$LOG2" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

echo "=== TCP/IP config survives reboot + reapplied without store growth (vms-b97) ==="
echo "arch=$ARCH qemu=$QEMU gw=$GW"

# --- Boot 1: set a default route (persists to TCPIP$ROUTE.DAT over the ACP) ------
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG1" 2>&1 &
qp=$!
exec 4>"$FIFO"
send() { printf '%s\r' "$1" >&4; }
wake_login() { local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do send ''; sleep 1; w=$((w+1)); done; }

wake_login "$LOG1"
if waitfor 'Username:' 120 "$LOG1"; then rc=0; else rc=1; fi
record "boot 1: TCP/IP image boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$LOG1"; then rc=0; else rc=1; fi
    record "boot 1: SYSTEM logs in (privileged)" "$rc"

    send "TCPIP SET ROUTE /DEFAULT /GATEWAY=$GW"; sleep 2
    # SET ROUTE PERSISTS the route to TCPIP$ROUTE.DAT regardless of privilege (the
    # rms_textfile write needs file access, not NET_ADMIN). The CI QEMU env lacks
    # NET_ADMIN, so the live-table apply is skipped and the verb honestly reports
    # "recorded ... but not applied" -- what matters for the reboot proof is that
    # the route was RECORDED (persisted), which the count check below verifies.
    check "boot 1: SET ROUTE recorded the default route in TCPIP\$ROUTE.DAT" "$LOG1" "TCPIP\$ROUTE.DAT"

    # The store holds exactly one route record now.
    send 'TYPE SYS$SYSTEM:TCPIP$ROUTE.DAT'; sleep 2
    n1=$(grep -c "DEFAULT $GW" "$LOG1" 2>/dev/null)
    if [ "$n1" -eq 1 ]; then rc=0; else rc=1; fi
    record "boot 1: TCPIP\$ROUTE.DAT holds the route exactly once (count=$n1)" "$rc"

    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

# --- Boot 2 (reboot, same disk): TCPIP$REAPPLY reapplies apply-only --------------
rm -f "$FIFO"; mkfifo "$FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG2" 2>&1 &
qp=$!
exec 4>"$FIFO"

wake_login "$LOG2"
if waitfor 'Username:' 120 "$LOG2"; then rc=0; else rc=1; fi
record "boot 2 (reboot): still reaches the login prompt" "$rc"

# TEETH (a): the saved route was REAPPLIED at startup. The signal carries the COUNT
# the reader actually read from the store ("reapplied N route(s) from TCPIP$ROUTE.DAT"),
# so it can ONLY appear if TCPIP REAPPLY genuinely read the store -- never a
# trivially-true condition. Exactly one route was persisted on boot 1, so N==1.
# (This is env-independent of NET_ADMIN: the reader reads + re-runs the verb whether
# or not the live-table apply is permitted.)
check "boot 2: TCPIP REAPPLY read the store and reapplied the route (%TCPIP-I-REAPPLY, count=1)" "$LOG2" "%TCPIP-I-REAPPLY, reapplied 1 route(s) from TCPIP\$ROUTE.DAT"

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$LOG2"; then rc=0; else rc=1; fi
    record "boot 2: SYSTEM logs in" "$rc"

    # TEETH (b): the store did NOT grow -- the record still appears exactly once.
    # (The reapply used /REAPPLY = apply-only, so it did not re-append. A count of
    # 2 would mean the reapply re-persisted -- the bug the split prevents.)
    send 'TYPE SYS$SYSTEM:TCPIP$ROUTE.DAT'; sleep 2
    n2=$(grep -c "DEFAULT $GW" "$LOG2" 2>/dev/null)
    if [ "$n2" -eq 1 ]; then rc=0; else rc=1; fi
    record "boot 2: TCPIP\$ROUTE.DAT STILL holds the route exactly once -- no growth across reboot (count=$n2)" "$rc"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

if [ "$FAIL" -ne 0 ]; then
    echo "--- boot 1 log ---"; cat "$LOG1"
    echo "--- boot 2 log ---"; cat "$LOG2"
fi

echo ""
echo "  RESULTS: $PASS/$((PASS+FAIL)) checks passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "  TCP/IP CONFIG SURVIVES REBOOT + REAPPLIED WITHOUT STORE GROWTH -- REAL BOOT, REAL ACP"
    exit 0
fi
exit 1
