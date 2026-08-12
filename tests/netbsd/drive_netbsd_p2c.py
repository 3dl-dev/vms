#!/usr/bin/env python3
#
# drive_netbsd_p2c.py - Phase 2c of epic vms-8e8 (rd vms-4b4, parent vms-dd8).
#
# This is the phase that PROVES the executive is REAL on the NetBSD substrate.
# Where P2b (drive_netbsd_p2b.py) proved one ping ioctl round-trips through a
# real in-kernel /dev/vms, P2c proves the INV-6-decisive property (CLAUDE.md
# Rule 9): ONE real VMS executive facility -- the COMMON EVENT FLAG CLUSTERS --
# holds SYSTEM-WIDE SHARED state in the kernel, so a flag set by one process is
# seen by a DIFFERENT process. A per-process userspace fake could report ioctl
# success while sharing nothing; a real executive cannot, because there is
# exactly one copy of the flag state and it lives in the kernel.
#
# On the installed, cached NetBSD/amd64 guest (comp + syssrc sets) it:
#
#   1. builds the OVMX/NetBSD `vms' pseudo-device (src/kernel-netbsd/), which now
#      carries the common-event-flag facility (SETEF/CLREF/READEF over EFN
#      64..127, held in module-global kernel memory under a kmutex),
#   2. builds the userspace event-flag tool (tests/netbsd/guest/vmseflag.c) which
#      reaches /dev/vms THROUGH kif_transport_netbsd.c (the NetBSD transport
#      seam),
#   3. INV-6 NEGATIVE CONTROL: runs the tool with the module NOT loaded ->
#      /dev/vms is absent, the tool must fail HONESTLY (SS$_NOSUCHDEV), never
#      fake success,
#   4. modloads the module, mknod's /dev/vms, then runs the CROSS-PROCESS PROOF:
#        - process A:  vmseflag set  64   (sets common flag 64)
#        - process B:  vmseflag read 64   (a DIFFERENT process -- must see SET)
#        - process B2: vmseflag read 65   (control flag -- must be CLEAR)
#        - process C:  vmseflag clear 64  (its "previous state" must be was-set,
#                                          proving C too saw A's set)
#        - process D:  vmseflag read 64   (must now be CLEAR -- CLREF shared too)
#      The cross-process visibility (A sets, B/C/D -- separate PIDs -- observe)
#      is the executive-is-real proof.
#   5. modunloads and halts.
#
# The whole run is bounded by run_p2c.sh's hard `timeout`, and every in-guest
# command has its own pexpect deadline, so nothing hangs.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md): booting a
# real NetBSD in QEMU to build and load a real kernel module and TEST it is
# exactly what tests/qemu/ does for the Linux vms.ko executive.
#
# NEGATIVE CONTROLS (the assertion has teeth):
#   - Built in (always): step 3, the module-absent honest-failure check.
#   - P2C_SKIP_SET=1 : skip process A's SETEF so common flag 64 stays clear; the
#     cross-process "B sees flag 64 SET" assertion must then go RED (proves the
#     positive assertion has teeth and the test can fail).

import os
import sys
import signal
import subprocess
import traceback

import anita


def log(msg):
    print("[drive_netbsd_p2c] %s" % msg, flush=True)


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
    # attached to the boot as a second CD; the guest mounts it and builds from
    # the copied-out sources. genisoimage is in the harness image.
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
    """Run one /bin/sh command in the guest; return (exit_status, output)."""
    _marker_seq[0] += 1
    mark = "OVMXP2C_%d" % _marker_seq[0]
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
    child.timeout = cmd_timeout
    child.send("\n")
    child.expect(r"login:")
    child.send("root\n")
    child.expect(r"# ")
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

    # P2c uses its OWN cache/workdir: like P2b its installed image carries the
    # comp + syssrc sets (needed to build a kernel module in-guest), but it must
    # not collide with the P2b cache.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-p2c-%s-%s" % (version, arch))

    guest_src_dir = env("OVMX_GUEST_SRC", "/netbsd/guest-src")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-src-p2c.iso")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1200"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "180"))
    build_timeout = int(env("NETBSD_BUILD_TIMEOUT", "900"))

    skip_set = bool(env("P2C_SKIP_SET"))

    log("NetBSD %s/%s  (OVMX/NetBSD common event flags, P2c)" % (version, arch))
    log("release URL:   %s" % url)
    log("work/cache dir: %s" % workdir)
    if skip_set:
        log("NEGATIVE CONTROL: P2C_SKIP_SET set -- process A will NOT set flag "
            "64; the cross-process 'B sees flag 64 SET' assertion must go RED")

    p2c_sets = ["kern-GENERIC", "modules", "base", "etc", "comp", "syssrc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=p2c_sets),
        workdir=workdir,
        memory_size="1024M",
        disk_size="8G",
        persist=True,
        vmm_args=accel_args(),
    )

    build_source_iso(guest_src_dir, src_iso)

    child = None
    try:
        import pexpect

        log("installing NetBSD with comp+syssrc (cache-aware)...")
        a.install()
        log("install step complete (image present)")

        if iso_sha512:
            if not verify_boot_iso(workdir, arch, iso_name, iso_sha512):
                return 3
        else:
            log("WARNING: no NETBSD_BOOT_ISO_SHA512 provided; skipping ISO verify")

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

        # ---- stage the sources from the CD --------------------------------
        rc, _ = run(child,
                    "mkdir -p /root/ovmx && "
                    "{ mount_cd9660 /dev/cd0a /mnt || "
                    "  mount_cd9660 /dev/cd1a /mnt || "
                    "  mount_cd9660 /dev/cd0d /mnt ; } && "
                    "cp -R /mnt/kmod /mnt/probe /root/ovmx/ && "
                    "umount /mnt && chmod -R u+w /root/ovmx && "
                    "ls -R /root/ovmx",
                    cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage OVMX sources from the CD in the guest")
            return 10

        # ---- build the kernel module (now carries the eflag facility) -----
        rc, out = run(child, "cd /root/ovmx/kmod && make 2>&1", build_timeout)
        if rc != 0:
            log("FAIL: in-guest kernel-module build failed")
            return 11
        rc, _ = run(child, "test -f /root/ovmx/kmod/vms.kmod", cmd_timeout)
        if rc != 0:
            log("FAIL: build reported success but vms.kmod is not present")
            return 12
        log("OK: vms.kmod built in-guest (with the common-event-flag facility)")

        # ---- build the event-flag tool ------------------------------------
        rc, _ = run(child,
                    "cd /root/ovmx/probe && "
                    "cc -O -Wall -Wextra -I. -o vmseflag "
                    "vmseflag.c kif_transport_netbsd.c 2>&1",
                    build_timeout)
        if rc != 0:
            log("FAIL: in-guest vmseflag build failed")
            return 13
        log("OK: vmseflag built in-guest (through kif_transport_netbsd.c)")

        EF = "/root/ovmx/probe/vmseflag"

        # ---- 3. INV-6 NEGATIVE CONTROL: no module loaded ------------------
        run(child, "rm -f /dev/vms", cmd_timeout)
        rc, out = run(child, "%s read 64" % EF, cmd_timeout)
        if rc == 0:
            log("FAIL (INV-6): tool returned SUCCESS with no /dev/vms present "
                "-- the faked-success bug INV-6 forbids")
            return 20
        if "SS$_NOSUCHDEV" not in out or "NOT faking success" not in out:
            log("FAIL (INV-6): tool failed, but not via the honest "
                "device-unreachable path (unexpected reason)")
            return 21
        log("OK (INV-6): with no module loaded the tool FAILED HONESTLY "
            "(SS$_NOSUCHDEV), it did not fake success")

        # ---- 4. load the module + create the node -------------------------
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

        # ---- 4a. process A sets common flag 64 ----------------------------
        if not skip_set:
            rc, out = run(child, "%s set 64" % EF, cmd_timeout)
            if rc != 0 or "EFLAG SETEF efn=64" not in out:
                log("FAIL: process A could not set common flag 64")
                return 15
            log("OK: process A ($SETEF 64) succeeded")
        else:
            log("SKIP: process A's SETEF skipped (P2C_SKIP_SET) -- the "
                "cross-process 'B sees flag 64 SET' assertion must fail next")

        # ---- 4b. process B (DIFFERENT process) reads flag 64 --------------
        # THIS is the INV-6-decisive step: B is a separate OS process from A,
        # yet it observes A's set because the flag lives in the KERNEL.
        rc, out = run(child, "%s read 64" % EF, cmd_timeout)
        if rc != 0 or "EFLAG SET efn=64" not in out:
            log("FAIL: process B did NOT observe common flag 64 as SET -- the "
                "cross-process shared-kernel-state proof did not hold "
                "(tool exit %d)" % rc)
            return 16
        log("OK: process B (a DIFFERENT process) observed common flag 64 SET -- "
            "the flag lives in the KERNEL, shared across processes (INV-6)")

        # ---- 4c. control flag: process B2 reads flag 65 (must be CLEAR) ----
        rc, out = run(child, "%s read 65" % EF, cmd_timeout)
        if "EFLAG CLEAR efn=65" not in out:
            log("FAIL: control flag 65 was not CLEAR -- the read does not "
                "actually reflect per-flag state")
            return 17
        log("OK: control flag 65 read CLEAR -- the read reflects real per-flag "
            "state, not a blanket 'set'")

        # ---- 4d. process C clears flag 64; prev-state must be was-set ------
        rc, out = run(child, "%s clear 64" % EF, cmd_timeout)
        if rc != 0 or "was-set" not in out:
            log("FAIL: process C's $CLREF 64 did not report previous state "
                "was-set -- C did not see A's set either")
            return 18
        log("OK: process C ($CLREF 64) saw previous state was-set -- it too "
            "observed A's cross-process set")

        # ---- 4e. process D confirms flag 64 now CLEAR (CLREF shared too) ---
        rc, out = run(child, "%s read 64" % EF, cmd_timeout)
        if "EFLAG CLEAR efn=64" not in out:
            log("FAIL: process D still saw flag 64 SET after $CLREF -- the "
                "clear was not shared across processes")
            return 19
        log("OK: process D observed flag 64 CLEAR after $CLREF -- the clear was "
            "shared across processes too")

        # ---- 5. cleanup ---------------------------------------------------
        run(child, "modunload vms", cmd_timeout)
        try:
            a.halt()
        except Exception as e:
            log("note: halt raised (harmless): %s" % e)

        log("P2C: ALL CHECKS PASSED -- a real in-kernel VMS common-event-flag "
            "facility on NetBSD shares state across processes (INV-6)")
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
