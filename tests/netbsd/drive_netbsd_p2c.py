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
#      compiles the SAME event-flag facility source the Linux vms.ko builds --
#      src/kernel-core/vms_eflag.c -- with the NetBSD backend. The COMMON event
#      flag clusters (EFN 64..127) live in the module's KERNEL memory, guarded by
#      the facility's own exec_* locks (NetBSD kmutex/cv). There is NO NetBSD copy
#      of the facility logic,
#   2. builds the userspace event-flag tool (tests/netbsd/guest/vmseflag.c) which
#      reaches /dev/vms THROUGH kif_transport_netbsd.c (the NetBSD transport
#      seam); each op first $ASCEFCs the well-known PERMANENT common cluster
#      (VMS-faithful; the facility rejects an unassociated common EFN),
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
#      then the CV WAIT PROOF (4f): a waiter BLOCKS in-kernel on flag 66 and is
#      WOKEN by a DIFFERENT process's set -- the shared exec_cv_wait/broadcast
#      handshake across processes. Both are the executive-is-real proof.
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

import netbsd_console


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


# ---- deterministic console (rd vms-2d9) --------------------------------------
# All console driving goes through the shared, deterministic NetBSDConsole
# (tests/netbsd/netbsd_console.py): unique prompt, resync-on-prompt, unique
# per-command markers. The thin wrappers below keep every existing call site
# (`wait_for_login(child, ...)', `login(child, ...)', `run(child, cmd, t)')
# unchanged; the console is created lazily from the live child.
_con = None


def _console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def wait_for_login(child, boot_deadline):
    _console(child).wait_for_login(boot_deadline)


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


# ---- in-guest command helper (delegates to the deterministic console) --------


def run(child, cmd, timeout, echo=True):
    """Run one /bin/sh command in the guest; return (exit_status, output)."""
    return _console(child).run(cmd, timeout, echo)


def login(child, cmd_timeout):
    _console(child).login_root_sh(cmd_timeout)


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
    # SHARED installed-disk cache (rd vms-2d9): identical workdir + sets across
    # P2a/P2b/P2c so ONE cached wd0.img (keyed on the NetBSD version only) serves
    # all three. The OVMX module is built in-guest after boot, never baked into
    # the disk -- so nothing OVMX-source-dependent belongs here. Keep IDENTICAL
    # across the three drivers.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-shared-%s-%s" % (version, arch))

    guest_src_dir = env("OVMX_GUEST_SRC", "/netbsd/guest-src")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-src-p2c.iso")

    # Deadlines are generous for the CI case: GitHub runners have no /dev/kvm,
    # so the guest runs under TCG and every in-guest command is much slower than
    # a local KVM run. The build is quiet (redirected to a file) so it no longer
    # floods the console, but a slow guest can still take a while per command.
    # Backstop headroom for a GENUINE cold install/boot (rd vms-2d9): the shared
    # version-keyed cache means routine runs restore a warm image (~8min), but
    # the first-ever/version-bump install boots cold on a possibly-loaded runner,
    # where the boot and the first login can be slow. This is not a substitute
    # for the deterministic console (that fixes the desync); it is only room for
    # a legitimately slow cold boot so it does not trip a tight deadline.
    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "2400"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))
    build_timeout = int(env("NETBSD_BUILD_TIMEOUT", "1800"))

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
                    "cp -R /mnt/kmod /mnt/probe /mnt/kernel-core /root/ovmx/ && "
                    "umount /mnt && chmod -R u+w /root/ovmx && "
                    "ls -R /root/ovmx",
                    cmd_timeout)
        if rc != 0:
            log("FAIL: could not stage OVMX sources from the CD in the guest")
            return 10

        # ---- build the kernel module (now carries the eflag facility) -----
        # The in-guest kernel-module build is a CLEAN rebuild every run: the
        # sources are freshly copied from the source ISO above (no build cache
        # is carried across runs -- only the installed OS image is cached). We
        # `make clean' first anyway, belt-and-suspenders, so a stray object can
        # never mask a source change.
        #
        # CRITICAL for CI robustness: the build output is redirected to a FILE,
        # not streamed to the serial console. Under TCG (no KVM, GitHub runners)
        # the verbose kernel `make' -- dozens of long gcc command lines -- floods
        # the emulated serial console and desynchronises the pexpect command/reply
        # handshake, so the NEXT command's marker can be lost (this is what timed
        # the CI job out at the step after the build). Keeping the console quiet
        # on success makes the handshake crisp; on failure we cat the log so the
        # compiler diagnostic is still visible.
        rc, out = run(child,
                      "cd /root/ovmx/kmod && make clean >/dev/null 2>&1; "
                      "if make >/tmp/kmodbuild.log 2>&1; then echo KMOD_BUILD_OK; "
                      "else echo KMOD_BUILD_FAIL; cat /tmp/kmodbuild.log; fi",
                      build_timeout)
        if "KMOD_BUILD_OK" not in out:
            log("FAIL: in-guest kernel-module build failed (diagnostic above)")
            return 11
        rc, _ = run(child, "test -f /root/ovmx/kmod/vms.kmod", cmd_timeout)
        if rc != 0:
            log("FAIL: build reported success but vms.kmod is not present")
            return 12
        log("OK: vms.kmod built in-guest (the SHARED src/kernel-core/vms_eflag.c "
            "compiled with the NetBSD backend)")

        # ---- build the event-flag tool ------------------------------------
        # Same quiet-console discipline as the module build above.
        rc, out = run(child,
                      "cd /root/ovmx/probe && "
                      "if cc -O -Wall -Wextra -I. -o vmseflag "
                      "vmseflag.c kif_transport_netbsd.c >/tmp/toolbuild.log 2>&1; "
                      "then echo TOOL_BUILD_OK; "
                      "else echo TOOL_BUILD_FAIL; cat /tmp/toolbuild.log; fi",
                      build_timeout)
        if "TOOL_BUILD_OK" not in out:
            log("FAIL: in-guest vmseflag build failed (diagnostic above)")
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

        # ---- 4f. CROSS-PROCESS CV WAIT PROOF (the payoff for the shared -----
        #          exec_cv_wait/exec_cv_broadcast on NetBSD cv(9)) -----------
        # A waiter process BLOCKS in-kernel on common flag 66 (clear); a
        # DIFFERENT process then sets 66; the blocked waiter must WAKE and
        # return SS$_NORMAL. This exercises the SAME facility's wait/wake
        # handshake -- the lost-wakeup-free cv contract -- across two processes,
        # on the cluster's shared kernel cv+mutex. It is the strongest form of
        # the shared-state proof: not just visibility of a word, but a real
        # in-kernel sleep woken across a process boundary.
        rc, _ = run(child,
                    "rm -f /tmp/wait.out /tmp/wait.pid; "
                    "%s wait 66 >/tmp/wait.out 2>&1 & echo $! >/tmp/wait.pid" % EF,
                    cmd_timeout)
        # let the waiter descend into the in-kernel wait
        run(child, "sleep 3", cmd_timeout)
        rc, out = run(child, "cat /tmp/wait.out 2>/dev/null; echo ---END---",
                      cmd_timeout)
        if "WAITDONE" in out:
            log("FAIL: the waiter returned BEFORE any process set flag 66 -- "
                "the wait has no teeth (it did not actually block)")
            return 22
        log("OK: the waiter is BLOCKED in-kernel on common flag 66 (no WAITDONE "
            "yet) -- exec_cv_wait is really sleeping")

        # a DIFFERENT process sets flag 66 -> must wake the blocked waiter
        rc, out = run(child, "%s set 66" % EF, cmd_timeout)
        if rc != 0 or "EFLAG SETEF efn=66" not in out:
            log("FAIL: could not set common flag 66 to wake the waiter")
            return 23
        # bounded poll for the waiter to complete
        rc, out = run(child,
                      "i=0; while [ $i -lt 15 ]; do "
                      "grep -q 'EFLAG WAITDONE efn=66' /tmp/wait.out && break; "
                      "i=$((i+1)); sleep 1; done; "
                      "cat /tmp/wait.out 2>/dev/null; echo ---END---",
                      cmd_timeout)
        if "EFLAG WAITDONE efn=66" not in out:
            log("FAIL: the blocked waiter did NOT wake after a DIFFERENT process "
                "set common flag 66 -- the cross-process cv wake was LOST")
            return 24
        log("OK: the blocked waiter WOKE when a DIFFERENT process set common flag "
            "66 -- the shared exec_cv_wait/broadcast holds cross-process on "
            "NetBSD cv(9) (THE PAYOFF)")

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
