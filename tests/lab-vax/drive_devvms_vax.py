#!/usr/bin/env python3
#
# drive_devvms_vax.py - P4-B runtime proof (rd vms-f78bb, parent vms-476, epic
# vms-8e8): bring the OVMX executive `vms' pseudo-device up on REAL NetBSD/vax
# under SIMH and prove a version/ping ioctl round-trips through a live /dev/vms.
#
# This is the VAX analogue of tests/netbsd/drive_netbsd_p2b.py (the NetBSD/amd64
# module-load + ping proof), but on the VAX architecture under SIMH -- there is
# no qemu-system-vax (docs/design-ovmx-netbsd-syskrnl.md sec 6), so the emulator
# of record is SIMH (the same one the OpenVMS/vax labs use). It reuses the
# cached NetBSD/vax disk from tests/lab-vax (rd vms-0041); it NEVER installs in
# the hot path (the shared host is disk-constrained -- design-p4 sec 5).
#
# THE PATH THIS PROVES ("try modload first", design-p4 sec 4.2 / risk 2):
#   1. Boot the cached NetBSD/vax disk under SIMH, with a SECOND CD-ROM attached
#      (rq2) carrying the two CROSS-BUILT elf32-vax artifacts:
#        - vms.kmod   the loadable OVMX executive module (built on the host by
#                     tools/cross-vax/build-devvms-vax.sh; ALL OVMX symbols
#                     resolved so modload binds only real NetBSD KPIs),
#        - vmsprobe   the userspace ping probe reaching /dev/vms through the
#                     NetBSD transport seam (kif_transport_netbsd.c).
#      The VAX in-guest disk cannot hold the comp+syssrc sets a bsd.kmodule.mk
#      in-guest build needs (447 MB syssrc alone), so the module is cross-built
#      on the host against the pinned NetBSD/vax kernel headers and DELIVERED, not
#      compiled in the guest -- the one deliberate difference from the amd64 P2b.
#   2. INV-6 NEGATIVE CONTROL: with no module loaded, /dev/vms is absent and the
#      probe must fail HONESTLY (SS$_NOSUCHDEV), never fake success.
#   3. modload the module, mknod /dev/vms with the dmesg'd major, run the probe:
#      the version/ping ioctl must round-trip -> "PROBE: PING OK".
#   4. modunload + halt.
#
# If modload of the module FAILS because NetBSD/vax's modules(9) framework is
# too thin to carry a cdevsw module (the documented risk), this driver reports
# that specific failure clearly so the operator can fall back to the compile-
# into-kernel path (design-p4 risk 2). It never fakes a pass.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting a
# real NetBSD/vax under SIMH to load and TEST a real kernel module is exactly
# what tests/qemu/ does for the Linux vms.ko and tests/netbsd/ does for amd64.
#
# NEGATIVE CONTROLS (the assertion has teeth):
#   - Built in (always): step 2, the module-absent honest-failure check.
#   - P4B_SKIP_LOAD=1 : skip the modload so step 3's "PING OK" cannot happen; the
#     harness must then go RED (proves the positive assertion has teeth).
#
# The whole run is bounded by run-devvms.sh's hard `timeout`, and every in-guest
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
# alongside this script by run-devvms.sh -- no sys.path insert needed.
from vaxharness import HARNESS_ERROR, PROOF_FAILED


def log(msg):
    print("[drive_devvms_vax] %s" % msg, flush=True)


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
    """Bundle the cross-built artifacts into an ISO9660 image attached to the
    boot as a second CD (rq2). Always carries vms.kmod + vmsprobe; also carries
    the custom MODULAR kernel (netbsd-OVMX) when present, for the install-kernel
    session. The guest mounts it and copies them out."""
    for f in ("vms.kmod", "vmsprobe"):
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
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))   # slow VAX boot
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    # MODE:
    #   prove          (default) boot single-user, modload the vms module against
    #                  the MODULAR kernel, mknod /dev/vms, PING.
    #   install-kernel boot the current (GENERIC) kernel single-user and swap in
    #                  the custom MODULAR kernel (netbsd-OVMX) as /netbsd, so the
    #                  next boot has the loadable-module framework vax GENERIC
    #                  omits. Idempotent (no-op if /netbsd is already OVMX).
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("P4B_SKIP_LOAD"))

    log("NetBSD %s/%s  (OVMX/NetBSD `vms' pseudo-device on VAX under SIMH, P4-B)"
        % (version, arch))
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:           %s" % artifacts_dir)
    if skip_load:
        log("NEGATIVE CONTROL: P4B_SKIP_LOAD set -- module will NOT be loaded; "
            "the PING OK assertion must go RED")

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s (run lab-vax install first)" % iso_path)
        return HARNESS_ERROR

    # ISO dist: anita guesses arch 'vax' from the ISO name and selects the simh
    # vmm automatically. sets must match what produced the cached disk.
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

        # 2. Boot the cached disk SINGLE-USER, with our module CD attached as a
        #    SECOND CD (rq2). SINGLE-USER IS REQUIRED, not incidental: on a
        #    multiuser NetBSD, init(8) raises the kernel securelevel to 1, and
        #    secmodel_securelevel(9) then DENIES KAUTH_SYSTEM_MODULE -- so
        #    `modload' of an out-of-tree module returns EPERM ("Operation not
        #    permitted"). At securelevel 0 (single-user, before init raises it)
        #    loading a module is permitted for root. This is the standard way to
        #    load a test module on a secure BSD; it does NOT weaken the proof (the
        #    module still serves a REAL in-kernel /dev/vms; INV-6 holds).
        #
        #    anita only boots multiuser (`>>> boot dua0'), so we drive SIMH's
        #    console ROM ourselves: reuse anita's start_simh() (it writes the
        #    netbsd.ini with the disk + our rq2 CD + these vmm_args and spawns
        #    SIMH), then at the `>>>' prompt boot with R5 = RB_SINGLE (0x2) ->
        #    `B/R5:2 DUA0'.
        src_abs = os.path.abspath(src_iso)
        vmm_args = ["set rq2 cdrom", "attach -r rq2 " + src_abs]
        log("booting cached NetBSD/vax disk SINGLE-USER under SIMH with module CD "
            "on rq2 (deadline %ds)..." % boot_deadline)

        a.dist.set_workdir(a.workdir)
        a.n_cdrom = 0
        child = a.start_simh(vmm_args)
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        # Single-user: the kernel boots, mounts / read-only, and init asks for
        # the shell path. RETURN takes /bin/sh; then a `# ' root shell prompt.
        r = child.expect([r"Enter pathname of shell or RETURN for /bin/sh:",
                          r"# "])
        if r == 0:
            child.send("\n")
            child.expect(r"# ")

        con = console(child)
        # Bring the single-user shell up to a known state: a full PATH, a
        # writable root (so mknod /dev/vms and the staging copy work -- / is
        # mounted read-only in single-user), quiet echo, then a unique prompt so
        # every subsequent run() resyncs unambiguously.
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
        # vax GENERIC has no `options MODULAR', so `modctl' returns ENOSYS and no
        # module can load. We build a custom kernel (GENERIC + MODULAR) on the
        # host and install it here as /netbsd (SIMH cannot inject a kernel like
        # `qemu -kernel', so it must reach the disk). Mount the OVMX CD, copy
        # netbsd-OVMX onto /netbsd (keeping the GENERIC kernel as /netbsd.GENERIC),
        # sync, and halt; the next boot runs the MODULAR kernel. Idempotent.
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
            # Flush the swap to the disk image by remounting root read-only
            # (a clean-unmount-equivalent), so an abrupt SIMH teardown cannot lose
            # it. Then return: the finally-block terminates SIMH at once (sending
            # `halt' would leave SIMH parked at its own `sim>' prompt, not exit).
            run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
            log("OK: installed MODULAR kernel as /netbsd (GENERIC kept as "
                "/netbsd.GENERIC), flushed to disk; the next boot runs it")
            return 0

        # ---- stage the artifacts from the OVMX CD --------------------------
        # Diagnostics first: MSCP CD naming on NetBSD/vax is not obvious, so log
        # the CD devices the kernel actually attached before we try to mount.
        run(child, "dmesg | grep -iE 'cd[0-9]|mscp|uba' | tail -20; "
                   "ls /dev/cd* /dev/rcd* 2>/dev/null", cmd_timeout)

        # NetBSD/vax names MSCP CD-ROMs `racd0'/`racd1' (RRD40 on the ra-class
        # MSCP driver), NOT `cd0'. The module CD (SIMH rq2) enumerates as one of
        # them; try every racd/cd unit x partition, verify vms.kmod is present.
        # DECISION IS ON THE EXIT CODE (rc), never on a substring of the captured
        # output: the VAX console ECHOES the command, so the literal command text
        # (e.g. any success marker) lands in `out' and would fool an in-`out'
        # check. rc is the shell's real $? and cannot be spoofed by echo.
        rc, out = run(child,
                      "mkdir -p /root/ovmx; ok=; "
                      "for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                      "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                      "  test -e $dev || continue; "
                      "  if mount_cd9660 $dev /mnt 2>/dev/null; then "
                      "    if test -f /mnt/vms.kmod; then ok=$dev; "
                      "      cp /mnt/vms.kmod /mnt/vmsprobe /root/ovmx/ && "
                      "      umount /mnt && chmod +x /root/ovmx/vmsprobe && break; "
                      "    else umount /mnt 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX module CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vms.kmod && test -x /root/ovmx/vmsprobe",
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not find/mount the OVMX module CD in the guest "
                "(see the CD-device diagnostics above)")
            return PROOF_FAILED
        log("OK: staged vms.kmod + vmsprobe from the OVMX CD")

        # ---- 2. INV-6 NEGATIVE CONTROL: probe with NO module loaded --------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "/root/ovmx/vmsprobe", cmd_timeout)
        if rc == 0:
            log("FAIL (INV-6): probe returned SUCCESS with no /dev/vms present "
                "-- the faked-success bug INV-6 forbids")
            return PROOF_FAILED
        if "SS$_NOSUCHDEV" not in out or "NOT faking success" not in out:
            log("FAIL (INV-6): probe failed, but not via the honest "
                "device-unreachable path (unexpected reason)")
            return PROOF_FAILED
        log("OK (INV-6): with no module loaded the probe FAILED HONESTLY "
            "(SS$_NOSUCHDEV), it did not fake success")

        # ---- 3. load the module, create the node, run the probe ------------
        # All decisions on rc (the real $?), never on a substring of `out' (the
        # VAX console echoes the command, which would spoof an in-`out' marker).
        if not skip_load:
            rc, out = run(child, "modload /root/ovmx/vms.kmod", cmd_timeout)
            if rc != 0:
                log("FAIL: modload of vms.kmod on NetBSD/vax FAILED (rc=%d). This "
                    "is the modules(9)-on-vax risk (design-p4 risk 2); the "
                    "documented fallback is compiling the driver into a custom "
                    "NetBSD/vax kernel. Console output above." % rc)
                return PROOF_FAILED
            # Parse the char major the module printed to dmesg (runtime output,
            # not command text), mknod /dev/vms, and assert the node exists.
            rc, out = run(child,
                          "MAJ=`dmesg | sed -n "
                          "'s/.*vms: registered, char major \\([0-9][0-9]*\\).*/\\1/p'"
                          " | tail -1`; "
                          "echo \"parsed major=$MAJ\"; "
                          "test -n \"$MAJ\" && "
                          "mknod /dev/vms c $MAJ 0 && chmod 666 /dev/vms && "
                          "ls -l /dev/vms && test -c /dev/vms",
                          cmd_timeout)
            if rc != 0:
                log("FAIL: module loaded but /dev/vms node could not be created "
                    "(major parse / mknod failed)")
                return PROOF_FAILED
            log("OK: module loaded and /dev/vms created on NetBSD/vax")
        else:
            log("SKIP: module NOT loaded (P4B_SKIP_LOAD) -- expecting the PING "
                "OK assertion to fail next")

        rc, out = run(child, "/root/ovmx/vmsprobe", cmd_timeout)
        if rc != 0 or "PROBE: PING OK" not in out:
            log("FAIL: the version/ping ioctl did not round-trip on NetBSD/vax "
                "(probe exit %d)" % rc)
            return PROOF_FAILED
        log("OK: PING OK -- one ioctl round-tripped through a REAL in-kernel "
            "/dev/vms on NetBSD/vax under SIMH, via kif_transport_netbsd.c")

        # ---- 4. cleanup ----------------------------------------------------
        if not skip_load:
            run(child, "modunload vms 2>/dev/null; true", cmd_timeout)

        # Single-user: no multiuser shutdown path; halt the kernel best-effort
        # then let the finally-block terminate SIMH. `halt' from the single-user
        # shell syncs and powers down.
        try:
            child.sendline("halt")
        except Exception as e:
            log("note: halt send raised (harmless): %s" % e)

        log("DEVVMS-VAX: ALL CHECKS PASSED")
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
