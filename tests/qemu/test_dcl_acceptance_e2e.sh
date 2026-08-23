#!/bin/bash
# test_dcl_acceptance_e2e.sh - boot-and-RUN-COMMANDS DCL/SHOW acceptance gate.
#
# THE HOLE THIS CLOSES (the systemic fix the operator demanded):
#   V0.5-2 shipped with SHOW USERS empty, SHOW DEVICE not reporting Mounted,
#   SHOW QUOTA fabricating a UIC, and WRITE F$xxx() printing the literal --
#   NONE of it caught, because tests/qemu/test_release_acceptance_e2e.sh boots,
#   logs in, and then checks only `PRODUCT SHOW PRODUCT`'s version string. It
#   never runs the commands a user actually types. A release can go out with
#   every basic SHOW command broken and still pass the acceptance gate.
#
#   This gate boots the REAL runtime (the pre-mastered ODS-2 ovmx-distrib.img,
#   same disk test_distrib_boot.sh boots), logs in SYSTEM/MANAGER, runs a
#   battery of the most basic DCL/SHOW commands a user types on first login,
#   and ASSERTS THE OUTPUT IS VMS-FAITHFUL -- grounded in real OpenVMS SHOW
#   output (project Rule 1), not merely "non-empty".
#
# EACH ASSERTION CARRIES A NEGATIVE CONTROL (negctl below): every command
# segment is run through a search that must FIND a token known to be present
# (the echoed command) and REJECT a random sentinel known to be absent. That
# proves the search mechanism over THAT segment can actually report both
# present and absent -- so a green "the bug marker is absent" line is real
# evidence, not a vacuous pass over an empty/unsearchable segment.
#
# THIS GATE IS EXPECTED TO BE RED until the in-flight cluster of fixes lands.
# Each assertion names the bug it guards:
#   - SHOW USERS empty / "0 users"          -> vms-01f / vms-72c
#   - SHOW DEVICE not "Mounted" (bare Online) -> vms-e6f
#   - SHOW DEVICES (plural) %DCL-*-IVKEYW    -> vms-9344 (surface)
#   - WRITE F$GETSYI() prints the literal    -> vms-65f
#   - SHOW QUOTA fabricated "[200,1]"        -> vms-73c4
# A red line here means that command is still broken; do NOT weaken the
# assertion to make it pass. It goes green only when the runtime is fixed.
#
# Usage (run INSIDE the ovmx-boot image, which supplies qemu-system-* AND the
# pre-mastered /boot/{vmlinuz,initramfs-ovmx-slim.cpio.gz,ovmx-distrib.img}):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm \
#       -e EXPECTED_BOOT_BANNER='OpenVMX V0.5-2' \
#       -e EXPECTED_COMPAT_VERSION='V9.2-3' \
#       -v $PWD/tests/qemu/test_dcl_acceptance_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
# (tests/qemu/run_dcl_acceptance_e2e.sh derives the two EXPECTED_* values from
# src/libvms/include/ovmx_identity.h -- the INV-1 single source -- and invokes
# this the same way.)
#
# Env knobs:
#   EXPECTED_BOOT_BANNER    the brand+version the boot must print (e.g.
#                           "OpenVMX V0.5-2"). Default derived from a copy of
#                           ovmx_identity.h if present, else "OpenVMX".
#   EXPECTED_COMPAT_VERSION the VMS-compat version F$GETSYI("VERSION") must
#                           report (e.g. "V9.2-3"). Default "V9.2-3".
#   BOOT_TIMEOUT            seconds to reach the login prompt (default 180).
#   CMD_TIMEOUT            seconds for a single command to return the prompt
#                           (default 30).
#
# Exit 0 = every basic command produced VMS-faithful output. Exit 1 = at least
# one command is broken (see the printed transcript). RED-UNTIL-FIXED is the
# CORRECT state of this gate today.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
CMD_TIMEOUT="${CMD_TIMEOUT:-30}"
EXPECTED_BOOT_BANNER="${EXPECTED_BOOT_BANNER:-OpenVMX}"
EXPECTED_COMPAT_VERSION="${EXPECTED_COMPAT_VERSION:-V9.2-3}"
# Ground-source constants, from distro/Dockerfile.bootable's master step and
# src/kernel/vms_internal.h (VMS_SYSTEM_UIC == [1,4]):
VOLUME_LABEL="OVMXSYS"    # vmsfs_master --ods2 ... OVMXSYS (Dockerfile.bootable)
SYSTEM_UIC='[1,4]'        # SYSTEM's UIC; the fabricated bug value is [200,1]

DISTRIB_IMG=/boot/ovmx-distrib.img
KERNEL=/boot/vmlinuz
SLIM_INITRD=/boot/initramfs-ovmx-slim.cpio.gz
ARCH=$(uname -m)

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$SLIM_INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this INSIDE the ovmx-boot image (distro/Dockerfile.bootable), which bakes in the pre-mastered distribution disk"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# --- assertion helpers (all operate on a captured console SEGMENT) ----------
# must_have: a VMS-faithful substring MUST be present.
must_have() { local seg="$1" pat="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiF -- "$pat"; then ok "$desc"
    else bad "$desc [expected substring: '$pat']"; fi; }
# must_match: a VMS-faithful regex MUST match.
must_match() { local seg="$1" re="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiE -- "$re"; then ok "$desc"
    else bad "$desc [expected match: /$re/]"; fi; }
# must_not_have: a broken/fabricated bug marker MUST be absent (bug guard).
must_not_have() { local seg="$1" pat="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiF -- "$pat"; then bad "$desc [found bug marker: '$pat']"
    else ok "$desc"; fi; }
# negctl: PROVES the search over THIS segment is not vacuous -- it must FIND a
# token known present (the echoed command) and REJECT a random sentinel known
# absent. If either half is wrong the segment is empty/unsearchable and every
# must_*/must_not_have above it cannot be trusted.
negctl() { local seg="$1" present="$2" desc="$3"
    local sentinel="ZZ_NEGCTRL_${$}_${RANDOM}${RANDOM}_ZZ"
    local a=1 b=1
    printf '%s\n' "$seg" | grep -qiF -- "$present" && a=0
    printf '%s\n' "$seg" | grep -qiF -- "$sentinel" && b=0
    if [ "$a" -eq 0 ] && [ "$b" -eq 1 ]; then
        ok "NEGCTL $desc: search over the real segment finds a present token ('$present') and rejects a bogus sentinel -- the assertions above can genuinely go red"
    else
        bad "NEGCTL $desc: search is vacuous (present-token found=$([ $a -eq 0 ] && echo yes || echo NO), sentinel rejected=$([ $b -eq 1 ] && echo yes || echo NO)) -- assertions above cannot be trusted"
    fi; }

echo "=== OVMX DCL/SHOW acceptance e2e: boot the runtime, log in, RUN THE COMMANDS A USER TYPES, assert VMS-faithful output ==="
echo "arch=$ARCH qemu=$QEMU"
echo "expected boot banner   : $EXPECTED_BOOT_BANNER"
echo "expected compat version: $EXPECTED_COMPAT_VERSION (F\$GETSYI VERSION)"
echo "volume label           : $VOLUME_LABEL ; SYSTEM UIC: $SYSTEM_UIC"
echo ""

DISK=/tmp/dcl-acceptance-e2e.img
LOG=/tmp/dcl-acceptance-e2e-console.log
FIFO=/tmp/dcl-acceptance-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

# Whole-VM hard cap: boot + a settle + ~13 commands, each bounded by CMD_TIMEOUT.
WALL=$((BOOT_TIMEOUT + CMD_TIMEOUT * 16 + 120))

cleanup() { exec 4>&- 2>/dev/null || true; [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; rm -f "$FIFO"; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$WALL" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$SLIM_INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern limit-seconds since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qaF -- "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}
# run_cmd CMD  ->  echoes CMD, waits for the returned DCL prompt, sets SEG to
# everything the command produced (its echo + output + trailing prompt).
SEG=""
run_cmd() {
    local cmd="$1" off
    off=$(wc -c <"$LOG")
    send "$cmd"
    # Wait for the returned "$ " prompt after the command completes. Bounded;
    # if it never returns, SEG still captures whatever appeared (the caller's
    # assertions then go red honestly rather than the script hanging).
    wait_for '$ ' "$CMD_TIMEOUT" "$off"
    sleep 1   # settle: let the last line flush after the prompt is seen
    SEG=$(segment_since "$off")
}

# --- Boot the real runtime to the login prompt ------------------------------
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
# vms-2213: LOGINOUT on OPA0: waits for the operator's RETURN; feed a CR each
# second (as a real operator would) until Username: appears.
w=0
until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
    send ''; sleep 1; w=$((w + 1))
done
if wait_for 'Username:' 5; then
    ok "runtime boots to the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- ASSERTION 0: the boot banner names the product+version -----------------
# Grounded in ovmx_identity.h (OVMX_PRODUCT_BANNER, INV-1 single source).
BOOT_SEG=$(segment_since 0)
must_have "$BOOT_SEG" "$EXPECTED_BOOT_BANNER" "BOOT BANNER: boot prints '$EXPECTED_BOOT_BANNER' (ovmx_identity.h OVMX_PRODUCT_VERSION)"
# negctl present-token is the brand, which is printed regardless of the version
# (never the expected banner itself -- that is exactly what the assertion tests).
negctl   "$BOOT_SEG" 'OpenVMX' "boot banner"

# --- Log in as SYSTEM -------------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in (LOGINOUT.EXE -> DCL.EXE off the mounted ODS-2 disk)"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$ ' 20 "$LOGIN_OFF"

# ===========================================================================
# THE BATTERY: the basic commands a user types on first login. Each block
# asserts VMS-faithful output, guards the specific shipped bug, and carries a
# negative control.
# ===========================================================================

# --- SHOW TIME (sane clock) -------------------------------------------------
run_cmd 'SHOW TIME'
CURYEAR=$(date +%Y)
must_have  "$SEG" "$CURYEAR" "SHOW TIME: reports the real year ($CURYEAR)"
must_match "$SEG" '[0-9]{2}:[0-9]{2}:[0-9]{2}' "SHOW TIME: reports an HH:MM:SS time"
negctl     "$SEG" 'SHOW TIME' "SHOW TIME"

# --- SHOW USERS (vms-01f / vms-72c: shipped EMPTY / "0 users") --------------
run_cmd 'SHOW USERS'
must_have     "$SEG" 'SYSTEM' "SHOW USERS [vms-01f/72c]: lists the SYSTEM interactive process (NOT empty)"
# Accept the real-VMS wording ('interactive users = N') or OVMX's current
# header ('number of users = N') -- either must show a nonzero count.
must_match    "$SEG" '(interactive users = [1-9]|number of users = [1-9])' "SHOW USERS [vms-01f/72c]: reports >= 1 user (real VMS: 'Total number of interactive users = 1')"
must_not_have "$SEG" 'users = 0' "SHOW USERS [vms-01f/72c]: does NOT report 0 users (the shipped-empty bug)"
must_not_have "$SEG" 'No interactive users' "SHOW USERS [vms-01f/72c]: does NOT print 'No interactive users'"
negctl        "$SEG" 'SHOW USERS' "SHOW USERS"

# --- SHOW DEVICE DKA0: (vms-e6f: shipped bare "Online", no Mounted/label) ---
run_cmd 'SHOW DEVICE DKA0:'
# The DKA0: DATA line, not the echoed command 'SHOW DEVICE DKA0:' (which also
# contains 'DKA0'): exclude any line naming the SHOW verb.
DKA0_LINE=$(printf '%s\n' "$SEG" | grep -i 'DKA0:' | grep -iv 'SHOW ' | head -1)
must_have  "$SEG" 'DKA0' "SHOW DEVICE DKA0: [vms-e6f]: names the device DKA0:"
must_have  "$DKA0_LINE" 'Mounted' "SHOW DEVICE DKA0: [vms-e6f]: device status is 'Mounted' (NOT bare 'Online')"
must_have  "$DKA0_LINE" "$VOLUME_LABEL" "SHOW DEVICE DKA0: [vms-e6f]: shows the volume label '$VOLUME_LABEL'"
must_match "$DKA0_LINE" '[1-9][0-9]{3,}' "SHOW DEVICE DKA0: [vms-e6f]: shows a nonzero free-block count (128MB ODS-2 volume has thousands free)"
must_not_have "$DKA0_LINE" 'Online' "SHOW DEVICE DKA0: [vms-e6f]: DKA0: status is not the bare 'Online' bug"
negctl     "$SEG" 'SHOW DEVICE' "SHOW DEVICE DKA0:"

# --- SHOW DEVICES (plural accepted) (vms-9344 surface) ----------------------
run_cmd 'SHOW DEVICES'
must_have     "$SEG" 'DKA0' "SHOW DEVICES [vms-9344]: plural form is accepted and lists devices"
must_not_have "$SEG" 'IVKEYW' "SHOW DEVICES [vms-9344]: not rejected with %DCL-*-IVKEYW"
must_not_have "$SEG" 'IVVERB' "SHOW DEVICES [vms-9344]: not rejected with %DCL-*-IVVERB"
negctl        "$SEG" 'SHOW DEVICES' "SHOW DEVICES"

# --- WRITE SYS$OUTPUT F$GETSYI("VERSION") (vms-65f: prints the literal) -----
run_cmd 'WRITE SYS$OUTPUT F$GETSYI("VERSION")'
must_have     "$SEG" "$EXPECTED_COMPAT_VERSION" "WRITE F\$GETSYI [vms-65f]: emits the real VMS version '$EXPECTED_COMPAT_VERSION'"
# The stripped literal 'F$GETSYIVERSION' can only appear if the lexical was
# printed verbatim -- it never appears in the echoed command (which has the
# parens+quotes), so this is a clean bug guard.
must_not_have "$SEG" 'F$GETSYIVERSION' "WRITE F\$GETSYI [vms-65f]: does NOT print the literal 'F\$GETSYIVERSION' (the shipped bug)"
negctl        "$SEG" 'F$GETSYI' "WRITE F\$GETSYI"

# --- SHOW QUOTA (vms-73c4: fabricated "[200,1]") ----------------------------
run_cmd 'SHOW QUOTA'
# VMS-faithful: either the real current UIC ([1,4] for SYSTEM) OR an honest
# %SYSTEM-F-NODISKQUOTA when no quota is enabled -- NOT a fabricated UIC.
# Accept [1,4] or the zero-padded [001,004] form OVMX prints elsewhere.
must_match    "$SEG" '(\[0*1,0*4\]|NODISKQUOTA)' "SHOW QUOTA [vms-73c4]: shows the real SYSTEM UIC [1,4] OR an honest %SYSTEM-F-NODISKQUOTA"
must_not_have "$SEG" '[200,1]' "SHOW QUOTA [vms-73c4]: does NOT print the fabricated UIC '[200,1]' (the shipped bug)"
negctl        "$SEG" 'SHOW QUOTA' "SHOW QUOTA"

# --- SHOW SYSTEM (lists the real processes) ---------------------------------
run_cmd 'SHOW SYSTEM'
must_have  "$SEG" 'SYSTEM' "SHOW SYSTEM: lists the SYSTEM process"
must_match "$SEG" '[0-9A-Fa-f]{8}' "SHOW SYSTEM: shows 8-hex-digit VMS PIDs"
negctl     "$SEG" 'SHOW SYSTEM' "SHOW SYSTEM"

# --- SHOW PROCESS (current process works) -----------------------------------
run_cmd 'SHOW PROCESS'
must_have "$SEG" 'SYSTEM' "SHOW PROCESS: names the current user SYSTEM"
must_not_have "$SEG" 'IVKEYW' "SHOW PROCESS: not rejected as an invalid keyword"
negctl    "$SEG" 'SHOW PROCESS' "SHOW PROCESS"

# --- SHOW DEFAULT (VMS filespec, no Unix path) ------------------------------
run_cmd 'SHOW DEFAULT'
must_match    "$SEG" '[A-Z$_]+:\[[A-Z0-9._]+\]' "SHOW DEFAULT: prints a VMS device:[directory] filespec"
must_not_have "$SEG" '/tmp' "SHOW DEFAULT: no Unix path leaks into the default directory"
negctl        "$SEG" 'SHOW DEFAULT' "SHOW DEFAULT"

# --- bare DIRECTORY at login lists files ------------------------------------
run_cmd 'DIRECTORY'
must_match "$SEG" 'Total of [1-9]' "DIRECTORY: a bare DIRECTORY at login lists >= 1 file"
must_match "$SEG" 'Directory ' "DIRECTORY: prints a VMS 'Directory <spec>' header"
must_not_have "$SEG" '/tmp' "DIRECTORY: no Unix path leaks into the listing"
negctl     "$SEG" 'DIRECTORY' "DIRECTORY"

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL DCL/SHOW ACCEPTANCE CHECKS PASSED"
    exit 0
fi
echo ""
echo "NOTE: red lines above name the bug each guards. This gate is EXPECTED to"
echo "be RED until the in-flight command fixes land -- it must NOT be weakened"
echo "to go green. It goes green only when the runtime produces VMS-faithful"
echo "output for these basic commands."
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
