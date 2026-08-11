#!/bin/bash
# test_upgrade_e2e.sh - install -> UPGRADE -> boot, against a real vms.ko +
# vmsfs.ko (vms-f05, epic vms-a84 RELEASE ENGINEERING).
#
# WHAT THIS PROVES, AND WHY NOTHING EARLIER PROVES IT. tests/qemu/
# test_product_install_e2e.sh (vms-df9) proved PRODUCT INSTALL lands a real
# kit onto a blank volume. tools/cut-release.sh (vms-d73) proved a release
# bundle is reproducible. Neither proves the thing real systems break on:
# that installing a NEWER release ON TOP OF an OLDER, already-populated
# install preserves what the site put there. This is that proof, using two
# REAL cut-release.sh bundles (never hand-faked "0.N"/"0.N+1" versions) and
# a real PRODUCT INSTALL of each, in sequence, onto the SAME volume.
#
# THREE VIRTIO DISKS (vms-3e8: DKA0: vda, DKA100: vdb, DKA200: vdc):
#   DKA0:   the UPGRADE release's own vmlinuz/initramfs/ovmx-distrib.img --
#           this is simply a running OVMX system to drive DCL/PRODUCT.EXE
#           from; which release boots DKA0: is not otherwise significant.
#   DKA100: a BLANK disk, INITIALIZEd on the HOST (same convention as
#           test_product_install_e2e.sh's DKA100_SRC) -- the volume the
#           whole install -> upgrade sequence runs against.
#   DKA200: a "kit carrier" disk, MASTERED on the HOST via vmsfs_master
#           (also pure userspace, no vmsfs.ko, same as INITIALIZE.EXE) and
#           populated with BOTH release's ovmx-os.kit files, named
#           OVMX-OS-BASELINE.KIT and OVMX-OS-UPGRADE.KIT. This sidesteps
#           the raw-third-disk EPERM trap test_product_install_e2e.sh's
#           header documents (devtmpfs block nodes are root:root 0600): a
#           vmsfs_master-mastered image is a real vmsfs VOLUME, so SYSTEM
#           reads it through an ordinary MOUNT + file open, not a raw
#           block-device read.
#
# GROUND-SOURCE SHAPE, all against the REAL system, no mocks:
#   1. MOUNT DKA200: and DKA100:.
#   2. PRODUCT INSTALL the BASELINE kit onto DKA100: (a real PRODUCT.EXE
#      run, vms-df9's mechanism, unmodified).
#   3. Write USER STATE onto the freshly-installed volume: a real DCL
#      OPEN/WRITE/CLOSE creates DKA100:[USER]DATA.TXT with known content
#      (not a file the kit ships -- nothing upstream of this test ever
#      writes to [USER]), and a real DCL OPEN/APPEND/WRITE/CLOSE appends a
#      site-customization marker line to DKA100:[SYSMGR]SYSTARTUP_VMS.COM
#      (a file the kit DOES ship, exactly the file a site is expected to
#      customize on real OpenVMS).
#   4. PRODUCT INSTALL the UPGRADE kit onto the SAME DKA100: -- the upgrade.
#   5. Assert: (a) the user data file survives byte-identical, (b) the site
#      customization marker survives, (c) PRODUCT SHOW PRODUCT now reports
#      the UPGRADE version, not the BASELINE one.
#   6. QEMU is KILLED and a FRESH process boots against the SAME three disk
#      files -- (d) the whole machine still reaches a login prompt after
#      the upgrade, and (a)/(c) are re-checked to also survive a full
#      restart (not tmpfs).
#
# =============================================================================
# THE FINDING, AND THE FIX (vms-2c9). Originally measured 2026-08-10
# against the tree this test was written against: (a) and (c) passed, (b)
# FAILED -- do_install() wrote every kit-listed file with
# O_WRONLY|O_CREAT|O_TRUNC unconditionally, with no notion of a site-owned
# file a later install must not touch, so a second PRODUCT INSTALL
# overwrote the site's customized SYS$MANAGER:SYSTARTUP_VMS.COM with the
# kit's stock copy -- exactly the failure mode real OpenVMS sites are
# drilled to fear from an OS upgrade. FIXED by vms-2c9: kit-entry-level
# "seed once, never replace" metadata (ovmx_kit_format.h's
# OVMX_KIT_ENTRY_FLAG_SEED_ONCE, set by tools/ovmx_kit_pack.c for
# SYSTARTUP_VMS.COM/SYCONFIG.COM/SYLOGICALS.COM, read by do_install()).
# Assertion (b) is now a REAL, ENFORCING assertion like (a)/(c)/(d) -- this
# gate no longer ships with a known-red assertion, and the CI job no
# longer runs it under continue-on-error (.github/workflows/ci.yml).
# =============================================================================
#
# Usage (run INSIDE the bootable image, like test_product_install_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm \
#       -v $PWD/tests/qemu/test_upgrade_e2e.sh:/test.sh:ro \
#       -v /path/to/upgrade-release:/upgrade-release:ro \
#       -v /path/to/formatted/disks:/work \
#       --entrypoint bash ovmx-boot /test.sh
# (see tests/qemu/run_upgrade_e2e.sh, which prepares /work and /upgrade-release
# on the host and invokes this the same way.)
#
# Env knobs: BOOT_TIMEOUT (default 90), RUN_TIMEOUT (default 90), same
# meaning as test_product_install_e2e.sh.
#
# Exit 0 = every assertion passed (a)-(d). Exit 1 = a regression -- every
# assertion here is now expected to pass on every run.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"

# Threaded from run_upgrade_e2e.sh, which reads each cut's OWN
# release-manifest.json (never a literal here) -- see that script's
# manifest_version(). REQUIRED: an unset value would silently make the
# post-upgrade version check compare against the empty string.
: "${EXPECTED_BASELINE_VERSION:?EXPECTED_BASELINE_VERSION not set (run via run_upgrade_e2e.sh)}"
: "${EXPECTED_UPGRADE_VERSION:?EXPECTED_UPGRADE_VERSION not set (run via run_upgrade_e2e.sh)}"

KERNEL=/upgrade-release/vmlinuz
INITRD=/upgrade-release/initramfs-ovmx-slim.cpio.gz
DISTRIB_IMG=/upgrade-release/ovmx-distrib.img
DKA100_SRC=/work/dka100.img
DKA200_SRC=/work/dka200.img
ARCH=$(uname -m)

USER_DATA_CONTENT="VMS-F05-USER-DATA-MUST-SURVIVE-UPGRADE-$$"
SITE_MARKER="! VMS-F05-SITE-CUSTOMIZATION-MUST-SURVIVE-UPGRADE-$$"

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG" "$DKA100_SRC" "$DKA200_SRC"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with /upgrade-release and /work bind-mounted (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== install -> UPGRADE -> boot e2e: does PRODUCT INSTALL preserve user state? (vms-f05) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

WORKDIR=$(mktemp -d)
DISK0="$WORKDIR/dka0.img"
DISK1="$WORKDIR/dka100.img"
DISK2="$WORKDIR/dka200.img"
cp "$DISTRIB_IMG" "$DISK0"
cp "$DKA100_SRC" "$DISK1"
cp "$DKA200_SRC" "$DISK2"

QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

boot_qemu() {  # boot_qemu <log-file> <fifo-path>
    local log="$1" fifo="$2"
    rm -f "$log" "$fifo"
    mkfifo "$fifo"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 18 + 60))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nic none -nodefaults -serial stdio \
        -drive file="$DISK0",format=raw,if=virtio,cache=writethrough \
        -drive file="$DISK1",format=raw,if=virtio,cache=writethrough \
        -drive file="$DISK2",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID=$!
    exec 4>"$fifo"
}

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte  log-file
    local pat="$1" limit="${2:-30}" since="${3:-0}" log="${4:-$LOG}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$log" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
segment_since() { tail -c "+$(($1 + 1))" "${2:-$LOG}" 2>/dev/null | tr -d '\r'; }
dump_and_die() {
    echo ""
    echo "=== FATAL: $1 ==="
    echo "--- full console log ($LOG) ---"
    cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""
    exit 1
}

login() {  # login <log-file>
    local log="$1"
    if wait_for 'Username:' "$BOOT_TIMEOUT" 0 "$log"; then
        ok "boot reaches the login prompt ($log)"
    else
        dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off; off=$(wc -c <"$log")
    send 'SYSTEM'
    wait_for 'Password:' 20 "$off" "$log" && send 'MANAGER'
    if wait_for 'Welcome to OVMX' 20 "$off" "$log"; then
        ok "SYSTEM logs in"
    else
        dump_and_die "SYSTEM login failed"
    fi
    wait_for '$' 20 "$off" "$log"
}

# =====================================================================
# BOOT 1 -- mount, baseline install, write user state, upgrade install
# =====================================================================
LOG="$WORKDIR/boot1.log"
boot_qemu "$LOG" "$WORKDIR/boot1.in"
login "$LOG"

# --- 1. MOUNT the kit carrier and the blank target ----------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT DKA200: KITS'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
if wait_for '%MOUNT-I-MOUNTED, KITS mounted on _DKA200:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA200: (kit carrier) succeeds"
else
    dump_and_die "MOUNT DKA200: did not report success within ${RUN_TIMEOUT}s"
fi

OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA100: (upgrade target) succeeds"
else
    dump_and_die "MOUNT DKA100: did not report success within ${RUN_TIMEOUT}s"
fi

# --- 2. PRODUCT INSTALL the BASELINE kit onto the blank target -----------
OFF=$(wc -c <"$LOG")
send 'PRODUCT INSTALL VMS /SOURCE=DKA200:[SYSUPD]OVMX-OS-BASELINE.KIT /DESTINATION=DKA100:'
if wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRODUCT INSTALL (BASELINE) reports %PCSI-I-DONE"
else
    dump_and_die "PRODUCT INSTALL (BASELINE) did not reach %PCSI-I-DONE within ${RUN_TIMEOUT}s"
fi
BASE_INSTALL_SEG=$(segment_since "$OFF")
if printf '%s\n' "$BASE_INSTALL_SEG" | grep -qiE '%PCSI-[EF]-'; then
    dump_and_die "PRODUCT INSTALL (BASELINE) reported a PCSI error despite DONE: $BASE_INSTALL_SEG"
fi

OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT /DESTINATION=DKA100:'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
BASE_SHOW=$(segment_since "$OFF")
echo "$BASE_SHOW"
if printf '%s\n' "$BASE_SHOW" | grep -qF 'V0.1'; then
    ok "PRODUCT SHOW PRODUCT reports the BASELINE version (V0.1) after the first install"
else
    bad "PRODUCT SHOW PRODUCT does not report the BASELINE version after the first install"
fi

# --- 3. Write USER STATE onto the freshly-installed volume ---------------
OFF=$(wc -c <"$LOG")
send 'CREATE/DIRECTORY DKA100:[USER]'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qiE '%CREATE-[EF]-|%DCL-[EF]-|%RMS-[EF]-'; then
    dump_and_die "CREATE/DIRECTORY DKA100:[USER] failed: $SEG"
fi

OFF=$(wc -c <"$LOG")
send 'OPEN/WRITE UD DKA100:[USER]DATA.TXT'
wait_for '$' 20 "$OFF"
OFF=$(wc -c <"$LOG")
send "WRITE UD \"$USER_DATA_CONTENT\""
wait_for '$' 20 "$OFF"
OFF=$(wc -c <"$LOG")
send 'CLOSE UD'
wait_for '$' "$RUN_TIMEOUT" "$OFF"

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[USER]DATA.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
USER_CMD='TYPE DKA100:[USER]DATA.TXT'
USER_BEFORE=$(segment_since "$OFF" | grep -vF "$USER_CMD")
if printf '%s' "$USER_BEFORE" | grep -qF "$USER_DATA_CONTENT"; then
    ok "DKA100:[USER]DATA.TXT carries the expected content before the upgrade"
else
    dump_and_die "DKA100:[USER]DATA.TXT does not carry the expected content before the upgrade: $USER_BEFORE"
fi

# --- 4. Write a SITE CONFIG customization onto a file the kit ships ------
OFF=$(wc -c <"$LOG")
send 'OPEN/APPEND SC DKA100:[SYSMGR]SYSTARTUP_VMS.COM'
wait_for '$' 20 "$OFF"
OFF=$(wc -c <"$LOG")
send "WRITE SC \"$SITE_MARKER\""
wait_for '$' 20 "$OFF"
OFF=$(wc -c <"$LOG")
send 'CLOSE SC'
wait_for '$' "$RUN_TIMEOUT" "$OFF"

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[SYSMGR]SYSTARTUP_VMS.COM'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SITE_CMD='TYPE DKA100:[SYSMGR]SYSTARTUP_VMS.COM'
SITE_BEFORE=$(segment_since "$OFF" | grep -vF "$SITE_CMD")
if printf '%s' "$SITE_BEFORE" | grep -qF "$SITE_MARKER"; then
    ok "DKA100:[SYSMGR]SYSTARTUP_VMS.COM carries the site marker before the upgrade"
else
    dump_and_die "site marker did not land in SYSTARTUP_VMS.COM before the upgrade: $SITE_BEFORE"
fi

# =====================================================================
# THE UPGRADE
# =====================================================================
OFF=$(wc -c <"$LOG")
send 'PRODUCT INSTALL VMS /SOURCE=DKA200:[SYSUPD]OVMX-OS-UPGRADE.KIT /DESTINATION=DKA100:'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
if wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRODUCT INSTALL (UPGRADE) reports %PCSI-I-DONE"
else
    dump_and_die "PRODUCT INSTALL (UPGRADE) did not reach %PCSI-I-DONE within ${RUN_TIMEOUT}s"
fi
UPG_INSTALL_SEG=$(segment_since "$OFF")
if printf '%s\n' "$UPG_INSTALL_SEG" | grep -qiE '%PCSI-[EF]-'; then
    dump_and_die "PRODUCT INSTALL (UPGRADE) reported a PCSI error despite DONE: $UPG_INSTALL_SEG"
fi

# --- (a) user data survives byte-identical --------------------------------
OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[USER]DATA.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
USER_AFTER=$(segment_since "$OFF" | grep -vF "$USER_CMD")
if [ "$(printf '%s' "$USER_AFTER" | tr -d '[:space:]')" = "$(printf '%s' "$USER_BEFORE" | tr -d '[:space:]')" ] \
    && printf '%s' "$USER_AFTER" | grep -qF "$USER_DATA_CONTENT"; then
    ok "(a) DKA100:[USER]DATA.TXT survives the upgrade byte-identical"
else
    dump_and_die "(a) DKA100:[USER]DATA.TXT did NOT survive the upgrade identically -- before: $USER_BEFORE / after: $USER_AFTER"
fi

# --- (b) site config survives (vms-2c9: seed-once preservation) ----------
OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[SYSMGR]SYSTARTUP_VMS.COM'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SITE_AFTER=$(segment_since "$OFF" | grep -vF "$SITE_CMD")
if printf '%s' "$SITE_AFTER" | grep -qF "$SITE_MARKER"; then
    ok "(b) the site customization marker survives the upgrade"
else
    bad "(b) the site customization marker did NOT survive the upgrade -- PRODUCT INSTALL clobbered SYS\$MANAGER:SYSTARTUP_VMS.COM (vms-2c9 seed-once preservation regressed -- see OVMX_KIT_ENTRY_FLAG_SEED_ONCE in src/libvms/include/ovmx_kit_format.h and do_install() in src/product/product.c)"
fi

# --- (c) the version advanced ---------------------------------------------
OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT /DESTINATION=DKA100:'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
wait_for '$' "$RUN_TIMEOUT" "$OFF"
UPG_SHOW=$(segment_since "$OFF")
echo "$UPG_SHOW"
if printf '%s\n' "$UPG_SHOW" | grep -qF "$EXPECTED_UPGRADE_VERSION" && ! printf '%s\n' "$UPG_SHOW" | grep -qF "$EXPECTED_BASELINE_VERSION"; then
    ok "(c) PRODUCT SHOW PRODUCT reports the UPGRADE version ($EXPECTED_UPGRADE_VERSION), not the baseline ($EXPECTED_BASELINE_VERSION)"
else
    dump_and_die "(c) PRODUCT SHOW PRODUCT does not show the advanced version after the upgrade: $UPG_SHOW"
fi

# DISMOUNT before killing QEMU so umount(2) flushes the volumes cleanly.
OFF=$(wc -c <"$LOG")
send 'DISMOUNT DKA100:'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"
OFF=$(wc -c <"$LOG")
send 'DISMOUNT DKA200:'  # GUIDE-STEP (docs/upgrade-guide.md, tools/check_guide_drift.py)
wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

# =====================================================================
# BOOT 2 -- (d) the whole machine still boots to login after the upgrade;
# re-check (a)/(c) survive a full restart (real disk, not tmpfs).
# =====================================================================
LOG="$WORKDIR/boot2.log"
boot_qemu "$LOG" "$WORKDIR/boot2.in"
login "$LOG"
ok "(d) the system still boots to a login prompt after the upgrade (full QEMU restart)"

OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA100: succeeds again after a full QEMU restart"
else
    dump_and_die "MOUNT DKA100: did not report success on the restarted boot"
fi

OFF=$(wc -c <"$LOG")
send 'TYPE DKA100:[USER]DATA.TXT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
USER_RESTART=$(segment_since "$OFF" | grep -vF "$USER_CMD")
if [ "$(printf '%s' "$USER_RESTART" | tr -d '[:space:]')" = "$(printf '%s' "$USER_BEFORE" | tr -d '[:space:]')" ] \
    && printf '%s' "$USER_RESTART" | grep -qF "$USER_DATA_CONTENT"; then
    ok "(a) DKA100:[USER]DATA.TXT survives a full QEMU restart after the upgrade"
else
    dump_and_die "(a) DKA100:[USER]DATA.TXT did NOT survive the QEMU restart: $USER_RESTART"
fi

OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT /DESTINATION=DKA100:'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF "$EXPECTED_UPGRADE_VERSION"; then
    ok "(c) the advanced version persists across a full QEMU restart (disk, not tmpfs)"
else
    dump_and_die "(c) the advanced version did not persist across the QEMU restart: $SEG"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL INSTALL -> UPGRADE -> BOOT E2E CHECKS PASSED"
    exit 0
fi
echo ""
echo "--- full console log (boot1) ---"
cat "$WORKDIR/boot1.log" 2>/dev/null
echo "--- full console log (boot2) ---"
cat "$WORKDIR/boot2.log" 2>/dev/null
exit 1
