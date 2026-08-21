#!/usr/bin/env bash
# probe_4ac.sh - diagnostic: why does SYSTEM get %RMS-E-CRE in SYS$SYSTEM/SYS$MANAGER?
# Boots the real runtime, logs in as SYSTEM, dumps UIC/privs, the actual owner+
# protection of the system tree, a create attempt, and HELP -- then prints the
# FULL console (so provision's boot output is visible too). NOT a test; no asserts.
set -u
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DISK=/tmp/probe-sysdisk.img
CONSOLE_LOG=/tmp/probe-console.log
FIFO=/tmp/probe-console.in
BOOT_TIMEOUT=300; STEP_TIMEOUT=60; COMMAND_TIMEOUT=25

rm -f "$DISK" "$CONSOLE_LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
  QEMU=qemu-system-aarch64; MACHINE="-machine virt -cpu cortex-a57"; CONSOLE="console=ttyAMA0"
else
  QEMU=qemu-system-x86_64; MACHINE=""; CONSOLE="console=ttyS0"
fi

# shellcheck disable=SC2086
timeout 500 $QEMU $MACHINE -kernel "$KERNEL" -initrd "$INITRD" -nographic \
  -append "$CONSOLE loglevel=3 quiet" -m 256M -smp 1 -nic none -nodefaults \
  -serial stdio -drive file="$DISK",format=raw,if=virtio -no-reboot \
  <"$FIFO" >"$CONSOLE_LOG" 2>&1 &
QEMU_PID=$!
exec 3>"$FIFO"
cleanup() { exec 3>&- 2>/dev/null || true; kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true; rm -f "$FIFO"; }
trap cleanup EXIT
send() { printf '%s\r' "$1" >&3; }
wait_for() {
  local pattern="$1" limit="${2:-$STEP_TIMEOUT}" since="${3:-0}" waited=0
  while [ "$waited" -lt "$((limit * 4))" ]; do
    tail -c "+$((since + 1))" "$CONSOLE_LOG" 2>/dev/null | grep -qF "$pattern" && return 0
    kill -0 "$QEMU_PID" 2>/dev/null || { echo "guest exited waiting for '$pattern'"; return 1; }
    sleep 0.25; waited=$((waited + 1))
  done
  echo "TIMEOUT waiting for '$pattern'"; return 1
}
run() {  # send a command, wait for prompt
  local off; off=$(wc -c <"$CONSOLE_LOG"); send "$1"
  wait_for '$ ' "$COMMAND_TIMEOUT" "$off" || echo "(no prompt after: $1)"
}

wait_for 'Username:' "$BOOT_TIMEOUT" || { echo "NO LOGIN PROMPT"; tail -80 "$CONSOLE_LOG"; exit 1; }
OFF=$(wc -c <"$CONSOLE_LOG")
send 'SYSTEM'; wait_for 'Password:' "$STEP_TIMEOUT" "$OFF"
send 'MANAGER'; wait_for 'Welcome to OpenVMX' "$STEP_TIMEOUT" "$OFF" || echo "(login may have failed)"

run 'SHOW PROCESS'
run 'SHOW PROCESS/PRIVILEGES'
run 'DIRECTORY/OWNER/PROTECTION SYS$COMMON:[000000]SYSEXE.DIR'
run 'DIRECTORY/OWNER/PROTECTION SYS$SYSTEM:LOGINOUT.EXE'
run 'DIRECTORY/OWNER/PROTECTION SYS$SYSDEVICE:[000000]SYS0.DIR'
run 'SET DEFAULT SYS$MANAGER:'
run 'SHOW DEFAULT'
run 'COPY LOGIN.COM PROBE_4AC.TXT'
run 'DIRECTORY SYS$MANAGER:'
run 'COPY LOGIN.COM SYS$SYSTEM:PROBE_SYS.TXT'
run 'HELP SHOW'
run 'LOGOUT'
sleep 2

echo ""
echo "======================== FULL CONSOLE ========================"
cat "$CONSOLE_LOG" | tr -d '\r'
echo "======================== END CONSOLE ========================"
