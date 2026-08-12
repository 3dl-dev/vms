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


def wait_for_login(child, boot_deadline):
    # Replicates anita.Anita.boot()'s terminal-negotiation loop but with an
    # EXPLICIT, bounded child.timeout so a boot that never reaches `login:`
    # raises pexpect.TIMEOUT (-> nonzero exit) instead of hanging. anita's own
    # boot() hardcodes child.timeout=3600 in configure_child(); we want the
    # tighter, self-imposed deadline here (the outer run_tests.sh `timeout` is
    # the belt to this suspenders).
    import pexpect
    child.timeout = boot_deadline
    while True:
        r = child.expect([r"\033\[c", r"\033\[5n", r"login:"])
        if r == 0:
            child.send("\033[?1;2c")   # terminal-id query -> answer like xterm
        elif r == 1:
            child.send("\033[0n")      # terminal-status query -> "ready"
        elif r == 2:
            return                     # login prompt reached


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "amd64")
    url = env("NETBSD_URL",
              "https://cdn.netbsd.org/pub/NetBSD/NetBSD-%s/%s/" % (version, arch))
    iso_name = env("NETBSD_BOOT_ISO", "boot-com.iso")
    iso_sha512 = env("NETBSD_BOOT_ISO_SHA512", "")

    # The cache/work directory. anita puts its download mirror AND the installed
    # wd0.img here; persisting it across runs is the whole caching story.
    workdir = env("NETBSD_WORKDIR", "/cache/anita-netbsd-%s-%s" % (version, arch))

    # What we assert `uname -srm` prints. Defaults key on the pinned version/arch
    # -> "NetBSD 10.1 amd64". The negative-control modes override these to prove
    # the harness can actually go red (see run_tests.sh / README).
    expect_version = env("EXPECT_VERSION", version)
    expect_arch = env("EXPECT_ARCH", arch)
    expected_line = "NetBSD %s %s" % (expect_version, expect_arch)

    # Timeouts (seconds). The install is bounded only by run_tests.sh's outer
    # hard `timeout` (a cold install legitimately takes many minutes under TCG);
    # boot and the command assertion get tight internal deadlines here.
    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "900"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "120"))

    # Negative control: FORCE_TIMEOUT shrinks the boot deadline so the boot
    # cannot complete in time -> the harness must FAIL via the timeout path.
    if env("FORCE_TIMEOUT"):
        boot_deadline = 1
        log("NEGATIVE CONTROL: FORCE_TIMEOUT set -- boot deadline forced to 1s")

    log("NetBSD %s/%s" % (version, arch))
    log("release URL:   %s" % url)
    log("work/cache dir: %s" % workdir)
    log("asserting `uname -srm` == %r" % expected_line)

    minimal_sets = ["kern-GENERIC", "modules", "base", "etc"]

    a = anita.Anita(
        dist=anita.URL(url, sets=minimal_sets),
        workdir=workdir,
        memory_size="512M",
        disk_size="2G",
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

        # 4. Log in on the console (anita installs a passwordless root; login()
        #    sends `root` and expects the `# ` shell prompt).
        child.timeout = cmd_timeout
        a.login()
        log("logged in; running the smoke command")

        # 5. THE ASSERTION. Send `uname -srm` and require the exact expected
        #    line. If the guest prints anything else, the shell prompt returns
        #    first (index 1) and we fail, showing what it actually printed.
        expected_re = re.escape(expected_line)
        child.sendline("uname -srm")
        idx = child.expect([expected_re, r"# "], timeout=cmd_timeout)
        if idx == 0:
            log("PASS: uname -srm printed %r" % expected_line)
            child.expect(r"# ", timeout=cmd_timeout)   # drain the trailing prompt
            rc = 0
        else:
            actual = child.before
            if isinstance(actual, bytes):
                actual = actual.decode("ascii", "ignore")
            actual = actual.replace("\r", "").strip()
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
