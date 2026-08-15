#!/usr/bin/env python3
#
# drive_proctab_vax.py - process table / $GETJPI / $PROCESS_SCAN runtime proof
# (rd vms-2e0, parent vms-945e, epic vms-8e8) proven cross-process against a
# REAL /dev/vms on NetBSD/vax under SIMH (INV-6 honest).
#
# P4-B (vms-f78bb, done) proved ONE ioctl round-trips through a live /dev/vms
# on real NetBSD/vax. P4-E (vms-4e7, done) closed the INV-6-decisive gap for
# EVENT FLAGS: a per-process userspace FAKE could answer P4-B's single ioctl
# too, but it cannot fake a COMMON flag being visible to a genuinely different
# process. This driver is the PROCTAB analogue of drive_eflag_vax.py and the
# VAX analogue of tests/netbsd/drive_netbsd_p4a.py's PROCTAB phase: process A
# registers a name via $SETPRN and stays alive; a DIFFERENT process B resolves
# it by name with $GETJPI (VMS_JPI_SEL_PRCNAM); a DIFFERENT process C
# enumerates the shared table with $PROCESS_SCAN and finds the SAME row (same
# pid, same uic) -- proving the row lives in the executive's shared process
# table (src/kernel-core/vms_proctab.c, compiled into the NetBSD `vms'
# pseudo-device), not in either reader (CLAUDE.md Rule 9).
#
# THIS IS THE PILOT ADOPTION OF vaxharness.py (rd vms-cf5). Every console wait
# in the boot handshake below goes through safe_expect() (never a hand-rolled
# child.expect() with TIMEOUT/EOF in the pattern list -- that is the crash
# class vaxharness.py exists to kill), and the driver's pass/fail decision is
# recorded as a Proof of StepResults, emitted as one JSON line at the end. The
# WRAPPER (run-proctab.sh) applies negctl_gate.sh's vaxharness_negctl_gate()
# to this script's raw process exit code -- this script itself is NEVER
# negctl-mode-aware (see vaxharness.negctl_gate()'s docstring): PROCTAB_SKIP_BG
# does not early-return a "negctl ok"; it just omits process A, so the SAME
# positive-assertion script below fails FOR REAL, and the ordinary nonzero
# exit code is what the wrapper inverts.
#
# WHY VAX SPECIFICALLY (mirrors vms-4e7's point for event flags). vax is ILP32
# / non-IEEE-float / ELF32 -- a width class the amd64 (LP64) P4-A proof cannot
# exercise. A struct-layout or width bug in the shared wire contract
# (src/kernel-netbsd/vms_proctab_nb.h -- e.g. vms_pid/uic as uint32_t) could
# compile clean and pass on every 64-bit OVMX target and only misbehave here.
# Scope is PROCTAB ONLY; the other boot-required facilities already proven
# cross-process on vax are event flags (vms-4e7); the rest (lnm/mbx/ast/
# access/lock) are the remaining children of vms-945e.
#
# ARTIFACTS + STAGING (identical shape to drive_devvms_vax.py / drive_eflag_
# vax.py): the module (vms.kmod, carrying the SAME vms_proctab.c the Linux
# vms.ko builds) and the process-table guest tool (vmsproctab,
# tests/netbsd/guest/vmsproctab.c, reaching /dev/vms through
# kif_transport_netbsd.c) are CROSS-BUILT on the host
# (tools/cross-vax/build-proctab-vax.sh) against the pinned NetBSD/vax kernel
# headers and DELIVERED into the guest on a second CD (rq2) -- the vax system
# disk cannot hold the comp+syssrc sets an in-guest build would need. The
# custom MODULAR kernel (GENERIC + options MODULAR, P4-B's compile-into-kernel
# fallback) is installed the same way P4-B's / P4-E's is; see
# tests/lab-vax/README.md "P4-B" for why plain modload does not work on stock
# vax GENERIC.
#
# SINGLE-USER BOOT (securelevel 0), same reason as P4-B/P4-E: at multiuser
# securelevel 1, secmodel_securelevel(9) denies KAUTH_SYSTEM_MODULE and modload
# of an out-of-tree module returns EPERM. This does not weaken the proof --
# the module still serves a REAL in-kernel /dev/vms, and INV-6 holds.
#
# THE CROSS-PROCESS PROOF (collapsed into a single in-guest shell command, the
# SAME loss-tolerant-transport technique drive_eflag_vax.py / drive_netbsd_
# p2c.py use for the lossy TCG/SIMH serial, rd vms-3e7): process A ($SETPRN
# P4PROC1, then blocks indefinitely so its PCB stays present) / process B
# ($GETJPI by name, must find A's row: name+pid+uic) / process C
# ($PROCESS_SCAN, must enumerate the SAME row -- same pid as B saw) / A killed.
#
# INV-6 NEGATIVE CONTROL (mandatory, built in): with NO module loaded (so
# /dev/vms is absent), vmsproctab must fail HONESTLY (SS$_NOSUCHDEV), never
# fake success.
#
# NEGATIVE CONTROL WITH TEETH (env-gated, mirrors P4-E's EFLAG_SKIP_SET /
# drive_netbsd_p4a.py's P4A_SKIP_PROCTAB_BG):
#   PROCTAB_SKIP_LOAD=1 : modload itself is skipped -> the whole proof must go
#                         RED the same way EFLAG_SKIP_LOAD does.
#   PROCTAB_SKIP_BG=1   : process A is never launched -> process B's by-name
#                         $GETJPI must find nothing -> the cross-process "B
#                         sees A's row" assertion must go RED.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting a
# real NetBSD/vax under SIMH to load and TEST a real kernel module is exactly
# what tests/qemu/ does for the Linux vms.ko and tests/netbsd/ does for amd64.
#
# The whole run is bounded by run-proctab.sh's hard `timeout`, and every
# in-guest command here has its own console deadline, so nothing hangs.

import os
import re
import sys
import signal
import subprocess
import traceback

import anita

# netbsd_console.py (the deterministic console, rd vms-2d9) lives under
# tests/netbsd/; the orchestrator sets OVMX_NETBSD_DIR to it.
sys.path.insert(0, os.environ.get("OVMX_NETBSD_DIR", "/netbsd"))
import netbsd_console

# vaxharness.py (rd vms-cf5) lives in THIS directory (tests/lab-vax), staged
# alongside this script by run-proctab.sh -- no sys.path insert needed.
from vaxharness import Proof, safe_expect


def log(msg):
    print("[drive_proctab_vax] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


_con = None


def console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def run(child, cmd, timeout, echo=True, retriable=True, bg_safe=False):
    return console(child).run(cmd, timeout, echo, retriable, bg_safe)


def phase_token(out, key):
    """Return VALUE from the LAST `KEY=VALUE' line a collapsed phase script
    emitted on its own line -- same helper drive_eflag_vax.py /
    drive_netbsd_p4a.py use. Last match wins so an echoed command line cannot
    shadow the real result."""
    val = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith(key + "="):
            val = s[len(key) + 1:].strip()
    return val


def build_source_iso(artifacts_dir, out_iso):
    """Bundle the cross-built artifacts into an ISO9660 image attached to the
    boot as a second CD (rq2). Always carries vms.kmod + vmsproctab; also
    carries the custom MODULAR kernel (netbsd-OVMX) when present, for the
    install-kernel session. The guest mounts it and copies them out."""
    for f in ("vms.kmod", "vmsproctab"):
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing cross-built artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXSRC",
           "-o", out_iso, artifacts_dir]
    log("building source ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("source ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


def boot_single_user(child, boot_deadline, proof):
    """Boot the cached disk SINGLE-USER via the ROM `>>>' monitor, then reach
    a raw `# ' shell. THE PILOT ADOPTION: every wait in this handshake goes
    through safe_expect() -- never a hand-rolled child.expect() with
    pexpect.TIMEOUT/EOF in the pattern list (vaxharness.py bug #1). A TIMEOUT/
    EOF/ERROR here is recorded as a failed StepResult and raised so main()'s
    top-level handler reports it; safe_expect() itself never raises."""
    child.timeout = boot_deadline

    r = safe_expect(child, [r">>>"], timeout=boot_deadline)
    proof.step_from_expect("rom-monitor-prompt", r, 0,
                            detail="SIMH `>>>' ROM monitor prompt")
    if not r.ok:
        raise RuntimeError("ROM monitor prompt (`>>>') never appeared: %s"
                            % (r.error or r.kind))

    child.send("B/R5:2 DUA0\r")

    r = safe_expect(
        child,
        [r"Enter pathname of shell or RETURN for /bin/sh:", r"# "],
        timeout=boot_deadline)
    # Either pattern is a legitimate outcome here (sysinst prompts for a
    # shell pathname on some builds, drops straight to `# ' on others) --
    # so this is recorded directly rather than via step_from_expect() (which
    # asserts one SPECIFIC expected index; there is no single "expected" one
    # in this two-way branch).
    proof.record("single-user-shell-or-sh-prompt", r.ok,
                  marker_seen=(str(r.index) if r.ok else None),
                  detail="expected 'Enter pathname...' (0) or '# ' (1); got kind=%s"
                         % r.kind)
    if not r.ok:
        raise RuntimeError("single-user shell prompt never appeared: %s"
                            % (r.error or r.kind))
    if r.index == 0:
        child.send("\n")
        r2 = safe_expect(child, [r"# "], timeout=boot_deadline)
        proof.step_from_expect("sh-prompt-after-pathname", r2, 0)
        if not r2.ok:
            raise RuntimeError("`# ' prompt never appeared after RETURN: %s"
                                % (r2.error or r2.kind))


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "vax")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    # Same workdir + sets the lab-vax install produced the cached disk with, so
    # a.install() is a cache-aware no-op (never a hot-path reinstall).
    workdir = env("NETBSD_WORKDIR", "/cache/anita-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-proctab-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))   # slow VAX boot
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "900"))

    # MODE:
    #   prove          (default) boot single-user, modload the vms module
    #                  against the MODULAR kernel, mknod /dev/vms, run the
    #                  cross-process process-table proof.
    #   install-kernel boot the current (GENERIC) kernel single-user and swap
    #                  in the custom MODULAR kernel (netbsd-OVMX) as /netbsd
    #                  (idempotent, no-op if /netbsd is already OVMX).
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("PROCTAB_SKIP_LOAD"))
    skip_bg = bool(env("PROCTAB_SKIP_BG"))

    proof = Proof("proctab-vax")

    log("NetBSD %s/%s  (OVMX/NetBSD process table on VAX under SIMH, vms-2e0)"
        % (version, arch))
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:           %s" % artifacts_dir)
    if skip_load:
        log("NEGATIVE CONTROL: PROCTAB_SKIP_LOAD set -- module will NOT be "
            "loaded; the whole proof must go RED")
    if skip_bg:
        log("NEGATIVE CONTROL: PROCTAB_SKIP_BG set -- process A will NOT "
            "register a name; process B's by-name $GETJPI must find nothing "
            "and the cross-process proof must go RED")

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s (run lab-vax install first)" % iso_path)
        return 3

    a = anita.Anita(
        dist=anita.ISO(iso_path, sets=sets),
        workdir=workdir,
        persist=True,          # keep/consume the cached disk; never rebuild it
    )

    build_source_iso(artifacts_dir, src_iso)

    child = None
    try:
        import pexpect

        # 1. Install (cache-aware no-op if the cached wd0.img already exists).
        log("ensuring cached NetBSD/vax disk is present (cache-aware install)...")
        a.install()
        log("cached disk present")

        # 2. Boot the cached disk SINGLE-USER, with our artifact CD attached as
        #    a SECOND CD (rq2). See P4-B (drive_devvms_vax.py) for why
        #    single-user (securelevel 0) is REQUIRED, not incidental.
        src_abs = os.path.abspath(src_iso)
        vmm_args = ["set rq2 cdrom", "attach -r rq2 " + src_abs]
        log("booting cached NetBSD/vax disk SINGLE-USER under SIMH with the "
            "proctab artifact CD on rq2 (deadline %ds)..." % boot_deadline)

        a.dist.set_workdir(a.workdir)
        a.n_cdrom = 0
        child = a.start_simh(vmm_args)
        boot_single_user(child, boot_deadline, proof)

        con = console(child)
        child.sendline("PATH=/sbin:/bin:/usr/sbin:/usr/bin; export PATH")
        child.expect(r"# ")
        # / may be dirty from a prior (killed) boot; fsck before remounting rw.
        child.sendline("fsck -y / 2>&1 | tail -1")
        child.expect(r"# ", timeout=cmd_timeout)
        child.sendline("mount -u -w /")
        child.expect(r"# ")
        child.sendline("stty -echo 2>/dev/null; true")
        child.expect(r"# ")
        con.set_unique_prompt()
        log("single-user root shell ready on NetBSD/vax")

        rc, out = run(child, "uname -srm; sysctl kern.securelevel", cmd_timeout)
        log("guest uname + securelevel: %s" % " | ".join(out.split()))

        # ---- install-kernel mode: swap /netbsd -> the MODULAR kernel --------
        if mode == "install-kernel":
            rc, out = run(child,
                          "ok=; for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                          "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                          "  test -e $dev || continue; "
                          "  if mount_cd9660 $dev /mnt 2>/dev/null; then "
                          "    if test -f /mnt/netbsd-OVMX; then ok=$dev; break; "
                          "    else umount /mnt 2>/dev/null; fi; "
                          "  fi; done; "
                          "test -n \"$ok\" || { echo NO_KERNEL_CD; exit 1; }; "
                          "test -f /netbsd.GENERIC || cp /netbsd /netbsd.GENERIC; "
                          "cp /mnt/netbsd-OVMX /netbsd.new && "
                          "mv /netbsd.new /netbsd && sync && umount /mnt && "
                          "ls -l /netbsd /netbsd.GENERIC",
                          cmd_timeout)
            if rc != 0:
                log("FAIL: could not install the MODULAR kernel onto /netbsd")
                proof.record("install-kernel", False, detail="cp/mv failed")
                proof.emit_result_line()
                return 30
            run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
            log("OK: installed MODULAR kernel as /netbsd (GENERIC kept as "
                "/netbsd.GENERIC), flushed to disk; the next boot runs it")
            proof.record("install-kernel", True)
            proof.emit_result_line()
            return 0

        # ---- stage the artifacts from the OVMX CD --------------------------
        run(child, "dmesg | grep -iE 'cd[0-9]|mscp|uba' | tail -20; "
                   "ls /dev/cd* /dev/rcd* 2>/dev/null", cmd_timeout)

        # NetBSD/vax names MSCP CD-ROMs `racd0'/`racd1'. Decision is on the
        # exit code (rc), never on a substring of `out': the VAX console
        # ECHOES the command, so any literal success marker in the command
        # text would spoof an in-`out' check.
        rc, out = run(child,
                      "mkdir -p /root/ovmx; ok=; "
                      "for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                      "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                      "  test -e $dev || continue; "
                      "  if mount_cd9660 $dev /mnt 2>/dev/null; then "
                      "    if test -f /mnt/vms.kmod; then ok=$dev; "
                      "      cp /mnt/vms.kmod /mnt/vmsproctab /root/ovmx/ && "
                      "      umount /mnt && chmod +x /root/ovmx/vmsproctab && break; "
                      "    else umount /mnt 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX module CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vms.kmod && test -x /root/ovmx/vmsproctab",
                      cmd_timeout)
        proof.record("stage-artifacts", rc == 0,
                      detail="mount OVMX CD, copy vms.kmod+vmsproctab")
        if rc != 0:
            log("FAIL: could not find/mount the OVMX artifact CD in the guest "
                "(see the CD-device diagnostics above)")
            proof.emit_result_line()
            return 10
        log("OK: staged vms.kmod + vmsproctab from the OVMX CD")

        PT = "/root/ovmx/vmsproctab"

        # ---- 3. INV-6 NEGATIVE CONTROL: no module loaded --------------------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "%s getjpi_name nobody" % PT, cmd_timeout)
        inv6_honest = (rc != 0 and "SS$_NOSUCHDEV" in out
                       and "NOT faking success" in out)
        if rc == 0:
            proof.record("inv6-module-absent", False,
                          detail="tool returned SUCCESS with no /dev/vms "
                                 "present -- the faked-success bug INV-6 forbids")
            log("FAIL (INV-6): tool returned SUCCESS with no /dev/vms present "
                "-- the faked-success bug INV-6 forbids")
            proof.emit_result_line()
            return 20
        if not inv6_honest:
            proof.record("inv6-module-absent", False,
                          detail="failed, but not via the honest "
                                 "device-unreachable path")
            log("FAIL (INV-6): tool failed, but not via the honest "
                "device-unreachable path (unexpected reason)")
            proof.emit_result_line()
            return 21
        proof.record("inv6-module-absent", True,
                      detail="SS$_NOSUCHDEV, NOT faking success")
        log("OK (INV-6): with no module loaded the tool FAILED HONESTLY "
            "(SS$_NOSUCHDEV), it did not fake success")

        # ---- 4. load the module + create the node ---------------------------
        # NEGCTL CONTRACT (mirrors drive_eflag_vax.py's EFLAG_SKIP_LOAD
        # exactly): when skip_load is set, modload is skipped and /dev/vms
        # stays ABSENT (removed by the INV-6 check above, never recreated) --
        # the driver does NOT special-case an early "negctl satisfied" exit
        # here. It falls through into the SAME cross-process proof every
        # positive run executes; that proof then fails FOR REAL (every
        # vmsproctab invocation hits the honest device-unreachable path), and
        # this function returns its ordinary nonzero failure code below.
        # run-proctab.sh's negctl-load mode (via negctl_gate.sh) is what
        # inverts "the driver exited nonzero" into "teeth confirmed" -- one
        # exit-code contract, shared by the positive and negative runs.
        if skip_load:
            log("SKIP: modload skipped (PROCTAB_SKIP_LOAD) -- /dev/vms stays "
                "absent; the cross-process proof below must fail for real")
            proof.record("modload", False,
                          detail="skipped (PROCTAB_SKIP_LOAD negative control)")
        else:
            rc, out = run(child,
                          "modunload vms 2>/dev/null; "
                          "modload /root/ovmx/vms.kmod && "
                          "MAJ=`dmesg | sed -n "
                          "'s/.*vms: registered, char major \\([0-9][0-9]*\\).*/\\1/p'"
                          " | tail -1` && "
                          "test -n \"$MAJ\" && rm -f /dev/vms && "
                          "mknod /dev/vms c $MAJ 0 && chmod 666 /dev/vms && "
                          "test -c /dev/vms && echo MODLOAD=OK || echo MODLOAD=FAIL",
                          cmd_timeout)
            if phase_token(out, "MODLOAD") != "OK":
                proof.record("modload", False, detail="modload/mknod failed")
                log("FAIL: could not load the module / create /dev/vms on "
                    "NetBSD/vax (this is the modules(9)-on-vax risk P4-B "
                    "documents; console output above)")
                proof.emit_result_line()
                return 14
            proof.record("modload", True)
            log("OK: module loaded and /dev/vms created on NetBSD/vax")

        # ---- 4a-4c. CROSS-PROCESS PROCESS-TABLE PROOF (collapsed) ----------
        # Same loss-tolerant-transport technique as drive_eflag_vax.py: ONE
        # in-guest command performs every step, asserts each property IN-GUEST
        # over reliable local pipes, and emits a SINGLE result token, so a lost
        # marker on the lossy SIMH serial cannot corrupt the proof. Process A
        # ($SETPRN, blocks indefinitely) is a DISTINCT `vmsproctab' invocation
        # from B ($GETJPI by name) and C ($PROCESS_SCAN); B and C's pid+uic
        # (procscan prints no uic, so only pid) are cross-checked so the two
        # readers are proven to see the SAME row, not merely "a" row.
        # NEGCTL CONTRACT (PROCTAB_SKIP_BG, mirrors PROCTAB_SKIP_LOAD /
        # EFLAG_SKIP_SET exactly -- and is the fix for the EXACT bug class
        # rd vms-cf5 documents): when skip_bg is set, process A is simply
        # never launched. The driver does NOT special-case an early
        # "negctl satisfied" exit here (drive_netbsd_p4a.py's older
        # P4A_SKIP_PROCTAB_BG mode did exactly that -- an early mode-aware
        # `return 30` for "found nothing, negctl confirmed" -- which is
        # precisely the driver/wrapper contract mismatch vaxharness.py's
        # negctl_gate() forbids). Instead this falls through into the SAME
        # positive-assertion script every positive run executes; with no A,
        # B's by-name $GETJPI finds nothing for real, so the SAME "getjpi"
        # failure branch below fires naturally and this function returns its
        # ordinary nonzero code. run-proctab.sh's negctl-bg mode (via
        # negctl_gate.sh, negctl=1) is what inverts that nonzero exit into
        # "teeth confirmed" -- one exit-code contract, shared by every mode.
        if skip_bg:
            log("SKIP: process A's $SETPRN skipped (PROCTAB_SKIP_BG) -- "
                "process B's by-name $GETJPI must find nothing next, and "
                "the cross-process assertion below must fail for real")
            proc = ("GJ=$(%s getjpi_name P4VPROC1 2>&1); "
                    "PS=$(%s procscan_find P4VPROC1 128 2>&1); "
                    "GPID=$(echo \"$GJ\" | sed -n 's/.*pid=\\([0-9]*\\).*/\\1/p'); "
                    "GUIC=$(echo \"$GJ\" | sed -n 's/.*uic=\\(0x[0-9a-fA-F]*\\).*/\\1/p'); "
                    "PPID=$(echo \"$PS\" | sed -n 's/.*pid=\\([0-9]*\\).*/\\1/p'); "
                    "F=''; "
                    "echo \"$GJ\" | grep -q 'PROCTAB GETJPI_FOUND name=P4VPROC1' "
                    "|| F=\"$F getjpi\"; "
                    "echo \"$PS\" | grep -q 'PROCTAB PROCSCAN_FOUND' || F=\"$F procscan\"; "
                    "[ -n \"$GPID\" ] && [ -n \"$PPID\" ] && [ \"$GPID\" = \"$PPID\" ] "
                    "|| F=\"$F pidmismatch\"; "
                    "[ -n \"$GUIC\" ] || F=\"$F nouic\"; "
                    "[ -z \"$F\" ] && echo \"PROCTAB=PASS pid=$GPID uic=$GUIC\" "
                    "|| echo \"PROCTAB=FAIL:$F\""
                    % (PT, PT))
        else:
            proc = ("rm -f /tmp/pt_a.out; "
                    "%s bg P4VPROC1 >/tmp/pt_a.out 2>&1 & AP=$!; sleep 2; "
                    "GJ=$(%s getjpi_name P4VPROC1 2>&1); "
                    "PS=$(%s procscan_find P4VPROC1 128 2>&1); "
                    "kill $AP 2>/dev/null; wait 2>/dev/null; "
                    "GPID=$(echo \"$GJ\" | sed -n 's/.*pid=\\([0-9]*\\).*/\\1/p'); "
                    "GUIC=$(echo \"$GJ\" | sed -n 's/.*uic=\\(0x[0-9a-fA-F]*\\).*/\\1/p'); "
                    "PPID=$(echo \"$PS\" | sed -n 's/.*pid=\\([0-9]*\\).*/\\1/p'); "
                    "F=''; "
                    "echo \"$GJ\" | grep -q 'PROCTAB GETJPI_FOUND name=P4VPROC1' "
                    "|| F=\"$F getjpi\"; "
                    "echo \"$PS\" | grep -q 'PROCTAB PROCSCAN_FOUND' || F=\"$F procscan\"; "
                    "[ -n \"$GPID\" ] && [ -n \"$PPID\" ] && [ \"$GPID\" = \"$PPID\" ] "
                    "|| F=\"$F pidmismatch\"; "
                    "[ -n \"$GUIC\" ] || F=\"$F nouic\"; "
                    "[ -z \"$F\" ] && echo \"PROCTAB=PASS pid=$GPID uic=$GUIC\" "
                    "|| echo \"PROCTAB=FAIL:$F\""
                    % (PT, PT, PT))
        rc, out = run(child, proc, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "PROCTAB")

        # NOTE: skip_bg is deliberately NOT special-cased from here on -- the
        # exact same interpretation below handles both modes (a PASS token is
        # a real pass; any FAIL is a real, ordinary failure), which is what
        # keeps this driver's exit code mode-agnostic. With process A never
        # launched, B's GETJPI naturally finds nothing and the SAME "getjpi"
        # branch just below fires for real -- there is no separate negctl
        # code path to drift out of sync with the wrapper.
        if not tok or not tok.startswith("PASS"):
            proof.record("proctab-cross-process", False, detail="tok=%s" % tok)
            if not tok or "getjpi" in tok:
                log("FAIL: process B (a DIFFERENT process) did NOT resolve "
                    "process A's name via $GETJPI -- the cross-process "
                    "shared-kernel-state proof did not hold on NetBSD/vax "
                    "(phase token: %s)" % tok)
                proof.emit_result_line()
                return 16
            if "procscan" in tok:
                log("FAIL: process C's $PROCESS_SCAN did not enumerate A's "
                    "row (phase token: %s)" % tok)
                proof.emit_result_line()
                return 17
            log("FAIL: process B and process C did not agree on the SAME "
                "row (pid/uic mismatch) -- the two readers may not be "
                "observing shared kernel state (phase token: %s)" % tok)
            proof.emit_result_line()
            return 18

        m = re.search(r"pid=(\d+)\s+uic=(0x[0-9a-fA-F]+)", tok)
        pid_seen = m.group(1) if m else "?"
        uic_seen = m.group(2) if m else "?"
        proof.record("proctab-cross-process", True,
                      detail="pid=%s uic=%s" % (pid_seen, uic_seen))
        log("OK: process B (a DIFFERENT process) resolved A's name via "
            "$GETJPI (pid=%s uic=%s) -- the process table is shared kernel "
            "state (INV-6)" % (pid_seen, uic_seen))
        log("OK: process C (a THIRD, different process) enumerated the "
            "shared process table with $PROCESS_SCAN and found the SAME "
            "row (pid=%s) that B resolved -- both readers observed ONE "
            "executive-resident row, not per-process state" % pid_seen)

        # ---- cleanup ------------------------------------------------------
        run(child, "modunload vms 2>/dev/null; true", cmd_timeout)
        try:
            child.sendline("halt")
        except Exception as e:
            log("note: halt send raised (harmless): %s" % e)

        proof.emit_result_line()
        log("PROCTAB-VAX: ALL CHECKS PASSED -- a real in-kernel VMS process "
            "table on NetBSD/vax shares state across processes (INV-6)")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console")
        log("  %s" % e)
        proof.record("unhandled", False, detail="pexpect.TIMEOUT: %s" % e)
        proof.emit_result_line()
        return 1
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console)")
        log("  %s" % e)
        proof.record("unhandled", False, detail="pexpect.EOF: %s" % e)
        proof.emit_result_line()
        return 1
    except Exception:
        log("FAIL: unexpected error driving the harness")
        traceback.print_exc()
        proof.record("unhandled", False, detail="unexpected exception")
        proof.emit_result_line()
        return 2
    finally:
        try:
            if child is not None and child.isalive():
                child.terminate(force=True)
        except Exception:
            pass


def _on_term(signum, frame):
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _on_term)
    sys.exit(main())
