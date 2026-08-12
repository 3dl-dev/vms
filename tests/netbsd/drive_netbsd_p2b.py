#!/usr/bin/env python3
#
# drive_netbsd_p2b.py - Phase 2b of epic vms-8e8 (rd vms-bfe, parent vms-dd8).
#
# Extends the P2a NetBSD/amd64 harness (drive_netbsd.py) from "boot and assert
# uname" to "build + load a REAL in-kernel /dev/vms and prove one ioctl end to
# end through the transport seam". On the installed, cached NetBSD/amd64 guest it:
#
#   1. builds the OVMX/NetBSD `vms' pseudo-device (src/kernel-netbsd/) in-guest
#      with the NetBSD kernel-module toolchain (comp + syssrc sets),
#   2. builds the userspace probe (tests/netbsd/guest/vmsprobe.c) which reaches
#      /dev/vms THROUGH kif_transport_netbsd.c (the NetBSD transport seam),
#   3. INV-6 NEGATIVE CONTROL: runs the probe with the module NOT loaded ->
#      /dev/vms is absent, the probe must fail HONESTLY (SS$_NOSUCHDEV), never
#      fake success,
#   4. modloads the module, mknod's /dev/vms with the dynamically assigned
#      major, and runs the probe -> the version/ping ioctl must round-trip and
#      the probe must print PING OK,
#   5. modunloads and halts.
#
# The whole run is bounded by run_p2b.sh's hard `timeout`, and every in-guest
# command here has its own pexpect deadline, so nothing hangs.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting a
# real NetBSD in QEMU to build and load a real kernel module and TEST it is
# exactly what tests/qemu/ does for the Linux vms.ko executive.
#
# NEGATIVE CONTROLS (the assertion has teeth):
#   - Built in (always): step 3, the module-absent honest-failure check.
#   - P2B_SKIP_LOAD=1 : skip the modload so step 4's "PING OK" cannot happen;
#     the harness must then go RED (proves the positive assertion has teeth).

import os
import re
import sys
import signal
import shutil
import subprocess
import traceback

import anita


def log(msg):
    print("[drive_netbsd_p2b] %s" % msg, flush=True)


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
        log("  expected: %s" % expected_sha512)
        log("  got:      %s" % got)
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


def wait_for_login(child, boot_deadline):
    import pexpect
    child.timeout = boot_deadline
    while True:
        r = child.expect([r"\033\[c", r"\033\[5n", r"login:"])
        if r == 0:
            child.send("\033[?1;2c")
        elif r == 1:
            child.send("\033[0n")
        elif r == 2:
            return


def build_source_iso(guest_src_dir, out_iso):
    # Bundle the baked-in guest sources (kmod/ + probe/) into an ISO9660 image
    # that we attach to the boot as a second CD. The guest mounts it and copies
    # the sources out to build them. genisoimage is in the harness image
    # (tests/netbsd/Dockerfile).
    if not os.path.isdir(guest_src_dir):
        raise RuntimeError("guest source dir missing: %s" % guest_src_dir)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXSRC",
           "-o", out_iso, guest_src_dir]
    log("building source ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("source ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


# ---- in-guest command helper -------------------------------------------------

_marker_seq = [0]


def run(child, cmd, timeout, echo=True):
    """Run one /bin/sh command in the guest; return (exit_status, output).

    Uses a unique end-of-command marker so we key on the command's real exit
    status, never on a bare shell prompt that could appear inside output. The
    echoed command line contains "MARK=$?=" literally (with `$?`), which cannot
    match MARK=<digits>=, so the first digit-match is always the real result.
    """
    import pexpect
    _marker_seq[0] += 1
    mark = "OVMXP2B_%d" % _marker_seq[0]
    child.sendline("%s; echo %s=$?=" % (cmd, mark))
    child.expect(r"%s=(\d+)=" % mark, timeout=timeout)
    rc = int(child.match.group(1))
    out = child.before
    if isinstance(out, bytes):
        out = out.decode("ascii", "ignore")
    out = out.replace("\r", "")
    if echo:
        log("$ %s   -> exit %d" % (cmd, rc))
        text = out.strip()
        if text:
            for line in text.splitlines():
                print("    | %s" % line, flush=True)
    return rc, out


def login(child, cmd_timeout):
    import pexpect
    child.timeout = cmd_timeout
    child.send("\n")
    child.expect(r"login:")
    child.send("root\n")
    child.expect(r"# ")
    # A clean, quiet POSIX shell for the marker protocol.
    child.sendline("exec /bin/sh")
    child.expect(r"# ")
    child.sendline("PATH=/sbin:/usr/sbin:/bin:/usr/bin; export PATH; umask 022")
    child.expect(r"# ")


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "amd64")
    url = env("NETBSD_URL",
              "https://cdn.netbsd.org/pub/NetBSD/NetBSD-%s/%s/" % (version, arch))
    iso_name = env("NETBSD_BOOT_ISO", "boot-com.iso")
    iso_sha512 = env("NETBSD_BOOT_ISO_SHA512", "")

    # P2b uses its OWN cache/workdir: its installed image carries the `comp` +
    # `syssrc` sets (needed to build a kernel module in-guest) that the P2a
    # image does not, so it must not collide with the P2a cache.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-p2b-%s-%s" % (version, arch))

    guest_src_dir = env("OVMX_GUEST_SRC", "/netbsd/guest-src")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-src.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1200"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "180"))
    build_timeout = int(env("NETBSD_BUILD_TIMEOUT", "900"))

    skip_load = bool(env("P2B_SKIP_LOAD"))

    log("NetBSD %s/%s  (OVMX/NetBSD `vms' pseudo-device, P2b)" % (version, arch))
    log("release URL:   %s" % url)
    log("work/cache dir: %s" % workdir)
    if skip_load:
        log("NEGATIVE CONTROL: P2B_SKIP_LOAD set -- module will NOT be loaded; "
            "the PING OK assertion must go RED")

    # comp: the toolchain + /usr/share/mk (bsd.kmodule.mk).
    # syssrc: the kernel sources at /usr/src/sys that an out-of-tree module
    #         build (-isystem $S) requires. This is the widening the P2a README
    #         anticipated ("P2b can widen the sets ... comp + syssrc").
    p2b_sets = ["kern-GENERIC", "modules", "base", "etc", "comp", "syssrc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=p2b_sets),
        workdir=workdir,
        memory_size="1024M",
        disk_size="8G",           # comp + syssrc + build scratch
        persist=True,             # keep the installed image; this is the cache
        vmm_args=accel_args(),
    )

    # Build the source ISO up front (fail fast if the baked-in sources moved).
    build_source_iso(guest_src_dir, src_iso)

    child = None
    try:
        import pexpect

        # 1. Install (no-op if the cached wd0.img already exists).
        log("installing NetBSD with comp+syssrc (cache-aware)...")
        a.install()
        log("install step complete (image present)")

        # 2. Verify the pinned boot ISO checksum from anita's download mirror.
        if iso_sha512:
            if not verify_boot_iso(workdir, arch, iso_name, iso_sha512):
                return 3
        else:
            log("WARNING: no NETBSD_BOOT_ISO_SHA512 provided; skipping ISO verify")

        # 3. Boot the installed image on a snapshot overlay (the golden cache is
        #    never written), attaching the source ISO as a second CD-ROM.
        a.persist = False
        cd_args = ["-drive",
                   "file=%s,media=cdrom,readonly=on" % os.path.abspath(src_iso)]
        log("booting installed image (snapshot overlay; deadline %ds) with the "
            "source ISO attached..." % boot_deadline)
        child = a.start_boot(vmm_args=cd_args)
        wait_for_login(child, boot_deadline)
        a.child = child

        login(child, cmd_timeout)
        log("logged in")

        # ---- copy the sources out of the CD -------------------------------
        rc, _ = run(child,
                    "mkdir -p /root/ovmx && "
                    "{ mount_cd9660 /dev/cd0a /mnt || "
                    "  mount_cd9660 /dev/cd1a /mnt || "
                    "  mount_cd9660 /dev/cd0d /mnt ; } && "
                    "cp -R /mnt/kmod /mnt/probe /mnt/kernel-core /root/ovmx/ && "
                    "umount /mnt && chmod -R u+w /root/ovmx && "
                    "ls -R /root/ovmx",
                    cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage OVMX sources from the CD in the guest")
            return 10

        # ---- build the kernel module --------------------------------------
        rc, out = run(child, "cd /root/ovmx/kmod && make 2>&1", build_timeout)
        if rc != 0:
            log("FAIL: in-guest kernel-module build failed")
            return 11
        rc, _ = run(child, "test -f /root/ovmx/kmod/vms.kmod", cmd_timeout)
        if rc != 0:
            log("FAIL: build reported success but vms.kmod is not present")
            return 12
        log("OK: vms.kmod built in-guest")

        # ---- build the probe ----------------------------------------------
        rc, _ = run(child,
                    "cd /root/ovmx/probe && "
                    "cc -O -Wall -Wextra -I. -o vmsprobe "
                    "vmsprobe.c kif_transport_netbsd.c 2>&1",
                    build_timeout)
        if rc != 0:
            log("FAIL: in-guest probe build failed")
            return 13
        log("OK: vmsprobe built in-guest (through kif_transport_netbsd.c)")

        # ---- 3. INV-6 NEGATIVE CONTROL: probe with NO module loaded --------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "/root/ovmx/probe/vmsprobe", cmd_timeout)
        if rc == 0:
            log("FAIL (INV-6): probe returned SUCCESS with no /dev/vms present "
                "-- that is the faked-success bug INV-6 forbids")
            return 20
        if "SS$_NOSUCHDEV" not in out or "NOT faking success" not in out:
            log("FAIL (INV-6): probe failed, but not via the honest "
                "device-unreachable path (unexpected reason)")
            return 21
        log("OK (INV-6): with no module loaded the probe FAILED HONESTLY "
            "(SS$_NOSUCHDEV), it did not fake success")

        # ---- 4. load the module, create the node, run the probe -----------
        if not skip_load:
            rc, out = run(child,
                          "modload /root/ovmx/kmod/vms.kmod && "
                          "MAJ=`dmesg | sed -n "
                          "'s/.*vms: registered, char major \\([0-9][0-9]*\\).*/\\1/p'"
                          " | tail -1` && "
                          "echo \"parsed major=$MAJ\" && "
                          "test -n \"$MAJ\" && "
                          "mknod /dev/vms c $MAJ 0 && chmod 666 /dev/vms && "
                          "ls -l /dev/vms && test -c /dev/vms",
                          cmd_timeout)
            if rc != 0:
                log("FAIL: could not load the module / create /dev/vms")
                return 14
            log("OK: module loaded and /dev/vms created")
        else:
            log("SKIP: module NOT loaded (P2B_SKIP_LOAD) -- expecting the "
                "PING OK assertion to fail next")

        rc, out = run(child, "/root/ovmx/probe/vmsprobe", cmd_timeout)
        if rc != 0 or "PROBE: PING OK" not in out:
            log("FAIL: the version/ping ioctl did not round-trip "
                "(probe exit %d)" % rc)
            return 15
        log("OK: PING OK -- one ioctl round-tripped through a REAL in-kernel "
            "/dev/vms on NetBSD, reached via kif_transport_netbsd.c")

        # ---- 5. cleanup ----------------------------------------------------
        if not skip_load:
            run(child, "modunload vms", cmd_timeout)

        try:
            a.halt()
        except Exception as e:
            log("note: halt raised (harmless): %s" % e)

        log("P2B: ALL CHECKS PASSED")
        return 0

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out driving the NetBSD console")
        log("  %s" % e)
        return 1
    except pexpect.EOF as e:
        log("FAIL: qemu exited unexpectedly (EOF on the console)")
        log("  %s" % e)
        return 1
    except Exception:
        log("FAIL: unexpected error driving the harness")
        traceback.print_exc()
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
