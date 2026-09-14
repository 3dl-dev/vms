#!/bin/bash
# test_ssh_boot_e2e.sh (rd vms-843a) -- runs INSIDE the SSH test-overlay bootable
# image (built --build-arg OVMX_TEST_ENABLE_SSH=1). Cold-boots the OVMX distro
# under QEMU with the aux server auto-started (the SSH overlay's SYSTARTUP runs
# TCPIP$CONFIG then @SYS$STARTUP:TCPIP$STARTUP) and a SLIRP hostfwd to guest :22,
# then connects INBOUND with a standard ssh client and a SYSUAF password login and
# asserts the session lands in DCL -- proving SSH -> SYSUAF -> DCL end to end on a
# booted distro over the proven (B) aux-launch path: TCPIP$INETD accepts :22 over
# BGn:, ACP-stages SYS$SYSTEM:VMSSSHD.EXE off the ODS-2 disk, execs sshd -i on the
# accepted fd; the wrapped sshd authenticates the password against the binary
# SYSUAF (Purdy) and drops into LOGINOUT -> DCL, all bytes over the executive
# socket. No shell, no fork of upstream OpenSSH.
#
# This asserts a REAL cold boot: the shipped image ships no SSH and opens no port;
# only this test-overlay build (OVMX_TEST_ENABLE_SSH=1) stages + auto-starts SSH.
set -u
FAIL=0
ok(){  echo "  PASS: $*"; }
bad(){ echo "  FAIL: $*"; FAIL=1; }

MARKER="OVMX_DCL_LANDED_843a"

# Boot artifacts the bootable image ships (distro/Dockerfile.bootable runner stage).
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
[ -f "$INITRD" ] || INITRD=/boot/initramfs-ovmx-slim.cpio.gz
IMG=/boot/ovmx-distrib.img
for f in "$KERNEL" "$INITRD" "$IMG"; do
    [ -f "$f" ] || { echo "FATAL: missing boot artifact $f (image /boot layout)"; ls -la /boot 2>/dev/null; exit 1; }
done
command -v ssh >/dev/null 2>&1     || { echo "FATAL: ssh client missing in the harness image"; exit 1; }
command -v sshpass >/dev/null 2>&1 || { echo "FATAL: sshpass missing in the harness image"; exit 1; }

DISK=/tmp/ssh-e2e.img
cp "$IMG" "$DISK"                       # writable copy for -drive
LOG=/tmp/ssh-e2e-console.log
HOSTPORT=2222                           # host side of the SLIRP forward -> guest :22
rm -f "$LOG"

ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

# virtio-net NIC + inbound hostfwd (:2222 -> guest :22), same shape as the daytime
# proof. 768M: sshd's privsep re-exec (sshd-session/sshd-auth) + key exchange want
# a little more headroom than the tiny daytime service.
qemu-system-x86_64 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 quiet" \
    -m 768M -smp 2 $ACCEL \
    -netdev "user,id=net0,hostfwd=tcp::${HOSTPORT}-:22" \
    -device virtio-net-pci,netdev=net0 \
    -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot </dev/null >"$LOG" 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null' EXIT

wait_for(){ # pattern limit-seconds
    local pat="$1" lim="$2" i=0
    while [ "$i" -lt "$lim" ]; do
        grep -qF "$pat" "$LOG" 2>/dev/null && return 0
        kill -0 "$QPID" 2>/dev/null || return 1     # qemu died
        sleep 2; i=$((i+2))
    done
    return 1
}

if wait_for '%OVMX-I-EXEC' 120; then
    ok "executive attached on a real cold boot (vms.ko)"
else
    bad "executive never attached within 120s"
fi

# The SSH overlay SYSTARTUP brings the NIC up (TCPIP$CONFIG) and starts the SSH
# server (TCPIP$SSH_STARTUP -> RUN/DETACHED VMSSSHD.EXE, sshd -D) during LPMAIN,
# before the login prompt.
if wait_for '%SSH-I-SRVSTARTED' 180; then
    ok "SSH server (VMSSSHD) started detached on cold boot"
else
    bad "SSH server never started (TCPIP\$CONFIG / TCPIP\$SSH_STARTUP) -- see console tail"
fi

sleep 5                                 # let sshd bind :22 + listen over BGn:

# Inbound SSH: password login as SYSTEM (SYSUAF password MANAGER), non-interactive,
# feeding one DCL WRITE + LOGOUT on stdin. A landed DCL session echoes the marker.
SSH_ERR=/tmp/ssh-e2e-client.err
RESP=$(sshpass -p MANAGER ssh \
        -p "$HOSTPORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no -o NumberOfPasswordPrompts=1 \
        -o ConnectTimeout=15 \
        SYSTEM@127.0.0.1 2>"$SSH_ERR" <<EOF
WRITE SYS\$OUTPUT "$MARKER"
LOGOUT
EOF
)
echo "  ssh response: [$RESP]"

if printf '%s' "$RESP" | grep -qF "$MARKER"; then
    ok "SSH password login for SYSUAF user SYSTEM landed in DCL and ran a command -- TCPIP\$INETD launched VMSSSHD.EXE (SYS\$SYSTEM: ACP-stage -> execve, sshd -i), SYSUAF/Purdy auth, LOGINOUT->DCL, bytes over the executive BGn: socket (vms-843a)"
else
    bad "no DCL marker in the SSH response -- login did not land in DCL"
fi

# INV-6 negative-ish sanity: a bad password must NOT land in DCL (auth is real).
BADRESP=$(sshpass -p WRONGPASS ssh \
        -p "$HOSTPORT" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        -o KbdInteractiveAuthentication=no -o NumberOfPasswordPrompts=1 \
        -o ConnectTimeout=15 \
        SYSTEM@127.0.0.1 2>/dev/null <<EOF
WRITE SYS\$OUTPUT "$MARKER"
LOGOUT
EOF
)
if printf '%s' "$BADRESP" | grep -qF "$MARKER"; then
    bad "a WRONG password reached DCL -- SYSUAF auth is not gating (INV-6 violation)"
else
    ok "wrong password did NOT reach DCL (SYSUAF password auth is real, fail-closed)"
fi

if [ "$FAIL" != 0 ]; then
    echo "--- ssh client stderr ---"; tail -40 "$SSH_ERR" 2>/dev/null
    echo "--- console tail ---"; tail -80 "$LOG" 2>/dev/null
fi
echo "=== test_ssh_boot_e2e: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) ==="
exit "$FAIL"
