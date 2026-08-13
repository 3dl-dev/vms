#!/usr/bin/env python3
#
# drive_netbsd.py - boot NetBSD/amd64 under qemu-system-x86_64, drive its serial
# console non-interactively, run `uname -srm`, and assert the result.
# (rd vms-7f7, Phase 2a of epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md §6.)
#
# WHY THIS EXISTS. Epic vms-8e8 adds an "OVMX/NetBSD" SYSKRNL so OVMX can run as
# a real runtime on architectures Linux never ported to (VAX). There is NO
# qemu-system-vax, so the whole NetBSD executive is de-risked FIRST on a known
# architecture -- NetBSD/amd64 under qemu-system-x86_64 -- before the VAX arch
# port. This harness is the foundation: it proves we can boot a real NetBSD,
# drive its console, run a command, and propagate the verdict as an exit status.
# P2b (a `vms` pseudo-device on this NetBSD) and P2c (an executive facility test
# against a real /dev/vms on NetBSD) build directly on the installed, cached
# disk image this produces. This is the NetBSD-substrate sibling of the Linux
# harness in tests/qemu/ (run_tests.sh / inject_and_run.sh).
#
# APPROACH -- anita (Automated NetBSD Installation and Testing).
# See tests/netbsd/README.md for the full rationale. In short: anita is the
# official NetBSD tool that scripts sysinst over an emulated serial console; it
# turns the official release into a real, complete, reusable NetBSD disk image.
# That installed image -- not a transient installer ramdisk -- is what P2b needs
# (a place to build/load the vms driver), so the slower install is the right
# trade, and it is paid ONCE: the image is cached and every later run just boots
# it. We drive install + boot through anita, then own the console assertion.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9 / docs/runtime-target.md). Booting
# NetBSD in QEMU to TEST it is exactly what tests/qemu/ does for Linux; it is not
# a Docker/emulator "runtime". NetBSD-as-a-real-runtime is the product goal.
#
# HONEST FAILURE. A hung boot must FAIL, not hang: pexpect's per-child timeout is
# an internal boot deadline, and run_tests.sh wraps this whole process in a hard
# `timeout`. A wrong `uname` FAILS. The negative-control modes below prove the
# assertion and the timeout can actually go red.

import os
import re
import sys
import signal
import traceback

import anita

import netbsd_console


def log(msg):
    print("[drive_netbsd] %s" % msg, flush=True)


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
    # anita mirrors the downloaded install media under
    # <workdir>/download/<arch>/installation/cdrom/<iso_name>. We verify the
    # PINNED serial-console boot ISO against the CDN's published SHA512. anita
    # itself does not check checksums (verified: no hashlib/sha in anita.py), so
    # this is the integrity gate. It runs on every invocation -- including cached
    # ones -- so a corrupted or tampered cache is caught, not trusted.
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
    # KVM where the host offers it (local dev boxes); plain TCG otherwise
    # (GitHub-hosted runners have no nested virt). Same qemu binary either way.
    if kvm_available():
        log("acceleration: KVM (/dev/kvm present and writable)")
        return ["-accel", "kvm", "-cpu", "host", "-smp", "4"]
    log("acceleration: TCG (no usable /dev/kvm) -- boot/install will be slower")
    return ["-smp", "2"]


# ---- deterministic console (rd vms-2d9) --------------------------------------
# The console negotiation, login and command execution go through the shared,
# deterministic NetBSDConsole (tests/netbsd/netbsd_console.py): unique prompt,
# resync-on-prompt, unique per-command markers -- so this smoke harness drives
# the guest console the same reliable way the P2b/P2c harnesses do, and stops
# racing a bare `# ' under TCG. Created lazily from the live child.
_con = None


def _console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def wait_for_login(child, boot_deadline):
    # Bounded, EXPLICIT child.timeout so a boot that never reaches `login:'
    # raises pexpect.TIMEOUT (-> nonzero exit) instead of hanging on anita's
    # hardcoded 3600s (the outer run_tests.sh `timeout' is the belt to this
    # suspenders). The negotiation loop lives in the shared console.
    _console(child).wait_for_login(boot_deadline)


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "amd64")
    url = env("NETBSD_URL",
              "https://cdn.netbsd.org/pub/NetBSD/NetBSD-%s/%s/" % (version, arch))
    iso_name = env("NETBSD_BOOT_ISO", "boot-com.iso")
    iso_sha512 = env("NETBSD_BOOT_ISO_SHA512", "")

    # The cache/work directory. anita puts its download mirror AND the installed
    # wd0.img here; persisting it across runs is the whole caching story.
    #
    # SHARED installed-disk cache (rd vms-2d9): all three NetBSD/amd64 harnesses
    # (this smoke test, P2b, P2c) install the SAME sets into the SAME workdir so
    # ONE cached wd0.img -- keyed on the NetBSD version only (see ci.yml) -- serves
    # every job. This smoke test does not itself build in-guest, but it installs
    # the SAME (build-capable) sets so that if IT is the job that populates the
    # shared cache, the image is usable by P2b/P2c too. Keep this workdir + the
    # `shared_sets' below IDENTICAL across the three drivers.
    workdir = env("NETBSD_WORKDIR",
                  "/cache/anita-netbsd-shared-%s-%s" % (version, arch))

    # What we assert `uname -srm` prints. Defaults key on the pinned version/arch
    # -> "NetBSD 10.1 amd64". The negative-control modes override these to prove
    # the harness can actually go red (see run_tests.sh / README).
    expect_version = env("EXPECT_VERSION", version)
    expect_arch = env("EXPECT_ARCH", arch)
    expected_line = "NetBSD %s %s" % (expect_version, expect_arch)

    # Timeouts (seconds). The install is bounded only by run_tests.sh's outer
    # hard `timeout` (a cold install legitimately takes many minutes under TCG);
    # boot and the command assertion get tight internal deadlines here.
    # Headroom for a genuine cold install/boot of the shared image (rd vms-2d9);
    # routine runs restore the shared warm cache and boot quickly.
    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "2400"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    # Negative control: FORCE_TIMEOUT shrinks the boot deadline so the boot
    # cannot complete in time -> the harness must FAIL via the timeout path.
    if env("FORCE_TIMEOUT"):
        boot_deadline = 1
        log("NEGATIVE CONTROL: FORCE_TIMEOUT set -- boot deadline forced to 1s")

    log("NetBSD %s/%s" % (version, arch))
    log("release URL:   %s" % url)
    log("work/cache dir: %s" % workdir)
    log("asserting `uname -srm` == %r" % expected_line)

    # The SHARED set list + disk size -- IDENTICAL to P2b/P2c (rd vms-2d9) so any
    # of the three jobs produces the same build-capable image for the shared
    # cache. This smoke test only boots + asserts `uname', but installing the
    # same sets is what lets one install serve all three jobs.
    shared_sets = ["kern-GENERIC", "modules", "base", "etc", "comp", "syssrc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=shared_sets),
        workdir=workdir,
        memory_size="1024M",
        disk_size="8G",
        persist=True,             # keep the installed image; this is the cache
        vmm_args=accel_args(),
    )

    child = None
    try:
        import pexpect

        # 1. Install (no-op if the cached wd0.img already exists).
        log("installing NetBSD (cache-aware; downloads + sysinst on cold cache)...")
        a.install()
        log("install step complete (image present)")

        # 2. Verify the pinned boot ISO checksum from anita's download mirror.
        if iso_sha512:
            if not verify_boot_iso(workdir, arch, iso_name, iso_sha512):
                return 3
        else:
            log("WARNING: no NETBSD_BOOT_ISO_SHA512 provided; skipping ISO verify")

        # 3. Boot the installed image. persist=False here so the boot runs
        #    against a qemu snapshot overlay -- the cached golden image is never
        #    written, so a killed/timed-out boot cannot corrupt the cache.
        a.persist = False
        log("booting installed image (snapshot overlay; deadline %ds)..." % boot_deadline)
        child = a.start_boot()
        wait_for_login(child, boot_deadline)
        a.child = child

        # 4. Log in on the console and install the unique prompt. Uses the
        #    deterministic console (not anita's a.login()) so the smoke test
        #    drives the console the same robust way as P2b/P2c.
        con = _console(child)
        con.login_root_sh(cmd_timeout)
        log("logged in; running the smoke command")

        # 5. THE ASSERTION. Run `uname -srm` through the deterministic console
        #    (unique end marker + prompt resync) and require the exact expected
        #    line in its output. No racing a bare `# '.
        _rc, out = con.run("uname -srm", cmd_timeout)
        if expected_line in out:
            log("PASS: uname -srm printed %r" % expected_line)
            rc = 0
        else:
            actual = out.strip()
            log("FAIL: uname -srm did not print %r" % expected_line)
            log("  console showed instead:\n%s" % actual)
            rc = 1

        # 6. Clean shutdown of the guest.
        try:
            a.halt()
        except Exception as e:
            log("note: halt raised (harmless): %s" % e)
        return rc

    except pexpect.TIMEOUT as e:
        log("FAIL: timed out waiting on the NetBSD console (boot/login/command)")
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
        # Belt-and-suspenders: make sure the qemu child is dead even if anita's
        # own cleanup did not run (e.g. we were killed mid-boot).
        try:
            if child is not None and child.isalive():
                child.terminate(force=True)
        except Exception:
            pass


def _on_term(signum, frame):
    # Turn a SIGTERM from the outer `timeout` into a normal unwind so `finally`
    # runs and qemu is reaped.
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _on_term)
    sys.exit(main())
