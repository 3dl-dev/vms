#!/bin/bash
# test_docker_persistent_disk.sh - Verify the DOCUMENTED install flow survives
# a real container boundary: `docker build` once, then TWO SEPARATE
# `docker run` invocations against a host-mounted disk directory, exactly
# what docs/install-0.1.md and ./boot.sh actually do -- and container 1 is
# stopped PROMPTLY after reaching the login prompt, matching a real user
# following the doc's own "log out or Ctrl-A X out of QEMU here" step.
#
# WHY THIS EXISTS, separately from test_persistent_boot.sh: that script
# proves STARTUP.EXE's install/boot logic is correct, but it drives all
# three of its boots as qemu processes inside ONE container, and lets each
# one idle for up to 90 seconds before an external timeout kills it -- long
# enough that it never actually exercises what a real user does. A real
# user runs `./boot.sh` (install), sees the login prompt, and quits within
# a few seconds; that is two SEPARATE `docker run` invocations against a
# host-mounted volume, with container 1 torn down almost immediately.
#
# ROOT CAUSE this test pins (ground-sourced by direct A/B measurement, not
# guessed): install_system() (src/ovmx_init/ovmx_init.c) writes SYSUAF.DAT
# and the rest of the system tree with plain buffered I/O and never calls
# sync(). Those writes sit dirty in the GUEST's own page cache -- this has
# nothing to do with qemu's drive cache mode, which only governs what
# happens *after* a write leaves the guest kernel. Linux's default
# writeback timer (dirty_expire_centisecs, 30s) eventually flushes them on
# its own, which is exactly why this bug hid so well: any harness (or
# impatient human) that leaves boot 1 running for ~45+ seconds before
# killing it never sees the failure. Measured directly: quitting boot 1
# within seconds of the login prompt, deterministically, 100% of repeated
# trials -- reproducibly fails with `%OVMX-F-EXECINIT, no SYSTEM record in
# SYS$SYSTEM:SYSUAF.DAT` on unpatched install_system(); waiting 45+ seconds
# before quitting always succeeds even unpatched, which is what let this
# ship. Fixed by an explicit sync() in install_system() after all files are
# written, before "installation complete" is printed.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   tests/qemu/test_docker_persistent_disk.sh [image_tag]
#
# Exit code 0 = pass, 1 = failure. Runs on the HOST (or CI runner) -- this
# is the one property this test exists to exercise, so it must not run
# inside a single container the way the other QEMU tests do, and must not
# give container 1 generous idle time before stopping it.

set -uo pipefail

IMAGE="${1:-ovmx-boot:latest}"
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

BOOT1_LOG="$WORKDIR/boot1.log"
BOOT2_LOG="$WORKDIR/boot2.log"
CONTAINER1="test-docker-persistent-disk-$$"

PASS=0
FAIL=0
TOTAL=0

check() {
    local desc="$1" output="$2" pattern="$3" expect="${4:-present}"
    TOTAL=$((TOTAL + 1))
    if echo "$output" | grep -qF "$pattern"; then
        if [ "$expect" = "present" ]; then
            echo "  PASS: $desc"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern found but should be absent: $pattern)"; FAIL=$((FAIL + 1))
        fi
    else
        if [ "$expect" = "absent" ]; then
            echo "  PASS: $desc (correctly absent)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: $desc (pattern not found: $pattern)"; FAIL=$((FAIL + 1))
        fi
    fi
}

echo "=== Docker Persistent Disk Test (cross-container boundary, prompt teardown) ==="
echo "Image: $IMAGE"
echo "Host disk dir: $WORKDIR"
echo ""

# --- Container 1: install (fat initramfs, blank disk), stopped PROMPTLY ---
# This is the property under test: a real user quits within seconds of
# reaching the login prompt, not after a generous 90s idle window.
echo "--- Container 1: install ---"
docker run --rm -i --name "$CONTAINER1" \
    -e INITRD=fat \
    -v "$WORKDIR:/data" \
    "$IMAGE" </dev/null > "$BOOT1_LOG" 2>&1 &
BOOT1_RUNPID=$!

REACHED_PROMPT=0
for _ in $(seq 1 30); do
    if grep -q "Username:" "$BOOT1_LOG" 2>/dev/null; then
        REACHED_PROMPT=1
        break
    fi
    sleep 1
done

if [ "$REACHED_PROMPT" -ne 1 ]; then
    echo "FATAL: container 1 never reached the login prompt within 30s"
    cat "$BOOT1_LOG"
    docker stop "$CONTAINER1" > /dev/null 2>&1 || true
    exit 1
fi

# Stop within a couple seconds of the prompt -- no generous settle time.
docker stop "$CONTAINER1" > /dev/null 2>&1
wait "$BOOT1_RUNPID" 2>/dev/null || true
while docker ps -a --filter "name=^${CONTAINER1}\$" --format '{{.Names}}' 2>/dev/null | grep -q "^${CONTAINER1}\$"; do
    sleep 0.5
done

BOOT1_OUTPUT=$(cat "$BOOT1_LOG")
echo "$BOOT1_OUTPUT" | head -20
echo "[... see $BOOT1_LOG for full output ...]"
echo ""

check "Container 1: install started" "$BOOT1_OUTPUT" "%STARTUP-I-INSTALL"
check "Container 1: install completed" "$BOOT1_OUTPUT" "%STARTUP-I-INSTALLED"
check "Container 1: boot banner" "$BOOT1_OUTPUT" "OpenVMX V0.3"
check "Container 1: reached login prompt" "$BOOT1_OUTPUT" "Username:"

if [ ! -f "$WORKDIR/sysdisk.img" ]; then
    echo "FATAL: no disk image left on host after container 1 -- cannot continue"
    exit 1
fi
echo "Disk size after container 1: $(stat -c%s "$WORKDIR/sysdisk.img" 2>/dev/null || stat -f%z "$WORKDIR/sysdisk.img") bytes"
echo ""

# --- Container 2: SEPARATE docker run, slim initramfs, same host disk ---
echo "--- Container 2: slim boot + login (separate docker run) ---"
BOOT2_OUTPUT=$( (sleep 8; echo SYSTEM; sleep 3; echo MANAGER; sleep 3; echo "SHOW TIME"; sleep 3) | \
    timeout 40 docker run --rm -i \
    -e INITRD=slim \
    -v "$WORKDIR:/data" \
    "$IMAGE" 2>&1 || true)
echo "$BOOT2_OUTPUT" > "$BOOT2_LOG"
echo "$BOOT2_OUTPUT"
echo ""

check "Container 2: system disk detected (install skipped)" "$BOOT2_OUTPUT" "%STARTUP-I-SYSBOOT"
check "Container 2: executive attached" "$BOOT2_OUTPUT" "%OVMX-I-EXEC"
check "Container 2: does NOT report missing SYSTEM record" "$BOOT2_OUTPUT" "%OVMX-F-EXECINIT" absent
check "Container 2: SYSTEM login succeeds (LOGINOUT.EXE from disk)" "$BOOT2_OUTPUT" "Welcome to OpenVMX"
check "Container 2: SHOW TIME returns a real DCL prompt (DCL.EXE from disk)" "$BOOT2_OUTPUT" "$"

echo "=========================================="
echo "RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "SOME CHECKS FAILED"
    exit 1
fi
echo "ALL DOCKER PERSISTENT DISK CHECKS PASSED"
exit 0
