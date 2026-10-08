#!/bin/sh
# test_semantic_oracle.sh - run the semantic-oracle probes on the booted OVMX
# runtime and save their transcripts (rd vms-8d1).
#
# Boots the REAL runtime (real vms.ko executive) from the semantic-oracle probe
# disk /boot/ovmx-distrib-semprobe.img -- ovmx-distrib.img plus the probe images
# SYS$COMMON:[SYSTEST]SP_*.EXE (distro/Dockerfile.bootable) -- logs in SYSTEM/MANAGER
# and RUNs every SP_<FAMILY>.EXE found there. Each probe prints a canonical
# transcript between "=== SEMPROBE <family> BEGIN ===" and "... END ===";
# this script writes each one to $OUT_DIR/<family>.txt.
#
# It does NOT judge the transcripts: tools/oracle/semantic/semantic_diff.py
# compares them with the real-VMS goldens (docs/oracle/semantics/) and the
# known-diff ratchet, on the CI host. Exit 0 = every probe ran and produced a
# transcript (an aborted transcript still counts -- the diff judges it);
# exit 1 = boot/login failed, no probe found, or a probe printed no transcript.
#
# Run INSIDE the ovmx-boot image with $OUT_DIR bind-mounted.

set -u

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
CMD_TIMEOUT="${CMD_TIMEOUT:-240}"
OUT_DIR="${OUT_DIR:-/out}"

DISTRIB_IMG="${SEMPROBE_IMG:-/boot/ovmx-distrib-semprobe.img}"
KERNEL="${KERNEL:-/boot/vmlinuz}"
SLIM_INITRD="${SLIM_INITRD:-/boot/initramfs-ovmx-slim.cpio.gz}"
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    [ -w /dev/kvm ] && MACHINE="-enable-kvm -cpu host"
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$SLIM_INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this INSIDE the ovmx-boot image"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }
mkdir -p "$OUT_DIR"

echo "=== OVMX semantic oracle (vms-8d1): boot, login, RUN SYS\$COMMON:[SYSTEST]SP_*.EXE ==="

W=$(mktemp -d)
DISK=$W/disk.img
LOG=$W/console.log
FIFO=$W/console.in
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"
QPID=""

stop_vm() { exec 4>&- 2>/dev/null || true; [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; [ -n "$QPID" ] && wait "$QPID" 2>/dev/null; QPID=""; }
cleanup() { stop_vm; rm -rf "$W"; }
trap cleanup EXIT

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern limit-seconds since-byte
    pat="$1"; limit="${2:-30}"; since="${3:-0}"; waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qaF -- "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
die() { echo "FATAL: $1"; echo "--- console log (tail) ---"; tail -c 6000 "$LOG"; exit 1; }

# Boot the probe disk (the SAME disk image each time: a probe's files and names
# persist exactly as they would across a reboot of a real system) and log in.
boot_login() {
    stop_vm
    : >"$LOG"
    # shellcheck disable=SC2086
    timeout 3000 $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nic none -nodefaults -serial stdio \
        -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$FIFO" >"$LOG" 2>&1 &
    QPID=$!
    exec 4>"$FIFO"
    w=0
    until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
        send ''; sleep 1; w=$((w + 1))
    done
    wait_for 'Username:' 5 || die "boot never reached Username: within ${BOOT_TIMEOUT}s"
    off=$(wc -c <"$LOG")
    send 'SYSTEM'
    wait_for 'Password:' 30 "$off" && send 'MANAGER'
    wait_for 'Welcome to OpenVMX' 30 "$off" || die "SYSTEM login failed"
    wait_for '$ ' 20 "$off"
    sleep 2
}

SEG=""
PROMPT=0
run_cmd() {  # cmd [timeout] -> SEG; PROMPT=1 if DCL came back
    off=$(wc -c <"$LOG")
    send "$1"
    PROMPT=0
    # the command is done when the console ends in a fresh DCL prompt
    waited=0
    while [ "$waited" -lt "$(( ${2:-$CMD_TIMEOUT} * 4 ))" ]; do
        sleep 0.25; waited=$((waited + 1))
        [ "$(wc -c <"$LOG")" -gt "$((off + ${#1} + 2))" ] || continue
        if [ "$(tail -c 2 "$LOG")" = '$ ' ]; then PROMPT=1; break; fi
        kill -0 "$QPID" 2>/dev/null || break
    done
    sleep 0.5
    SEG=$(segment_since "$off")
}

boot_login
run_cmd 'DIRECTORY/NOHEADING/NOTRAILING SYS$COMMON:[SYSTEST]SP_*.EXE,SP_*.COM' 60
# an image probe (SP_x.EXE) is RUN; a DCL-family probe (SP_x.COM, comgen.py) is
# run with @ -- each prints the same BEGIN/END-marked transcript
PROBES=$(printf '%s\n' "$SEG" | grep -o 'SP_[A-Z0-9_]*\.\(EXE\|COM\)' | sort -u)
[ -n "$PROBES" ] || die "no SP_*.EXE probe found in SYS\$COMMON:[SYSTEST] (listing: $SEG)"
echo "probes: $(echo $PROBES)"

FAIL=0
for pf in $PROBES; do
    p=${pf%.*}
    fam=$(echo "${p#SP_}" | tr '[:upper:]' '[:lower:]')
    case "$pf" in
        *.COM) run_cmd "@SYS\$COMMON:[SYSTEST]$p.COM" ;;
        *)     run_cmd "RUN SYS\$COMMON:[SYSTEST]$p.EXE" ;;
    esac
    printf '%s\n' "$SEG" | awk -v f="$fam" '
        $0 == "=== SEMPROBE " f " BEGIN ===" { on = 1 }
        on { print }
        on && ($0 == "=== SEMPROBE " f " END ===" || $0 == "=== SEMPROBE aborted END ===") { exit }' \
        > "$OUT_DIR/$fam.txt"
    n=$(grep -c . "$OUT_DIR/$fam.txt")
    if [ "$PROMPT" -ne 1 ]; then
        # The probe never returned to DCL (a service blocked and the hang guard's
        # AST never came): its transcript ends where it stopped -- the diff reports
        # the rest MISSING. Reboot so the remaining probes still run.
        echo "WARN: $p did not return to DCL within ${CMD_TIMEOUT}s; transcript kept ($n lines), rebooting"
        boot_login
    fi
    if [ "$n" -lt 1 ]; then
        echo "FAIL: $p printed no transcript; console segment:"
        printf '%s\n' "$SEG" | tail -40
        FAIL=$((FAIL + 1))
    else
        echo "OK: $p -> $OUT_DIR/$fam.txt ($n lines)"
    fi
done
send 'LOGOUT'
sleep 2
[ "$FAIL" -eq 0 ] || exit 1
exit 0
