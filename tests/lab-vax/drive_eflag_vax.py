#!/usr/bin/env python3
#
# drive_eflag_vax.py - P4-E runtime proof (rd vms-4e7, parent vms-476, epic
# vms-8e8; blocked-on vms-f78bb): common EVENT FLAGS proven cross-process
# against a REAL /dev/vms on NetBSD/vax under SIMH (INV-6 honest).
#
# vms-f78bb (P4-B, done) proved ONE ioctl round-trips through a live /dev/vms
# on real NetBSD/vax. This closes the INV-6-decisive gap that leaves open: a
# per-process userspace FAKE could answer that single ioctl too. This driver is
# the VAX analogue of tests/netbsd/drive_netbsd_p2c.py -- it proves a COMMON
# event flag cluster set by one OS process is observed by a genuinely DIFFERENT
# process, and that a blocked waiter is woken across a process boundary,
# because the flag state lives in the executive's KERNEL memory
# (src/kernel-core/vms_eflag.c, compiled into the NetBSD `vms' pseudo-device),
# not in either process (CLAUDE.md Rule 9).
#
# WHY VAX SPECIFICALLY (rd vms-4e7's point). vax is ILP32 / non-IEEE-float /
# ELF32 -- a width class the amd64 (LP64) proof cannot exercise. A struct-layout
# or width bug in the shared wire contract (src/kernel-netbsd/vms_eflag_nb.h)
# could compile clean and pass on every 64-bit OVMX target and only misbehave
# here. Scope is EVENT FLAGS ONLY (rd vms-4e7's audit clarification); the other
# boot-required facilities (proctab/lnm/mbx/ast/access) are the separate
# vms-945e, not this driver.
#
# ARTIFACTS + STAGING (identical shape to drive_devvms_vax.py / P4-B): the
# module (vms.kmod, carrying the SAME vms_eflag.c the Linux vms.ko builds) and
# the event-flag guest tool (vmseflag, tests/netbsd/guest/vmseflag.c, reaching
# /dev/vms through kif_transport_netbsd.c) are CROSS-BUILT on the host
# (tools/cross-vax/build-eflag-vax.sh) against the pinned NetBSD/vax kernel
# headers and DELIVERED into the guest on a second CD (rq2) -- the vax system
# disk cannot hold the comp+syssrc sets an in-guest build would need. The custom
# MODULAR kernel (GENERIC + options MODULAR, vms-f78bb's compile-into-kernel
# fallback) is installed the same way P4-B's is; see that driver / README.md
# "P4-B" section for why plain modload does not work on stock vax GENERIC.
#
# SINGLE-USER BOOT (securelevel 0), same reason as P4-B: at multiuser
# securelevel 1, secmodel_securelevel(9) denies KAUTH_SYSTEM_MODULE and modload
# of an out-of-tree module returns EPERM. This does not weaken the proof --
# the module still serves a REAL in-kernel /dev/vms, and INV-6 holds.
#
# THE CROSS-PROCESS PROOF (collapsed into single in-guest shell commands, the
# SAME loss-tolerant-transport technique drive_netbsd_p2c.py / p4a.py use for
# the lossy TCG/SIMH serial, rd vms-3e7):
#   4a-4e. process A ($SETEF 64) / process B (a DIFFERENT process, $READEF 64
#          sees SET) / process B2 ($READEF 65 control flag stays CLEAR) /
#          process C ($CLREF 64, previous-state must be was-set) / process D
#          ($READEF 64 now CLEAR) -- each `vmseflag` invocation is its own OS
#          process; VMS-faithful $ASCEFC of the well-known PERMANENT cluster
#          happens inside vmseflag.c itself.
#   4f.    CV WAIT PROOF: a waiter process $WAITFRs on common flag 66 and
#          BLOCKS in-kernel; a DIFFERENT process $SETEFs it; the blocked waiter
#          must WAKE. This is the strongest form -- not just visibility of a
#          word, but a real in-kernel sleep woken across a process boundary.
#
# INV-6 NEGATIVE CONTROL (mandatory, built in): with NO module loaded (so
# /dev/vms is absent), vmseflag must fail HONESTLY (SS$_NOSUCHDEV), never fake
# success. This is what gives the positive proof teeth: a per-process fake
# would pass the positive AND ALSO pass with no /dev/vms present.
#
# NEGATIVE CONTROLS WITH TEETH (env-gated, mirrors P2c's P2C_SKIP_SET):
#   EFLAG_SKIP_SET=1  : process A's SETEF is skipped -> the cross-process "B
#                       sees flag 64 SET" assertion must go RED.
#   EFLAG_SKIP_LOAD=1 : modload itself is skipped -> the whole proof must go
#                       RED the same way P4B_SKIP_LOAD does for the PING proof.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting a
# real NetBSD/vax under SIMH to load and TEST a real kernel module is exactly
# what tests/qemu/ does for the Linux vms.ko and tests/netbsd/ does for amd64.
#
# The whole run is bounded by run-eflag.sh's hard `timeout`, and every in-guest
# command here has its own console deadline, so nothing hangs.

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
# alongside this script by run-eflag.sh -- no sys.path insert needed.
from vaxharness import HARNESS_ERROR, PROOF_FAILED


def log(msg):
    print("[drive_eflag_vax] %s" % msg, flush=True)


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
    emitted on its own line -- same helper drive_netbsd_p2c.py /
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
    boot as a second CD (rq2). Always carries vms.kmod + vmseflag; also carries
    the custom MODULAR kernel (netbsd-OVMX) when present, for the
    install-kernel session. The guest mounts it and copies them out."""
    for f in ("vms.kmod", "vmseflag"):
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


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "vax")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    # Same workdir + sets the lab-vax install produced the cached disk with, so
    # a.install() is a cache-aware no-op (never a hot-path reinstall).
    workdir = env("NETBSD_WORKDIR", "/cache/anita-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-eflag-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))   # slow VAX boot
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "900"))

    # MODE:
    #   prove          (default) boot single-user, modload the vms module
    #                  against the MODULAR kernel, mknod /dev/vms, run the
    #                  cross-process event-flag proof.
    #   install-kernel boot the current (GENERIC) kernel single-user and swap
    #                  in the custom MODULAR kernel (netbsd-OVMX) as /netbsd
    #                  (idempotent, no-op if /netbsd is already OVMX).
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("EFLAG_SKIP_LOAD"))
    skip_set = bool(env("EFLAG_SKIP_SET"))

    log("NetBSD %s/%s  (OVMX/NetBSD common event flags on VAX under SIMH, P4-E)"
        % (version, arch))
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:           %s" % artifacts_dir)
    if skip_load:
        log("NEGATIVE CONTROL: EFLAG_SKIP_LOAD set -- module will NOT be "
            "loaded; the whole proof must go RED")
    if skip_set:
        log("NEGATIVE CONTROL: EFLAG_SKIP_SET set -- process A will NOT set "
            "flag 64; the cross-process 'B sees flag 64 SET' assertion must "
            "go RED")

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s (run lab-vax install first)" % iso_path)
        return HARNESS_ERROR

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
            "eflag artifact CD on rq2 (deadline %ds)..." % boot_deadline)

        a.dist.set_workdir(a.workdir)
        a.n_cdrom = 0
        child = a.start_simh(vmm_args)
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        r = child.expect([r"Enter pathname of shell or RETURN for /bin/sh:",
                          r"# "])
        if r == 0:
            child.send("\n")
            child.expect(r"# ")

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
                return PROOF_FAILED
            run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
            log("OK: installed MODULAR kernel as /netbsd (GENERIC kept as "
                "/netbsd.GENERIC), flushed to disk; the next boot runs it")
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
                      "      cp /mnt/vms.kmod /mnt/vmseflag /root/ovmx/ && "
                      "      umount /mnt && chmod +x /root/ovmx/vmseflag && break; "
                      "    else umount /mnt 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX module CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vms.kmod && test -x /root/ovmx/vmseflag",
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not find/mount the OVMX artifact CD in the guest "
                "(see the CD-device diagnostics above)")
            return PROOF_FAILED
        log("OK: staged vms.kmod + vmseflag from the OVMX CD")

        EF = "/root/ovmx/vmseflag"

        # ---- 3. INV-6 NEGATIVE CONTROL: no module loaded --------------------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "%s read 64" % EF, cmd_timeout)
        if rc == 0:
            log("FAIL (INV-6): tool returned SUCCESS with no /dev/vms present "
                "-- the faked-success bug INV-6 forbids")
            return PROOF_FAILED
        if "SS$_NOSUCHDEV" not in out or "NOT faking success" not in out:
            log("FAIL (INV-6): tool failed, but not via the honest "
                "device-unreachable path (unexpected reason)")
            return PROOF_FAILED
        log("OK (INV-6): with no module loaded the tool FAILED HONESTLY "
            "(SS$_NOSUCHDEV), it did not fake success")

        # ---- 4. load the module + create the node ---------------------------
        # NEGCTL CONTRACT (mirrors drive_devvms_vax.py's P4B_SKIP_LOAD exactly):
        # when skip_load is set, modload is skipped and /dev/vms stays ABSENT
        # (removed by the INV-6 check above, never recreated) -- the driver
        # does NOT special-case an early "negctl satisfied" exit here. It falls
        # through into the SAME cross-process proof every positive run
        # executes; that proof then fails FOR REAL (every vmseflag invocation
        # hits the honest device-unreachable path), and this function returns
        # its ordinary nonzero failure code below. run-eflag.sh's negctl-load
        # mode is what inverts "the driver exited nonzero" into "teeth
        # confirmed" -- exactly one exit-code contract for this script, shared
        # by the positive and negative runs, so the two sides can never
        # disagree about what "the harness passed" means.
        if skip_load:
            log("SKIP: modload skipped (EFLAG_SKIP_LOAD) -- /dev/vms stays "
                "absent; the cross-process proof below must fail for real")
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
                log("FAIL: could not load the module / create /dev/vms on "
                    "NetBSD/vax (this is the modules(9)-on-vax risk P4-B "
                    "documents; console output above)")
                return PROOF_FAILED
            log("OK: module loaded and /dev/vms created on NetBSD/vax")

        # ---- 4a-4e. CROSS-PROCESS SHARED-STATE PROOF (collapsed) -----------
        # Same loss-tolerant-transport technique as drive_netbsd_p2c.py: ONE
        # in-guest command performs every step, asserts each property IN-GUEST
        # over reliable local pipes, and emits a SINGLE result token, so a lost
        # marker on the lossy SIMH serial cannot corrupt the proof. It is
        # idempotent (sets flag 64 itself before reading/clearing it), so a
        # retry re-establishes its own precondition. Every process is still a
        # DISTINCT `vmseflag' invocation (A sets, B reads, B2 reads the control
        # flag, C clears, D re-reads) -- the cross-process property is
        # UNCHANGED; only the transport is collapsed.
        if skip_set:
            log("SKIP: process A's SETEF skipped (EFLAG_SKIP_SET) -- the "
                "cross-process 'B sees flag 64 SET' assertion must fail next")
        set_a = "" if skip_set else (
            "%s set 64 >/tmp/e.out 2>&1; "
            "grep -q 'EFLAG SETEF efn=64' /tmp/e.out || F=\"$F setA\"; " % EF)
        efl = ("F=''; " + set_a +
               "%s read 64 >/tmp/e.out 2>&1; "
               "grep -q 'EFLAG SET efn=64' /tmp/e.out || F=\"$F seeB\"; "
               "%s read 65 >/tmp/e.out 2>&1; "
               "grep -q 'EFLAG CLEAR efn=65' /tmp/e.out || F=\"$F ctl65\"; "
               "%s clear 64 >/tmp/e.out 2>&1; "
               "grep -q 'was-set' /tmp/e.out || F=\"$F clrC\"; "
               "%s read 64 >/tmp/e.out 2>&1; "
               "grep -q 'EFLAG CLEAR efn=64' /tmp/e.out || F=\"$F seeD\"; "
               "[ -z \"$F\" ] && echo EFLAG=PASS || echo \"EFLAG=FAIL:$F\""
               % (EF, EF, EF, EF))
        rc, out = run(child, efl, cmd_timeout)
        tok = phase_token(out, "EFLAG")
        if tok != "PASS":
            if not tok or "setA" in tok or "seeB" in tok:
                log("FAIL: process B did NOT observe common flag 64 as SET -- "
                    "the cross-process shared-kernel-state proof did not hold "
                    "on NetBSD/vax (phase token: %s)" % tok)
                return PROOF_FAILED
            if "ctl65" in tok:
                log("FAIL: control flag 65 was not CLEAR -- the read does not "
                    "actually reflect per-flag state")
                return PROOF_FAILED
            if "clrC" in tok:
                log("FAIL: process C's $CLREF 64 did not report previous "
                    "state was-set -- C did not see A's set either")
                return PROOF_FAILED
            log("FAIL: process D still saw flag 64 SET after $CLREF -- the "
                "clear was not shared across processes")
            return PROOF_FAILED
        if not skip_set:
            log("OK: process A ($SETEF 64) succeeded")
        log("OK: process B (a DIFFERENT process) observed common flag 64 SET "
            "on NetBSD/vax -- the flag lives in the KERNEL, shared across "
            "processes (INV-6)")
        log("OK: control flag 65 read CLEAR -- the read reflects real "
            "per-flag state, not a blanket 'set'")
        log("OK: process C ($CLREF 64) saw previous state was-set -- it too "
            "observed A's cross-process set")
        log("OK: process D observed flag 64 CLEAR after $CLREF -- the clear "
            "was shared across processes too")

        # ---- 4f. CROSS-PROCESS CV WAIT PROOF --------------------------------
        # A waiter process BLOCKS in-kernel on common flag 66 (clear); a
        # DIFFERENT process then sets 66; the blocked waiter must WAKE. Same
        # collapsed-background technique as drive_netbsd_p2c.py's 4f
        # (bg_safe=True opts back into retry despite the internal ` & ').
        cvw = ("%s clear 66 >/dev/null 2>&1; rm -f /tmp/wait.out; F=''; "
               "%s wait 66 >/tmp/wait.out 2>&1 & WP=$!; sleep 3; "
               "grep -q 'EFLAG WAITDONE efn=66' /tmp/wait.out && F=\"$F notblocked\"; "
               "%s set 66 >/tmp/set66.out 2>&1; "
               "grep -q 'EFLAG SETEF efn=66' /tmp/set66.out || F=\"$F noset\"; "
               "i=0; while [ $i -lt 15 ]; do "
               "grep -q 'EFLAG WAITDONE efn=66' /tmp/wait.out && break; "
               "i=$((i+1)); sleep 1; done; "
               "grep -q 'EFLAG WAITDONE efn=66' /tmp/wait.out || F=\"$F nowake\"; "
               "kill $WP 2>/dev/null; wait 2>/dev/null; "
               "[ -z \"$F\" ] && echo CVWAIT=PASS || echo \"CVWAIT=FAIL:$F\""
               % (EF, EF, EF))
        rc, out = run(child, cvw, cmd_timeout, bg_safe=True)
        tok = phase_token(out, "CVWAIT")
        if tok != "PASS":
            if tok and "notblocked" in tok:
                log("FAIL: the waiter returned BEFORE any process set flag 66 "
                    "-- the wait has no teeth (it did not actually block)")
                return PROOF_FAILED
            if tok and "noset" in tok:
                log("FAIL: could not set common flag 66 to wake the waiter")
                return PROOF_FAILED
            log("FAIL: the blocked waiter did NOT wake after a DIFFERENT "
                "process set common flag 66 on NetBSD/vax -- the cross-process "
                "cv wake was LOST (phase token: %s)" % tok)
            return PROOF_FAILED
        log("OK: the waiter BLOCKED in-kernel on common flag 66 on NetBSD/vax "
            "(no WAITDONE until a DIFFERENT process set it)")
        log("OK: the blocked waiter WOKE when a DIFFERENT process set common "
            "flag 66 -- the shared exec_cv_wait/broadcast holds cross-process "
            "on real NetBSD/vax under SIMH (THE PAYOFF, rd vms-4e7)")

        # ---- 5. cleanup ------------------------------------------------------
        run(child, "modunload vms 2>/dev/null; true", cmd_timeout)
        try:
            child.sendline("halt")
        except Exception as e:
            log("note: halt send raised (harmless): %s" % e)

        log("EFLAG-VAX: ALL CHECKS PASSED -- a real in-kernel VMS "
            "common-event-flag facility on NetBSD/vax shares state across "
            "processes (INV-6)")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console")
        log("  %s" % e)
        return HARNESS_ERROR
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console)")
        log("  %s" % e)
        return HARNESS_ERROR
    except Exception:
        log("FAIL: unexpected error driving the harness")
        traceback.print_exc()
        return HARNESS_ERROR
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
