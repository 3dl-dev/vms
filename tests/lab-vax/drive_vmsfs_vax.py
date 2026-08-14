#!/usr/bin/env python3
#
# drive_vmsfs_vax.py - vms-544d runtime proof (epic vms-8e8, tributary A / V5):
# a mastered OVMX ODS-2 volume MOUNTS + READS on REAL NetBSD/vax under SIMH,
# satisfying the PID-1 /vms mount (gates vms-7b1 boot-disk assembly -> the
# capstone vms-d59 "boot to a DCL prompt on vax").
#
# The VAX analogue of the NetBSD/amd64 ODS-2 mount+read proof
# (tests/netbsd/drive_netbsd_vmsfs.py, rd vms-308). It builds ON the P4-B vax
# runtime substrate (tests/lab-vax, rd vms-f78bb): the custom GENERIC+MODULAR
# /netbsd kernel is already installed on the cached disk, so this just boots it
# SINGLE-USER (securelevel 0, where modload is permitted), loads the ODS-2
# vnode/VFS module, mounts a mastered volume, and reads a known file back.
#
# INV-DRIFT: the module is the SAME shared src/kernel-core/vmsfs ODS-2 core the
# Linux vmsfs.ko and the amd64 proof build, with only the thin NetBSD vnode
# backend -- no vax-specific ODS-2 reimplementation.
#
# THE FLOW:
#   1. Boot the MODULAR kernel SINGLE-USER, with the mastered ODS-2 volume
#      attached as a SECOND MSCP DISK (rq1 -> NetBSD ra1) and the artifact CD
#      (rq2) carrying the cross-built elf32-vax vmsfs.kmod + vmsfs_mount helper.
#   2. INV-6 NEGATIVE CONTROL: with no module loaded, the mount(2) must FAIL
#      honestly (no "vmsfs" filesystem registered) -- never a faked mount.
#   3. modload vmsfs.kmod (-> vfs_attach registers "vmsfs"), mount the ra1 volume
#      READ-ONLY, then assert:
#        - `ls /ods2' lists HELLO.TXT (VOP_READDIR over the shared dir scanner),
#        - `cat /ods2/HELLO.TXT' returns the known content byte-for-byte
#          (VOP_LOOKUP -> VOP_READ through the shared retrieval-map core).
#   4. umount, modunload.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under SIMH
# to load + exercise a real kernel filesystem module is exactly what tests/qemu/
# does for the Linux vmsfs.ko and tests/netbsd/ does for amd64.
#
# NEGATIVE CONTROLS (teeth):
#   - Built in (always): step 2, module-absent mount-fails.
#   - VMSFS_SKIP_MODLOAD=1 : skip the modload so the mount/read cannot succeed;
#     the harness must then go RED.

import os
import sys
import signal
import subprocess
import traceback

import anita

sys.path.insert(0, os.environ.get("OVMX_NETBSD_DIR", "/netbsd"))
import netbsd_console


# The known HELLO.TXT content mastered by tests/qemu/mkimage_vmsfs.c
# (TEST_CONTENT). Kept in sync with that tool.
HELLO_CONTENT = "Hello from VMSFS block device!"


def log(msg):
    print("[drive_vmsfs_vax] %s" % msg, flush=True)


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
    """Bundle the cross-built vmsfs.kmod + vmsfs_mount into an ISO9660 image
    attached to the boot as a second CD (rq2)."""
    for f in ("vmsfs.kmod", "vmsfs_mount"):
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing cross-built artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXVFS",
           "-o", out_iso, artifacts_dir]
    log("building source ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("source ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


def main():
    version = env("NETBSD_VERSION", "10.1")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    workdir = env("NETBSD_WORKDIR", "/cache/anita-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    ods2_img = env("OVMX_ODS2_IMG", "/cache/ovmx-ods2-vax.img")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vmsfs-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    # MODE: prove (default) = mount+read; install-kernel = swap in the MODULAR
    # kernel (identical to drive_devvms_vax.py's, so one disk carries the module
    # framework both vax proofs need). netbsd-OVMX rides the same artifact CD.
    mode = env("OVMX_MODE", "prove")
    skip_load = bool(env("VMSFS_SKIP_MODLOAD"))

    log("NetBSD %s/vax  (OVMX ODS-2 mount+read on VAX under SIMH, vms-544d)" % version)
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts: %s   ods2 image: %s" % (artifacts_dir, ods2_img))
    if skip_load:
        log("NEGATIVE CONTROL: VMSFS_SKIP_MODLOAD set -- module will NOT be "
            "loaded; the mount+read assertion must go RED")

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s" % iso_path); return 3
    if not os.path.isfile(ods2_img):
        log("FAIL: mastered ODS-2 image not found at %s" % ods2_img); return 3

    a = anita.Anita(dist=anita.ISO(iso_path, sets=sets), workdir=workdir, persist=True)
    build_source_iso(artifacts_dir, src_iso)

    child = None
    try:
        import pexpect

        log("ensuring cached NetBSD/vax disk is present (cache-aware install)...")
        a.install()
        log("cached disk present")

        # Boot the MODULAR kernel SINGLE-USER (securelevel 0 -> modload permitted;
        # see drive_devvms_vax.py). Attach the ODS-2 volume as a 2nd MSCP disk
        # (rq1 -> ra1) and the artifact CD as rq2. anita's start_simh already sets
        # rq0 (system disk) + rq3 (install CD); rq1/rq2 are ours.
        src_abs = os.path.abspath(src_iso)
        ods2_abs = os.path.abspath(ods2_img)
        vmm_args = [
            "set rq1 ra92", "attach rq1 " + ods2_abs,   # the mastered ODS-2 volume
            "set rq2 cdrom", "attach -r rq2 " + src_abs,  # vmsfs.kmod + vmsfs_mount
        ]
        log("booting MODULAR kernel SINGLE-USER; ODS-2 volume on rq1, artifact CD "
            "on rq2 (deadline %ds)..." % boot_deadline)

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

        # ---- install-kernel mode: swap /netbsd -> the MODULAR kernel --------
        # Identical to drive_devvms_vax.py: vax GENERIC has no MODULAR, so install
        # the custom GENERIC+MODULAR kernel (netbsd-OVMX, on the artifact CD) as
        # /netbsd (keeping GENERIC as /netbsd.GENERIC), flush, and let the next
        # boot run it. Idempotent.
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
                return 30
            run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
            log("OK: installed MODULAR kernel as /netbsd; the next boot runs it")
            return 0

        # ---- diagnostics + device nodes ------------------------------------
        run(child, "dmesg | grep -iE 'ra[0-9]|mscp' | tail -20", cmd_timeout)
        # ra1 device nodes may not exist; MAKEDEV them. mkdir the mount point.
        run(child, "cd /dev && sh MAKEDEV ra1 2>/dev/null; cd /; "
                   "mkdir -p /ods2 /mnt2; ls -l /dev/ra1* 2>/dev/null", cmd_timeout)

        # ---- stage vmsfs.kmod + vmsfs_mount from the artifact CD -----------
        # NetBSD/vax names MSCP CD-ROMs racd0/racd1. Decision on rc (the console
        # echoes the command, so an in-`out' marker would be spoofed).
        rc, out = run(child,
                      "mkdir -p /root/ovmx; ok=; "
                      "for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                      "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                      "  test -e $dev || continue; "
                      "  if mount_cd9660 $dev /mnt2 2>/dev/null; then "
                      "    if test -f /mnt2/vmsfs.kmod; then ok=$dev; "
                      "      cp /mnt2/vmsfs.kmod /mnt2/vmsfs_mount /root/ovmx/ && "
                      "      umount /mnt2 && chmod +x /root/ovmx/vmsfs_mount && break; "
                      "    else umount /mnt2 2>/dev/null; fi; "
                      "  fi; done; "
                      "echo \"OVMX CD = $ok\"; ls -l /root/ovmx; "
                      "test -f /root/ovmx/vmsfs.kmod && test -x /root/ovmx/vmsfs_mount",
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not find/mount the OVMX artifact CD in the guest")
            return 10
        log("OK: staged vmsfs.kmod + vmsfs_mount from the artifact CD")

        # The whole-disk partition of a raw MSCP disk with no NetBSD disklabel:
        # try c (whole disk on vax), then a and d. The module validates the ODS-2
        # home block (magic + checksum), so a wrong-offset partition fails EINVAL
        # and only the correct one mounts -- INV-6-honest.
        parts = "c a d"

        # ---- 2. INV-6 NEGATIVE CONTROL: mount with NO module loaded --------
        rc, out = run(child,
                      "ok=1; for p in %s; do "
                      "  /root/ovmx/vmsfs_mount /dev/ra1$p /ods2 2>/dev/null && ok=0 && break; "
                      "done; "
                      "umount /ods2 2>/dev/null; "
                      "test $ok -ne 0" % parts,
                      cmd_timeout)
        if rc != 0:
            log("FAIL (INV-6): a 'vmsfs' mount SUCCEEDED with no module loaded "
                "-- the faked-mount bug INV-6 forbids")
            return 20
        log("OK (INV-6): with no vmsfs module loaded, the mount FAILED HONESTLY")

        if skip_load:
            log("SKIP: module NOT loaded (VMSFS_SKIP_MODLOAD) -- expecting the "
                "mount+read to fail next")
        else:
            rc, out = run(child, "modload /root/ovmx/vmsfs.kmod", cmd_timeout)
            if rc != 0:
                log("FAIL: modload of vmsfs.kmod on NetBSD/vax FAILED (rc=%d). "
                    "Console output above." % rc)
                return 14
            log("OK: vmsfs module loaded (vfs_attach registered 'vmsfs')")

        # ---- 3. mount the ODS-2 volume read-only ---------------------------
        rc, out = run(child,
                      "mounted=; for p in %s; do "
                      "  if /root/ovmx/vmsfs_mount /dev/ra1$p /ods2; then mounted=ra1$p; break; fi; "
                      "done; "
                      "echo \"mounted on $mounted\"; mount | grep -i ods2 || true; "
                      "test -n \"$mounted\"" % parts,
                      cmd_timeout)
        if rc != 0:
            log("FAIL: could not mount the ODS-2 volume on any ra1 partition")
            return 15
        log("OK: ODS-2 volume mounted read-only on NetBSD/vax")

        # ---- readdir: HELLO.TXT must appear (VOP_READDIR on the root) -----
        rc, out = run(child, "ls /ods2", cmd_timeout)
        if "HELLO.TXT" not in out:
            log("FAIL (readdir): HELLO.TXT not listed in the mounted volume")
            return 16
        log("OK (readdir): HELLO.TXT appears in the mounted ODS-2 volume")

        # ---- read-back: assert the known bytes (VOP_LOOKUP -> VOP_READ) ----
        rc, out = run(child, "cat /ods2/HELLO.TXT", cmd_timeout)
        if HELLO_CONTENT not in out:
            log("FAIL (read): HELLO.TXT did not read back the expected content")
            log("  expected substring: %r" % HELLO_CONTENT)
            return 17
        log("OK (read): HELLO.TXT read back the expected content through the "
            "shared vmsfs core on NetBSD/vax under SIMH")

        # ---- 4. cleanup ----------------------------------------------------
        run(child, "cd /; umount /ods2 2>/dev/null; true", cmd_timeout)
        if not skip_load:
            run(child, "modunload vmsfs 2>/dev/null; true", cmd_timeout)

        log("VMSFS-VAX: ALL CHECKS PASSED")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console"); log("  %s" % e); return 1
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console)"); log("  %s" % e); return 1
    except Exception:
        log("FAIL: unexpected error driving the harness"); traceback.print_exc(); return 2
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
