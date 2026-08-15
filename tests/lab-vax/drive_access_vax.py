#!/usr/bin/env python3
#
# drive_access_vax.py - access-mode enforcement + cross-process AST delivery
# runtime proof (rd vms-4b7, parent vms-945e, epic vms-8e8) proven against a
# REAL /dev/vms on NetBSD/vax under SIMH (INV-6 honest).
#
# P4-E (vms-4e7, done) closed the INV-6 gap for event flags. P4-proctab
# (vms-2e0, done) closed it for the process table. This driver is the
# ACCESS-MODE + AST analogue -- the VAX runtime proof for the ACCESS and AST
# phases tests/netbsd/drive_netbsd_p4a.py already proves on NetBSD/amd64, and
# THE MOST INTRICATE of the facility proofs (rd vms-4b7's framing): it covers
# TWO facilities sharing one guest tool (tests/netbsd/guest/vmsaccess.c):
#
#   ACCESS MODES ($SETMODE/$GETMODE/$SETPRV/ENTER_IMAGE/IMAGE_RUNDOWN,
#   src/kernel-core/vms_access.c) -- an intra-process round trip (`selftest')
#   proves SETMODE/GETMODE/ENTER_IMAGE/IMAGE_RUNDOWN really gate the current
#   access mode (a mode-raise without CMEXEC/CMKRNL, or an ENTER_IMAGE from
#   the wrong starting mode, is refused -- vms_access.c enforces it, this tool
#   does not just believe it); then CROSS-PROCESS: process A mutates its OWN
#   permanent privilege mask with $SETPRV (a real state change recorded in
#   the executive's per-process control block) and records a discoverable
#   name with $SETPRN; a DIFFERENT process B's $GETJPI on that name reads
#   back the SAME mutated mask -- the mask lives in the executive's shared
#   process table (src/kernel-core/vms_proctab.c), not in either process.
#
#   ASTs ($DCLAST/$SETAST/DELIVERAST, src/kernel-core/vms_ast.c) -- $DCLAST is
#   SELF-DIRECTED by VMS design, so there is no direct "A writes, B reads"
#   case for the AST queue itself (the `selftest' op above proves the queue
#   is real executive state via a full DCLAST/DELIVERAST round trip). The
#   genuinely CROSS-PROCESS proof reuses the mailbox facility's
#   write-attention integration (src/kernel-core/vms_mbx.c calling
#   vms_ast_notify_arrival, src/kernel-core/vms_ast.c): process A arms a
#   write-attention AST on a temporary mailbox and $HIBERs (blocks
#   in-kernel); a DIFFERENT process B's write to that SAME mailbox is what
#   lands the AST in A's executive-resident queue and WAKES A's $HIBER --
#   proving both the AST queue and the HIBER/wake path are real, shared
#   kernel state a per-process fake could never be woken by, and that the
#   delivered astprm/acmode match exactly what A armed.
#
# THIS DRIVER ADOPTS vaxharness.py (rd vms-cf5), following drive_proctab_
# vax.py's shape (the pilot adoption) rather than re-deriving it: the
# boot-console handshake goes through safe_expect(), and the pass/fail
# decision is recorded as a Proof of StepResults, emitted as one JSON line at
# the end. run-access.sh applies negctl_gate.sh's vaxharness_negctl_gate() to
# this script's raw process exit code -- this script itself is NEVER
# negctl-mode-aware (see vaxharness.negctl_gate()'s docstring): none of the
# *_SKIP_* env vars below early-return a "negctl ok"; each just omits one
# real action, so the SAME positive-assertion script fails FOR REAL, and the
# ordinary nonzero exit code is what the wrapper inverts.
#
# WHY VAX SPECIFICALLY (mirrors vms-4e7's / vms-2e0's point). vax is ILP32 /
# non-IEEE-float / ELF32 -- a width class the amd64 (LP64) P4-A proof cannot
# exercise. A struct-layout or width bug in the shared wire contracts
# (src/kernel-netbsd/vms_access_nb.h's cur_privs/perm_privs, vms_ast_nb.h's
# astadr/astprm -- all explicit uint64_t) could compile clean and pass on
# every 64-bit OVMX target and only misbehave here -- and AST delivery +
# access-mode transitions are exactly where a vax width/frame bug hides
# (astprm truncated across the ioctl boundary, a PSL$C_ mode byte
# misinterpreted, etc).
#
# ARTIFACTS + STAGING (identical shape to drive_proctab_vax.py / drive_eflag_
# vax.py): the module (vms.kmod, carrying the SAME vms_ast.c/vms_access.c the
# Linux vms.ko builds) and the access/AST guest tool (vmsaccess,
# tests/netbsd/guest/vmsaccess.c, reaching /dev/vms through
# kif_transport_netbsd.c) are CROSS-BUILT on the host
# (tools/cross-vax/build-access-vax.sh) against the pinned NetBSD/vax kernel
# headers and DELIVERED into the guest on a second CD (rq2). The custom
# MODULAR kernel (GENERIC + options MODULAR) is installed the same way P4-B's
# / P4-E's / P4-proctab's is; see tests/lab-vax/README.md "P4-B" for why
# plain modload does not work on stock vax GENERIC.
#
# SINGLE-USER BOOT (securelevel 0), same reason as every sibling proof: at
# multiuser securelevel 1, secmodel_securelevel(9) denies KAUTH_SYSTEM_MODULE
# and modload of an out-of-tree module returns EPERM. This does not weaken
# the proof -- the module still serves a REAL in-kernel /dev/vms, INV-6 holds.
#
# INV-6 NEGATIVE CONTROL (mandatory, built in): with NO module loaded (so
# /dev/vms is absent), vmsaccess must fail HONESTLY (SS$_NOSUCHDEV), never
# fake success.
#
# NEGATIVE CONTROLS WITH TEETH (env-gated, mirror P4-proctab's PROCTAB_SKIP_*
# / P4-E's EFLAG_SKIP_*):
#   ACCESS_SKIP_LOAD=1     : modload itself is skipped -> the WHOLE proof
#                            (selftest, cross-process access, AND the AST
#                            phase, since all three need /dev/vms) must go
#                            RED the same way *_SKIP_LOAD does for every
#                            sibling proof.
#   ACCESS_SKIP_SETPRIV=1  : process A's $SETPRV/$SETPRN is never run -> B's
#                            by-name $GETJPI must find nothing -> the
#                            cross-process "B observes A's mutated privilege
#                            mask" assertion must go RED.
#   AST_SKIP_WRITE=1       : process B's mailbox write is never issued -> A's
#                            armed write-attention AST must NOT fire within
#                            the bounded poll budget (A never wakes) -> the
#                            THE PAYOFF assertion must go RED. THIS IS THE
#                            TEETH CHECK a per-process fake cannot pass: a
#                            fake that "delivers" an AST without any real
#                            cross-process signal would fire anyway with
#                            nothing to wake it, and this negative control
#                            catches exactly that (rd vms-4b7's explicit ask).
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting
# a real NetBSD/vax under SIMH to load and TEST a real kernel module is
# exactly what tests/qemu/ does for the Linux vms.ko and tests/netbsd/ does
# for amd64.
#
# The whole run is bounded by run-access.sh's hard `timeout`, and every
# in-guest command here has its own console deadline, so nothing hangs.

import os
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
# alongside this script by run-access.sh -- no sys.path insert needed.
from vaxharness import Proof, safe_expect


def log(msg):
    print("[drive_access_vax] %s" % msg, flush=True)


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
    emitted on its own line -- same helper drive_proctab_vax.py / drive_eflag_
    vax.py use. Last match wins so an echoed command line cannot shadow the
    real result."""
    val = None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith(key + "="):
            val = s[len(key) + 1:].strip()
    return val


def build_source_iso(artifacts_dir, out_iso):
    """Bundle the cross-built artifacts into an ISO9660 image attached to the
    boot as a second CD (rq2). Always carries vms.kmod + vmsaccess; also
    carries the custom MODULAR kernel (netbsd-OVMX) when present, for the
    install-kernel session. The guest mounts it and copies them out."""
    for f in ("vms.kmod", "vmsaccess"):
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
    a raw `# ' shell. Follows drive_proctab_vax.py's vaxharness.py adoption:
    every wait in this handshake goes through safe_expect() -- never a
    hand-rolled child.expect() with pexpect.TIMEOUT/EOF in the pattern list
    (vaxharness.py bug #1). A TIMEOUT/EOF/ERROR here is recorded as a failed
    StepResult and raised so main()'s top-level handler reports it;
    safe_expect() itself never raises."""
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
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-access-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))   # slow VAX boot
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "900"))
    poll_budget = int(env("NETBSD_POLL_BUDGET", "30"))

    # MODE:
    #   prove          (default) boot single-user, modload the vms module
    #                  against the MODULAR kernel, mknod /dev/vms, run the
    #                  access-mode + cross-process AST proof.
    #   install-kernel boot the current (GENERIC) kernel single-user and swap
    #                  in the custom MODULAR kernel (netbsd-OVMX) as /netbsd
    #                  (idempotent, no-op if /netbsd is already OVMX).
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("ACCESS_SKIP_LOAD"))
    skip_setpriv = bool(env("ACCESS_SKIP_SETPRIV"))
    skip_write = bool(env("AST_SKIP_WRITE"))

    proof = Proof("access-vax")

    log("NetBSD %s/%s  (OVMX/NetBSD access-mode + cross-process AST on VAX "
        "under SIMH, vms-4b7)" % (version, arch))
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:           %s" % artifacts_dir)
    if skip_load:
        log("NEGATIVE CONTROL: ACCESS_SKIP_LOAD set -- module will NOT be "
            "loaded; the whole proof must go RED")
    if skip_setpriv:
        log("NEGATIVE CONTROL: ACCESS_SKIP_SETPRIV set -- process A will "
            "NOT $SETPRV/$SETPRN -- process B's by-name $GETJPI must find "
            "nothing and the cross-process access-mode proof must go RED")
    if skip_write:
        log("NEGATIVE CONTROL: AST_SKIP_WRITE set -- process B will NOT "
            "write to A's mailbox -- A's armed write-attention AST must NOT "
            "fire and the AST payoff assertion must go RED")

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
            "access artifact CD on rq2 (deadline %ds)..." % boot_deadline)

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
                      "      cp /mnt/vms.kmod /mnt/vmsaccess /root/ovmx/ && "
                      "      umount /mnt && chmod +x /root/ovmx/vmsaccess && break; "
                      "    else umount /mnt 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX module CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vms.kmod && test -x /root/ovmx/vmsaccess",
                      cmd_timeout)
        proof.record("stage-artifacts", rc == 0,
                      detail="mount OVMX CD, copy vms.kmod+vmsaccess")
        if rc != 0:
            log("FAIL: could not find/mount the OVMX artifact CD in the guest "
                "(see the CD-device diagnostics above)")
            proof.emit_result_line()
            return 10
        log("OK: staged vms.kmod + vmsaccess from the OVMX CD")

        AC = "/root/ovmx/vmsaccess"

        # ---- 3. INV-6 NEGATIVE CONTROL: no module loaded --------------------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "%s selftest" % AC, cmd_timeout)
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
        # NEGCTL CONTRACT (mirrors every sibling proof's *_SKIP_LOAD exactly):
        # when skip_load is set, modload is skipped and /dev/vms stays
        # ABSENT (removed by the INV-6 check above, never recreated) -- the
        # driver does NOT special-case an early "negctl satisfied" exit here.
        # It falls through into the SAME positive-assertion proof every
        # positive run executes; that proof then fails FOR REAL (every
        # vmsaccess invocation hits the honest device-unreachable path), and
        # this function returns its ordinary nonzero failure code below.
        # run-access.sh's negctl-load mode is what inverts that nonzero exit
        # into "teeth confirmed" -- one exit-code contract, shared by every
        # mode.
        if skip_load:
            log("SKIP: modload skipped (ACCESS_SKIP_LOAD) -- /dev/vms stays "
                "absent; the access + AST proofs below must fail for real")
            proof.record("modload", False,
                          detail="skipped (ACCESS_SKIP_LOAD negative control)")
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

        # =====================================================================
        # ACCESS: intra-process round trip (SETMODE/GETMODE/ENTER_IMAGE/
        # DCLAST/DELIVERAST/IMAGE_RUNDOWN), then A mutates its permanent
        # privilege mask and B (a DIFFERENT process) observes it via $GETJPI.
        # Collapsed into ONE in-guest command, the same loss-tolerant-
        # transport technique drive_proctab_vax.py / drive_eflag_vax.py use
        # for the lossy SIMH serial. NEGCTL CONTRACT (ACCESS_SKIP_SETPRIV,
        # mirrors PROCTAB_SKIP_BG exactly): when set, process A is simply
        # never launched, so B's by-name $GETJPI finds nothing and the SAME
        # "getpriv" failure branch below fires naturally -- no separate
        # negctl code path to drift out of sync with the wrapper.
        # =====================================================================
        if skip_setpriv:
            acc = ("S=$(%s selftest 2>&1); SR=$?; "
                   "G=$(%s getpriv P4VPRIV1 2>&1); GR=$?; "
                   "{ echo '### access selftest'; echo \"$S\"; "
                   "echo '### access B getpriv (A never ran)'; echo \"$G\"; } "
                   ">>/tmp/acc_tx; "
                   "F=''; "
                   "{ [ $SR -eq 0 ] && echo \"$S\" | grep -q 'ACCESS SELFTEST PASS'; } "
                   "|| F=\"$F selftest\"; "
                   "{ [ $GR -eq 0 ] && echo \"$G\" | grep -q 'cmexec_clear=1'; } "
                   "|| F=\"$F getpriv\"; "
                   "[ -z \"$F\" ] && echo ACCESS=PASS || echo \"ACCESS=FAIL:$F\""
                   % (AC, AC))
        else:
            acc = ("S=$(%s selftest 2>&1); SR=$?; "
                   "rm -f /tmp/acc_a.out; "
                   "%s setpriv_bg P4VPRIV1 %d >/tmp/acc_a.out 2>&1 & AP=$!; sleep 2; "
                   "G=$(%s getpriv P4VPRIV1 2>&1); GR=$?; "
                   "kill $AP 2>/dev/null; wait 2>/dev/null; "
                   "{ echo '### access selftest'; echo \"$S\"; "
                   "echo '### access A setpriv'; cat /tmp/acc_a.out; "
                   "echo '### access B getpriv'; echo \"$G\"; } >>/tmp/acc_tx; "
                   "F=''; "
                   "{ [ $SR -eq 0 ] && echo \"$S\" | grep -q 'ACCESS SELFTEST PASS'; } "
                   "|| F=\"$F selftest\"; "
                   "{ [ $GR -eq 0 ] && echo \"$G\" | grep -q 'cmexec_clear=1'; } "
                   "|| F=\"$F getpriv\"; "
                   "[ -z \"$F\" ] && echo ACCESS=PASS || echo \"ACCESS=FAIL:$F\""
                   % (AC, AC, poll_budget, AC))
        rc, out = run(child, acc, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "ACCESS")
        if tok != "PASS":
            proof.record("access-cross-process", False, detail="tok=%s" % tok)
            log("FAIL: access-mode proof failed on NetBSD/vax (phase token: %s)"
                % tok)
            proof.emit_result_line()
            return 60 if (tok and "selftest" in tok) else 61
        proof.record("access-cross-process", True)
        log("OK: SETMODE/GETMODE/ENTER_IMAGE/DCLAST/DELIVERAST/IMAGE_RUNDOWN "
            "round-trip through the real /dev/vms on NetBSD/vax -- access-mode "
            "transitions are ENFORCED (mode raise needs CMEXEC/CMKRNL; "
            "ENTER_IMAGE/IMAGE_RUNDOWN gate on the starting mode)")
        if not skip_setpriv:
            log("OK: process B (a DIFFERENT process) observed A's real "
                "$SETPRV mutation via $GETJPI -- the privilege mask is "
                "shared kernel state (INV-6)")

        # =====================================================================
        # AST: A arms a mailbox write-attention AST and $HIBERs; B's write on
        # that SAME mailbox fires the AST and wakes A -- cross-process. NOT
        # collapsed into one in-guest command like ACCESS above: the
        # write-attention path is timing-sensitive (B's $QIO write must land
        # AFTER A is actually parked in $HIBER on the armed channel), and
        # driving it as discrete steps -- arm+HIBER, observe ARMED, then
        # write (or skip), then observe FIRED -- gives A the interval it
        # needs; this mirrors tests/netbsd/drive_netbsd_p4a.py's AST phase
        # (which made the identical choice for the SAME reason on amd64/
        # anita). The helper A is launched with a trailing `&' so the console
        # driver treats it as a non-retriable background launch (it must not
        # be duplicated); the remaining steps are idempotent polls/reads and
        # retry to the deadline.
        # =====================================================================
        rc, _ = run(child,
                    "rm -f /tmp/ast_a.out; "
                    "%s wrtattn_bg P4VAST1 %d >/tmp/ast_a.out 2>&1 &"
                    % (AC, poll_budget), cmd_timeout)
        rc, out = run(child,
                      "i=0; while [ $i -lt %d ]; do "
                      "grep -q 'AST WRTATTN ARMED' /tmp/ast_a.out && break; "
                      "i=$((i+1)); sleep 1; done; cat /tmp/ast_a.out; "
                      "echo ---END---" % poll_budget, cmd_timeout)
        # vmsaccess.c prints this as one whole line ("AST WRTATTN MBX
        # DEVNAM=<name>"), which IS a bare `KEY=VALUE' line for phase_token's
        # purposes once the full prefix is used as the key.
        ast_devnam = phase_token(out, "AST WRTATTN MBX DEVNAM")
        if "AST WRTATTN ARMED" not in out or not ast_devnam:
            proof.record("ast-armed", False,
                          detail="process A never armed its write-attention "
                                 "AST (bounded poll expired)")
            log("FAIL: process A never armed its write-attention AST "
                "(bounded poll expired)")
            proof.emit_result_line()
            return 70
        if "AST WRTATTN FIRED" in out:
            proof.record("ast-armed", False,
                          detail="AST fired before ANY process wrote to the "
                                 "mailbox -- the wait has no teeth")
            log("FAIL: the AST fired before ANY process wrote to the "
                "mailbox -- the wait has no teeth (it did not actually block)")
            proof.emit_result_line()
            return 71
        proof.record("ast-armed", True, detail="devnam=%s" % ast_devnam)
        log("OK: process A armed a write-attention AST on mailbox %s and "
            "blocked in $HIBER (no premature FIRED)" % ast_devnam)

        if skip_write:
            log("SKIP: process B's mailbox write skipped (AST_SKIP_WRITE) -- "
                "A's armed AST must NOT fire next")
        else:
            rc, out = run(child, "%s wrtattn_write %s" % (AC, ast_devnam),
                          cmd_timeout)
            if rc != 0 or "AST WRTATTN WRITE status=" not in out:
                proof.record("ast-write", False,
                              detail="process B's write to A's mailbox failed")
                log("FAIL: process B's write to A's mailbox failed")
                proof.emit_result_line()
                return 72
            proof.record("ast-write", True)
            log("OK: process B (a DIFFERENT process) wrote to A's mailbox")

        rc, out = run(child,
                      "i=0; while [ $i -lt %d ]; do "
                      "grep -q 'AST WRTATTN FIRED' /tmp/ast_a.out && break; "
                      "i=$((i+1)); sleep 1; done; cat /tmp/ast_a.out; "
                      "echo ---END---" % poll_budget, cmd_timeout)
        fired = ("AST WRTATTN FIRED astprm=0x" in out and "match=1" in out)
        if skip_write:
            # TEETH: with B's write skipped, A's $HIBER must NOT have woken --
            # if FIRED appears anyway (a per-process fake, or a leftover wake
            # from something else), the negative control has no teeth and this
            # is a REAL failure (the driver is never negctl-mode-aware; the
            # SAME assertion below just evaluates the opposite way this run).
            if fired:
                proof.record("ast-crossprocess", False,
                              detail="AST fired with no process ever writing "
                                     "to the mailbox -- a per-process fake or "
                                     "a spurious wake; the negative control "
                                     "has no teeth")
                log("FAIL (negctl teeth): process A's AST FIRED even though "
                    "no process ever wrote to its mailbox -- this would be a "
                    "per-process fake, exactly what INV-6 forbids")
                proof.emit_result_line()
                return 74
            proof.record("ast-crossprocess", False,
                          detail="AST_SKIP_WRITE: correctly did NOT fire "
                                 "(bounded poll expired) -- teeth confirmed")
            log("OK (negctl teeth): with no process ever writing to A's "
                "mailbox, the armed AST correctly did NOT fire and A's "
                "$HIBER never woke -- run-access.sh's negctl-astwrite mode "
                "inverts this same nonzero exit into PASS")
            proof.emit_result_line()
            return 73
        if not fired:
            proof.record("ast-crossprocess", False,
                          detail="A's $HIBER never woke / DELIVERAST did not "
                                 "return the armed write-attention AST")
            log("FAIL: process A's $HIBER never woke / DELIVERAST did not "
                "return the write-attention AST B's write should have fired")
            proof.emit_result_line()
            return 73
        proof.record("ast-crossprocess", True, detail="devnam=%s" % ast_devnam)
        log("OK: process B's mailbox write WOKE process A's $HIBER and "
            "delivered the EXACT armed AST (astprm/acmode match) -- the AST "
            "queue + HIBER/wake path are shared kernel state reachable from "
            "a DIFFERENT process on NetBSD/vax (INV-6, THE AST PAYOFF, "
            "rd vms-4b7)")

        # ---- evidence: read the full in-guest transcript ONCE ---------------
        run(child, "echo '=== ACCESS-VAX TRANSCRIPT ==='; "
                   "cat /tmp/acc_tx 2>/dev/null; echo '=== END ==='", cmd_timeout)

        # ---- cleanup ----------------------------------------------------
        run(child, "modunload vms 2>/dev/null; true", cmd_timeout)
        try:
            child.sendline("halt")
        except Exception as e:
            log("note: halt send raised (harmless): %s" % e)

        proof.emit_result_line()
        log("ACCESS-VAX: ALL CHECKS PASSED -- access-mode enforcement AND "
            "cross-process AST delivery, both compiled from "
            "src/kernel-core/ with the NetBSD backend, hold on real "
            "NetBSD/vax under SIMH (INV-6)")
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
