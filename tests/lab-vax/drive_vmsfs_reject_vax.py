#!/usr/bin/env python3
#
# drive_vmsfs_reject_vax.py - vms-1c7 regression proof (epic vms-8e8): mounting
# a REAL-ODS2-formatted (DECFILE11B) volume through vmsfs.kmod on NetBSD/vax
# must be rejected with a CLEAN error (EINVAL, "bad home block magic") -- the
# module understands only the OVMX bespoke-VMFS format, not real ODS-2 -- and
# the guest kernel must NOT panic doing so.
#
# THE BUG (rd vms-1c7, found during vms-d5d raw-read verification): the
# mount-rejection `fail:' cleanup in src/kernel-netbsd/vmsfs/vmsfs_vfsops.c
# closed the device vnode FREAD-only, but vn_bdev_openpath() opened it
# FREAD | FWRITE and bumped v_writecount -- so the close left the open/ref
# counts imbalanced and the guest PANICKED ("vrelel: bad ref count") when the
# last vnode ref dropped, instead of returning cleanly to the mount(2) caller.
# THE FIX: close FREAD | FWRITE in the fail: path too, matching
# vn_bdev_openpath()'s open mode and vmsfs_vfs_unmount()'s own close.
#
# THE FLOW (single boot, single-user, reusing the SAME cached disk + MODULAR
# kernel + vmsfs.kmod cross-build run-vmsfs.sh already produces):
#   1. Boot MODULAR kernel single-user, with a REAL-ODS2 (DECFILE11B) volume
#      attached as a second MSCP disk (rq1 -> ra1) -- mastered by the
#      standalone tests/qemu/mkimage_ods2_real.c host tool, NOT
#      tests/qemu/mkimage_vmsfs.c's bespoke-VMFS mastering -- and the
#      artifact CD (rq2) carrying vmsfs.kmod + vmsfs_mount.
#   2. modload vmsfs.kmod (must succeed; a modload failure is a HARNESS_ERROR
#      -- it means the mount-rejection path this proof targets was never
#      reached).
#   3. Attempt to mount the real-ODS2 volume on every ra1 partition. THE
#      ASSERTION: every attempt must FAIL (nonzero exit) -- never mount --
#      and the console log must show "bad home block magic" and must NOT
#      show "panic" or "vrelel" anywhere.
#   4. ALIVE CHECK: run a fresh, unrelated command right after the rejected
#      mount attempt and assert it returns cleanly. This is the actual teeth
#      against the pre-fix bug: a panicked guest kernel cannot answer this --
#      the console hangs and the harness itself times out (HARNESS_ERROR),
#      which is exactly how the bug was originally found.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under
# SIMH to load + exercise a real kernel filesystem module is exactly what
# tests/lab-vax/'s sibling drivers already do.
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
    print("[drive_vmsfs_reject_vax] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


_con = None


def console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def run(child, cmd, timeout, echo=True, retriable=True):
    return console(child).run(cmd, timeout, echo, retriable=retriable)


def build_source_iso(artifacts_dir, out_iso):
    for f in ("vmsfs.kmod", "vmsfs_mount"):
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing cross-built artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXREJ",
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
    real_ods2_img = env("OVMX_REAL_ODS2_IMG", "/cache/ovmx-ods2-real-vax.img")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vmsfs-reject-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))
    mount_timeout = int(env("VMSFS_REJECT_MOUNT_TIMEOUT", "180"))

    log("NetBSD %s/vax  (vms-1c7 mount-rejection-panic regression proof)" % version)
    log("cached disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts: %s   real-ODS2 image: %s" % (artifacts_dir, real_ods2_img))

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s" % iso_path); return HARNESS_ERROR
    if not os.path.isfile(real_ods2_img):
        log("FAIL: real-ODS2 image not found at %s" % real_ods2_img); return HARNESS_ERROR

    a = anita.Anita(dist=anita.ISO(iso_path, sets=sets), workdir=workdir, persist=True)
    build_source_iso(artifacts_dir, src_iso)

    child = None
    try:
        import pexpect

        log("ensuring cached NetBSD/vax disk is present (cache-aware install)...")
        a.install()
        log("cached disk present")

        src_abs = os.path.abspath(src_iso)
        ods2_abs = os.path.abspath(real_ods2_img)
        vmm_args = [
            "set rq1 ra92", "attach rq1 " + ods2_abs,      # the REAL-ODS2 volume
            "set rq2 cdrom", "attach -r rq2 " + src_abs,   # vmsfs.kmod + vmsfs_mount
        ]
        log("booting MODULAR kernel SINGLE-USER; real-ODS2 volume on rq1, "
            "artifact CD on rq2 (deadline %ds)..." % boot_deadline)

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

        # ---- device nodes ----------------------------------------------
        run(child, "dmesg | grep -iE 'ra[0-9]|mscp' | tail -20", cmd_timeout)
        run(child, "cd /dev && sh MAKEDEV ra1 2>/dev/null; cd /; "
                   "mkdir -p /ods2 /mnt2; ls -l /dev/ra1* 2>/dev/null", cmd_timeout)

        # ---- stage vmsfs.kmod + vmsfs_mount from the artifact CD -------
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
            return HARNESS_ERROR
        log("OK: staged vmsfs.kmod + vmsfs_mount from the artifact CD")

        # ---- modload (must succeed to reach the code path under test) --
        rc, out = run(child, "modload /root/ovmx/vmsfs.kmod", cmd_timeout,
                      retriable=False)
        if rc != 0:
            log("FAIL: modload of vmsfs.kmod FAILED (rc=%d) -- cannot exercise "
                "the mount-rejection path" % rc)
            return HARNESS_ERROR
        log("OK: vmsfs module loaded (vfs_attach registered 'vmsfs')")

        parts = "c a d"

        # ---- THE PROOF: mount the REAL-ODS2 volume; must be REJECTED ---
        # dmesg is cleared first so the post-attempt dmesg tail is
        # unambiguously about THIS mount attempt.
        run(child, "dmesg -c >/dev/null 2>&1; true", cmd_timeout)
        log("=" * 70)
        log("ATTEMPTING to mount the REAL-ODS2 (DECFILE11B) volume via "
            "vmsfs.kmod -- this is the vms-1c7 pre-fix panic trigger")
        log("=" * 70)
        # A single bounded timeout on this one command: pre-fix, a panic hangs
        # the console and this raises pexpect.TIMEOUT -> HARNESS_ERROR (the
        # honest "the harness itself broke" signal, same as the original
        # discovery). Post-fix, vmsfs_mount fails fast on every partition.
        rc, out = run(child,
                      "ok=1; for p in %s; do "
                      "  /root/ovmx/vmsfs_mount /dev/ra1$p /ods2 2>&1 && ok=0 && break; "
                      "done; "
                      "umount /ods2 2>/dev/null; "
                      "test $ok -ne 0" % parts,
                      mount_timeout, retriable=False)
        if rc != 0:
            log("FAIL (INV-6/vms-1c7): mounting the real-ODS2 volume as 'vmsfs' "
                "SUCCEEDED (rc=%d) -- it must be REJECTED, not silently accepted "
                "as a foreign format" % rc)
            return PROOF_FAILED
        log("OK: the mount attempt was rejected on every partition (rc=%d)" % rc)

        rc, dmesg_out = run(child, "dmesg | tail -40", cmd_timeout)
        combined = (out or "") + "\n" + (dmesg_out or "")
        if "bad home block magic" not in combined:
            log("FAIL: the rejection did not log the expected 'bad home block "
                "magic' diagnostic -- got:\n%s" % combined)
            return PROOF_FAILED
        log("OK: kernel logged 'bad home block magic' -- the format-mismatch "
            "detection itself still works")

        lowered = combined.lower()
        if "panic" in lowered or "vrelel" in lowered:
            log("FAIL (vms-1c7): the console log contains 'panic'/'vrelel' -- "
                "the guest kernel panicked during mount rejection:\n%s" % combined)
            return PROOF_FAILED
        log("OK (vms-1c7): no 'panic'/'vrelel' anywhere in the mount-rejection "
            "console output")

        # ---- ALIVE CHECK: the actual teeth ------------------------------
        # If the pre-fix bug fired, the guest kernel is dead by now and this
        # command's marker never returns -> run() raises pexpect.TIMEOUT,
        # caught below as HARNESS_ERROR. A live guest answers immediately.
        rc, out = run(child, "echo VMS_1C7_ALIVE_CHECK_OK; uname -srm", cmd_timeout)
        if rc != 0 or "VMS_1C7_ALIVE_CHECK_OK" not in out:
            log("FAIL: guest did not answer the post-rejection alive check "
                "cleanly (rc=%d, out=%r)" % (rc, out))
            return PROOF_FAILED
        alive_tail = out.strip().splitlines()[-1] if out.strip() else ""
        log("OK (vms-1c7): guest kernel is ALIVE and responsive after the "
            "rejected mount -- %s" % alive_tail)

        # cleanup
        run(child, "modunload vmsfs 2>/dev/null; true", cmd_timeout)

        log("VMSFS-REJECT-VAX: ALL CHECKS PASSED -- real-ODS2 mount cleanly "
            "REJECTED (EINVAL/'bad home block magic'), NO panic, guest alive "
            "(vms-1c7 fixed)")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD/vax console -- if this "
            "happened right after the mount attempt, THIS IS THE PRE-FIX "
            "vms-1c7 PANIC (guest kernel died, console never returned)")
        log("  %s" % e)
        return HARNESS_ERROR
    except pexpect.EOF as e:
        log("FAIL: SIMH exited unexpectedly (EOF on the console) -- if this "
            "happened right after the mount attempt, THIS IS CONSISTENT WITH "
            "THE PRE-FIX vms-1c7 PANIC")
        log("  %s" % e)
        return HARNESS_ERROR
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
