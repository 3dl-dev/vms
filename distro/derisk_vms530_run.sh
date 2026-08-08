#!/bin/bash
# derisk_vms530_run.sh — vms-530 day-1 GO/NO-GO evidence script.
#
# Boots the real ovmx-boot-derisk QEMU image (built from distro/Dockerfile.bootable
# with the vms-530 scratch HELLOVMS.EXE added), logs in as SYSTEM (LOGINOUT.EXE
# setuid()s the session to SYSTEM's Linux UIC-mapped uid/gid BEFORE exec'ing
# DCL.EXE -- tools/vms_login.c), then RUNs HELLOVMS.EXE (a trivial LINK.EXE
# --executable, PT_INTERP=IMGACT.EXE, cross-image import of printf/getuid/
# geteuid from the real DECC$SHR.EXE) from that non-root DCL session.
#
# Scratch driver for the de-risk investigation only -- not a CI gate, not
# referenced by any build target. See docs/derisk-vms-530-imgact-qemu.md.
#
# Usage: distro/derisk_vms530_run.sh [image-tag]

set -uo pipefail
IMG="${1:-ovmx-boot-derisk}"
TIMEOUT=90
LOG=/tmp/derisk-vms530-console.log
FIFO=/tmp/derisk-vms530.in
rm -f "$LOG" "$FIFO"
mkfifo "$FIFO"

docker rm -f derisk-vms530 >/dev/null 2>&1 || true
# Open the FIFO read/write (4<>) on our OWN fd first so opening it never
# blocks (a plain read-only open() blocks until a writer connects) -- then
# hand that already-open fd to docker's stdin. This also means the FIFO
# never sees EOF between sends, so one docker run keeps its DCL session
# across every command below.
exec 4<>"$FIFO"
docker run --rm -i --name derisk-vms530 "$IMG" <&4 >"$LOG" 2>&1 &
DOCKER_BG_PID=$!

cleanup() { exec 4>&- 2>/dev/null || true; docker kill derisk-vms530 >/dev/null 2>&1 || true; rm -f "$FIFO"; }
trap cleanup EXIT

send() { printf '%s\r' "$1" >&4; }

wait_for() {
    local pattern="$1" limit="${2:-30}" since="${3:-0}" waited=0 seen_running=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pattern"; then
            return 0
        fi
        if docker ps --filter "name=derisk-vms530" --filter "status=running" -q | grep -q .; then
            seen_running=1
        elif [ "$seen_running" -eq 1 ]; then
            return 1   # container WAS running and died -- real failure
        fi
        sleep 0.25
        waited=$((waited + 1))
    done
    return 1
}

echo "=== vms-530 de-risk: boot + login + RUN HELLOVMS.EXE ==="

wait_for '%OVMX-I-EXEC' 60 || { echo "FAIL: executive never attached"; cat "$LOG"; exit 1; }
echo "OK: executive attached"

wait_for 'Username:' 30 || { echo "FAIL: no login prompt"; cat "$LOG"; exit 1; }
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" || { echo "FAIL: no password prompt"; cat "$LOG"; exit 1; }
send 'MANAGER'
wait_for 'Welcome to OVMX' 30 "$LOGIN_OFF" || { echo "FAIL: SYSTEM login did not complete"; cat "$LOG"; exit 1; }
echo "OK: SYSTEM login complete (LOGINOUT.EXE setuid + DCL.EXE activated via IMGACT)"

DIR_OFF=$(wc -c <"$LOG")
send 'DIRECTORY /vms/SYS0/SYSCOMMON/SYSEXE/'
wait_for '$ ' 15 "$DIR_OFF" || { echo "FAIL: DCL prompt did not return after full DIRECTORY"; cat "$LOG"; exit 1; }
echo "--- full SYSEXE DIRECTORY ---"
tail -c "+$((DIR_OFF + 1))" "$LOG" | tr -d '\r'
echo "--- end full SYSEXE DIRECTORY ---"

DIR_OFF=$(wc -c <"$LOG")
send 'DIRECTORY /vms/SYS0/SYSCOMMON/SYSEXE/HELLOVMS.EXE'
wait_for '$ ' 15 "$DIR_OFF" || { echo "FAIL: DCL prompt did not return after DIRECTORY"; cat "$LOG"; exit 1; }
echo "--- DIRECTORY output ---"
tail -c "+$((DIR_OFF + 1))" "$LOG" | tr -d '\r'
echo "--- end DIRECTORY output ---"

CTL_OFF=$(wc -c <"$LOG")
send 'RUN /vms/NONEXISTENT-vms530-control.exe'
wait_for '$ ' 15 "$CTL_OFF" || { echo "FAIL: DCL prompt did not return after control RUN"; cat "$LOG"; exit 1; }
echo "--- control RUN (nonexistent image) output ---"
tail -c "+$((CTL_OFF + 1))" "$LOG" | tr -d '\r'
echo "--- end control output ---"

CTL2_OFF=$(wc -c <"$LOG")
send 'COPY /vms/SYS0/SYSCOMMON/SYSEXE/HELLOVMS.EXE /vms/SYS0/SYSCOMMON/SYSEXE/HVCOPY.EXE'
wait_for '$ ' 20 "$CTL2_OFF" || { echo "FAIL: DCL prompt did not return after COPY"; cat "$LOG"; exit 1; }
echo "--- COPY control output ---"
tail -c "+$((CTL2_OFF + 1))" "$LOG" | tr -d '\r'
echo "--- end COPY control output ---"

CTL3_OFF=$(wc -c <"$LOG")
send 'RUN /vms/SYS0/SYSCOMMON/SYSEXE/HVCOPY.EXE'
wait_for '$ ' 15 "$CTL3_OFF" || { echo "FAIL: DCL prompt did not return after control RUN 2"; cat "$LOG"; exit 1; }
echo "--- control RUN (DCL-COPY-fresh HELLOVMS.EXE, isolates install-path vs COPY-path) output ---"
tail -c "+$((CTL3_OFF + 1))" "$LOG" | tr -d '\r'
echo "--- end control 2 output ---"

CMD_OFF=$(wc -c <"$LOG")
send 'RUN /vms/SYS0/SYSCOMMON/SYSEXE/HELLOVMS.EXE'
wait_for '$ ' 20 "$CMD_OFF" || { echo "FAIL: DCL prompt did not return after RUN"; cat "$LOG"; exit 1; }

SEGMENT=$(tail -c "+$((CMD_OFF + 1))" "$LOG" | tr -d '\r')
echo "--- RUN output ---"
echo "$SEGMENT"
echo "--- end RUN output ---"

if echo "$SEGMENT" | grep -q '%HELLOVMS-I-ACTIVATED'; then
    echo "OK: HELLOVMS.EXE activated via IMGACT.EXE"
else
    echo "FAIL: HELLOVMS.EXE did not activate"
    exit 1
fi

if echo "$SEGMENT" | grep -qE '%HELLOVMS-I-IDENT, uid=[1-9][0-9]* euid=[1-9][0-9]*'; then
    echo "OK: HELLOVMS.EXE ran as a NON-ROOT uid/euid"
elif echo "$SEGMENT" | grep -q '%HELLOVMS-I-IDENT, uid=0 euid=0'; then
    echo "INCONCLUSIVE: HELLOVMS.EXE ran as uid=0 (root) -- SYSTEM's mapped UIC resolved to root"
else
    echo "FAIL: no uid/euid line found"
    exit 1
fi

echo "=== full console log ==="
cat "$LOG"
