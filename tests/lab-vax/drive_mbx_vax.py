#!/usr/bin/env python3
#
# drive_mbx_vax.py - mailbox ($CREMBX/$ASSIGN/$QIO, MBAn:) runtime proof (rd
# vms-fe8, parent vms-945e -> vms-476, epic vms-8e8) proven cross-process
# against a REAL /dev/vms on NetBSD/vax under SIMH (INV-6 honest).
#
# P4-A (vms-d7a, done, amd64) proved the mailbox facility cross-process on
# NetBSD/amd64: a message one process $QIO-writes into a mailbox it did NOT
# create is read back byte-for-byte by a DIFFERENT process, because the
# mailbox and its message queue live in the executive's shared KERNEL memory
# (src/kernel-core/vms_mbx.c, compiled into the NetBSD `vms' pseudo-device),
# not in either process (CLAUDE.md Rule 9). P4-E (vms-4e7) and the proctab
# proof (vms-2e0) already carried that INV-6-decisive pattern to vax for
# event flags and the process table; this driver is the MAILBOX analogue,
# and the last of the boot-required facilities vms-945e tracks for vax
# (proctab/lnm/mbx/ast/access -- proctab and mbx now both proven on vax).
#
# ADOPTS vaxharness.py (rd vms-cf5), the same way drive_proctab_vax.py does:
# the boot-console handshake routes through safe_expect() (never a
# hand-rolled child.expect() with pexpect.TIMEOUT/EOF in the pattern list --
# that is the crash class vaxharness.py exists to kill), and the driver's
# pass/fail decision is recorded as a Proof of StepResults, emitted as one
# JSON line at the end. The WRAPPER (run-mbx.sh) applies negctl_gate.sh's
# vaxharness_negctl_gate() to this script's raw process exit code -- this
# script itself is NEVER negctl-mode-aware (see vaxharness.negctl_gate()'s
# docstring): MBX_SKIP_CREATE does not early-return a "negctl ok"; it just
# omits process A, so the SAME positive-assertion script below fails FOR
# REAL, and the ordinary nonzero exit code is what the wrapper inverts.
#
# WHY VAX SPECIFICALLY (mirrors vms-4e7's / vms-2e0's point). vax is ILP32 /
# non-IEEE-float / ELF32 -- a width class the amd64 (LP64) P4-A proof cannot
# exercise. A struct-layout or width bug in the shared wire contract
# (src/kernel-netbsd/vms_mbx_nb.h -- e.g. vms_mbx_write_args's chan/len as
# uint32_t, or the IOC_VOID big-io path for WRITE/READ) could compile clean
# and pass on every 64-bit OVMX target and only misbehave here.
#
# ARTIFACTS + STAGING (identical shape to drive_eflag_vax.py /
# drive_proctab_vax.py): the module (vms.kmod, carrying the SAME vms_mbx.c
# the Linux vms.ko builds) and the mailbox guest tool (vmsmbx,
# tests/netbsd/guest/vmsmbx.c, reaching /dev/vms through
# kif_transport_netbsd.c) are CROSS-BUILT on the host
# (tools/cross-vax/build-mbx-vax.sh) against the pinned NetBSD/vax kernel
# headers and DELIVERED into the guest on a second CD (rq2) -- the vax system
# disk cannot hold the comp+syssrc sets an in-guest build would need. The
# custom MODULAR kernel (GENERIC + options MODULAR, P4-B's compile-into-
# kernel fallback) is installed the same way P4-B's / P4-E's / the proctab
# proof's is; see tests/lab-vax/README.md "P4-B" for why plain modload does
# not work on stock vax GENERIC.
#
# SINGLE-USER BOOT (securelevel 0), same reason as P4-B/P4-E: at multiuser
# securelevel 1, secmodel_securelevel(9) denies KAUTH_SYSTEM_MODULE and
# modload of an out-of-tree module returns EPERM. This does not weaken the
# proof -- the module still serves a REAL in-kernel /dev/vms, and INV-6
# holds.
#
# THE CROSS-PROCESS PROOF (collapsed into a single in-guest shell command,
# the SAME loss-tolerant-transport technique drive_eflag_vax.py /
# drive_proctab_vax.py use for the lossy TCG/SIMH serial, rd vms-3e7):
# process A `vmsmbx create_hold <holdsecs>` ($CREMBX(temporary) then holds
# its channel open so the mailbox stays alive) / process B (a DIFFERENT
# process) `vmsmbx write <devnam> <text>` ($ASSIGN + $QIO write into the
# mailbox A created) / process C (a THIRD, different process) `vmsmbx read
# <devnam> <text>` ($ASSIGN + $QIO read, must match B's bytes exactly) -- the
# devnam (MBAn:) is captured from A's own stdout, so B and C reach the SAME
# mailbox A created by NAME, never by any shared in-process state.
#
# INV-6 NEGATIVE CONTROL (mandatory, built in): with NO module loaded (so
# /dev/vms is absent), vmsmbx must fail HONESTLY (SS$_NOSUCHDEV), never fake
# success.
#
# NEGATIVE CONTROLS WITH TEETH (env-gated, mirrors EFLAG_SKIP_SET /
# PROCTAB_SKIP_BG):
#   MBX_SKIP_LOAD=1   : modload itself is skipped -> the whole proof must go
#                       RED the same way EFLAG_SKIP_LOAD / PROCTAB_SKIP_LOAD
#                       does.
#   MBX_SKIP_CREATE=1 : process A is never launched -> no devnam is ever
#                       captured -> process B's write (and C's read) must
#                       fail for real -> the cross-process assertion must go
#                       RED. This is what gives the positive "C read B's
#                       bytes through the SAME mailbox A created" proof
#                       teeth: a per-process fake that "reads back" without a
#                       real shared executive would not need A's mailbox to
#                       exist at all.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting
# a real NetBSD/vax under SIMH to load and TEST a real kernel module is
# exactly what tests/qemu/ does for the Linux vms.ko and tests/netbsd/ does
# for amd64.
#
# The whole run is bounded by run-mbx.sh's hard `timeout`, and every in-guest
# command here has its own console deadline, so nothing hangs.

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
# alongside this script by run-mbx.sh -- no sys.path insert needed.
from vaxharness import Proof, safe_expect


def log(msg):
    print("[drive_mbx_vax] %s" % msg, flush=True)


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
    drive_proctab_vax.py use. Last match wins so an echoed command line
    cannot shadow the real result."""
    val = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith(key + "="):
            val = s[len(key) + 1:].strip()
    return val


def build_source_iso(artifacts_dir, out_iso):
    """Bundle the cross-built artifacts into an ISO9660 image attached to the
    boot as a second CD (rq2). Always carries vms.kmod + vmsmbx; also carries
    the custom MODULAR kernel (netbsd-OVMX) when present, for the
    install-kernel session. The guest mounts it and copies them out."""
    for f in ("vms.kmod", "vmsmbx"):
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
    a raw `# ' shell. Every wait in this handshake goes through
    safe_expect() -- never a hand-rolled child.expect() with pexpect.TIMEOUT/
    EOF in the pattern list (vaxharness.py bug #1), mirroring
    drive_proctab_vax.py's pilot adoption. A TIMEOUT/EOF/ERROR here is
    recorded as a failed StepResult and raised so main()'s top-level handler
    reports it; safe_expect() itself never raises."""
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
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-mbx-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))   # slow VAX boot
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "900"))

    # MODE:
    #   prove          (default) boot single-user, modload the vms module
    #                  against the MODULAR kernel, mknod /dev/vms, run the
    #                  cross-process mailbox proof.
    #   install-kernel boot the current (GENERIC) kernel single-user and swap
    #                  in the custom MODULAR kernel (netbsd-OVMX) as /netbsd
    #                  (idempotent, no-op if /netbsd is already OVMX).
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("MBX_SKIP_LOAD"))
    skip_create = bool(env("MBX_SKIP_CREATE"))

    proof = Proof("mbx-vax")

    log("NetBSD %s/%s  (OVMX/NetBSD mailboxes on VAX under SIMH, vms-fe8)"
        % (version, arch))
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:           %s" % artifacts_dir)
    if skip_load:
        log("NEGATIVE CONTROL: MBX_SKIP_LOAD set -- module will NOT be "
            "loaded; the whole proof must go RED")
    if skip_create:
        log("NEGATIVE CONTROL: MBX_SKIP_CREATE set -- process A will NOT "
            "create the mailbox; process B's write (and C's read) must fail "
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
            "mbx artifact CD on rq2 (deadline %ds)..." % boot_deadline)

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
                      "      cp /mnt/vms.kmod /mnt/vmsmbx /root/ovmx/ && "
                      "      umount /mnt && chmod +x /root/ovmx/vmsmbx && break; "
                      "    else umount /mnt 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX module CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vms.kmod && test -x /root/ovmx/vmsmbx",
                      cmd_timeout)
        proof.record("stage-artifacts", rc == 0,
                      detail="mount OVMX CD, copy vms.kmod+vmsmbx")
        if rc != 0:
            log("FAIL: could not find/mount the OVMX artifact CD in the guest "
                "(see the CD-device diagnostics above)")
            proof.emit_result_line()
            return 10
        log("OK: staged vms.kmod + vmsmbx from the OVMX CD")

        MB = "/root/ovmx/vmsmbx"

        # ---- 3. INV-6 NEGATIVE CONTROL: no module loaded --------------------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "%s read MBA0: bogus" % MB, cmd_timeout)
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
        # NEGCTL CONTRACT (mirrors drive_eflag_vax.py's EFLAG_SKIP_LOAD /
        # drive_proctab_vax.py's PROCTAB_SKIP_LOAD exactly): when skip_load is
        # set, modload is skipped and /dev/vms stays ABSENT (removed by the
        # INV-6 check above, never recreated) -- the driver does NOT
        # special-case an early "negctl satisfied" exit here. It falls
        # through into the SAME cross-process proof every positive run
        # executes; that proof then fails FOR REAL (every vmsmbx invocation
        # hits the honest device-unreachable path), and this function
        # returns its ordinary nonzero failure code below. run-mbx.sh's
        # negctl-load mode (via negctl_gate.sh) is what inverts "the driver
        # exited nonzero" into "teeth confirmed" -- one exit-code contract,
        # shared by every mode.
        if skip_load:
            log("SKIP: modload skipped (MBX_SKIP_LOAD) -- /dev/vms stays "
                "absent; the cross-process proof below must fail for real")
            proof.record("modload", False,
                          detail="skipped (MBX_SKIP_LOAD negative control)")
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

        # ---- 4a-4c. CROSS-PROCESS MAILBOX PROOF (collapsed, FILE-based) ----
        # Same loss-tolerant-transport technique as drive_eflag_vax.py: ONE
        # in-guest command performs every step and emits a SINGLE result
        # token, so a lost marker on the lossy SIMH serial cannot corrupt the
        # proof. Process A (`create_hold`, backgrounded, holds its channel
        # open so the TEMPORARY mailbox survives) is a DISTINCT `vmsmbx'
        # invocation from B (`write`) and C (`read`); B and C reach the
        # mailbox purely by the MBAn: NAME A's own stdout reported -- never
        # through any state shared IN this shell, which is what makes "C
        # read back exactly what B wrote" a genuine cross-process,
        # shared-executive proof (INV-6).
        #
        # FILE-based capture, not $(...) + inline sed on the write/read
        # steps (rd vms-2e0's postmortem, flagged for this driver too): an
        # earlier proctab-vax revision crammed three inline `sed -n
        # 's/.../.../p'` extractions plus $(...) command substitution into
        # one ~900-char line, and a real nightly run came back with driver
        # exit=2 and a completely EMPTY captured console line (not even the
        # unconditional trailing echo) -- consistent with that long compound
        # command getting corrupted/truncated on the lossy VAX/SIMH serial.
        # The fix (mirrors drive_netbsd_p4a.py's proven MBX-phase shape,
        # generalized the way the fixed drive_proctab_vax.py generalizes
        # it): each `vmsmbx' invocation writes straight to its OWN file
        # (`>file 2>&1`), the PASS/FAIL decision is a plain `grep -q ...
        # /file` against those files (no $(...), no inline sed beyond the
        # single short DEVNAM extraction from A's own file), and a
        # TRANSCRIPT is ALWAYS dumped afterward -- pass or fail -- so a
        # repeat of this failure mode is diagnosable from the console
        # instead of a bare "tok=None". The devnam is parsed PYTHON-side
        # from that dumped transcript, not via in-guest sed.
        #
        # NEGCTL CONTRACT (MBX_SKIP_CREATE, mirrors PROCTAB_SKIP_BG /
        # EFLAG_SKIP_SET exactly): when skip_create is set, process A is
        # simply never launched. The driver does NOT special-case an early
        # "negctl satisfied" exit here (see drive_proctab_vax.py's identical
        # reasoning, and rd vms-cf5's whole point). Instead this falls
        # through into the SAME positive-assertion script every positive run
        # executes; with no A, no MBAn: name is ever captured, so B's write
        # (and thus C's read) fail for real via the SAME "devnam"/"write"/
        # "read" branches below, and this function returns its ordinary
        # nonzero code -- never 0, never a distinct "harness-error" code
        # (the ACCESS/vms-4b7 bug class): every failure path below returns a
        # plain positive int, and negctl_gate()/negctl_gate.sh only ever
        # test zero-vs-nonzero, so any of these codes inverts correctly
        # under run-mbx.sh's negctl-create mode.
        TX = "/tmp/mbx_transcript"
        msg = "MBXPROOF-vms-fe8"
        if skip_create:
            log("SKIP: process A's create_hold skipped (MBX_SKIP_CREATE) -- "
                "no MBAn: name will ever be captured, and process B's write "
                "(and C's read) must fail for real next")
            mbx = ("rm -f /tmp/mbx_b.out /tmp/mbx_c.out %s; "
                   "DEVNAM=''; "
                   "%s write \"$DEVNAM\" '%s' >/tmp/mbx_b.out 2>&1; "
                   "%s read \"$DEVNAM\" '%s' >/tmp/mbx_c.out 2>&1; "
                   "{ echo '### mbx B write (A never created a mailbox)'; "
                   "cat /tmp/mbx_b.out; "
                   "echo '### mbx C read (A never created a mailbox)'; "
                   "cat /tmp/mbx_c.out; } >>%s; "
                   "F=''; "
                   "grep -qE '^MBX WRITE devnam=.* len=' /tmp/mbx_b.out || F=\"$F write\"; "
                   "grep -qE '^MBX READ devnam=.* len=[0-9]+ match=1' /tmp/mbx_c.out || F=\"$F read\"; "
                   "[ -z \"$F\" ] && echo MBX=PASS || echo \"MBX=FAIL:$F\""
                   % (TX, MB, msg, MB, msg, TX))
        else:
            mbx = ("rm -f /tmp/mbx_a.out /tmp/mbx_b.out /tmp/mbx_c.out %s; "
                   "%s create_hold 8 >/tmp/mbx_a.out 2>&1 & AP=$!; sleep 3; "
                   "DEVNAM=$(grep 'MBX CREATE' /tmp/mbx_a.out | "
                   "sed 's/.*devnam=//; s/ .*//' | head -1); "
                   "%s write \"$DEVNAM\" '%s' >/tmp/mbx_b.out 2>&1; "
                   "%s read \"$DEVNAM\" '%s' >/tmp/mbx_c.out 2>&1; "
                   "kill $AP 2>/dev/null; wait 2>/dev/null; "
                   "{ echo '### mbx A (create_hold)'; cat /tmp/mbx_a.out; "
                   "echo \"### mbx devnam=$DEVNAM\"; "
                   "echo '### mbx B write'; cat /tmp/mbx_b.out; "
                   "echo '### mbx C read'; cat /tmp/mbx_c.out; } >>%s; "
                   "F=''; "
                   "grep -q 'MBX CREATE' /tmp/mbx_a.out || F=\"$F create\"; "
                   "[ -n \"$DEVNAM\" ] || F=\"$F devnam\"; "
                   "grep -qE '^MBX WRITE devnam=.* len=' /tmp/mbx_b.out || F=\"$F write\"; "
                   "grep -qE '^MBX READ devnam=.* len=[0-9]+ match=1' /tmp/mbx_c.out || F=\"$F read\"; "
                   "[ -z \"$F\" ] && echo MBX=PASS || echo \"MBX=FAIL:$F\""
                   % (TX, MB, MB, msg, MB, msg, TX))
        rc, out = run(child, mbx, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "MBX")

        # ALWAYS dump the transcript -- pass or fail -- so a repeat of the
        # proctab truncation failure mode is diagnosable from the console
        # instead of a bare "tok=None". Also the source for the devnam
        # parse below (no in-guest sed needed for it).
        rc2, tx_out = run(
            child,
            "echo '=== MBX TRANSCRIPT ==='; cat %s; echo '=== END ==='" % TX,
            cmd_timeout)
        log("mbx transcript:\n%s" % tx_out.strip())

        # NOTE: skip_create is deliberately NOT special-cased from here on --
        # the exact same interpretation below handles both modes (a PASS
        # token is a real pass; any FAIL is a real, ordinary failure), which
        # is what keeps this driver's exit code mode-agnostic.
        if not tok or not tok.startswith("PASS"):
            proof.record("mbx-cross-process", False, detail="tok=%s" % tok)
            if not tok or "create" in tok or "devnam" in tok:
                log("FAIL: process A never created a mailbox (or its MBAn: "
                    "name was never captured) -- no mailbox exists for B/C "
                    "to reach (phase token: %s; see transcript above)" % tok)
                proof.emit_result_line()
                return 16
            if "write" in tok:
                log("FAIL: process B (a DIFFERENT process) could not WRITE "
                    "into the mailbox process A created -- the cross-process "
                    "shared-kernel-state proof did not hold on NetBSD/vax "
                    "(phase token: %s; see transcript above)" % tok)
                proof.emit_result_line()
                return 17
            log("FAIL: process C (a THIRD, different process) did NOT read "
                "back the exact bytes process B wrote -- the mailbox message "
                "queue was not shared across processes (phase token: %s; "
                "see transcript above)" % tok)
            proof.emit_result_line()
            return 18

        m = re.search(r"MBX CREATE devnam=(\S+) status=", tx_out)
        devnam_seen = m.group(1) if m else "?"
        proof.record("mbx-cross-process", True, detail="devnam=%s" % devnam_seen)
        log("OK: process B (a DIFFERENT process) $ASSIGNed + $QIO-wrote into "
            "the mailbox (%s) process A created -- the mailbox is shared "
            "kernel state (INV-6)" % devnam_seen)
        log("OK: process C (a THIRD, different process) $ASSIGNed the SAME "
            "mailbox and $QIO-read back B's bytes EXACTLY (byte-for-byte "
            "match) -- proving the message queue lives in the executive, not "
            "in either process (THE PAYOFF, rd vms-fe8)")

        # ---- cleanup ------------------------------------------------------
        run(child, "modunload vms 2>/dev/null; true", cmd_timeout)
        try:
            child.sendline("halt")
        except Exception as e:
            log("note: halt send raised (harmless): %s" % e)

        proof.emit_result_line()
        log("MBX-VAX: ALL CHECKS PASSED -- a real in-kernel VMS mailbox "
            "facility on NetBSD/vax shares state across processes (INV-6)")
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
