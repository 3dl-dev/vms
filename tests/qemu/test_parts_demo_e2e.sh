#!/bin/bash
# test_parts_demo_e2e.sh - the OVMX 0.2 "killer app" demo, end to end
# (vms-5dd, parent vms-2579, docs/design-parts-demo.md, docs/release-plan-
# 0.2-to-0.5.md section 4).
#
# WHAT THIS PROVES, AND WHY NOTHING EARLIER PROVES IT.
#
# Three prior items each proved one link in this chain in isolation:
#   - vms-e97/vms-f20: PARTS.EXE builds through the VMS-native toolchain and
#     RUNS under QEMU via `$ RUN SYS$SYSTEM:PARTS` against a HAND-REPACKED
#     initramfs (tests/qemu/test_parts_rms_qemu.sh) - never the shipped kit.
#   - vms-cde: the REAL bootable image (distro/Dockerfile.bootable) masters
#     PARTS.EXE + PARTS_SETUP.COM into SYS$UPDATE: (tests/qemu/
#     test_parts_mastering.sh) - proved by extracting the initramfs with
#     `docker cp`, WITHOUT ever booting it.
#   - vms-e5c/vms-221: SYSTEM can sys$create an RMS indexed file under a
#     genuinely SYSTEM-writable SYS$SCRATCH: (tests/qemu/test_syssvc_
#     scratch_writable.c, test_syssvc_rms_scratch_create.c) - proved against
#     a bare insmod'd vms.ko, NEVER through a real STARTUP.EXE boot, and
#     test_syssvc_scratch_writable.c's own header says so explicitly: "The
#     full-boot integration ... was verified by hand ... That path has no
#     ctest/CI hook of its own."
#
# NONE of the three ever ran together: no test boots the ACTUAL shipped
# image, logs in as an ACTUAL SYSTEM DCL session, runs the ACTUAL install
# procedure a manager would type (`$ @SYS$UPDATE:PARTS_SETUP.COM`), and then
# runs the ACTUAL foreign command (`$ PARTS`, not `$ RUN SYS$SYSTEM:PARTS`) -
# the literal 0.2 pitch ("it runs a real VMS app"). This is that test.
#
# WHAT "ZERO UNIX LEAKS" MEANS HERE, AND HOW IT IS CHECKED (not just
# asserted): src/apps/parts/parts.c's self-demo tries an ordered list of VMS
# filespecs and falls back to /tmp/PARTS.DAT only if every VMS candidate's
# sys$create() fails - printing %PARTS-I-TRYCRE for each rejected candidate
# on the way down. So the demo can "work" while silently leaking to /tmp, and
# would have for most of this project's life (see test_syssvc_scratch_
# writable.c's header: SYS$SCRATCH: was root:root 0755 until vms-e5c, and
# PARTS's own sys$create() still hit EACCES on a genuinely SYSTEM-writable
# SYS$SCRATCH: until vms-221's kernel fix). This gate asserts the FIRST
# candidate (SYS$SCRATCH:PARTS.DAT) is what actually got created - no
# %PARTS-I-TRYCRE line, no /tmp/ anywhere in the transcript - and independen
# tly confirms the file exists there via a real `$ DIRECTORY` from the same
# DCL session, not just PARTS's own stdout claim.
#
# WHAT WOULD MAKE THIS TEST FAIL HONESTLY, RATHER THAN FAKE A PASS: any of
#   - boot never reaches Username: (install path broken)
#   - SYSTEM cannot log in (LOGINOUT/SYSUAF broken)
#   - PARTS_SETUP.COM does not print its %PARTS-I-* lines (COPY/DEFINE broke)
#   - bare `$ PARTS` falls through to %DCL-W-IVVERB (foreign-command dispatch,
#     vms-96e, not wired or not reached from a symbol defined by a .COM)
#   - PARTS falls back off SYS$SCRATCH: (a %PARTS-I-TRYCRE line appears, or
#     the create-line names a different candidate, or /tmp/ appears anywhere)
#   - a keyed sys$get returns the wrong record (%PARTS-E-VERIFY) or nothing
#     loads (no %PARTS-S-FOUND / no PN000001)
#   - `$ DIRECTORY SYS$SCRATCH:PARTS.DAT` from the SAME session does not
#     show the file (the app's own claim would be uncorroborated)
# stops this test red with the offending transcript segment printed - it
# does not retry, does not relax an assertion, and does not silently accept
# a Unix-path fallback as "good enough".
#
# Usage (run INSIDE the bootable image, like test_release_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_parts_demo_e2e.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for the install boot to reach Username:
#                  (default 180 - a blank-disk install copies the whole
#                  shipped tree, PARTS.EXE included, before the prompt).
#   SETUP_TIMEOUT  seconds to wait for PARTS_SETUP.COM to finish (default 60).
#   RUN_TIMEOUT    seconds to wait for `$ PARTS` to reach %PARTS-S-DONE
#                  (default 1500 - the shipped image's default load is
#                  PARTS_DEFAULT_COUNT (10000) records via real sys$put()
#                  RMS indexed-file inserts under QEMU TCG emulation, an
#                  order of magnitude more than test_parts_rms_qemu.sh's
#                  1000-record speed knob; generous on purpose so a slow CI
#                  runner is not mistaken for a broken demo).
#
# Exit 0 = the full demo transcript is correct end to end with zero Unix
# leaks. Exit 1 = a real failure (see the printed transcript segment).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETUP_TIMEOUT="${SETUP_TIMEOUT:-60}"
RUN_TIMEOUT="${RUN_TIMEOUT:-1500}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
# PRE-INSTALLED distribution disk (vms-8ab): PID 1 no longer installs on a blank
# disk (vms-2f0, operator ruling 2026-08-10), so this demo seeds its disk from
# the mastered image and boots an already-installed system.
DISTRIB_IMG=/boot/ovmx-distrib.img
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== PARTS 0.2 demo e2e: boot -> login -> @SETUP -> \$ PARTS -> keyed lookup (vms-5dd) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

DISK=/tmp/parts-e2e.img
LOG=/tmp/parts-e2e-console.log
FIFO=/tmp/parts-e2e-console.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$DISTRIB_IMG" "$DISK"
mkfifo "$FIFO"

cleanup() { [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null; }
trap cleanup EXIT

# shellcheck disable=SC2086
timeout "$((BOOT_TIMEOUT + SETUP_TIMEOUT + RUN_TIMEOUT + 120))" $QEMU $MACHINE \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "$CONSOLE loglevel=3 quiet" \
    -m 512M -smp 2 -nic none -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
QPID=$!
exec 4>"$FIFO"

send() { printf '%s\r' "$1" >&4; }
wait_for() {  # pattern  limit-seconds  since-byte
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
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

# --- 1. Boot: blank disk -> STARTUP.EXE installs -> login prompt ------------
if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
# vms-2213: OPA0: LOGINOUT now waits for RETURN before presenting Username:
# (the "press RETURN to log in" console behaviour). Wake it with a CR -- a
# real operator's keystroke -- so the login prompt appears.
send ''
if wait_for 'Username:' "$BOOT_TIMEOUT"; then
    ok "install completes and reaches the login prompt"
else
    dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
fi

# --- 2. Log in as SYSTEM -----------------------------------------------------
LOGIN_OFF=$(wc -c <"$LOG")
send 'SYSTEM'
wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
    ok "SYSTEM logs in (LOGINOUT.EXE activated)"
else
    dump_and_die "SYSTEM login failed"
fi
wait_for '$' 20 "$LOGIN_OFF"

# --- 2b. BOOT/LOGIN OUTPUT FIDELITY (vms-2213) ------------------------------
# Four VMS-fidelity properties of the boot->login console transcript, asserted
# against the real mastered image. Grounding: VSI OpenVMS System Manager's
# Manual, Vol I, "Logging In to the System" (announce banner, then Username:,
# then Password:, then a single SYS$WELCOME).
FID_SEG=$(tail -c "+$((LOGIN_OFF + 1))" "$LOG" 2>/dev/null | tr -d '\r')
FULL=$(tr -d '\r' <"$LOG")
# (1) Exactly ONE post-login welcome (SYS$WELCOME), not two. SYLOGIN.COM no
#     longer WRITEs a second "Welcome ..." line of its own.
WELCOME_N=$(printf '%s\n' "$FID_SEG" | grep -c 'Welcome to OpenVMX')
if [ "$WELCOME_N" -eq 1 ]; then
    ok "exactly ONE welcome message after login (single SYS\$WELCOME)"
else
    bad "expected exactly 1 welcome after login, saw $WELCOME_N"
fi
# (2) The boot identification banner prints BEFORE the first Username:.
BANNER_POS=$(printf '%s' "$FULL" | grep -aboE 'OpenVMX V[0-9]' | head -1 | cut -d: -f1)
UNAME_POS=$(printf '%s' "$FULL" | grep -aboF 'Username:' | head -1 | cut -d: -f1)
if [ -n "$BANNER_POS" ] && [ -n "$UNAME_POS" ] && [ "$BANNER_POS" -lt "$UNAME_POS" ]; then
    ok "boot banner prints before the first Username: prompt (banner@$BANNER_POS < prompt@$UNAME_POS)"
else
    bad "banner-before-prompt ordering wrong (banner@${BANNER_POS:-none} prompt@${UNAME_POS:-none})"
fi
# (3) The rigorous "waited for RETURN" proof (Username: absent until the CR is
#     sent, at a point past the boot banner) lives in the dedicated
#     tests/qemu/test_job_control_console.sh, which drives the clean login path
#     without an install racing in between. Here, assertion (2) already shows
#     the observable outcome: before this fix the prompt raced AHEAD of the
#     boot banner; after it, the banner is strictly first.
# (4) No routine kernel-driver printk leaked onto the user console.
if printf '%s\n' "$FULL" | grep -q 'vms: registered process'; then
    bad "kernel printk 'vms: registered process ...' leaked onto the console"
else
    ok "no 'vms: registered process' kernel chatter on the user console"
fi

# --- 3. The install step: @SYS$UPDATE:PARTS_SETUP.COM -----------------------
SETUP_OFF=$(wc -c <"$LOG")
send '@SYS$UPDATE:PARTS_SETUP.COM'
if wait_for '%PARTS-I-DONE' "$SETUP_TIMEOUT" "$SETUP_OFF"; then
    ok "PARTS_SETUP.COM ran to completion (%PARTS-I-DONE)"
else
    dump_and_die "PARTS_SETUP.COM did not reach %PARTS-I-DONE within ${SETUP_TIMEOUT}s"
fi
SETUP_SEG=$(segment_since "$SETUP_OFF")
check_setup() { if printf '%s\n' "$SETUP_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
check_setup '%PARTS-I-SETUP' "install procedure announced itself"
check_setup '%PARTS-I-COPY'  "install procedure staged the COPY step"
check_setup '%PARTS-I-DEFINE, defining PARTS :== $SYS$SYSTEM:PARTS.EXE' \
            "install procedure defined the PARTS foreign command"
if printf '%s\n' "$SETUP_SEG" | grep -qiE '%COPY-[EF]-|%RMS-[EF]-|%DCL-[EF]-'; then
    bad "PARTS_SETUP.COM's COPY step reported no fatal/error condition"
else
    ok "PARTS_SETUP.COM's COPY step reported no fatal/error condition"
fi

# --- 4. The demo itself: the bare foreign command $ PARTS -------------------
RUN_OFF=$(wc -c <"$LOG")
send 'PARTS'
if wait_for '%PARTS-S-DONE' "$RUN_TIMEOUT" "$RUN_OFF"; then
    ok "PARTS ran to completion under QEMU (%PARTS-S-DONE)"
else
    # Distinguish "foreign-command dispatch never fired" from "it ran and
    # failed partway" -- both are real failures, but the transcript segment
    # tells a very different story for each.
    dump_and_die "\$ PARTS did not reach %PARTS-S-DONE within ${RUN_TIMEOUT}s"
fi
RUN_SEG=$(segment_since "$RUN_OFF")
check_run() { if printf '%s\n' "$RUN_SEG" | grep -qF "$1"; then ok "$2"; else bad "$2"; fi; }
check_run '%PARTS-I-BEGIN' "\$ PARTS activated (foreign-command dispatch, vms-96e)"

# THE ZERO-UNIX-LEAKS ASSERTION. Not "a file got created somewhere" -- the
# FIRST candidate, SYS$SCRATCH:, succeeded, so there was never a fallback.
check_run '%PARTS-I-CREATE, created indexed file SYS$SCRATCH:PARTS.DAT' \
    "created the RMS indexed file at SYS\$SCRATCH:PARTS.DAT (a VMS filespec, first candidate, no fallback)"
if printf '%s\n' "$RUN_SEG" | grep -qF '%PARTS-I-TRYCRE'; then
    bad "no fallback attempts (%PARTS-I-TRYCRE absent) -- SYS\$SCRATCH: worked on the first try"
else
    ok "no fallback attempts (%PARTS-I-TRYCRE absent) -- SYS\$SCRATCH: worked on the first try"
fi
if printf '%s\n' "$RUN_SEG" | grep -qF '/tmp/'; then
    bad "no Unix path (/tmp/) anywhere in the PARTS transcript"
else
    ok "no Unix path (/tmp/) anywhere in the PARTS transcript"
fi

check_run '%PARTS-I-LOADED' "loaded records into the indexed file"
check_run '%PARTS-I-LOOKUP' "ran keyed random lookups (proving indexed, not sequential, access)"
check_run '%PARTS-S-FOUND'  "a keyed random sys\$get returned a record"
check_run 'PN000001'        "the first loaded part (PN000001) was found by keyed lookup"
check_run '%PARTS-W-NOTFOUND, no part with key PN999999' \
    "a key that was never loaded (PN999999) correctly reported NOTFOUND (RMS\$_RNF)"

if printf '%s\n' "$RUN_SEG" | grep -qF '%PARTS-E-VERIFY'; then
    bad "no wrong-record from a keyed lookup (%PARTS-E-VERIFY absent)"
else
    ok "no wrong-record from a keyed lookup (%PARTS-E-VERIFY absent)"
fi
if printf '%s\n' "$RUN_SEG" | grep -qF '%PARTS-F-'; then
    bad "PARTS reported no fatal error"
else
    ok "PARTS reported no fatal error"
fi
if printf '%s\n' "$RUN_SEG" | grep -qiE '%DCL-[EF]-ABORT|terminated abnormally'; then
    bad "DCL did not surface an abnormal image termination (vms-17f9)"
else
    ok "DCL did not surface an abnormal image termination (vms-17f9)"
fi

# --- 5. Independent corroboration: the VMS layer itself sees the file -------
# Not just PARTS's own stdout claim -- a second, independent command in the
# SAME DCL session confirms the file exists at the VMS filespec.
#
# CAUTION (found by actually running this test, vms-5dd CI-wiring pass): the
# console is a cooked tty, so the guest echoes back every keystroke we send --
# the raw DIR_SEG's FIRST line is the literal echoed input "DIRECTORY
# SYS$SCRATCH:PARTS.DAT", which itself contains the substring "PARTS.DAT". A
# bare `grep -qF 'PARTS.DAT'` over the whole segment therefore matches on the
# echoed COMMAND, not on any line DIRECTORY actually printed, and would report
# a false PASS even when DIRECTORY finds nothing (observed for real: a run
# whose actual output was "Total of 0 files, 0 blocks." still made this
# assertion green before this fix). Strip the echoed command line before
# searching, and additionally require the "Total of" summary to report a
# NONZERO file count -- corroboration means DIRECTORY found the file, not that
# the string "PARTS.DAT" appears anywhere in the segment.
DIR_OFF=$(wc -c <"$LOG")
DIR_CMD='DIRECTORY SYS$SCRATCH:PARTS.DAT'
send "$DIR_CMD"
wait_for 'Total of' 20 "$DIR_OFF"
DIR_SEG=$(segment_since "$DIR_OFF")
DIR_BODY=$(printf '%s\n' "$DIR_SEG" | grep -vF "$DIR_CMD")
if printf '%s\n' "$DIR_BODY" | grep -qE 'Total of [1-9][0-9]* files?, [0-9]+ blocks' \
    && printf '%s\n' "$DIR_BODY" | grep -qF 'PARTS.DAT'; then
    ok "\$ DIRECTORY SYS\$SCRATCH:PARTS.DAT independently confirms the file exists"
else
    bad "\$ DIRECTORY SYS\$SCRATCH:PARTS.DAT independently confirms the file exists"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "=== transcript: PARTS_SETUP.COM ==="
printf '%s\n' "$SETUP_SEG" | grep -E '%PARTS|%COPY|%RMS|%DCL' | sed 's/^/  /'
echo "=== transcript: \$ PARTS ==="
printf '%s\n' "$RUN_SEG" | grep -E '%PARTS|PN00|%DCL' | sed 's/^/  /'
echo "=== transcript: \$ DIRECTORY SYS\$SCRATCH:PARTS.DAT ==="
printf '%s\n' "$DIR_SEG" | sed 's/^/  /'
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL PARTS DEMO E2E CHECKS PASSED -- ZERO UNIX LEAKS"
    exit 0
fi
echo ""
echo "--- full console log ---"
cat "$LOG"
exit 1
