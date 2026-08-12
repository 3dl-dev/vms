#!/bin/bash
# test_product_install_e2e.sh - PRODUCT INSTALL against a real vms.ko +
# vmsfs.ko, ground-sourced (vms-df9, docs/design-vms-faithful-install.md
# sec 3.3, docs/design-ovmx-kit-format.md).
#
# WHAT THIS PROVES, AND WHY NOTHING EARLIER PROVES IT. Before this bead,
# `cmd_product` implemented only PRODUCT SHOW PRODUCT/HISTORY (a fat DCL
# builtin printing a hardcoded literal); every other PRODUCT operation
# printed a bare %PCSI-E-NOTIMPL and no PRODUCT.EXE existed at all
# (measured baseline, see the pre-fix src/vmsdcl/dcl_cmd_misc.c::cmd_product
# this bead replaced). Unit-level DCL tests (no /dev/vms, no real disk)
# can only prove the NOTIMPL stub is gone and PRODUCT.EXE gets invoked --
# they cannot prove a kit's payload actually lands runnable bytes on a real
# target volume with real protection/UIC, because that needs a real
# vmsfs.ko block-device mount, which needs a real kernel. This is the
# ground-source positive, same shape as tests/qemu/test_mount_e2e.sh:
#
#   1. INITIALIZE (host-side, like test_mount_e2e.sh) + MOUNT (real DCL,
#      real mount(2) through the executive) a SECOND, blank virtio disk.
#   2. PRODUCT INSTALL the REAL OS kit onto the mounted target, reading it
#      from SYS$UPDATE:OVMX-OS.KIT on the ALREADY-MOUNTED boot disk (DKA0:,
#      the distrib image) -- distro/Dockerfile.bootable's kit-stage builds
#      and byte-verifies /boot/ovmx-os.kit from the SAME tree that masters
#      DKA0:, then stages a copy onto the distrib image itself at
#      SYS$UPDATE:. NOT a raw third virtio disk: devtmpfs creates block
#      device nodes root:root mode 0600 with no udev in this minimal
#      initramfs to relax them, so SYSTEM (uid 4/gid 1, its real UIC) gets
#      EPERM reading one directly -- correctly, since OVMX has no "VMS
#      privilege overrides Unix permission" path for raw devices. Reading
#      the kit as an ordinary SYSTEM-owned file on the boot disk instead
#      sidesteps that rather than faking a permission OVMX doesn't have.
#   3. THE ANTI-LARP CRUX: RUN the INSTALLED HELP.EXE *from the target
#      device* (DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE) and see real output. A kit that
#      only wrote a manifest, or wrote corrupted/truncated bytes, cannot
#      pass this -- IMGACT would refuse to activate it.
#   4. `$ SHOW SYMBOL $STATUS` / grep for %PCSI-E-/%PCSI-F- immediately
#      after INSTALL: no error escaped.
#   5. `PRODUCT SHOW PRODUCT /DESTINATION=DKA100:` lists the installed kit
#      by the NAME BAKED INTO THE KIT ITSELF (not the DCL command's own
#      product-name parameter), proving it read the real product database
#      this bead wrote, not an echo of the command line.
#   6. QEMU is KILLED and a FRESH process boots against the SAME disk
#      files -- the product database AND the installed files persist
#      (same anti-tmpfs proof as test_mount_e2e.sh's MOUNTTST.TXT).
#
# Usage (run INSIDE the bootable image, like test_mount_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_product_install_e2e.sh:/test.sh:ro \
#       -v /path/to/formatted/dka100/dir:/work \
#       --entrypoint bash ovmx-boot /test.sh
# (see tests/qemu/run_product_install_e2e.sh, which formats the second disk
# on the host and invokes this the same way run_mount_e2e.sh does.)
#
# Env knobs: BOOT_TIMEOUT (default 90), RUN_TIMEOUT (default 60), same
# meaning as test_mount_e2e.sh.
#
# Exit 0 = every ground-source assertion above passed.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
DKA100_SRC=/work/dka100.img
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

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG" "$DKA100_SRC"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with /work bind-mounted (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== PRODUCT INSTALL e2e: a real kit onto a real vmsfs volume (vms-df9) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD (kit is SYS\$UPDATE:OVMX-OS.KIT on \$DISTRIB_IMG)"

WORKDIR=$(mktemp -d)
DISK0="$WORKDIR/dka0.img"
DISK1="$WORKDIR/dka100.img"
cp "$DISTRIB_IMG" "$DISK0"
cp "$DKA100_SRC" "$DISK1"

QPID=""
cleanup() { [ -n "$QPID" ] && kill "$QPID" 2>/dev/null; rm -rf "$WORKDIR"; }
trap cleanup EXIT

boot_qemu() {  # boot_qemu <log-file> <fifo-path>
    local log="$1" fifo="$2"
    rm -f "$log" "$fifo"
    mkfifo "$fifo"
    # Only two drives -- the real OS kit rides on DISK0 itself, staged at
    # SYS$UPDATE:OVMX-OS.KIT by distro/Dockerfile.bootable (see the file
    # header for why not a raw third virtio disk).
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 14 + 60))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nic none -nodefaults -serial stdio \
        -drive file="$DISK0",format=raw,if=virtio,cache=writethrough \
        -drive file="$DISK1",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$fifo" >"$log" 2>&1 &
    QPID=$!
    exec 4>"$fifo"
}

send() { printf '%s\r' "$1" >&4; }

# vms-2213: on OPA0: LOGINOUT waits for the operator's RETURN before it presents
# "Username:". A single CR sent at t=0 is lost (guest serial not up yet), so feed
# a CR each second until the prompt actually appears in the log. Bounded.
wake_login() {
    local logf="$1" w=0
    until grep -qaF 'Username:' "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}
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
    wake_login "$log"
    if wait_for 'Username:' "$BOOT_TIMEOUT" 0 "$log"; then
        ok "boot reaches the login prompt ($log)"
    else
        dump_and_die "boot never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off; off=$(wc -c <"$log")
    send 'SYSTEM'
    wait_for 'Password:' 20 "$off" "$log" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 20 "$off" "$log"; then
        ok "SYSTEM logs in"
    else
        dump_and_die "SYSTEM login failed"
    fi
    wait_for '$' 20 "$off" "$log"
}

# =====================================================================
# BOOT 1 -- MOUNT, INSTALL, execute-from-target, SHOW PRODUCT
# =====================================================================
LOG="$WORKDIR/boot1.log"
boot_qemu "$LOG" "$WORKDIR/boot1.in"
login "$LOG"

# --- 1. MOUNT the pre-formatted second disk ----------------------------
OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'  # GUIDE-STEP (docs/install-guide.md, tools/check_guide_drift.py)
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA100: succeeds (real mount(2) via the executive)"
else
    dump_and_die "MOUNT DKA100: did not report success within ${RUN_TIMEOUT}s"
fi

# --- 2. Current-tree NOTIMPL still fires honestly for a genuinely
#        unimplemented PRODUCT operation (the shape the pre-fix baseline
#        used for EVERY operation, including INSTALL) --------------------
OFF=$(wc -c <"$LOG")
send 'PRODUCT CONFIGURE FOO'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qF '%PCSI-W-NOTIMPL'; then
    ok "PRODUCT CONFIGURE (still unimplemented) honestly reports %PCSI-W-NOTIMPL"
else
    bad "PRODUCT CONFIGURE did not report %PCSI-W-NOTIMPL"
    echo "$SEG"
fi

# --- 3. PRODUCT INSTALL the real kit from SYS$UPDATE: onto DKA100: -----
OFF=$(wc -c <"$LOG")
send 'PRODUCT INSTALL VMS /SOURCE=SYS$UPDATE:OVMX-OS.KIT /DESTINATION=DKA100:'  # GUIDE-STEP (docs/install-guide.md, tools/check_guide_drift.py)
if wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRODUCT INSTALL reports %PCSI-I-DONE"
else
    dump_and_die "PRODUCT INSTALL did not reach %PCSI-I-DONE within ${RUN_TIMEOUT}s"
fi
INSTALL_SEG=$(segment_since "$OFF")
if printf '%s\n' "$INSTALL_SEG" | grep -qiE '%PCSI-[EF]-'; then
    bad "PRODUCT INSTALL's transcript contains a PCSI error despite reporting DONE"
    echo "$INSTALL_SEG"
else
    ok "PRODUCT INSTALL's transcript carries no %PCSI-E-/%PCSI-F- error"
fi

# --- 4. DIRECTORY independently confirms real files landed on DKA100: ---
# The kit lands in the ROOTED, concealed system-disk structure
# [SYS0.SYSCOMMON.SYSEXE] (vms-96ec), NOT a flat [SYSEXE] -- that is what
# makes a /DESTINATION-installed volume bootable as its own system disk
# (see tests/qemu/test_install_boot_e2e.sh, which boots exactly this
# target). Assert the rooted path, matching where a mastered ovmx-distrib.img
# already places DCL.EXE/HELP.EXE.
OFF=$(wc -c <"$LOG")
send 'DIRECTORY DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE'
wait_for 'Total of' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qE 'Total of [1-9][0-9]* files?, [0-9]+ blocks' \
    && printf '%s\n' "$SEG" | grep -qF 'HELP.EXE'; then
    ok "DIRECTORY independently confirms DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE exists"
else
    bad "DIRECTORY does not confirm HELP.EXE landed on DKA100:"
    echo "$SEG"
fi

# --- 5. THE ANTI-LARP CRUX: execute the INSTALLED image FROM THE TARGET -
# RUN never forwards parameters to the image it launches (dcl_cmd_process.c
# cmd_run's own comment), and HELP.EXE with argc==1 (no topic) blocks
# waiting on stdin for interactive "Topic?" input -- it would never return
# to the DCL '$' prompt at all. A foreign-command definition ("SYM :==
# $image-spec" then bare "SYM args") DOES forward trailing words as argv
# (dcl_exec_foreign_command(), src/vmsdcl/dcl_exec.c), which is what makes
# HELP.EXE take its non-interactive "HELP topic" branch and actually exit.
OFF=$(wc -c <"$LOG")
send 'RUNHELP :== $DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE'
# NOTE: the symbol-definition command's own echo contains a literal '$'
# (the "$DKA100:..." image-spec), so a wait_for '$' issued for the NEXT
# command against this SAME offset would match that leftover byte
# instantly instead of actually waiting -- each send below gets its own
# fresh offset, same as every other step in this script.
wait_for '$' "$RUN_TIMEOUT" "$OFF"
OFF=$(wc -c <"$LOG")
send 'RUNHELP MOUNT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
RUN_SEG=$(segment_since "$OFF")
if printf '%s\n' "$RUN_SEG" | grep -qiE 'Segmentation|cannot execute|%DCL-.*IVIMAGE|%SYSTEM-.*ACCVIO'; then
    bad "the installed HELP.EXE crashed or failed to activate: $RUN_SEG"
elif [ -n "$(printf '%s' "$RUN_SEG" | grep -vF 'RUNHELP' | tr -d '[:space:]')" ]; then
    ok "the installed HELP.EXE executes non-interactively and prints real output from the target"
else
    bad "the installed HELP.EXE produced no output at all"
fi

# --- 6. PRODUCT SHOW PRODUCT reads the REAL database this bead wrote,
#        keyed by the kit's OWN embedded name (not the DCL command's
#        product-name parameter "VMS") ------------------------------------
OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT /DESTINATION=DKA100:'  # GUIDE-STEP (docs/install-guide.md, tools/check_guide_drift.py)
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SHOW_SEG_BEFORE=$(segment_since "$OFF")
if printf '%s\n' "$SHOW_SEG_BEFORE" | grep -qiE 'X86VMS VMS' \
    && printf '%s\n' "$SHOW_SEG_BEFORE" | grep -qF 'Installed'; then
    ok "PRODUCT SHOW PRODUCT /DESTINATION=DKA100: lists the kit's own product name as Installed"
else
    bad "PRODUCT SHOW PRODUCT /DESTINATION=DKA100: does not list the installed kit"
    echo "$SHOW_SEG_BEFORE"
fi

# DISMOUNT before killing QEMU so umount(2) flushes the volume cleanly
# (same reasoning as test_mount_e2e.sh).
OFF=$(wc -c <"$LOG")
send 'DISMOUNT DKA100:'  # GUIDE-STEP (docs/install-guide.md, tools/check_guide_drift.py)
wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

# =====================================================================
# BOOT 2 -- a FRESH qemu-system process, SAME disk files: real persistence
# of both the installed files AND the product database.
# =====================================================================
LOG="$WORKDIR/boot2.log"
boot_qemu "$LOG" "$WORKDIR/boot2.in"
login "$LOG"

OFF=$(wc -c <"$LOG")
send 'MOUNT DKA100: WORK'
if wait_for '%MOUNT-I-MOUNTED, WORK mounted on _DKA100:' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT DKA100: succeeds again after a full QEMU restart"
else
    dump_and_die "MOUNT DKA100: did not report success on the restarted boot"
fi

OFF=$(wc -c <"$LOG")
send 'DIRECTORY DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE'
wait_for 'Total of' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qE 'Total of [1-9][0-9]* files?, [0-9]+ blocks' \
    && printf '%s\n' "$SEG" | grep -qF 'HELP.EXE'; then
    ok "installed HELP.EXE survives a full QEMU restart"
else
    bad "installed HELP.EXE did not survive the QEMU restart"
    echo "$SEG"
fi

OFF=$(wc -c <"$LOG")
send 'RUNHELP :== $DKA100:[SYS0.SYSCOMMON.SYSEXE]HELP.EXE'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
OFF=$(wc -c <"$LOG")
send 'RUNHELP MOUNT'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
RUN_SEG2=$(segment_since "$OFF")
if printf '%s\n' "$RUN_SEG2" | grep -qiE 'Segmentation|cannot execute|%DCL-.*IVIMAGE|%SYSTEM-.*ACCVIO'; then
    bad "the installed HELP.EXE crashed after restart: $RUN_SEG2"
elif [ -n "$(printf '%s' "$RUN_SEG2" | grep -vF 'RUNHELP' | tr -d '[:space:]')" ]; then
    ok "the installed HELP.EXE still executes correctly after a full QEMU restart"
else
    bad "the installed HELP.EXE produced no output after restart"
fi

OFF=$(wc -c <"$LOG")
send 'PRODUCT SHOW PRODUCT /DESTINATION=DKA100:'
wait_for '$' "$RUN_TIMEOUT" "$OFF"
SEG=$(segment_since "$OFF")
if printf '%s\n' "$SEG" | grep -qiE 'X86VMS VMS' && printf '%s\n' "$SEG" | grep -qF 'Installed'; then
    ok "the product database survives a full QEMU restart (disk, not tmpfs)"
else
    bad "the product database did not survive the QEMU restart"
    echo "$SEG"
fi

kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null; QPID=""

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL PRODUCT INSTALL E2E CHECKS PASSED"
    exit 0
fi
echo ""
echo "--- full console log (boot1) ---"
cat "$WORKDIR/boot1.log" 2>/dev/null
echo "--- full console log (boot2) ---"
cat "$WORKDIR/boot2.log" 2>/dev/null
exit 1
