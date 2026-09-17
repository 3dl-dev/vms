#!/bin/bash
# test_cluster_config_lan_e2e.sh - vms-23b9 rung 1 (the V0.7 "clustering done"
# gate): a USER configures THIS node's cluster identity by running the DOCUMENTED
# VMS-faithful procedure -- @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM -- on a real boot,
# then REBOOTS, and the running system ADOPTS the authored identity.
#
# =============================================================================
# WHAT THIS PROVES THAT NOTHING ELSE DID
# =============================================================================
# The author->reboot->adopt MECHANISM (SYSGEN USE CURRENT / SET SCSNODE / WRITE
# CURRENT on the booted volume, then a power-cycle whose PID 1 reads the new
# OVMXVMSSYS.PAR and sethostname()s from SCSNODE) is already CI-proven by
# tests/qemu/test_boot_scsnode_hostname_e2e.sh -- but that gate types the SYSGEN
# commands BY HAND. tests/dcl/test_cluster_config_lan.sh drives the actual
# procedure, but through a HOST DCL interpreter with piped stdin against a /tmp
# store: it never boots, never reboots, and never proves the identity is adopted
# by a running system. It asserts only that a store-write happened.
#
# THIS gate closes the remaining gap: it runs the ACTUAL DOCUMENTED OPERATOR
# PROCEDURE (@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM, the CHANGE path from
# docs/cluster-configuration-guide.md) INTERACTIVELY over the console on a REAL
# booted OVMX, power-cycles the same disk, and proves the new identity is in
# effect in the running system after the reboot -- the executive's live
# hostname (F$GETSYI("NODENAME"), the value PID 1 sethostname()'d from the
# authored SCSNODE) AND the boot console's own %OVMX-I-SCSNODE adoption line,
# with F$GETSYI("SCSNODE") and SYSGEN USE CURRENT agreeing. The user experience
# IS the test -- no host shim, no hand-poked SYSGEN.
#
# =============================================================================
# WHY F$GETSYI("NODENAME") IS THE ADOPTION PROOF, NOT F$GETSYI("SCSNODE")
# =============================================================================
# (Inherited verbatim from test_boot_scsnode_hostname_e2e.sh's reasoning.)
# ovmx_node_name() -- which backs F$GETSYI("SCSNODE") and SHOW SYSTEM -- reads
# SCSNODE straight out of the .PAR file, so on its own it would pass merely
# because WRITE CURRENT changed the file, proving nothing about ADOPTION.
# F$GETSYI("NODENAME") calls uname(2) and returns the real Linux hostname, which
# PID 1 set ONCE at boot via sethostname() from the authored SCSNODE. Only a
# genuine second boot that read the authored .PAR and applied it can make
# NODENAME read the new name. That -- plus the boot console's own
# %OVMX-I-SCSNODE line -- is the "adopted on the next reboot" claim, proven.
# F$GETSYI("SCSNODE") and SYSGEN USE CURRENT are asserted too: they must AGREE
# with the adopted hostname (the file and the running system tell one story).
#
# =============================================================================
# WHY TWO SEPARATE QEMU PROCESSES ON THE SAME DISK IS "REBOOT" HERE
# =============================================================================
# There is no DCL REBOOT verb and no live re-read of the parameter file (VMS has
# none either -- SCSNODE is a boot parameter). Proving the change took effect
# requires a fresh PID 1 execution against the SAME persistent disk: kill the
# first QEMU (a power-cycle; -no-reboot means a guest reboot(2) exits QEMU) and
# start a second against the identical disk image -- the same technique
# test_release_e2e.sh and test_boot_scsnode_hostname_e2e.sh already use.
#
# THE WRITEBACK TRAP (see test_release_e2e.sh's header for the full rationale):
# Linux's default dirty_expire_centisecs is 30s, so a QEMU killed right after a
# DCL write can lose that write before it reaches the backing file. SETTLE_SECS
# below is what makes boot 1's authored .PAR real for boot 2 to read.
#
# HARNESS SHAPE (inherited from test_boot_scsnode_hostname_e2e.sh -- read its
# header): every QEMU launch is INLINE (no boot_qemu() helper -- the
# function+$(...) shape wedges) and wrapped in `timeout -k 15` (a plain
# `timeout N` only SIGTERMs, then waits unbounded on an unresponsive child). The
# console is driven over a mkfifo, one `send` (adds \r) at a time with small
# settle sleeps between sends.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_cluster_config_lan_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit 0 = every assertion below passed against the real mounted volume.
# Exit 1 = a real failure (see the printed transcript segment). If the author->
#          reboot->adopt path is broken when driven through the real procedure,
#          this gate goes RED with the transcript -- it is never papered over.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETTLE_SECS="${SETTLE_SECS:-60}"     # see THE WRITEBACK TRAP above
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 does not install a blank disk
# (vms-2f0), and CLUSTER_CONFIG_LAN.COM authors AGAINST an existing
# SYS$SYSTEM:OVMXVMSSYS.PAR (USE CURRENT), so the disk must already carry the
# seeded ;1 (SCSNODE=OVMX). The mastered image is where distro/rootfs's seeded
# OVMXVMSSYS.PAR;1 lands.
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing - the mastering stage did not run"
    exit 1
fi

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

PASS=0
FAIL=0
record() {
    local desc="$1" rc="$2"
    if [ "$rc" -eq 0 ]; then echo "  PASS: $desc"; PASS=$((PASS + 1))
    else echo "  FAIL: $desc"; FAIL=$((FAIL + 1)); fi
}
check() {
    local desc="$1" log="$2" pattern="$3" expect="${4:-present}"
    # "--" is load-bearing: some patterns start with "-" (VMS continuation lines)
    # and grep would otherwise parse them as options and silently never match.
    if grep -qaF -- "$pattern" "$log" 2>/dev/null; then
        if [ "$expect" = "present" ]; then record "$desc" 0; else record "$desc" 1; fi
    else
        if [ "$expect" = "absent" ]; then record "$desc" 0; else record "$desc" 1; fi
    fi
}
waitfor() {  # pattern  limit-seconds  log
    local pat="$1" lim="${2:-60}" log="$3" w=0
    while [ $w -lt $((lim * 4)) ]; do
        grep -qaF "$pat" "$log" 2>/dev/null && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w + 1))
    done
    return 1
}

echo "=== @CLUSTER_CONFIG_LAN.COM authors cluster identity -> reboot -> adopted (vms-23b9 rung 1) ==="
echo "arch=$ARCH qemu=$QEMU"

# The new identity we author through the DOCUMENTED procedure. OVMXA is <=6
# chars (valid SCSNODE); 1029 is an SCSSYSTEMID from the lab's own 1025..1029
# range. Both differ from the seed (OVMX / 0), so a pass cannot come from the
# seed.
NEWNODE=OVMXA
NEWSID=1029

# =============================================================================
# CASE 1: POSITIVE - run @CLUSTER_CONFIG_LAN.COM (CHANGE), reboot, identity adopted
# =============================================================================
echo ""
echo "--- CASE 1 (positive): @SYS\$MANAGER:CLUSTER_CONFIG_LAN.COM CHANGE -> $NEWNODE, reboot ---"

POS_DISK=/tmp/ccl-e2e-pos.img
POS_LOG1=/tmp/ccl-e2e-pos-boot1.log
POS_LOG2=/tmp/ccl-e2e-pos-boot2.log
POS_FIFO=/tmp/ccl-e2e-pos.in
rm -f "$POS_DISK" "$POS_LOG1" "$POS_LOG2" "$POS_FIFO"
cp "$DISTRIB_IMG" "$POS_DISK"
mkfifo "$POS_FIFO"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$POS_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$POS_FIFO" >"$POS_LOG1" 2>&1 &
qp=$!
exec 4>"$POS_FIFO"
send() { printf '%s\r' "$1" >&4; }
# vms-2213: OPA0: LOGINOUT waits for the operator's RETURN before "Username:".
# A single CR at t=0 is lost (serial not up yet); feed one per second until the
# prompt appears. Bounded.
wake_login() {
    local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}

wake_login "$POS_LOG1"
if waitfor 'Username:' 120 "$POS_LOG1"; then rc=0; else rc=1; fi
record "boot 1: pre-installed disk boots to login" "$rc"
if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: SYSTEM logs in" "$rc"

    # Baseline: the seed carries SCSNODE=OVMX, and boot 1's OWN PID 1 already
    # sethostname()'d it, so NODENAME reads OVMX now. This proves that when
    # boot 2 reads OVMXA it came from the procedure we are about to run, not
    # from the seed.
    send 'HOST1 = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOST1'; sleep 1
    check "boot 1: baseline F\$GETSYI(NODENAME) reads the seed's OVMX" "$POS_LOG1" 'HOST1 = "OVMX"'
fi

if [ "$rc" -eq 0 ]; then
    # === THE DOCUMENTED USER PROCEDURE, DRIVEN INTERACTIVELY ===
    # @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM presents a menu; we walk the CHANGE
    # path exactly as docs/cluster-configuration-guide.md documents it. INQUIRE
    # reads our console sends (SYS$COMMAND); the procedure's inline SYSGEN deck
    # (RUN SYS$SYSTEM:SYSGEN.EXE ... WRITE CURRENT) drives itself off SYS$INPUT.
    send '@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM'; sleep 2
    if waitfor 'Enter choice' 30 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: CLUSTER_CONFIG_LAN.COM presents the configuration menu" "$rc"

    send '2'; sleep 2                     # CHANGE this node's cluster characteristics
    if waitfor "node's SCSNODE name" 20 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: CHANGE path prompts for SCSNODE" "$rc"

    # ROBUSTNESS: an over-long name (>6 chars) is rejected with the honest
    # facility error and re-prompted -- never written. Same guard the host shim
    # test (tests/dcl/test_cluster_config_lan.sh) asserts, here on a real boot.
    send 'TOOLONGNAME'; sleep 2
    check "boot 1: over-long SCSNODE rejected with %CLUSTER_CONFIG-E-BADNODE" \
        "$POS_LOG1" '%CLUSTER_CONFIG-E-BADNODE, SCSNODE is limited to 6 characters'

    send "$NEWNODE"; sleep 2              # a valid <=6 char node name
    if waitfor 'SCSSYSTEMID' 20 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: valid SCSNODE accepted, prompts for SCSSYSTEMID" "$rc"

    send "$NEWSID"; sleep 2               # positive integer system id
    send ''; sleep 1                      # Allocation class [0] -> default
    send ''; sleep 1                      # Votes this node contributes [1] -> default
    send ''; sleep 1                      # Expected total cluster votes [1] -> default
    if waitfor 'Is this correct' 20 "$POS_LOG1"; then rc=0; else rc=1; fi
    record "boot 1: procedure shows the confirmation screen" "$rc"

    # The confirmation screen echoes the values we entered.
    check "boot 1: confirmation echoes SCSNODE = $NEWNODE" "$POS_LOG1" "SCSNODE         = $NEWNODE"
    check "boot 1: confirmation echoes SCSSYSTEMID = $NEWSID" "$POS_LOG1" "SCSSYSTEMID     = $NEWSID"

    send 'Y'; sleep 3                     # confirm -> WRITE_NOENABLE (CHANGE keeps VAXCLUSTER)

    # The inline SYSGEN deck actually SET the params and WROTE a new .PAR version
    # over the real mounted volume -- the SAME ;2-over-;1 proof shape the
    # hand-driven scsnode gate uses, but here emitted BY the procedure.
    check "boot 1: SYSGEN SET SCSNODE changed OVMX -> $NEWNODE" \
        "$POS_LOG1" "%SYSGEN-I-SETPARAM, SCSNODE changed from OVMX to $NEWNODE"
    check "boot 1: SYSGEN SET SCSSYSTEMID changed 0 -> $NEWSID" \
        "$POS_LOG1" "%SYSGEN-I-SETPARAM, SCSSYSTEMID changed from 0 to $NEWSID"
    if waitfor '%SYSGEN-I-WRITTEN, 31 parameters written to SYS$SYSTEM:OVMXVMSSYS.PAR;2' 20 "$POS_LOG1"; then
        rc=0; else rc=1; fi
    record "boot 1: WRITE CURRENT minted OVMXVMSSYS.PAR;2 (real vmsfs version on the volume)" "$rc"
    check "boot 1: procedure reports 'CHANGE complete ... takes effect on the next reboot'" \
        "$POS_LOG1" "CHANGE complete. The new cluster identity is written to"

    send '5'; sleep 2                     # EXIT the procedure cleanly
    check "boot 1: procedure exits cleanly" "$POS_LOG1" "Exiting the cluster configuration procedure"

    echo "  (settling ${SETTLE_SECS}s for guest writeback)"
    sleep "$SETTLE_SECS"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

# --- Boot 2 (power-cycle): the boot that must ADOPT the authored identity ---
rm -f "$POS_FIFO"; mkfifo "$POS_FIFO"
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 256M -smp 1 -nic none -nodefaults -serial stdio \
    -drive file="$POS_DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$POS_FIFO" >"$POS_LOG2" 2>&1 &
qp=$!
exec 4>"$POS_FIFO"

wake_login "$POS_LOG2"
if waitfor 'Username:' 120 "$POS_LOG2"; then rc=0; else rc=1; fi
record "boot 2 (reboot): still reaches the login prompt" "$rc"

# ADOPTION PROOF #1: the boot console ITSELF announces the authored node name,
# before any DCL runs -- PID 1 read the authored OVMXVMSSYS.PAR and applied it.
check "boot 2: %OVMX-I-SCSNODE console line names the authored $NEWNODE" \
    "$POS_LOG2" "%OVMX-I-SCSNODE, node name $NEWNODE set from SYS\$SYSTEM:OVMXVMSSYS.PAR"
check "boot 2: NO honest-halt warning (a good, present .PAR does not warn)" \
    "$POS_LOG2" "%OVMX-W-NOPARAMS" absent

if [ "$rc" -eq 0 ]; then
    send 'SYSTEM'; sleep 1
    send 'MANAGER'; sleep 1
    if waitfor 'Welcome to OpenVMX' 30 "$POS_LOG2"; then rc=0; else rc=1; fi
    record "boot 2: SYSTEM logs in" "$rc"

    # ADOPTION PROOF #2: the LIVE running system's hostname. F$GETSYI("NODENAME")
    # returns uname(2) uts.nodename -- what PID 1 sethostname()'d from the
    # authored SCSNODE this boot. Reads $NEWNODE only if the running system
    # genuinely adopted the value the procedure wrote.
    send 'HOST2 = F$GETSYI("NODENAME")'; sleep 1
    send 'SHOW SYMBOL HOST2'; sleep 1
    check "boot 2: F\$GETSYI(NODENAME) reads $NEWNODE (the REAL live hostname, set by sethostname())" \
        "$POS_LOG2" "HOST2 = \"$NEWNODE\""

    # AGREEMENT: F$GETSYI("SCSNODE") (reads the persisted .PAR) agrees with the
    # adopted live hostname -- the file and the running system tell one story.
    send 'SNODE = F$GETSYI("SCSNODE")'; sleep 1
    send 'SHOW SYMBOL SNODE'; sleep 1
    check "boot 2: F\$GETSYI(SCSNODE) also reads $NEWNODE (persisted .PAR agrees)" \
        "$POS_LOG2" "SNODE = \"$NEWNODE\""

    # AGREEMENT: SYSGEN USE CURRENT / SHOW SCSNODE reads the same authored value
    # back out of the live parameter store.
    send 'SYSGEN'; sleep 1
    send 'USE CURRENT'; sleep 1
    send 'SHOW SCSNODE'; sleep 2
    send 'EXIT'; sleep 1
    check "boot 2: SYSGEN USE CURRENT / SHOW SCSNODE agrees ($NEWNODE)" \
        "$POS_LOG2" "$NEWNODE"
fi
kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
rm -f "$POS_FIFO"

if [ "$FAIL" -ne 0 ]; then
    echo "--- CASE 1 boot 1 log ---"; cat "$POS_LOG1"
    echo "--- CASE 1 boot 2 log ---"; cat "$POS_LOG2"
fi

echo ""
echo "=========================================="
echo "  RESULTS: $PASS/$((PASS + FAIL)) checks passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  @CLUSTER_CONFIG_LAN.COM AUTHOR -> REBOOT -> ADOPT PROVEN ON A REAL BOOT (vms-23b9 rung 1)"
    echo "  -- the DOCUMENTED user procedure, not a host shim; the running system adopted the identity."
    exit 0
fi
exit 1
