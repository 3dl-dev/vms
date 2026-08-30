#!/bin/bash
# test_install_menu.sh - the faithful install menu, driven live over the
# console, against a real vms.ko + vmsfs.ko (vms-dcf, docs/design-
# vms-faithful-install.md sec2/sec3.3, docs/oracle/
# installation-media-vax73-alpha84.md sec3a/sec5).
#
# WHAT THIS PROVES, AND WHY NOTHING EARLIER PROVES IT. Before this bead,
# there was no OVMX$INSTALL.COM at all: the distribution image booted
# straight to a login prompt (test_distrib_boot.sh), and MOUNT/PRODUCT
# INSTALL/INITIALIZE were only exercised as raw DCL commands
# (test_mount_e2e.sh, test_product_install_e2e.sh) -- nothing drove them
# through an operator-facing menu, and nothing proved a password set
# DURING install actually lands on the TARGET's own SYSUAF (as opposed to
# the distribution disk's).
#
#   1. Boot the DISTRIBUTION image (VDA0:) plus a second, blank-but-
#      formatted disk (VDA100:, label WORK -- pre-formatted on the HOST
#      with a real INITIALIZE.EXE, same convention as test_mount_e2e.sh /
#      test_product_install_e2e.sh, and for the same reason: OVMX's own
#      INITIALIZE DCL verb does not resolve a VMS device name to its
#      backing block device yet -- see the "PREREQUISITE GAP" comment in
#      OVMX$INSTALL.COM's header and this test's own INITIALIZE-branch
#      probe below).
#   2. NO LOGIN. The menu appears as the first thing on the console,
#      before Username: -- proving OVMX$INSTALL.COM is wired into the
#      DISTRIBUTION disk's SYSTARTUP_VMS.COM (not the installed target's).
#   3. Drive the menu EXACTLY as an operator would, PRESERVE branch (the
#      oracle's own bracketed default): INITIALIZE-or-PRESERVE, target
#      device, volume label, the "Is this OK?" confirmation gate, then
#      MOUNT/PRODUCT INSTALL run for real underneath.
#   4. The install asks for a NEW SYSTEM password (never echoed), then
#      drives AUTHORIZE against the target (DEFINE/JOB SYS$SYSTEM
#      redirects it there first, and SHOW LOGICAL independently confirms
#      the redirection took -- see the debug trail in the PR body). The
#      WRITE itself is NOT scored as pass/fail here -- see "NOT PROVEN"
#      below.
#   5. Dismount, kill QEMU.
#   6. Boot a FRESH qemu-system process against ONLY the target disk
#      (VDA100:, now VDA0: to that boot) using the ordinary initramfs --
#      i.e. boot it exactly like any other OVMX system disk, no
#      distribution disk involved. Proves the target's own
#      SYSTARTUP_VMS.COM does NOT run the install menu (the two-variant
#      staging, sec3.3) and that it reaches an ordinary login prompt.
#      Login-with-the-installed-password is NOT attempted here (see below).
#
# NOT PROVEN BY THIS RUN, AND WHY -- three prerequisite gaps this item
# found while building the procedure (ground-sourced, reported not fixed;
# CONSTRAINTS: pure DCL, no bash escape, no silent workaround):
#
#   (a) INITIALIZE <device>: <label> does not resolve a VMS device name to
#       its backing block device (cmd_initialize never calls
#       vms_kif_disk_resolve() the way cmd_mount does). Directly on the
#       host: `INITIALIZE.EXE VDA100: LABEL 8` creates and formats a
#       REGULAR FILE literally named "VDA100:" in the current directory
#       and reports %INIT-I-COMPLETE -- a silent fake success. Blocks
#       proving the INITIALIZE branch (only PRESERVE is exercised here).
#
#   (b) AUTHORIZE.EXE, forked via dcl_exec_utility(), never learns about a
#       device MOUNTed by its DCL parent -- only SYSDISK is registered in
#       its own per-process vmsfs device table at its own startup. SYS$
#       SYSTEM correctly resolves to the target via LNM (SHOW LOGICAL
#       SYS$SYSTEM shows the redirected value, confirmed live), but
#       AUTHORIZE's own file layer cannot open anything through it, so it
#       reports %UAF-E-NOSUCHUSER for an account that is really there.
#
#   (c) Independently of (b): plain RMS/POSIX file CREATE on a REAL
#       vmsfs.ko-mounted volume was found to fail outright in this
#       investigation -- `CREATE VDA0:[SYSEXE]PROBE.TXT` on the
#       DISTRIBUTION disk itself (no target, no redirection) returns
#       %RMS-E-CRE. SET PASSWORD (which does not fork) hits the same
#       wall via sysuaf_write_record()'s fopen()+rename() and reports
#       %UAF-E-WRITEFAIL, even for the caller's OWN account on its OWN
#       disk. tests/libvms/test_sysuaf_write_veracity.c's own header
#       explains why this was never caught: "WHY THIS IS A HOST TEST, NOT
#       A tests/qemu ONE" -- the real vmsfs.ko create path had never been
#       ground-source tested before this run.
#
# Together, (b) and (c) mean NEITHER of OVMX's two password-write
# mechanisms (AUTHORIZE, SET PASSWORD) can currently write a new password
# to ANY SYSUAF.DAT on a real mounted vmsfs volume -- not just the cross-
# device target case. OVMX$INSTALL.COM's design (AUTHORIZE against the
# target, matching the item spec) is correct and will work once these are
# fixed; this test captures the current, honest, non-silent failure for
# the record instead of asserting a pass that would not be true.
#
# Usage (run INSIDE the bootable image, like test_product_install_e2e.sh):
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   docker run --rm -v $PWD/tests/qemu/test_install_menu.sh:/test.sh:ro \
#       -v /path/to/formatted/dka100/dir:/work \
#       --entrypoint bash ovmx-boot /test.sh
# (see tests/qemu/run_install_menu.sh, which formats the second disk on
# the host and invokes this the same way run_product_install_e2e.sh does.)
#
# Env knobs: BOOT_TIMEOUT (default 90), RUN_TIMEOUT (default 90).
# Exit 0 = every ground-source assertion above passed.

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"
RUN_TIMEOUT="${RUN_TIMEOUT:-90}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-install-media.img
VDA100_SRC=/work/dka100.img
ARCH=$(uname -m)
NEW_PASSWORD="NEWSYSPW1"

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG" "$VDA100_SRC"; do
    [ -f "$f" ] || { echo "FATAL: $f not found - run this inside the ovmx-boot image with /work bind-mounted (see header)"; exit 1; }
done
command -v "$QEMU" >/dev/null 2>&1 || { echo "FATAL: $QEMU not available"; exit 1; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== install menu e2e: OVMX\$INSTALL.COM against a real vms.ko executive + Files-11 ACP (vms-dcf) ==="
echo "arch=$ARCH qemu=$QEMU kernel=$KERNEL initrd=$INITRD"

WORKDIR=$(mktemp -d)
DISK0="$WORKDIR/dka0.img"
DISK1="$WORKDIR/dka100.img"
cp "$DISTRIB_IMG" "$DISK0"
cp "$VDA100_SRC" "$DISK1"

QPID=""
# kill_boot - kill a boot_qemu background job AND the real qemu-system-*
# grandchild it spawns. QPID is the PID of the `timeout` WRAPPER around
# qemu (boot_qemu's own "timeout ... $QEMU ... &"), not qemu itself --
# found the hard way: `timeout` only forwards a signal to its child if it
# gets the chance to run its own handler, so this must never be `kill -9`
# on QPID alone (that kills the wrapper instantly with no forwarding,
# orphaning qemu-system-x86_64 still holding the disk image's write lock,
# which then fails the NEXT boot with "Failed to get 'write' lock"). Kill
# the child explicitly first, then the wrapper, then reap both.
kill_boot() {
    local pid="$1"
    [ -n "$pid" ] || return 0
    pkill -TERM -P "$pid" 2>/dev/null
    kill "$pid" 2>/dev/null
    sleep 1
    pkill -9 -P "$pid" 2>/dev/null
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}
cleanup() { kill_boot "$QPID"; rm -rf "$WORKDIR"; }
trap cleanup EXIT

boot_qemu() {  # boot_qemu <disk0> <disk1-or-empty> <log-file> <fifo-path>
    local d0="$1" d1="$2" log="$3" fifo="$4"
    # Close any fd4 left open from a PRIOR boot_qemu call before rebinding
    # it to a new fifo -- reusing fd4 across boots without an explicit
    # close in between was found to wedge the second boot silently.
    exec 4>&- 2>/dev/null || true
    rm -f "$log" "$fifo"
    mkfifo "$fifo"
    local drives=(-drive file="$d0",format=raw,if=virtio,cache=writethrough)
    [ -n "$d1" ] && drives+=(-drive file="$d1",format=raw,if=virtio,cache=writethrough)
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + RUN_TIMEOUT * 12 + 60))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 1 -nic none -nodefaults -serial stdio \
        "${drives[@]}" \
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
    kill_boot "$QPID"; QPID=""
    exit 1
}

# vms-2213-style CR feed: LOGINOUT/DCL wait for the operator's RETURN before
# presenting a prompt; a single CR at t=0 can be lost while the guest serial
# driver comes up. Feed one per second until the pattern appears. Bounded.
wake_for() {  # wake_for <pattern> <log-file>
    local pat="$1" logf="$2" w=0
    until grep -qaF "$pat" "$logf" 2>/dev/null || [ "$w" -ge 120 ]; do
        send ''; sleep 1; w=$((w + 1))
    done
}

# =====================================================================
# BOOT 1 -- the DISTRIBUTION disk. No login: the menu is the first thing.
# =====================================================================
LOG="$WORKDIR/boot1.log"
boot_qemu "$DISK0" "$DISK1" "$LOG" "$WORKDIR/boot1.in"

wake_for 'OVMX$INSTALL Option' "$LOG"
if wait_for 'OVMX$INSTALL Option' "$BOOT_TIMEOUT" 0; then
    ok "the DISTRIBUTION disk boots straight into the install menu -- no login required (oracle sec3a)"
else
    dump_and_die "the install menu never appeared within ${BOOT_TIMEOUT}s"
fi
BOOT_SEG=$(segment_since 0)
if printf '%s\n' "$BOOT_SEG" | grep -qF 'Username:'; then
    bad "a login prompt appeared BEFORE the install menu (should be menu-first, oracle sec3a)"
else
    ok "no login prompt precedes the install menu"
fi
for item in '1)  Install or reconfigure OVMX' '4)  Show installed products' \
            '8)  Execute DCL commands and procedures' '9)  Shut down this system'; do
    if printf '%s\n' "$BOOT_SEG" | grep -qF "$item"; then
        ok "menu offers: $item"
    else
        bad "menu missing: $item"
    fi
done
# Honest omission: the oracle's items 2/3/5/6/7 (layered-product catalog/
# install/reconfigure/remove, patches/PAKs) must NOT appear -- OVMX has none
# of those facilities.
for absent in 'layered products' 'patches' 'Product Authorization'; do
    if printf '%s\n' "$BOOT_SEG" | grep -qiF "$absent"; then
        bad "menu offers something OVMX cannot honor: $absent"
    else
        ok "menu correctly omits: $absent"
    fi
done

# --- Option 1: install ---------------------------------------------------
OFF=$(wc -c <"$LOG")
send '1'
if wait_for 'INITIALIZE or to PRESERVE' "$RUN_TIMEOUT" "$OFF"; then
    ok "option 1 asks INITIALIZE-or-PRESERVE (oracle sec3a)"
else
    dump_and_die "option 1 did not ask INITIALIZE-or-PRESERVE"
fi

# PRESERVE -- the oracle's own bracketed default. INITIALIZE is exercised
# separately (see header) and hits the reported prerequisite gap.
OFF=$(wc -c <"$LOG")
send 'PRESERVE'
if wait_for 'Enter device name for target disk' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRESERVE accepted, asks for the target device (oracle sec3a)"
else
    dump_and_die "did not proceed to the target-device prompt after PRESERVE"
fi

OFF=$(wc -c <"$LOG")
send 'VDA100:'
if wait_for 'Enter volume label for target system disk' "$RUN_TIMEOUT" "$OFF"; then
    ok "target device accepted, asks for the volume label (oracle sec3a)"
else
    dump_and_die "did not proceed to the volume-label prompt"
fi

OFF=$(wc -c <"$LOG")
send 'WORK'
if wait_for 'Is this OK' "$RUN_TIMEOUT" "$OFF"; then
    ok "volume label accepted, reaches the confirmation gate (oracle sec3a)"
else
    dump_and_die "did not reach the confirmation gate"
fi
GATE_SEG=$(segment_since "$OFF")
if printf '%s\n' "$GATE_SEG" | grep -qF 'VDA100:'; then
    ok "the confirmation gate names the target device (naming the operation, per item spec)"
else
    bad "the confirmation gate does not name the target device"
fi

OFF=$(wc -c <"$LOG")
send 'YES'
if wait_for '%MOUNT-I-MOUNTED' "$RUN_TIMEOUT" "$OFF"; then
    ok "MOUNT (real mount(2) via the executive) succeeds from inside the menu"
else
    dump_and_die "MOUNT did not report success from inside the menu"
fi

if wait_for '%PCSI-I-DONE' "$RUN_TIMEOUT" "$OFF"; then
    ok "PRODUCT INSTALL (real kit onto the real target) reports %PCSI-I-DONE"
else
    dump_and_die "PRODUCT INSTALL did not reach %PCSI-I-DONE"
fi
INSTALL_SEG=$(segment_since "$OFF")
if printf '%s\n' "$INSTALL_SEG" | grep -qiE '%PCSI-[EF]-|%MOUNT-[EF]-'; then
    bad "install transcript contains an error despite reporting DONE"
    echo "$INSTALL_SEG"
else
    ok "install transcript carries no MOUNT/PCSI error"
fi

# --- SYSTEM password: asked, and driven at AUTHORIZE against the target -
# NOTE: no OFF reset here -- the password prompt is automatic DCL output
# continuing from PRODUCT INSTALL with no intervening operator send(), so a
# fresh `wc -c` here would race the guest (it can print the prompt before
# this shell gets back around to measuring the log) and miss it. Reuse the
# OFF already in scope from the MOUNT/PRODUCT INSTALL step above.
if wait_for 'Password for SYSTEM account' "$RUN_TIMEOUT" "$OFF"; then
    ok "install asks for the SYSTEM password"
else
    dump_and_die "install never asked for the SYSTEM password"
fi
send "$NEW_PASSWORD"
if wait_for 'Reenter password for verification' "$RUN_TIMEOUT" "$OFF"; then
    ok "asks for password verification"
else
    dump_and_die "did not ask to reenter the password"
fi
send "$NEW_PASSWORD"

# The procedure drives AUTHORIZE NON-INTERACTIVELY via inline SYS$INPUT
# (vms-963): OVMX$INSTALL.COM feeds "MODIFY SYSTEM/PASSWORD='OVMX_PW1'" + EXIT
# as data lines after the RUN, and DCL apostrophe-substitutes the entered
# password into that line before AUTHORIZE reads it. There is no operator
# typing at UAF> -- so this test does NOT send MODIFY/EXIT; it asserts the
# write instead. AUTHORIZE opens the TARGET's SYSUAF (SYS$SYSTEM redirected via
# DEFINE/JOB to the rooted [SYS0.SYSCOMMON.SYSEXE]) and reports %UAF-I-SAVED on
# EXIT when the record was modified.
if wait_for 'UAF>' "$RUN_TIMEOUT" "$OFF"; then
    ok "AUTHORIZE starts against the target (SYS\$SYSTEM redirected via DEFINE/JOB)"
else
    dump_and_die "AUTHORIZE never started"
fi
if wait_for '%UAF-I-SAVED' "$RUN_TIMEOUT" "$OFF"; then
    ok "AUTHORIZE persisted the install-set SYSTEM password to the TARGET SYSUAF (%UAF-I-SAVED)"
else
    dump_and_die "AUTHORIZE did not report %UAF-I-SAVED -- the install-set password did not persist to the target SYSUAF"
fi
UAF_SEG=$(segment_since "$OFF")
if printf '%s\n' "$UAF_SEG" | grep -qE '%UAF-E-'; then
    bad "AUTHORIZE reported a UAF error writing the target SYSUAF"
    echo "$UAF_SEG"
else
    ok "AUTHORIZE wrote the target SYSUAF with no %UAF-E- error"
fi
if wait_for 'Enter SCSNODE' "$RUN_TIMEOUT" "$OFF"; then
    ok "returns from AUTHORIZE and continues the procedure (asks for SCSNODE)"
else
    dump_and_die "did not return from AUTHORIZE / never asked for SCSNODE"
fi

# The AUTHORIZE-against-target write is now ASSERTED above (%UAF-I-SAVED, no
# %UAF-E-). The two prerequisite gaps that formerly blocked it are fixed on
# base -- AUTHORIZE seeing the parent MOUNT (vms-8b6/#385) and RMS CREATE on a
# real vmsfs volume (vms-581/#378) -- and the last piece, the install-set
# password reaching the TARGET's rooted SYSUAF and being fed to AUTHORIZE
# non-interactively, is vms-963. BOOT 2 below closes the loop by logging in
# with that install-set password.
echo "--- AUTHORIZE-against-target segment (write asserted above) ---"
segment_since "$OFF"

# --- SYSTEM password prompts: never echoed --------------------------------
PW_SEG=$(segment_since "$OFF")
if printf '%s\n' "$PW_SEG" | grep -qF "$NEW_PASSWORD"; then
    bad "THE PLAINTEXT PASSWORD WAS ECHOED TO THE CONSOLE -- SET TERMINAL/NOECHO did not suppress it (a further gap this item found and reported, see PR body)"
else
    ok "the SYSTEM password was never echoed to the console"
fi

# Drive the SCS/SYSGEN sub-step FOR REAL (vms-597). Answering SCSNODE and
# SCSSYSTEMID non-blank makes OVMX$INSTALL.COM redirect SYS$SYSTEM at the
# target's ROOTED [SYS0.SYSCOMMON.SYSEXE] and RUN SYS$SYSTEM:SYSGEN.EXE
# against it. Before vms-597 the redirect named the flat, pre-rooted
# [SYSEXE] -- a directory a vms-96ec-rooted target does not have -- so the
# RUN failed "%DCL-E-IVIMAGE, image not found - SYS$SYSTEM:SYSGEN.EXE" and
# the whole SCS step could not run. This was previously UNCAUGHT because the
# step was answered blank (fall through SKIP_SCS); un-skipping it is what
# exercises the fix. NOTE: reaching here at all also needs the identical
# rooted-path fix vms-963 applies to the AUTHORIZE step above (same bug,
# separate block) -- both must be on the disk for the flow to get past
# AUTHORIZE to the SCSNODE prompt.
SCS_NODE="OVMXQA"        # <= 6 chars (SCSNODE max); distinct from the OVMX kit default
SCS_ID="1025"
send "$SCS_NODE"
if wait_for 'Enter SCSSYSTEMID' "$RUN_TIMEOUT" "$OFF"; then
    ok "asks for SCSSYSTEMID"
else
    dump_and_die "never asked for SCSSYSTEMID"
fi
OFF=$(wc -c <"$LOG")
send "$SCS_ID"
# SYSGEN activating and RUNNING against the rooted target is the DIRECT proof
# of the vms-597 fix: the image resolved through the rooted SYS$SYSTEM redirect
# (no %DCL-E-IVIMAGE) AND it was driven by the procedure's own INLINE SYS$INPUT.
# OVMX$INSTALL.COM now feeds USE CURRENT / SET SCSNODE / SET SCSSYSTEMID /
# WRITE CURRENT / EXIT as data lines after the RUN (the same idiom AUTHORIZE
# uses), so an UNATTENDED install configures the node with no operator typing.
# The console is NO LONGER used to drive the SYSGEN REPL -- sending REPL commands
# here would leak past SYSGEN (which reads its data block, not the terminal) to
# the menu after SYSGEN exits. Answering the two INQUIRE prompts non-blank
# (SCS_NODE/SCS_ID above) is the entire operator interaction.
#
# ANTI-LARP (vms-dd15, INV-6): we anchor on the runtime BANNER SYSGEN.EXE prints
# at startup ("OpenVMS System Generation Utility", vms_sysgen.c) and NEVER on the
# bare literal "SYSGEN>". Under the inline feed SYSGEN's SYS$INPUT is a non-tty
# tmpfile (dcl_sysinput_setup), so vms_sysgen.c emits this banner UNCONDITIONALLY
# (like AUTHORIZE's %UAF-I-AUTHVERSION); it is printed only by SYSGEN.EXE itself
# at runtime, never by the procedure, so it cannot false-pass on instruction text.
if wait_for 'OpenVMS System Generation Utility' "$RUN_TIMEOUT" "$OFF"; then
    ok "SYSGEN.EXE resolves, activates and runs against the rooted target (runtime banner emitted) -- no %DCL-E-IVIMAGE (vms-597)"
else
    dump_and_die "SYSGEN.EXE did not start against the target (vms-597 regressed: %DCL-E-IVIMAGE / unresolved SYS\$SYSTEM redirect / empty inline SYS\$INPUT -- no runtime banner)"
fi
# The procedure's OWN inline-SYS$INPUT block drives SYSGEN's REPL -- the test
# does NOT type these at the console. WRITE CURRENT emits %SYSGEN-I-WRITTEN only
# after it serializes the parameter set to the target's OVMXVMSSYS.PAR at
# runtime, so this proves the inline feed actually reached and executed
# WRITE CURRENT against the target -- another token SYSGEN.EXE alone prints,
# which the procedure never emits, so it too cannot false-pass on echoed text.
if wait_for '%SYSGEN-I-WRITTEN' "$RUN_TIMEOUT" "$OFF"; then
    ok "SYSGEN's inline-SYS\$INPUT WRITE CURRENT ran against the target and reported %SYSGEN-I-WRITTEN"
else
    dump_and_die "SYSGEN did not report %SYSGEN-I-WRITTEN -- the inline SYS\$INPUT never executed WRITE CURRENT against the target"
fi
if wait_for '%DISMOUNT-I-DISMOUNTED' "$RUN_TIMEOUT" "$OFF"; then
    ok "the procedure dismounts the target itself after configuring SCSNODE/SCSSYSTEMID"
else
    dump_and_die "did not dismount the target after the SYSGEN step"
fi
SCS_SEG=$(segment_since "$OFF")
if printf '%s\n' "$SCS_SEG" | grep -qE '%DCL-E-IVIMAGE|%SYSGEN-[EF]-'; then
    bad "the SCS/SYSGEN segment carries an IVIMAGE or a SYSGEN error"
    echo "$SCS_SEG"
else
    ok "the SCS/SYSGEN segment carries no IVIMAGE and no SYSGEN error"
fi
if wait_for 'installation is complete' "$RUN_TIMEOUT" "$OFF"; then
    ok "reports the installation complete and returns to the menu"
else
    bad "did not report installation complete"
fi

kill_boot "$QPID"; QPID=""

# =====================================================================
# BOOT 2 -- the installed target boots as an ORDINARY system disk (no
# distribution disk, no menu). Proves the two-variant SYSTARTUP_VMS.COM
# staging: the target's own copy does NOT invoke the install menu, unlike
# the distribution disk's (Boot 1 above). It then LOGS IN as SYSTEM with the
# INSTALL-SET password (vms-963): a successful login is positive proof the
# password reached the target's own SYSUAF and persisted the DISMOUNT/flush,
# since the kit default (MANAGER) would no longer authenticate.
# =====================================================================
LOG="$WORKDIR/boot2.log"
boot_qemu "$DISK1" "" "$LOG" "$WORKDIR/boot2.in"

wake_for 'Username:' "$LOG"
if wait_for 'Username:' "$BOOT_TIMEOUT" 0; then
    ok "the installed target boots as an ordinary system disk and reaches login (no install menu on the target)"
else
    dump_and_die "the installed target never reached a login prompt"
fi
TB=$(segment_since 0)
if printf '%s\n' "$TB" | grep -qF 'OVMX$INSTALL Option'; then
    bad "the installed TARGET's own boot ran the install menu (SYSTARTUP_VMS.COM variant leaked onto the target)"
else
    ok "the installed target's own SYSTARTUP_VMS.COM does NOT run the install menu (two-variant staging, sec3.3)"
fi
# End-to-end proof of the vms-597 SCS step: the SCSNODE the install wrote to
# the target's OVMXVMSSYS.PAR (via SYSGEN WRITE CURRENT through the rooted
# SYS$SYSTEM redirect) is what the booted target now reports as its node
# identity -- ovmx_init prints "%OVMX-I-SCSNODE, node name <X> set from
# SYS$SYSTEM:OVMXVMSSYS.PAR". Seeing the operator-chosen $SCS_NODE (not the
# OVMX kit default) proves the install's SCS configuration reached and
# persisted to the target's own parameter file.
if printf '%s\n' "$TB" | grep -qF "node name $SCS_NODE"; then
    ok "the install-set SCSNODE ($SCS_NODE) persisted to the target and the booted target reads it from OVMXVMSSYS.PAR (end-to-end vms-597)"
else
    bad "the install-set SCSNODE ($SCS_NODE) did NOT appear on the target's own boot -- SYSGEN WRITE CURRENT did not persist to the target's OVMXVMSSYS.PAR"
fi

# Close the loop: log in as SYSTEM with the INSTALL-SET password (NOT the kit
# default). A successful login is positive proof the install-set password
# persisted to the target's own SYSUAF across the DISMOUNT/flush (vms-963).
OFF2=$(wc -c <"$LOG")
send 'SYSTEM'
if wait_for 'Password:' "$BOOT_TIMEOUT" "$OFF2"; then
    :
else
    dump_and_die "the booted target gave no Password: prompt after Username SYSTEM"
fi
send "$NEW_PASSWORD"
if wait_for 'Welcome to OpenVMX' "$BOOT_TIMEOUT" "$OFF2"; then
    ok "SYSTEM logs in on the booted target with the INSTALL-SET password, not the kit default (vms-963 end-to-end)"
else
    dump_and_die "SYSTEM could not log in with the install-set password -- it did not persist to the booted target"
fi

kill_boot "$QPID"; QPID=""

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
echo "(AUTHORIZE-against-target write, and login with the installed password, are NOT"
echo " scored above -- see PR body. Everything else -- menu-first boot, oracle-pinned"
echo " menu text/omissions, INITIALIZE-or-PRESERVE, target/label/confirmation gate,"
echo " real MOUNT, real PRODUCT INSTALL, password prompts, the SCS/SYSGEN step"
echo " (SYSGEN.EXE resolves against the rooted target, no %DCL-E-IVIMAGE, and the"
echo " install-set SCSNODE persists to the target -- vms-597), and the two-variant"
echo " SYSTARTUP_VMS.COM staging -- is scored and ground-sourced.)"
if [ "$FAIL" -eq 0 ]; then
    echo "ALL SCORED INSTALL MENU E2E CHECKS PASSED"
    exit 0
fi
echo ""
echo "--- full console log (boot1) ---"; cat "$WORKDIR/boot1.log" 2>/dev/null
echo "--- full console log (boot2) ---"; cat "$WORKDIR/boot2.log" 2>/dev/null
exit 1
