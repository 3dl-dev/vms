#!/bin/bash
# test_release_install.sh - THE R1 release-acceptance gate (vms-37f, parent
# vms-718, docs/design-vms-faithful-install.md §3.3 "Release e2e"): prove
# media -> install -> target -> login ACROSS CONTAINER BOUNDARIES.
#
# WHAT THIS UNIFIES, AND THE DELTA IT ADDS. Three pieces already landed on the
# install epic, each proving part of the story but none the whole:
#   - tests/qemu/test_install_menu.sh (vms-dcf): drives OVMX$INSTALL.COM's full
#     menu over the console -- but every boot is inside ONE container.
#   - tests/qemu/test_install_boot_e2e.sh (vms-96ec): /DESTINATION install then
#     boots THAT target as its own system disk to a login -- but via a raw
#     `PRODUCT INSTALL` DCL command, not the operator menu, and all inside ONE
#     container.
#   - tests/qemu/test_docker_persistent_disk.sh (vms-9b7): crosses a real
#     container boundary (two separate `docker run`s, a host-mounted disk) --
#     but drives PID 1's auto-install, NOT the faithful install menu.
# This gate is the intersection none of them cover: the FULL MENU-DRIVEN install
# in container 1, the installed target surviving into a SEPARATE container 2
# that boots it ALONE and logs in + PRODUCT SHOW PRODUCT + DIRECTORY, and a
# container 3 that re-boots to prove first-boot completion ran once. It is
# where "the delta over the existing pieces is the full menu-driven flow tied
# across the container boundary" (the item spec) actually gets proven.
#
# THE CONTAINER BOUNDARY IS THE POINT (vms-9b7). Container 1 writes the install
# to a HOST-MOUNTED /work/target.img; containers 2 and 3 are independent
# `docker run`s that mount the SAME host file. Nothing but that file crosses
# between them -- the exact boundary where vms-9b7's install once vanished
# because the writes were left buffered. Every qemu drive here is
# cache=writethrough (the standing vms-9b7 fix on the device->host-file leg),
# this script mechanically ASSERTS that (grep on the inner harness), and its
# NEGATIVE CONTROL proves the SHARED host file is what makes the install cross
# the boundary at all: it re-runs the SAME fully-successful menu install (real
# MOUNT, PRODUCT INSTALL, DISMOUNT, cache=writethrough) with
# OVMX_NEGCTL_LOCAL_TARGET=1, which lands it on a CONTAINER-LOCAL disk instead of
# the host-mounted file, and asserts the following verify container -- booting
# the still-blank host target -- then CANNOT boot an installed system. A
# fully-successful install that lands anywhere but the shared file must not
# appear in the next container; that is the boundary property, proven
# DETERMINISTICALLY (no dependence on guest writeback timing, so the gate is not
# flaky).
#
# RED-FIRST (the vms-9b7 standing rule). Two independent ways this gate goes red
# on a tree where the property does not hold, both driven through the real input
# path (never a mock):
#   1. The NEGATIVE CONTROL above -- a real, successful install that does not
#      reach the shared file leaves the next container nothing to boot, so verify
#      fails. It runs every time this gate runs (not a separate opt-in step), so
#      a regression that made the boundary vacuous reddens the gate.
#   2. On the PRE-EPIC tree there is no ovmx-install-media.img, no OVMX$INSTALL
#      menu, no OS kit and no PRODUCT INSTALL /DESTINATION rooted layout at all,
#      so container 1 never reaches the menu / %PCSI-I-DONE and container 2
#      never reaches a SYSTEM login. (Not reproducible locally under the disk
#      budget -- CI on a fresh runner is the authoritative full-boot proof.)
#
# THE SYSTEM PASSWORD -- THE REAL ACCEPTANCE LOOP IS CLOSED. The menu prompts for
# a new SYSTEM password; container 1 sets a KNOWN, non-default password and
# asserts AUTHORIZE persisted it to the TARGET's own SYSUAF (%UAF-I-SAVED, no
# %UAF-E-), and container 2 -- a separate container booting the target alone --
# logs in as SYSTEM with THAT install-set password. Because the install
# overwrites the kit's default SYSTEM password, a successful login with the
# install-set password is positive proof it persisted (the default would no
# longer work). This is possible because the two prerequisite gaps vms-dcf
# originally reported are now FIXED and present in this base: cross-process
# visibility of a parent-MOUNTed device (vms-8b6, #385) and RMS CREATE on a
# mounted vmsfs volume (vms-581, #378). One separate DISPLAY gap remains -- the
# menu's SET TERMINAL/NOECHO does not suppress the password echo -- which does
# not affect the write or the login and is captured as an informational NOTE,
# not scored (see release_install_inner.sh's header).
#
# Runs on the HOST / CI runner (like test_docker_persistent_disk.sh) -- crossing
# the container boundary is the whole point, so it CANNOT run inside a single
# container. Usage (see tests/qemu/run_release_install.sh for the CI/ctest gate):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   OVMX_INITIALIZE_EXE=build/bin/INITIALIZE.EXE \
#     tests/qemu/test_release_install.sh ovmx-boot:latest
#
# Env knobs: OVMX_INITIALIZE_EXE (host-built INITIALIZE.EXE to format the blank
# target -- required), BOOT_TIMEOUT / RUN_TIMEOUT (forwarded to the inner
# harness). Exit 0 = every cross-container assertion passed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${1:-ovmx-boot:latest}"
INNER="$SCRIPT_DIR/release_install_inner.sh"
INIT_EXE="${OVMX_INITIALIZE_EXE:-}"

command -v docker >/dev/null 2>&1 || { echo "FATAL: docker not available"; exit 1; }
docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "FATAL: image '$IMAGE' not found"; exit 1; }
[ -f "$INNER" ] || { echo "FATAL: inner harness $INNER missing"; exit 1; }
[ -n "$INIT_EXE" ] && [ -x "$INIT_EXE" ] || { echo "FATAL: set OVMX_INITIALIZE_EXE to a host-built INITIALIZE.EXE (see header)"; exit 1; }

# The kept vms-9b7 lesson, asserted MECHANICALLY: the inner harness must drive
# every target write through the host file. If a future edit drops
# cache=writethrough, this reddens before a single boot runs.
if grep -qF 'cache=writethrough' "$INNER" && ! grep -qE 'cache=(writeback|unsafe|none)[,"]' "$INNER"; then
    echo "PASS: inner harness uses cache=writethrough on its drives (vms-9b7 lesson kept)"
else
    echo "FAIL: inner harness must use cache=writethrough (and no writeback/unsafe/none) on its drives -- vms-9b7"
    exit 1
fi

WORKDIR=$(mktemp -d)
cleanup() { docker rm -f "$C1" "$C2" "$C3" "$CN1" "$CN2" >/dev/null 2>&1 || true; rm -rf "$WORKDIR"; }
C1="rel-install-c1-$$"; C2="rel-install-c2-$$"; C3="rel-install-c3-$$"
CN1="rel-install-neg1-$$"; CN2="rel-install-neg2-$$"
trap cleanup EXIT

BT="${BOOT_TIMEOUT:-90}"; RT="${RUN_TIMEOUT:-90}"
PASS=0; FAIL=0
scored() { if [ "$1" -eq 0 ]; then echo "PASS: $2"; PASS=$((PASS + 1)); else echo "FAIL: $2"; FAIL=$((FAIL + 1)); fi; }

# format_blank_target <path> -- a fresh, PRESERVE-ready target (label WORK, the
# label the menu's operator types). Same host-format convention as
# run_install_menu.sh / run_install_boot_e2e.sh (the menu's own INITIALIZE
# branch is a filed gap, so PRESERVE needs a pre-formatted volume).
format_blank_target() {
    # --ods2: the atomic flip (vms-208) makes the runtime MOUNT a GENUINE
    # Files-11/ODS-2 volume over the executive ACP. INITIALIZE defaults to the
    # bespoke legacy vmsfs layout, which the ACP correctly refuses to mount
    # (%OVMX-F-MOUNTFAIL); a real install target IS an ODS-2 volume, so format
    # one (vms-37e).
    "$INIT_EXE" --ods2 "$1" WORK 64 >/dev/null 2>&1 || { echo "FATAL: INITIALIZE of $1 failed"; exit 1; }
}

run_container() {  # run_container <name> <mode> <target-img> [extra -e...]
    local name="$1" mode="$2" target="$3"; shift 3
    docker run --rm --name "$name" \
        -e "BOOT_TIMEOUT=$BT" -e "RUN_TIMEOUT=$RT" "$@" \
        -v "$target":/work/target.img \
        -v "$INNER":/inner.sh:ro \
        --entrypoint bash "$IMAGE" /inner.sh "$mode"
}

echo "=== vms-37f R1 release-acceptance gate: media -> menu install -> SEPARATE container -> login -> PRODUCT SHOW ==="
echo "image=$IMAGE  host workdir=$WORKDIR"
echo ""

# ---------------------------------------------------------------------
# CONTAINER 1 -- drive the full install menu onto a blank host target.
# ---------------------------------------------------------------------
TARGET="$WORKDIR/target.img"
format_blank_target "$TARGET"
BLANK_SIZE=$(stat -c%s "$TARGET" 2>/dev/null || stat -f%z "$TARGET")

echo "--- Container 1: menu-driven install ($C1) ---"
C1_LOG="$WORKDIR/c1.log"
run_container "$C1" install "$TARGET" >"$C1_LOG" 2>&1; C1_RC=$?
sed 's/^/    c1| /' "$C1_LOG"
scored "$C1_RC" "container 1 (menu install) exited 0"
grep -qF 'OVMX$INSTALL menu' "$C1_LOG"; scored $? "container 1 booted the media straight into the install menu"
grep -qF '%PCSI-I-DONE' "$C1_LOG";     scored $? "container 1 ran a real PRODUCT INSTALL to %PCSI-I-DONE"
grep -qF 'DISMOUNTs the target' "$C1_LOG"; scored $? "container 1 DISMOUNTed the target (guest flush)"
grep -qF 'persisted the new SYSTEM password to the TARGET' "$C1_LOG"; scored $? "container 1: AUTHORIZE persisted the install-set SYSTEM password to the target SYSUAF (%UAF-I-SAVED)"
echo ""

# ---------------------------------------------------------------------
# CONTAINER 2 -- SEPARATE docker run: boot the target ALONE, log in,
# PRODUCT SHOW PRODUCT, DIRECTORY SYS$SYSTEM:.  (The container boundary.)
# ---------------------------------------------------------------------
echo "--- Container 2: boot the installed target alone + login + PRODUCT SHOW (separate docker run, $C2) ---"
C2_LOG="$WORKDIR/c2.log"
run_container "$C2" verify "$TARGET" >"$C2_LOG" 2>&1; C2_RC=$?
sed 's/^/    c2| /' "$C2_LOG"
scored "$C2_RC" "container 2 (boot installed target + verify) exited 0"
grep -qF 'reaches login' "$C2_LOG";        scored $? "container 2: the installed target booted ALONE and reached login across the boundary"
grep -qF 'SYSTEM logs in with the INSTALL-SET password' "$C2_LOG"; scored $? "container 2: SYSTEM logged in with the INSTALL-SET password (not the kit default) across the boundary"
grep -qF 'lists the OS kit as Installed' "$C2_LOG"; scored $? "container 2: PRODUCT SHOW PRODUCT lists the OS kit"
grep -qF 'lists DCL.EXE' "$C2_LOG";        scored $? "container 2: DIRECTORY SYS\$SYSTEM: shows DCL.EXE from the target"
echo ""

# ---------------------------------------------------------------------
# CONTAINER 3 -- SEPARATE docker run: re-boot the SAME target to prove
# first-boot completion ran once (idempotent; no self-install, no menu).
# ---------------------------------------------------------------------
echo "--- Container 3: re-boot the target (prove first-boot ran once, $C3) ---"
C3_LOG="$WORKDIR/c3.log"
run_container "$C3" verify "$TARGET" >"$C3_LOG" 2>&1; C3_RC=$?
sed 's/^/    c3| /' "$C3_LOG"
scored "$C3_RC" "container 3 (re-boot) exited 0"
grep -qF 'ran no self-install phase' "$C3_LOG"; scored $? "container 3: re-boot ran no self-install phase (first-boot completion is once-only)"
grep -qF 'SYSTEM logs in' "$C3_LOG";            scored $? "container 3: SYSTEM still logs in on the second boot"
echo ""

# ---------------------------------------------------------------------
# NEGATIVE CONTROL -- prove the shared host file is load-bearing (the vms-9b7
# boundary). Run the SAME fully-successful menu install, but onto a
# CONTAINER-LOCAL disk (OVMX_NEGCTL_LOCAL_TARGET=1) so it never reaches the host
# file; then verify against the (still-blank) host target must FAIL. If it
# PASSED, the "boundary" would be vacuous and the gate would not be testing what
# it claims. Deterministic: no guest-writeback-timing dependence.
# ---------------------------------------------------------------------
echo "--- NEGATIVE CONTROL: successful install to a container-local disk, then verify the host target must FAIL (vms-9b7 boundary) ---"
NEG_TARGET="$WORKDIR/neg-target.img"
format_blank_target "$NEG_TARGET"
CN1_LOG="$WORKDIR/neg1.log"; CN2_LOG="$WORKDIR/neg2.log"
run_container "$CN1" install "$NEG_TARGET" -e OVMX_NEGCTL_LOCAL_TARGET=1 >"$CN1_LOG" 2>&1; CN1_RC=$?
grep -qF 'installing onto a CONTAINER-LOCAL disk' "$CN1_LOG"; scored $? "negctl install ran against a container-local disk, not the shared host file"
scored "$CN1_RC" "negctl install itself SUCCEEDED (proving the fault is the boundary, not a broken install)"
run_container "$CN2" verify "$NEG_TARGET" >"$CN2_LOG" 2>&1; NEG_RC=$?
sed 's/^/    neg| /' "$CN2_LOG"
if [ "$NEG_RC" -ne 0 ]; then
    scored 0 "negative control: the un-shared install does NOT appear in the next container (boundary is load-bearing)"
else
    scored 1 "negative control PASSED verify against a target that never received the install -- the gate is NOT proving the boundary"
fi
echo ""

echo "=========================================="
echo "RESULTS: $PASS passed, $FAIL failed"
echo "=========================================="
if [ "$FAIL" -gt 0 ]; then
    echo "RELEASE-INSTALL E2E GATE FAILED"
    exit 1
fi
echo "ALL RELEASE-INSTALL E2E CHECKS PASSED (media -> menu install -> separate container -> login -> PRODUCT SHOW)"
exit 0
