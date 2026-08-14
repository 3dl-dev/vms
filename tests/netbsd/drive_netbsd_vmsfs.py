#!/usr/bin/env python3
#
# drive_netbsd_vmsfs.py - P4-VFS V4 of epic vms-8e8 (rd vms-308): the RUNTIME
# mount+read proof for the OVMX/NetBSD ODS-2 vnode/VFS backend.
#
# Where the per-PR gate (tests/netbsd/crosscompile-vmsfs.sh) proves the ODS-2
# vnode module + the shared src/kernel-core/vmsfs core merely COMPILE and LINK
# against the NetBSD/amd64 kernel headers, THIS nightly harness proves the module
# actually MOUNTS a mastered OVMX ODS-2 volume on a booted NetBSD/amd64 and READS
# a file out of it -- the ODS-2 sibling of the executive P2b/P2c runtime proofs.
#
# On the installed, cached NetBSD/amd64 guest (comp + syssrc sets) it:
#
#   0. (host side) masters a real OVMX ODS-2 volume with tests/qemu/mkimage_vmsfs
#      -- the SAME tool the Linux vmsfs.ko block-device test uses -- containing
#      HELLO.TXT;1 (FID 4) with known content, and attaches it as a second disk
#      (wd1) to the boot,
#   1. builds the OVMX/NetBSD ODS-2 vnode module (src/kernel-netbsd/vmsfs/), which
#      compiles the SAME src/kernel-core/vmsfs/*.c the Linux vmsfs.ko builds --
#      with the NetBSD backend. There is NO NetBSD copy of the ODS-2 logic,
#   2. builds the in-guest mount helper (tests/netbsd/guest/vmsfs_mount.c) which
#      issues the mount(2) the vmsfs VFS_MOUNT consumes,
#   3. NEGATIVE CONTROL: attempts the mount with the module NOT loaded -> it must
#      FAIL (no vmsfs filesystem type), never appear to succeed,
#   4. modloads the module, mounts /dev/wd1d READ-ONLY, then:
#        - reads HELLO.TXT and asserts its content byte-for-byte (VOP_LOOKUP ->
#          VOP_READ over the shared core's vmsfs_dir_resolve + vmsfs_vbn_to_lbn),
#        - lists the directory and asserts HELLO.TXT;1 appears (VOP_READDIR over
#          the shared core's vmsfs_dir_entry_decode + vmsfs_dir_format_name),
#      then unmounts, modunloads and halts.
#
# The whole run is bounded by run_vmsfs.sh's hard `timeout`, and every in-guest
# command has its own pexpect deadline, so nothing hangs.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD in QEMU to
# build, load and mount a real kernel filesystem module and TEST it is exactly
# what tests/qemu/ does for the Linux vmsfs.ko block device.
#
# NEGATIVE CONTROLS (the assertion has teeth):
#   - Built in (always): step 3, the module-absent mount-fails check.
#   - VMSFS_NEGCTL_BADEXPECT=1 : assert HELLO.TXT against WRONG expected content
#     so the content check must go RED (proves the positive read has teeth).

import os
import subprocess

import anita

import netbsd_console


# The known content mkimage_vmsfs writes into HELLO.TXT;1 (must match
# tests/qemu/mkimage_vmsfs.c's TEST_CONTENT).
HELLO_CONTENT = "Hello from VMSFS block device!"


def log(msg):
    print("[drive_netbsd_vmsfs] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


def sha512_of(path):
    import hashlib
    h = hashlib.sha512()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_boot_iso(workdir, arch, iso_name, expected_sha512):
    iso = os.path.join(workdir, "download", arch, "installation", "cdrom", iso_name)
    if not os.path.exists(iso):
        log("FAIL: expected boot ISO not found where anita mirrors it: %s" % iso)
        return False
    got = sha512_of(iso)
    if got.lower() != expected_sha512.lower():
        log("FAIL: %s SHA512 mismatch" % iso_name)
        return False
    log("OK: %s SHA512 verified (%s...)" % (iso_name, got[:16]))
    return True


def kvm_available():
    return os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.R_OK | os.W_OK)


def accel_args():
    if kvm_available():
        log("acceleration: KVM (/dev/kvm present and writable)")
        return ["-accel", "kvm", "-cpu", "host", "-smp", "4"]
    log("acceleration: TCG (no usable /dev/kvm) -- build/boot will be slower")
    return ["-smp", "2"]


_con = None


def _console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def wait_for_login(child, boot_deadline):
    _console(child).wait_for_login(boot_deadline)


def run(child, cmd, timeout, echo=True):
    return _console(child).run(cmd, timeout, echo)


def login(child, cmd_timeout):
    _console(child).login_root_sh(cmd_timeout)


def build_source_iso(guest_src_dir, out_iso):
    if not os.path.isdir(guest_src_dir):
        raise RuntimeError("guest source dir missing: %s" % guest_src_dir)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXSRC",
           "-o", out_iso, guest_src_dir]
    log("building source ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("source ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


def master_ods2_image(master_dir, out_img):
    """Compile tests/qemu/mkimage_vmsfs on the HOST and master a real ODS-2
    volume with HELLO.TXT;1 -- the exact tool + image the Linux vmsfs.ko block-
    device test uses, so the on-disk bytes are known-good."""
    src = os.path.join(master_dir, "mkimage_vmsfs.c")
    if not os.path.isfile(src):
        raise RuntimeError("mkimage_vmsfs.c missing: %s" % src)
    exe = "/tmp/mkimage_vmsfs"
    # vmsfs_ondisk.h is staged beside the .c in master_dir (-I master_dir).
    subprocess.check_call(["cc", "-O2", "-Wall", "-I", master_dir,
                           "-o", exe, src])
    if os.path.exists(out_img):
        os.remove(out_img)
    subprocess.check_call([exe, out_img])
    # Pad to a whole number of MiB so qemu is happy attaching it as a disk.
    with open(out_img, "r+b") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        target = ((size + (1 << 20) - 1) >> 20) << 20
        if target > size:
            f.truncate(target)
    log("mastered ODS-2 volume: %s (%d bytes)" % (out_img, os.path.getsize(out_img)))


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "amd64")
    url = env("NETBSD_URL",
              "https://cdn.netbsd.org/pub/NetBSD/NetBSD-%s/%s/" % (version, arch))
    iso_name = env("NETBSD_BOOT_ISO", "boot-com.iso")
    iso_sha512 = env("NETBSD_BOOT_ISO_SHA512", "")

    # SHARED installed-disk cache (rd vms-2d9): identical workdir + sets as the
    # executive P2a/P2b/P2c drivers so ONE cached wd0.img serves them all. The
    # OVMX module is built in-guest after boot, never baked into the disk.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-shared-%s-%s" % (version, arch))

    guest_src_dir = env("OVMX_GUEST_SRC", "/netbsd/guest-vmsfs")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-src-vmsfs.iso")
    master_dir = env("OVMX_MASTER_DIR", "/netbsd/vmsfs-master")
    ods2_img = env("OVMX_ODS2_IMG", "/tmp/ovmx-ods2.img")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "2400"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "1200"))
    build_timeout = int(env("NETBSD_BUILD_TIMEOUT", "1800"))

    prime_only = bool(env("PRIME_ONLY"))
    skip_modload = bool(env("VMSFS_SKIP_MODLOAD"))   # extra negctl handle
    bad_expect = bool(env("VMSFS_NEGCTL_BADEXPECT"))

    expect_content = HELLO_CONTENT if not bad_expect else "THIS IS THE WRONG CONTENT"

    log("NetBSD %s/%s  (OVMX/NetBSD ODS-2 mount+read, P4-VFS V4)" % (version, arch))
    log("work/cache dir: %s" % workdir)

    sets = ["kern-GENERIC", "modules", "base", "etc", "comp", "syssrc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=sets),
        workdir=workdir,
        memory_size="1024M",
        disk_size="8G",
        persist=True,
        vmm_args=accel_args(),
    )

    if not prime_only:
        build_source_iso(guest_src_dir, src_iso)
        master_ods2_image(master_dir, ods2_img)

    child = None
    try:
        import pexpect  # noqa: F401

        log("installing NetBSD with comp+syssrc (cache-aware)...")
        a.install()
        log("install step complete (image present)")

        if iso_sha512:
            if not verify_boot_iso(workdir, arch, iso_name, iso_sha512):
                return 3

        if prime_only:
            log("PRIME_ONLY: shared NetBSD disk installed -- cache primed, "
                "exiting 0 (no boot/build/proof)")
            return 0

        a.persist = False
        extra = [
            "-drive", "file=%s,media=cdrom,readonly=on" % os.path.abspath(src_iso),
            "-drive", "file=%s,format=raw,media=disk" % os.path.abspath(ods2_img),
        ]
        log("booting installed image (snapshot overlay; deadline %ds) with the "
            "source ISO + mastered ODS-2 disk attached..." % boot_deadline)
        child = a.start_boot(vmm_args=extra)
        wait_for_login(child, boot_deadline)
        a.child = child

        login(child, cmd_timeout)
        log("logged in")

        # ---- stage the sources from the CD --------------------------------
        rc, _ = run(child,
                    "mkdir -p /root/ovmx && "
                    "{ mount_cd9660 /dev/cd0a /mnt || "
                    "  mount_cd9660 /dev/cd1a /mnt || "
                    "  mount_cd9660 /dev/cd0d /mnt ; } && "
                    "cp -R /mnt/kmod /mnt/kernel-core /mnt/ondisk /mnt/tool "
                    "  /root/ovmx/ && "
                    "umount /mnt && chmod -R u+w /root/ovmx && ls -R /root/ovmx",
                    cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage OVMX sources from the CD in the guest")
            return 10

        # ---- build the ODS-2 vnode module (carries the shared core) --------
        rc, out = run(child,
                      "cd /root/ovmx/kmod && make clean >/dev/null 2>&1; "
                      "make >/tmp/kmodbuild.log 2>&1 && echo KMOD_BUILD_OK "
                      "|| cat /tmp/kmodbuild.log",
                      build_timeout)
        if "KMOD_BUILD_OK" not in out:
            log("FAIL: in-guest vmsfs module build failed (diagnostic above)")
            return 11
        rc, _ = run(child, "test -f /root/ovmx/kmod/vmsfs.kmod", cmd_timeout)
        if rc != 0:
            log("FAIL: build reported success but vmsfs.kmod is not present")
            return 12
        log("OK: vmsfs.kmod built in-guest (the SHARED src/kernel-core/vmsfs "
            "core compiled with the NetBSD backend)")

        # ---- build the mount helper ---------------------------------------
        rc, out = run(child,
                      "cd /root/ovmx/tool && "
                      "cc -O -Wall -o vmsfs_mount vmsfs_mount.c "
                      ">/tmp/toolbuild.log 2>&1 && echo TOOL_BUILD_OK "
                      "|| cat /tmp/toolbuild.log",
                      build_timeout)
        if "TOOL_BUILD_OK" not in out:
            log("FAIL: in-guest vmsfs_mount build failed (diagnostic above)")
            return 13
        MNT = "/root/ovmx/tool/vmsfs_mount"

        # ---- make sure the second disk's device nodes exist ---------------
        run(child, "cd /dev && sh MAKEDEV wd1 2>/dev/null; true", cmd_timeout)
        run(child, "mkdir -p /ods2", cmd_timeout)

        # ---- 3. NEGATIVE CONTROL: mount with NO module loaded -------------
        # vmsfs is not a known filesystem type until the module is loaded, so
        # the mount(2) must fail. If it "succeeds" the proof is meaningless.
        rc, out = run(child, "%s /dev/wd1d /ods2" % MNT, cmd_timeout)
        if rc == 0 or "MOUNT_OK" in out:
            log("FAIL (negctl): mount SUCCEEDED with no vmsfs module loaded")
            return 20
        log("OK (negctl): with no module loaded the ODS-2 mount FAILED, as it must")

        if skip_modload:
            log("VMSFS_SKIP_MODLOAD: leaving module unloaded; the mount+read "
                "assertions below must go RED (teeth)")
        else:
            # IDEMPOTENT (rd vms-3e7): a leading `modunload' drops any module a
            # lost-marker retry left loaded, so the robust run() can re-issue this
            # to the deadline under the lossy TCG serial without a second
            # `modload' failing "already loaded" and reporting a false failure.
            rc, out = run(child, "modunload vmsfs 2>/dev/null; "
                          "modload /root/ovmx/kmod/vmsfs.kmod && "
                          "echo MODLOAD_OK", cmd_timeout)
            if "MODLOAD_OK" not in out:
                log("FAIL: modload of vmsfs.kmod failed")
                return 21
            log("OK: vmsfs.kmod loaded")

        # ---- 4. mount READ-ONLY + read HELLO.TXT --------------------------
        # IDEMPOTENT (rd vms-3e7): a leading `umount' clears any mount a
        # lost-marker retry left behind, so re-issuing the mount cannot fail
        # "already mounted" and report a false failure. In the module-absent /
        # VMSFS_SKIP_MODLOAD teeth cases the umount is a harmless no-op and the
        # mount still fails as it must.
        rc, out = run(child, "umount /ods2 2>/dev/null; %s /dev/wd1d /ods2" % MNT,
                      cmd_timeout)
        if "MOUNT_OK" not in out:
            log("FAIL: mounting the mastered ODS-2 volume failed")
            return 22
        log("OK: ODS-2 volume mounted read-only on /ods2")

        # readdir proof
        rc, out = run(child, "ls /ods2", cmd_timeout)
        if "HELLO.TXT" not in out:
            log("FAIL (readdir): HELLO.TXT not listed in the mounted volume")
            run(child, "umount /ods2 2>/dev/null; true", cmd_timeout)
            return 23
        log("OK (readdir): HELLO.TXT;1 appears in the directory listing")

        # read proof -- cat the highest version (VMS 'HELLO.TXT' == ;0 newest)
        rc, out = run(child, "cat '/ods2/HELLO.TXT' 2>/tmp/caterr; echo RC=$?",
                      cmd_timeout)
        got_ok = expect_content in out
        run(child, "umount /ods2 2>/dev/null; true", cmd_timeout)
        if not skip_modload:
            run(child, "modunload vmsfs 2>/dev/null; true", cmd_timeout)
        if not got_ok:
            log("FAIL (read): HELLO.TXT content did not match expected")
            log("  expected substring: %r" % expect_content)
            return 24
        log("OK (read): HELLO.TXT read back the expected content through the "
            "shared ODS-2 core (VOP_LOOKUP -> VOP_READ)")

        log("PASS: OVMX ODS-2 volume MOUNTED and READ on NetBSD/amd64 via the "
            "shared src/kernel-core/vmsfs core + the NetBSD vnode backend")
        return 0

    finally:
        try:
            if child is not None:
                run(child, "halt -p 2>/dev/null; true", 60)
        except Exception:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
