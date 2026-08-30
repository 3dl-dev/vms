#!/bin/bash
# test_boot_conformance.sh - the boot console FACILITY+IDENT SEQUENCE
#                             matches the OpenVMS oracle (vms-1fb)
#
# Runs inside the ovmx-boot Docker image (has QEMU + kernel + the SLIM
# initramfs + /boot/ovmx-distrib.img, the mastered distribution disk) --
# same image tests/qemu/test_persistent_boot.sh and test_executive_integral.sh
# already run against.
#
# WHAT THIS PROVES (docs/design-boot-faithful.md §2.5/§3.5/§3.7/§4 item 3/6)
#
# The Alpha 8.4 oracle capture (§3.5) shows the boot console in a specific
# ORDER: the OS identification banner immediately after SYSBOOT hands over,
# THEN %STDRV-I-STARTUP begun (printed exactly once, with NO "completed"
# counterpart anywhere in the boot), THEN STARTUP.COM's own narration, THEN
# the login prompt. Before vms-1fb the banner printed LAST -- after
# STARTUP.COM had already run -- the exact inversion of the oracle. This test
# boots the real mastered image and asserts, mechanically, on the actual
# console transcript:
#
#   1. %STDRV-I-STARTUP begun prints EXACTLY ONCE, and no "completed" line
#      exists anywhere in the transcript (vms-2f0's deletion still holds).
#   2. The STDRV timestamp is byte-shaped to the oracle: the day field is
#      SPACE-padded (" 7-AUG-2026"), never zero-padded ("07-AUG-2026"), and
#      the fractional-seconds field is exactly two digits (hundredths,
#      ".24"), never three (milliseconds).
#   3. The ORDERED SEQUENCE of every %FACILITY-SEVERITY-IDENT boot message,
#      plus the OS banner and the login prompt as two positional landmarks,
#      matches a PINNED expected sequence derived from the oracle plus
#      docs/design-boot-faithful.md §3.7's per-message disposition table
#      (OVMX has no OPCOM/audit-server/security-server facilities yet, so
#      the pinned sequence is honestly sparser than the full §3.5 capture --
#      see §3.7 for why faking those would be the reverse defect).
#
# This is a SEQUENCE test, not a full boot-to-login proof (that is
# test_persistent_boot.sh / test_job_control_console.sh / test_distrib_boot.sh
# -- this file reuses the SAME CR-wake helper those use, vms-2213, so it
# reaches the same login prompt those do, but its own assertions are about
# ORDER and SHAPE, not about the login session itself).
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_boot_conformance.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Exit code 0 = all checks pass, 1 = failures.

set -uo pipefail

TIMEOUT=90
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

FAIL=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; FAIL=1; }

# Boot to a captured console log, up to TIMEOUT.
#
# REUSED VERBATIM (vms-2213/#358's CR-wake helper, as
# test_persistent_boot.sh's run_qemu() already does) rather than reinvented:
# on OPA0: LOGINOUT waits for the operator's RETURN before it presents
# "Username:" (the "press RETURN to log in" console behaviour). A single CR
# sent at t=0 is lost (the guest serial driver is not up yet), so QEMU is
# backgrounded on a FIFO and a CR is fed every second -- as a real operator
# would -- until the prompt appears in the log or the guest exits. The full
# console log is printed on stdout.
run_qemu() {
    local initrd="$1" disk="$2" log fifo qp w=0
    log=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
    # No trailing "quiet" (vms-300): it used to RAISE console_loglevel back
    # up after "loglevel=3" set it, self-defeating for the pre-PID1 window --
    # see distro/boot/run-qemu.sh's own APPEND comment for the mechanism.
    # loglevel=3 alone is already stricter than "quiet" would be.
    # shellcheck disable=SC2086
    timeout "$TIMEOUT" $QEMU $MACHINE \
        -kernel "$KERNEL" \
        -initrd "$initrd" \
        -nographic \
        -append "$CONSOLE loglevel=3" \
        -m 256M \
        -smp 1 \
        -nic none \
        -nodefaults \
        -serial stdio \
        -drive file="$disk",format=raw,if=virtio \
        -no-reboot \
        <"$fifo" >"$log" 2>&1 &
    qp=$!
    exec 6>"$fifo"
    trap '' PIPE   # a CR fed just as the guest exits must not kill this subshell
    while kill -0 "$qp" 2>/dev/null; do
        grep -qaF 'Username:' "$log" 2>/dev/null && break
        printf '\r' >&6 2>/dev/null
        sleep 1; w=$((w + 1))
        [ "$w" -ge "$TIMEOUT" ] && break
    done
    exec 6>&-
    kill "$qp" 2>/dev/null
    wait "$qp" 2>/dev/null
    cat "$log"
    rm -f "$log" "$fifo"
}

echo "=== OVMX boot console conformance vs. the OpenVMS Alpha oracle (vms-1fb) ==="
echo "Architecture: $ARCH   QEMU: $QEMU"
echo "Kernel: $KERNEL   Distribution image: $DISTRIB_IMG"
echo ""

if [ ! -f "$DISTRIB_IMG" ]; then
    echo "FATAL: $DISTRIB_IMG missing — the mastering stage did not run"
    exit 1
fi

DISK="/tmp/boot-conformance.img"
rm -f "$DISK"
cp "$DISTRIB_IMG" "$DISK"
OUT=$(run_qemu "$SLIM_INITRD" "$DISK")
echo "$OUT" | head -60
echo "[... see full transcript below on failure ...]"
echo ""

CLEAN=$(printf '%s' "$OUT" | tr -d '\r')

# --- (b) STDRV-begun-only ---------------------------------------------------
STDRV_COUNT=$(printf '%s\n' "$CLEAN" | grep -cF '%STDRV-I-STARTUP')
if [ "$STDRV_COUNT" -eq 1 ]; then
    pass "%STDRV-I-STARTUP begun prints exactly once"
else
    bad "%STDRV-I-STARTUP printed $STDRV_COUNT times (expected exactly 1)"
fi
if printf '%s' "$CLEAN" | grep -qi 'startup completed'; then
    bad "an invented STDRV/STARTUP 'completed' line is present (vms-2f0 regression)"
else
    pass "no invented 'startup completed' line anywhere in the transcript"
fi

# --- (c) timestamp format ----------------------------------------------------
# Oracle (§3.1/§3.5): " 7-AUG-2026 18:51:27.24" -- day field SPACE-padded,
# fractional-seconds field exactly two digits (hundredths).
if printf '%s' "$CLEAN" | \
   grep -qE '%STDRV-I-STARTUP,.*startup begun at [ 0-9][0-9]-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}$'; then
    pass "STDRV timestamp shape matches the oracle (space-padded day, hundredths)"
else
    bad "STDRV timestamp shape does not match the oracle's ' D-MON-YYYY HH:MM:SS.CC' form"
fi
if printf '%s' "$CLEAN" | grep -qE '%STDRV-I-STARTUP,.*startup begun at 0[1-9]-'; then
    bad "STDRV day field is ZERO-padded (e.g. '07-'); the oracle is SPACE-padded (' 7-')"
else
    pass "STDRV day field is not zero-padded"
fi

# --- (a)/(d) the ordered facility+ident SEQUENCE -----------------------------
# Extract every %FACILITY-SEVERITY-IDENT token in transcript order, plus two
# positional landmarks the oracle also pins: the OS identification banner
# ("OpenVMX V<n>...", INV-0 brand substitution for the oracle's "OpenVMS")
# and the login prompt. Timestamps, PIDs, and everything else about full
# message TEXT are deliberately not compared here -- see this file's header.
RAW_SEQ=$(printf '%s\n' "$CLEAN" | \
    grep -oE '%[A-Z][A-Z0-9_]*-[A-Z]-[A-Z0-9_]+|OpenVMX V[0-9]|Username:' | \
    sed -E 's/^OpenVMX V[0-9]$/__BANNER__/; s/^Username:$/__USERNAME__/')

# Trim to the first __USERNAME__ (inclusive): the boot phase this test is
# pinning ends at the login prompt, and anything the CR-wake loop's trailing
# kill/drain adds after that point is not part of the boot sequence.
SEQ=$(printf '%s\n' "$RAW_SEQ" | sed -n '1,/^__USERNAME__$/p')

# PINNED expected sequence (docs/design-boot-faithful.md §3.7/§3.8): honestly
# sparser than the full §3.5 oracle capture -- OVMX has no OPCOM, audit
# server, or security server yet (§5, out of scope), so pinning those would
# be faking a facility OVMX does not have (Rule 10 in reverse). The SECOND
# %OVMX-I-EXEC (PROVISION.EXE's "system identity ... established by the
# executive", src/ovmx_provision/ovmx_provision.c -- a different message
# under the same OVMX-EXEC ident, not a repeat of the first) and the seven
# %INSTALL-I-ADDED lines (SYSTARTUP_VMS.COM's INSTALL ADD of every OVMX
# shareable, src/vmsdcl -- one line per shareable) are real, both outside
# ovmx_init.c's own audit scope (§3.7) but part of the actual console
# transcript this test pins.
#
# ORDERING (vms-2a9): %RUN-S-PROC_ID (JOB_CONTROL's creation) still prints
# AFTER the %INSTALL-I-ADDED block, unchanged from before vms-2a9. JOB_
# CONTROL is now created by a registered STDRV component (SYS$STARTUP:
# VMS$VMS.DAT) rather than a hardcoded call inside SYSTARTUP_VMS.COM, but
# the component runs at the END phase -- the LAST of the nine, after
# LPMAIN and everything SYSTARTUP_VMS.COM does -- specifically so this
# ordering would NOT change. An earlier phase (CONFIG) was tried first and
# measurably interleaved JOB_CONTROL's own LOGINOUT "Username:" prompt with
# STARTUP.COM's still-running console output; this test is what caught
# that (docs/design-boot-faithful.md §3.8 records the full account).
read -r -d '' EXPECTED <<'EOF' || true
%OVMX-I-EXEC
__BANNER__
%OVMX-I-SYSDISK
%OVMX-I-MOUNTED
%OVMX-I-SCSNODE
%STDRV-I-STARTUP
%OVMX-I-EXEC
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%INSTALL-I-ADDED
%RUN-S-PROC_ID
__USERNAME__
EOF

if [ "$SEQ" = "$EXPECTED" ]; then
    pass "boot facility+ident sequence matches the pinned oracle-derived shape"
else
    bad "boot facility+ident sequence does not match the pinned oracle-derived shape"
    echo "  --- expected ---"
    printf '%s\n' "$EXPECTED" | sed 's/^/    /'
    echo "  --- actual ---"
    printf '%s\n' "$SEQ" | sed 's/^/    /'
    echo "  --- diff (expected vs actual) ---"
    diff <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$SEQ") | sed 's/^/    /' || true
fi

# --- (d2) NO RAW KERNEL DMESG ON THE CONSOLE (vms-300) -----------------------
# The pinned SEQUENCE check above (RAW_SEQ/SEQ) is an EXTRACTION: grep -oE
# pulls out only the tokens matching its own patterns and silently ignores
# every other byte in the transcript. It therefore proves the tokens that ARE
# there are in the right order and shape, but proves nothing about whether
# OTHER, unwanted content -- like raw Linux kernel dmesg -- also made it onto
# the console alongside them. vms-300: QEMU's ttyS0 is both the kernel's own
# console and ovmx_init's stdout, one wire; without
# ovmx_boot_mute_kernel_console() (src/ovmx_init/ovmx_boot_linux.c, called
# from bare_metal_init() before any kernel module loads) vms.ko's
# pr_info lifecycle lines -- "vms: initializing VMS kernel module",
# "vms: disk unit VDA0: -> vda (253:0)", the kernel's own module-taint
# warning, etc. -- interleaved directly with the VMS boot banner. These two
# checks assert the SPECIFIC shapes the operator saw, independent of the
# curated noise list tests/qemu/test_job_control_console.sh already checks:
# a general kernel-timestamp prefix, and a general vms: raw prefix
# (the exact shape opcom_kmsg_classify() recognizes as an OVMX kernel-module
# record -- see src/ovmx_init/opcom_kmsg.c -- which belongs in
# SYS$MANAGER:OPERATOR.LOG, never verbatim on the console).
if printf '%s\n' "$CLEAN" | grep -qE '\[[[:space:]]*[0-9]+\.[0-9]+\]'; then
    bad "a kernel-timestamped line (e.g. '[   41.332690]') reached the console -- raw dmesg leak (vms-300)"
else
    pass "no kernel-timestamped '[ NNN.NNN]' line on the console (vms-300)"
fi
if printf '%s\n' "$CLEAN" | grep -qE '^vms: '; then
    bad "a raw 'vms:' kernel-module line reached the console verbatim -- must be OPERATOR.LOG-only via the kmsg bridge (vms-300)"
else
    pass "no raw 'vms:' kernel-module line on the console (vms-300)"
fi
# OPERATOR.LOG-routing side of the fix (that muting the console console did
# NOT also starve SYS$MANAGER:OPERATOR.LOG of these same records): NOT
# reasserted here. This file boots straight to the login prompt and never
# logs in or runs DCL (see this file's own header: "a SEQUENCE test, not a
# full boot-to-login proof"), so it has no in-guest way to TYPE the log. That
# side is covered elsewhere and left alone, not weakened: opcom_kmsg_classify()
# unit tests (tests/ovmx_init/test_opcom_kmsg.c) prove vms:/vmsfs: and SYSKRNL
# records route to OPCOM_KMSG_OPERATOR_LOG rather than being dropped, and
# ovmx_boot_mute_kernel_console() only ever touches the console sink -- never
# /dev/kmsg, the ring buffer opcom_kmsg.c's reader thread polls -- so the
# routing path this test cannot reach is structurally unaffected by this fix.

# --- (e) no empty-username "Username:" prompt storm (vms-3ab8) ----------------
# The 0.4 demo boot showed ~20 "Username:" prompts machine-gunned onto ONE
# physical line: LOGINOUT drained the operator's boot-time type-ahead RETURNs
# through its empty-username reprompt with no wait and no newline. A VMS-
# faithful console prints the prompt ONCE and blocks; a reprompt (empty line at
# a live prompt) lands on its OWN line. So the invariant is not "Username:
# appears once" (a stray live RETURN may legitimately produce a second prompt on
# a new line) but "no single line reprints Username: more than once".
MAX_PROMPTS_PER_LINE=$(printf '%s\n' "$CLEAN" | grep -F 'Username:' | \
    awk '{ n = gsub(/Username:/, "&"); if (n > m) m = n } END { print m + 0 }')
if [ "${MAX_PROMPTS_PER_LINE:-0}" -le 1 ]; then
    pass "no line reprints Username: more than once (no empty-username prompt storm)"
else
    bad "a line contains Username: ${MAX_PROMPTS_PER_LINE}x -- LOGINOUT empty-username prompt storm (vms-3ab8)"
fi

# --- (f) site-specific startup announcement printed EXACTLY ONCE (vms-3ab8) ---
# STARTUP.COM's RUN_SITE_STARTUP is the single owner; SYSTARTUP_VMS.COM (both
# the installed and the distribution-media variants) used to WRITE it too, so a
# real boot printed the line twice. The message is emitted at the LPMAIN phase,
# before the login prompt, so it is inside the transcript this test captures.
SITE_MSG_COUNT=$(printf '%s\n' "$CLEAN" | \
    grep -cF 'executing the site-specific startup commands')
if [ "$SITE_MSG_COUNT" -eq 1 ]; then
    pass "site-specific startup announcement printed exactly once"
else
    bad "site-specific startup announcement printed ${SITE_MSG_COUNT}x (expected exactly 1; vms-3ab8)"
fi

echo ""
echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "  ALL BOOT CONFORMANCE CHECKS PASSED"
    echo "=========================================="
    exit 0
else
    echo "  BOOT CONFORMANCE CHECKS FAILED"
    echo "=========================================="
    echo ""
    echo "--- full console transcript ---"
    echo "$OUT"
    exit 1
fi
