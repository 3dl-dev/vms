#!/usr/bin/env python3
#
# drive_rawdisk_vax.py - vms-d5d de-risk proof (epic vms-8e8 / vms-5eb A1):
# can an ORDINARY NetBSD/vax userspace process open(2) + pread(2) a raw ODS-2
# block off an MSCP disk, and off WHICH device node -- the block node
# ("/dev/ra1c") or the raw-character node ("/dev/rra1c")?
#
# This is the raw-I/O sibling of drive_vmsfs_vax.py (which proves the KERNEL
# vmsfs.kmod module mounts + reads ODS-2 through the block device via bread());
# this proof instead asks whether the SHARED USERSPACE ods2_bdev reader
# (src/vmsfs/ods2/ods2_bdev.c, the vms-5eb atomic-flip's planned vax path) can
# reach the same bytes with a plain open()+pread() -- no kernel module at all.
#
# THE FLOW (single boot, single-user, reusing the SAME cached disk + MODULAR
# kernel drive_vmsfs_vax.py already proved boots and modloads on):
#   1. Boot single-user with the mastered ODS-2 volume on rq1 (-> ra1, same
#      volume + home block drive_vmsfs_vax.py already validates via the kernel
#      module) and an artifact CD (rq2) carrying vms_rawpread (this proof's
#      probe), vmsfs.kmod + vmsfs_mount (reused, unmodified, from the vmsfs
#      proof's cross-build -- NOT rebuilt here).
#   2. BASELINE (no module loaded, nothing mounted): vms_rawpread against
#      /dev/ra1c (block) and /dev/rra1c (raw-char) at LBN 1 (the ODS-2 home
#      block) -- open+pread+home-block-checksum-validate on EACH node.
#   3. MOUNT vmsfs read-only (modload + vmsfs_mount, same as the vmsfs proof)
#      and repeat the SAME two probes WHILE MOUNTED -- does the kmod mount
#      hold the device against a concurrent userspace opener?
#   4. UNMOUNT + modunload (retire the mount) and repeat the SAME two probes
#      AFTER retirement -- the vms-5eb flip's planned sequence (mount->flip
#      ->retire->raw-open) for the vax lane.
#
# Each of the 6 probe runs (2 nodes x 3 mount-states) is logged verbatim;
# nothing is summarized away. The exit code reflects whether the BASELINE
# (pre-mount) probes succeeded on at least one node with a validated home
# block -- that is the vms-d5d question. Mount-state and node comparisons are
# reported as findings regardless of the exit code (see the FINDING lines).
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under
# SIMH to run a real userspace probe against a real MSCP disk is exactly what
# tests/lab-vax/ already does for the kernel-module proofs.
import os
import sys
import signal
import subprocess
import traceback

import anita

sys.path.insert(0, os.environ.get("OVMX_NETBSD_DIR", "/netbsd"))
import netbsd_console

from vaxharness import HARNESS_ERROR, PROOF_FAILED


def log(msg):
    print("[drive_rawdisk_vax] %s" % msg, flush=True)


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
    for f in ("vmsfs.kmod", "vmsfs_mount", "vms_rawpread"):
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXRAW",
           "-o", out_iso, artifacts_dir]
    log("building artifact ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("artifact ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


# probe results collected across the run, for the final FINDING summary.
_results = {}


def probe(child, cmd_timeout, label, dev, lbn=1):
    """Run vms_rawpread <dev> <lbn> in the guest and record + log the result."""
    rc, out = run(child, "/root/ovmx/vms_rawpread %s %d; echo RAWPREAD_RC=$?" % (dev, lbn),
                  cmd_timeout)
    m = {}
    for line in out.splitlines():
        line = line.strip()
        if "=" in line:
            k, _, v = line.partition("=")
            m[k.strip()] = v.strip()
    log("PROBE[%s] dev=%s -> OPEN=%s PREAD=%s HOME_MATCH=%s HOME_VOLNAME=%s "
        "(ods2_fmt=%s vmfs_magic=%s) RAWPREAD_RC=%s" % (
            label, dev, m.get("OPEN", "?"), m.get("PREAD", "?"),
            m.get("HOME_MATCH", "?"), m.get("HOME_VOLNAME", "?"),
            m.get("ODS2_FORMAT", "?"), m.get("VMFS_MAGIC", "?"),
            m.get("RAWPREAD_RC", "?")))
    _results[(label, dev)] = m
    return m


def main():
    version = env("NETBSD_VERSION", "10.1")
    # ISO path is never opened when the disk cache already has wd0.img --
    # anita.Anita.install() short-circuits on wd0_path() existing -- but
    # anita.ISO() still parses the *name* to guess the arch, so keep a
    # plausibly-named (not necessarily present) path here.
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    workdir = env("NETBSD_WORKDIR", "/cache/anita-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    ods2_img = env("OVMX_ODS2_IMG", "/cache/ovmx-ods2-vax.img")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-rawdisk-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    log("NetBSD %s/vax  (vms-d5d raw ODS-2 block open()+pread() de-risk proof)" % version)
    log("cached disk workdir: %s" % workdir)
    log("artifacts: %s   ods2 image: %s" % (artifacts_dir, ods2_img))

    if not os.path.isfile(ods2_img):
        log("FAIL: mastered ODS-2 image not found at %s" % ods2_img); return HARNESS_ERROR
    if not os.path.isfile(os.path.join(workdir, "wd0.img")):
        log("FAIL: no cached wd0.img at %s -- this proof reuses the ALREADY "
            "installed + MODULAR-kernel-equipped disk from run-vmsfs.sh; "
            "run that first (or copy its cache) rather than reinstalling here"
            % workdir)
        return HARNESS_ERROR

    a = anita.Anita(dist=anita.ISO(iso_path, sets=sets), workdir=workdir, persist=True)
    build_source_iso(artifacts_dir, src_iso)

    child = None
    try:
        import pexpect

        log("ensuring cached NetBSD/vax disk is present (cache-aware; must "
            "already exist -- no reinstall attempted)...")
        a.install()
        log("cached disk present")

        src_abs = os.path.abspath(src_iso)
        ods2_abs = os.path.abspath(ods2_img)
        vmm_args = [
            "set rq1 ra92", "attach rq1 " + ods2_abs,
            "set rq2 cdrom", "attach -r rq2 " + src_abs,
        ]
        log("booting MODULAR kernel SINGLE-USER; ODS-2 volume on rq1, artifact "
            "CD on rq2 (deadline %ds)..." % boot_deadline)

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

        rc, out = run(child, "uname -srm; sysctl kern.securelevel", cmd_timeout)
        log("guest uname + securelevel: %s" % " | ".join(out.split()))

        # ---- device nodes ---------------------------------------------------
        run(child, "dmesg | grep -iE 'ra[0-9]|mscp' | tail -20", cmd_timeout)
        rc, out = run(child, "cd /dev && sh MAKEDEV ra1 2>/dev/null; cd /; "
                      "mkdir -p /ods2 /mnt2 /root/ovmx; "
                      "ls -l /dev/ra1c /dev/rra1c 2>&1", cmd_timeout)
        log("device nodes: %s" % out.strip())

        # ---- stage vms_rawpread + vmsfs.kmod + vmsfs_mount from the CD -----
        rc, out = run(child,
                      "ok=; for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                      "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                      "  test -e $dev || continue; "
                      "  if mount_cd9660 $dev /mnt2 2>/dev/null; then "
                      "    if test -f /mnt2/vms_rawpread; then ok=$dev; "
                      "      cp /mnt2/vms_rawpread /mnt2/vmsfs.kmod /mnt2/vmsfs_mount /root/ovmx/ && "
                      "      umount /mnt2 && chmod +x /root/ovmx/vms_rawpread /root/ovmx/vmsfs_mount && break; "
                      "    else umount /mnt2 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX CD = $ok\"; ls -l /root/ovmx; "
                      "test -x /root/ovmx/vms_rawpread && test -x /root/ovmx/vmsfs_mount "
                      "&& test -f /root/ovmx/vmsfs.kmod",
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not find/stage the OVMX artifact CD in the guest")
            return HARNESS_ERROR
        log("OK: staged vms_rawpread + vmsfs.kmod + vmsfs_mount from the artifact CD")

        DEV_BLOCK = "/dev/ra1c"
        DEV_RAW = "/dev/rra1c"

        # ==================================================================
        # STAGE A: BASELINE -- no module loaded, nothing mounted.
        # ==================================================================
        log("=" * 70)
        log("STAGE A: BASELINE (module NOT loaded, volume NOT mounted)")
        log("=" * 70)
        a_block = probe(child, cmd_timeout, "baseline", DEV_BLOCK)
        a_raw = probe(child, cmd_timeout, "baseline", DEV_RAW)

        # ==================================================================
        # STAGE B: WHILE MOUNTED (kmod exclusivity check -- Q2, EXPECTED to
        # possibly block; this is not a wrinkle, it is the flip's planned
        # "retire the mount first" sequence being exercised for real).
        #
        # KNOWN GUEST-KERNEL PANIC (found running this proof, rd vms-d5d,
        # 2026-08): if OVMX_ODS2_IMG is a REAL ODS-2 (DECFILE11B) volume
        # (e.g. one mastered with `vms_initialize --ods2`) rather than the
        # VMFS-format volume vmsfs.kmod actually understands, the
        # vmsfs_mount attempt below correctly logs
        # "vmsfs: bad home block magic 0x00000001" but then the NetBSD/vax
        # guest PANICS in the mount-rejection cleanup path ("panic: vrelel:
        # bad ref count") -- a real vnode-refcount bug in the NetBSD vmsfs
        # VFS_MOUNT failure path (src/kernel-netbsd/vmsfs/vmsfs_vfsops.c),
        # not a SIMH/harness artifact. Only run STAGE B against a volume
        # vmsfs.kmod is actually expected to mount (the VMFS-format image
        # tests/qemu/mkimage_vmsfs.c masters); against a real-ODS2 image
        # this stage will be skipped (mount fails on every partition) --
        # UNLESS the panic fires first, in which case the harness times out
        # driving a dead console and exits HARNESS_ERROR. This is flagged
        # for the reader, not silently worked around: the flip (vms-5eb)
        # must not let any code path attempt a vmsfs-fstype mount(2) against
        # a real-ODS2-formatted disk until this is fixed.
        # ==================================================================
        rc, out = run(child, "modload /root/ovmx/vmsfs.kmod", cmd_timeout)
        modloaded = (rc == 0)
        if not modloaded:
            log("NOTE: modload of vmsfs.kmod failed (rc=%d) -- skipping the "
                "mounted-state probe; baseline + post-retire results still stand" % rc)
        else:
            parts = "c a d"
            rc, out = run(child,
                          "mounted=; for p in %s; do "
                          "  if /root/ovmx/vmsfs_mount /dev/ra1$p /ods2 2>/dev/null; then mounted=ra1$p; break; fi; "
                          "done; echo \"mounted on $mounted\"; test -n \"$mounted\"" % parts,
                          cmd_timeout)
            mounted = (rc == 0)
            if not mounted:
                log("NOTE: could not mount the ODS-2 volume -- skipping the "
                    "mounted-state probe")
            else:
                log("=" * 70)
                log("STAGE B: WHILE MOUNTED (vmsfs.kmod holds the volume read-only)")
                log("=" * 70)
                b_block = probe(child, cmd_timeout, "mounted", DEV_BLOCK)
                b_raw = probe(child, cmd_timeout, "mounted", DEV_RAW)
                run(child, "cd /; umount /ods2 2>/dev/null; true", cmd_timeout)
            run(child, "modunload vmsfs 2>/dev/null; true", cmd_timeout)

        # ==================================================================
        # STAGE C: AFTER RETIRE (mount unmounted, module unloaded) -- the
        # vms-5eb flip's planned sequence for the vax lane.
        # ==================================================================
        log("=" * 70)
        log("STAGE C: AFTER RETIRE (mount unmounted, module unloaded)")
        log("=" * 70)
        c_block = probe(child, cmd_timeout, "post-retire", DEV_BLOCK)
        c_raw = probe(child, cmd_timeout, "post-retire", DEV_RAW)

        # ---- FINDINGS ---------------------------------------------------
        log("=" * 70)
        log("FINDINGS (vms-d5d)")
        log("=" * 70)

        def ok(m):
            return (m.get("OPEN") == "ok" and m.get("PREAD", "").startswith("ok")
                    and m.get("HOME_MATCH") in ("ods2", "vmfs"))

        log("FINDING Q1 (device node): baseline block(%s)=%s  baseline raw(%s)=%s" % (
            DEV_BLOCK, "PASS" if ok(a_block) else "FAIL(%s/%s)" % (a_block.get("OPEN"), a_block.get("PREAD")),
            DEV_RAW, "PASS" if ok(a_raw) else "FAIL(%s/%s)" % (a_raw.get("OPEN"), a_raw.get("PREAD"))))

        if modloaded and (("mounted", DEV_BLOCK) in _results):
            b_block = _results[("mounted", DEV_BLOCK)]
            b_raw = _results[("mounted", DEV_RAW)]
            log("FINDING Q2 (kmod exclusivity): mounted block(%s)=%s  mounted raw(%s)=%s "
                "-- EXPECTED per the flip's mount-then-retire-then-raw-open sequence; "
                "not itself a defect either way" % (
                    DEV_BLOCK, "PASS" if ok(b_block) else "FAIL(%s/%s)" % (b_block.get("OPEN"), b_block.get("PREAD")),
                    DEV_RAW, "PASS" if ok(b_raw) else "FAIL(%s/%s)" % (b_raw.get("OPEN"), b_raw.get("PREAD"))))
        else:
            log("FINDING Q2 (kmod exclusivity): NOT EXERCISED this run (modload/mount step skipped -- see NOTE above)")

        log("FINDING Q1/Q3 (post-retire, the flip's actual planned state): "
            "block(%s)=%s  raw(%s)=%s" % (
                DEV_BLOCK, "PASS" if ok(c_block) else "FAIL(%s/%s)" % (c_block.get("OPEN"), c_block.get("PREAD")),
                DEV_RAW, "PASS" if ok(c_raw) else "FAIL(%s/%s)" % (c_raw.get("OPEN"), c_raw.get("PREAD"))))

        # exit-code contract: the baseline (pre-mount) probe is the vms-d5d
        # question ("CAN userspace pread raw ODS-2 blocks at all"). Succeed if
        # EITHER node validated a real home block at baseline.
        if ok(a_block) or ok(a_raw):
            log("VMS-D5D: raw open()+pread() of a validated ODS-2 home block "
                "SUCCEEDED from NetBSD/vax userspace (see FINDING lines above "
                "for which node(s))")
            return 0
        log("VMS-D5D: raw open()+pread() did NOT produce a validated ODS-2 home "
            "block on EITHER device node at baseline -- see probe output above")
        return PROOF_FAILED

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console"); log("  %s" % e); return HARNESS_ERROR
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console)"); log("  %s" % e); return HARNESS_ERROR
    except Exception:
        log("FAIL: unexpected error driving the harness"); traceback.print_exc(); return HARNESS_ERROR
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
