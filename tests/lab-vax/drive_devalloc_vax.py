#!/usr/bin/env python3
#
# drive_devalloc_vax.py - rd vms-618 (epic vms-8e8): prove the OVMX executive's
# DEVICE-ALLOCATION facility ($ALLOC / $DALLOC) is GENUINE on NetBSD/vax --
# backed by the real executive-resident device table (src/kernel-core/
# vms_devtab.c, ported into the NetBSD `vms' module by this item), not by a
# substrate-local handler that answers success.
#
# WHY THIS PROOF EXISTS. DCL's cmd_mount() (src/vmsdcl/dcl_cmd_misc.c) calls
# vms_kif_alloc(dev) BEFORE vms_kif_acp_mount(), so the installer's
# "$ MOUNT DKA100: WORK" could not get past $ALLOC while VMS_IOCTL_ALLOC
# answered ENOTTY on this substrate. The tempting "fix" -- a local ALLOC that
# returns SS$_NORMAL -- is the exact INV-6 fabrication class this project
# exists to excise: it would pass every single-process test and still be a
# facade. So the proof is deliberately shaped around the ONE property a facade
# cannot have:
#
#   *** A DEVICE ONE PROCESS ALLOCATES IS REFUSED TO ANOTHER PROCESS. ***
#
# THE FLOW (one boot, single-user, on the SAME cached MODULAR-kernel disk the
# other lab-vax proofs use; every step is a SEPARATE guest process, so "another
# process" really is another process):
#   0. modload vms.kmod + mknod /dev/vms, and read back the executive's own
#      unit-enumeration console lines -- positive evidence that DKA0:/DKA100:
#      were entered in the table FROM REAL DEVICES (ra1c/ra2c), not invented.
#   1. TEETH: alloc ZZZ0: -- a device that does not exist. MUST be
#      SS$_NOSUCHDEV (2680). A rubber-stamp ALLOC would say NORMAL here.
#   2. A: alloc_hold DKA100: -- $ALLOC succeeds (SS$_NORMAL), A holds it.
#   3. B (a DIFFERENT process, while A holds): alloc DKA100: -- MUST be
#      SS$_DEVALLOC (2112). This is the decisive step.
#   4. A releases it with an explicit $DALLOC (SS$_NORMAL) and exits.
#   5. C (a THIRD process, after the release): alloc DKA100: -- MUST now be
#      SS$_NORMAL, then $DALLOC SS$_NORMAL. So the release was real and
#      cross-process visible; the refusal in step 3 was about state, not a
#      blanket "no".
#   6. D (a FOURTH process, holding nothing): dalloc DKA100: -- MUST be
#      SS$_DEVNOTALLOC (2136), the oracle's %SYSTEM-W-DEVNOTALLOC.
#   7. The console terminal OPA0: -- created by the executive at module init,
#      by no process -- allocates and deallocates too.
#
# Exit 0 only if EVERY step above holds. Nothing is summarized away: each
# guest command's raw output is logged.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under
# SIMH to run real userspace processes against a real /dev/vms is exactly what
# tests/lab-vax/ already does for every other executive-facility proof.
import os
import re
import sys
import signal
import subprocess
import traceback

import anita

sys.path.insert(0, os.environ.get("OVMX_NETBSD_DIR", "/netbsd"))
import netbsd_console

from vaxharness import HARNESS_ERROR, PROOF_FAILED

# VMS condition values the executive returns (src/kernel/vms_internal.h; the
# NetBSD twin src/kernel-netbsd/vms_internal.h carries the same numbers).
SS_NORMAL = 1
SS_DEVALLOC = 2112       # device already allocated to another user
SS_DEVNOTALLOC = 2136    # device not allocated (by this process)
SS_NOSUCHDEV = 2680      # no such device available

TARGET = "DKA100:"       # the unit the installer's MOUNT allocates
CONSOLE = "OPA0:"        # the executive-created console terminal
ABSENT = "ZZZ0:"         # a unit that does not exist -- the teeth

HOLD_SECS = 20


def log(msg):
    print("[drive_devalloc_vax] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


_con = None


def console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def run(child, cmd, timeout, echo=True):
    return console(child).run(cmd, timeout, echo)


def build_source_iso(artifacts_dir, out_iso):
    for f in ("vms.kmod", "vmsdevalloc"):
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXDVA",
           "-o", out_iso, artifacts_dir]
    log("building artifact ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("artifact ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


_STATUS_RE = re.compile(r"DEVALLOC\s+(ALLOC|DALLOC)\s+devnam=(\S+)\s+status=(\d+)")


def statuses(text):
    """[(op, devnam, status_int), ...] in the order the guest printed them."""
    return [(m.group(1), m.group(2), int(m.group(3)))
            for m in _STATUS_RE.finditer(text)]


def main():
    version = env("NETBSD_VERSION", "10.1")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    workdir = env("NETBSD_WORKDIR", "/cache/devalloc-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    dka0_img = env("OVMX_DKA0_IMG", "/cache/ovmx-ods2-vax.img")
    dka100_img = env("OVMX_DKA100_IMG", "/cache/dka100-target.img")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-devalloc-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    log("NetBSD %s/vax  (rd vms-618: genuine $ALLOC/$DALLOC over the real "
        "executive device table)" % version)

    for p in (dka0_img, dka100_img):
        if not os.path.isfile(p):
            log("FAIL: disk image not found: %s" % p)
            return HARNESS_ERROR
    if not os.path.isfile(os.path.join(workdir, "wd0.img")):
        log("FAIL: no cached wd0.img at %s" % workdir)
        return HARNESS_ERROR

    a = anita.Anita(dist=anita.ISO(iso_path, sets=sets), workdir=workdir,
                    persist=True)
    build_source_iso(artifacts_dir, src_iso)

    npass = [0]
    nfail = [0]

    def ok(msg):
        npass[0] += 1
        log("  PASS: %s" % msg)

    def bad(msg):
        nfail[0] += 1
        log("  FAIL: %s" % msg)

    def expect_status(out, op, devnam, want, what):
        got = [s for (o, d, s) in statuses(out)
               if o == op and d.rstrip(":") == devnam.rstrip(":")]
        if not got:
            bad("%s: no '%s %s' status line in the guest output" %
                (what, op, devnam))
            return False
        if got[0] != want:
            bad("%s: %s %s returned status %d, want %d" %
                (what, op, devnam, got[0], want))
            return False
        ok("%s (%s %s -> %d)" % (what, op, devnam, got[0]))
        return True

    child = None
    try:
        import pexpect

        log("ensuring the cached NetBSD/vax disk is present (no reinstall)...")
        a.install()

        vmm_args = [
            "set rq1 ra92", "attach rq1 " + os.path.abspath(dka0_img),
            "set rq2 ra92", "attach rq2 " + os.path.abspath(dka100_img),
            "set rq3 cdrom", "attach -r rq3 " + os.path.abspath(src_iso),
        ]
        log("booting MODULAR kernel SINGLE-USER; rq1 -> ra1 (DKA0:), "
            "rq2 -> ra2 (DKA100:), artifact CD on rq3 (deadline %ds)..."
            % boot_deadline)

        a.dist.set_workdir(a.workdir)
        a.n_cdrom = 0
        child = a.start_simh(vmm_args)
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        r = child.expect([r"Enter pathname of shell or RETURN for /bin/sh:", r"# "])
        if r == 0:
            child.send("\n")
            child.expect(r"# ")

        con = console(child)
        child.sendline("PATH=/sbin:/bin:/usr/sbin:/usr/bin; export PATH")
        child.expect(r"# ")
        child.sendline("fsck -y / 2>&1 | tail -1")
        child.expect(r"# ", timeout=cmd_timeout)
        child.sendline("mount -u -w /")
        child.expect(r"# ")
        child.sendline("stty -echo 2>/dev/null; true")
        child.expect(r"# ")
        con.set_unique_prompt()
        log("single-user root shell ready on NetBSD/vax")

        # ---- device nodes for BOTH MSCP units --------------------------
        rc, out = run(child, "cd /dev && sh MAKEDEV ra1 ra2 2>/dev/null; cd /; "
                      "mkdir -p /mnt2 /root/ovmx; "
                      "ls -l /dev/ra1c /dev/ra2c 2>&1", cmd_timeout)
        log("device nodes: %s" % out.strip())

        # ---- stage the module + the probe off the artifact CD ----------
        rc, out = run(child,
                      "ok=; for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                      "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                      "  test -e $dev || continue; "
                      "  if mount_cd9660 $dev /mnt2 2>/dev/null; then "
                      "    if test -f /mnt2/vmsdevalloc; then ok=$dev; "
                      "      cp /mnt2/vmsdevalloc /mnt2/vms.kmod /root/ovmx/ && "
                      "      umount /mnt2 && chmod +x /root/ovmx/vmsdevalloc && break; "
                      "    else umount /mnt2 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX CD = $ok\"; ls -l /root/ovmx; "
                      "test -x /root/ovmx/vmsdevalloc && test -f /root/ovmx/vms.kmod",
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage the OVMX artifact CD in the guest")
            return HARNESS_ERROR
        log("OK: staged vms.kmod + vmsdevalloc from the artifact CD")

        # ---- load the executive ---------------------------------------
        rc, out = run(child, "modload /root/ovmx/vms.kmod && echo MODLOAD_OK",
                      cmd_timeout)
        if rc != 0 or "MODLOAD_OK" not in out:
            log("FAIL: modload of vms.kmod failed -- the executive is not up")
            return HARNESS_ERROR
        ok("vms.kmod modloaded (the executive is live)")

        rc, out = run(child, "dmesg | grep -i '^vms:' | tail -20", cmd_timeout)
        log("executive console lines:\n%s" % out)
        # POSITIVE EVIDENCE the table was populated from REAL devices.
        if re.search(r"disk unit DKA0:\s*->\s*ra1c", out):
            ok("the executive entered DKA0: from the REAL device ra1c "
               "(device-native unit map, not a literal)")
        else:
            bad("no 'disk unit DKA0: -> ra1c' line -- the device table was not "
                "populated from a real device")
        if re.search(r"disk unit DKA100:\s*->\s*ra2c", out):
            ok("the executive entered DKA100: from the REAL device ra2c")
        else:
            bad("no 'disk unit DKA100: -> ra2c' line -- the installer's target "
                "unit is not in the executive device table")

        m = re.search(r"vms: registered, char major (\d+)", out)
        if not m:
            rc, out2 = run(child, "dmesg | grep -i 'char major'", cmd_timeout)
            m = re.search(r"char major (\d+)", out2)
        if not m:
            log("FAIL: could not read the vms char major from dmesg")
            return HARNESS_ERROR
        major = m.group(1)
        rc, out = run(child, "rm -f /dev/vms; mknod /dev/vms c %s 0 && "
                      "chmod 600 /dev/vms && ls -l /dev/vms" % major, cmd_timeout)
        if rc != 0:
            log("FAIL: could not mknod /dev/vms (major %s)" % major)
            return HARNESS_ERROR
        ok("/dev/vms live at char major %s" % major)

        P = "/root/ovmx/vmsdevalloc"

        # ==============================================================
        # STEP 1 -- TEETH. A unit that is not in the table must be refused.
        # ==============================================================
        rc, out = run(child, "%s alloc %s" % (P, ABSENT), cmd_timeout)
        expect_status(out, "ALLOC", ABSENT, SS_NOSUCHDEV,
                      "TEETH: $ALLOC of a device that does not exist is "
                      "SS$_NOSUCHDEV (the table is a real lookup, not a "
                      "rubber stamp)")

        # ==============================================================
        # STEP 2/3 -- THE DECISIVE CROSS-PROCESS CHECK.
        # A holds DKA100:; B, a DIFFERENT process, must be refused.
        # ==============================================================
        rc, out = run(child,
                      "rm -f /tmp/a.log; %s alloc_hold %s %d > /tmp/a.log 2>&1 & "
                      "sleep 5; cat /tmp/a.log" % (P, TARGET, HOLD_SECS),
                      cmd_timeout)
        a_started = expect_status(out, "ALLOC", TARGET, SS_NORMAL,
                                  "process A: $ALLOC %s succeeds" % TARGET)

        rc, out = run(child, "%s alloc %s" % (P, TARGET), cmd_timeout)
        if a_started:
            expect_status(out, "ALLOC", TARGET, SS_DEVALLOC,
                          "process B (a DIFFERENT process, while A holds it): "
                          "$ALLOC %s is REFUSED SS$_DEVALLOC -- the allocation "
                          "lives in the EXECUTIVE, not in process A" % TARGET)
        else:
            bad("process B check skipped: A never got the allocation")

        # ---- STEP 4: A releases it explicitly and exits ---------------
        rc, out = run(child, "sleep %d; cat /tmp/a.log; wait 2>/dev/null; true"
                      % (HOLD_SECS + 5), cmd_timeout)
        log("process A transcript:\n%s" % out)
        dallocs = [s for (o, d, s) in statuses(out) if o == "DALLOC"]
        if dallocs and dallocs[0] == SS_NORMAL:
            ok("process A: $DALLOC %s released the allocation (SS$_NORMAL)"
               % TARGET)
        else:
            bad("process A: $DALLOC %s did not report SS$_NORMAL (saw %r)"
                % (TARGET, dallocs))

        # ---- STEP 5: a THIRD process can now allocate it --------------
        rc, out = run(child, "%s alloc_dalloc %s" % (P, TARGET), cmd_timeout)
        expect_status(out, "ALLOC", TARGET, SS_NORMAL,
                      "process C (a THIRD process, after A released): $ALLOC %s "
                      "now SUCCEEDS -- the release was real and cross-process "
                      "visible" % TARGET)
        expect_status(out, "DALLOC", TARGET, SS_NORMAL,
                      "process C: $DALLOC %s" % TARGET)

        # ---- STEP 6: $DALLOC without an allocation is refused ---------
        rc, out = run(child, "%s dalloc %s" % (P, TARGET), cmd_timeout)
        expect_status(out, "DALLOC", TARGET, SS_DEVNOTALLOC,
                      "process D (holding nothing): $DALLOC %s is refused "
                      "SS$_DEVNOTALLOC" % TARGET)

        # ---- STEP 7: the executive-created console terminal -----------
        rc, out = run(child, "%s alloc_dalloc %s" % (P, CONSOLE), cmd_timeout)
        expect_status(out, "ALLOC", CONSOLE, SS_NORMAL,
                      "the console terminal %s -- created by the EXECUTIVE at "
                      "module init, by no process -- allocates" % CONSOLE)
        expect_status(out, "DALLOC", CONSOLE, SS_NORMAL,
                      "$DALLOC %s" % CONSOLE)

        run(child, "modunload vms 2>/dev/null; true", cmd_timeout)

        log("=" * 70)
        log("DEVALLOC RESULT: %d passed, %d failed" % (npass[0], nfail[0]))
        log("=" * 70)
        if nfail[0] == 0:
            log("VMS-618: $ALLOC/$DALLOC on NetBSD/vax are GENUINE -- backed by")
            log("  the real executive-resident device table: a unit that does not")
            log("  exist is refused, a unit ONE process allocates is refused to")
            log("  ANOTHER with SS$_DEVALLOC, and the release is real and")
            log("  cross-process visible.")
            return 0
        return PROOF_FAILED

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console"); log("  %s" % e)
        return HARNESS_ERROR
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console)"); log("  %s" % e)
        return HARNESS_ERROR
    except Exception:
        log("FAIL: unexpected error driving the harness"); traceback.print_exc()
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
