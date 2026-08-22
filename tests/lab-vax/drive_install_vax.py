#!/usr/bin/env python3
#
# drive_install_vax.py - vms-d0e5 rung G: the two-disk SIMH INSTALL proof for
# the OVMX/NetBSD-vax runtime. Boots the OVMX DISTRIBUTION volume (DKA0:) with
# a BLANK ODS-2 target attached (DKA100:), drives OVMX$INSTALL.COM's PRESERVE
# path over the console to install OVMX onto the blank target, and asserts the
# install landed a bootable, rooted, genuinely-activatable system tree on that
# target.
#
# THE VAX MIRROR OF THE PROVEN x86_64 INSTALL E2E -- same model, different
# substrate. It reproduces, over SIMH/NetBSD-vax, exactly what these three
# tests/qemu/*.sh scripts prove on the Linux/QEMU runtime:
#   * test_install_menu.sh          -- the DISTRIBUTION disk boots straight into
#                                       the menu (no login), and the PRESERVE
#                                       branch (INITIALIZE-or-PRESERVE, target,
#                                       label, confirmation gate) runs MOUNT +
#                                       PRODUCT INSTALL + the SYSTEM-password /
#                                       SCSNODE steps for real underneath.
#   * test_install_boot_e2e.sh      -- the kit landed at the ROOTED path
#                                       DKA100:[SYS0.SYSCOMMON.SYSEXE] and the
#                                       OLD FLAT DKA100:[SYSEXE] is ABSENT
#                                       (rooted-not-flat -> a bootable layout).
#   * test_product_install_e2e.sh   -- THE ANTI-LARP CRUX: an installed image
#                                       RUN *from the target* really activates
#                                       and prints real output (not a stub, not
#                                       a crash).
#
# FORKED FROM drive_boot_vax.py's do_sysboot() SKELETON (start SIMH, drive the
# KA655 ROM `B/R5:2 DUA0', watch the console). It reuses that driver's console
# helpers VERBATIM -- imported, not re-derived (INV-DRIFT: one source of truth):
# expect_wake(), _console_text(), _hard_kill(), log(), env(), and the canonical
# HARNESS_ERROR / PROOF_FAILED exit contract. It deliberately does NOT use
# netbsd_console.NetBSDConsole.run(): the DCL install menu has no `# ' shell
# prompt and no `echo $?' -- it is an operator-facing INQUIRE/READ dialogue, so
# every step here is a raw child.expect(prompt) + child.send(resp + "\r"), the
# same way the x86_64 install e2e drives its QEMU serial with send()/wait_for().
#
# THE TWO DISKS (mirrors do_sysboot's single-disk vmm_args, doubled):
#   rq1 -> ra1 -> DKA0:    the DISTRIBUTION ODS-2 volume (ovmx-distrib-vax.img):
#                          the mastered system tree PLUS the OS kit at
#                          SYS$UPDATE:OVMX-OS-VAX.KIT and the distribution
#                          SYSTARTUP_VMS.COM that @'s OVMX$INSTALL.COM at boot.
#   rq2 -> ra2 -> DKA100:  the BLANK, freshly-formatted ODS-2 target the install
#                          writes onto (src/kernel-netbsd/vms_blockdev_netbsd.c's
#                          unit map: DKA100:/DUA100: -> ra2c).
# BOTH attached WITHOUT `-r' -- the install's real block writes to DKA100: (and
# PROVISION's ownership writes to DKA0:) must LAND in the host image files, so
# run-boot.sh's sha256 before/after diff on the target has teeth (INV-6).
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under
# SIMH to run the real ovmx_init + real vms.ko/vmsfs.ko against a real /dev/vms,
# driving the real OVMX$INSTALL.COM + real PRODUCT.EXE/AUTHORIZE.EXE/SYSGEN.EXE,
# is exactly what tests/qemu/ does for the Linux path. SIMH is emulation;
# nothing runs on the host.
#
# The whole run is bounded by run-boot.sh's hard `timeout'; every in-guest
# expect here carries its own console deadline; the finally-clause _hard_kill()s
# SIMH so nothing hangs.

import os
import re
import sys
import signal
import traceback

import anita

# Reuse drive_boot_vax's console helpers VERBATIM (imported, not copied). Its
# module-level body sets sys.path for netbsd_console and imports anita + the
# vaxharness exit contract; importing it here is side-effect-free beyond that
# (its main() is guarded by __name__ == "__main__").
import drive_boot_vax as db
from drive_boot_vax import expect_wake, _console_text, _hard_kill, log, env
from vaxharness import HARNESS_ERROR, PROOF_FAILED


# --- OVMX$INSTALL.COM's real console strings (read from the procedure) --------
# These are the exact prompt/output strings OVMX$INSTALL.COM emits
# (distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/OVMX$INSTALL.COM, reused BYTE-FOR-BYTE
# -- this driver does not edit it). Substrings are used so incidental
# punctuation ("? (Yes/No)", ": [PRESERVE]") does not have to be reproduced.
MENU_PROMPT      = "OVMX$INSTALL Option"          # INQUIRE OVMX_CHOICE
INIT_OR_PRESERVE = "INITIALIZE or to PRESERVE"    # INQUIRE OVMX_MODE
ASK_TARGET       = "Enter device name for target disk"
ASK_LABEL        = "Enter volume label for target system disk"
ASK_OK           = "Is this OK"                   # confirmation gate
MOUNTED          = "%MOUNT-I-MOUNTED"
PCSI_DONE        = "%PCSI-I-DONE"
ASK_PW1          = "Password for SYSTEM account"
ASK_PW2          = "Reenter password for verification"
UAF_SAVED        = "%UAF-I-SAVED"
ASK_SCSNODE      = "Enter SCSNODE"
ASK_SCSID        = "Enter SCSSYSTEMID"
SYSGEN_PROMPT    = "SYSGEN>"      # see SYSGEN_PROMPT_RE -- NOT matched literally
DISMOUNTED       = "%DISMOUNT-I-DISMOUNTED"
INSTALL_DONE     = "installation is complete"

# SYSGEN's OWN prompt, and nothing else (vms-d0e5, INV-6 teeth). A literal
# "SYSGEN>" search is a FALSE PASS: OVMX$INSTALL.COM itself prints the operator
# instruction "At the SYSGEN> prompt, type:" BEFORE it RUNs SYSGEN.EXE, so the
# literal matches the procedure's own echo whether or not SYSGEN ever started.
# That is exactly what happened on the 2026-08-22 run -- the driver reported
# "SYSGEN.EXE resolves and runs" while SYSGEN had in fact exited immediately and
# the SCS commands landed on the install menu (%OVMX-E-IVCHOICE, "USE CURRENT").
# The real prompt is emitted at the START of a line; the instructional text has
# it mid-sentence. Anchor there so only a genuine REPL prompt can satisfy it.
SYSGEN_PROMPT_RE = r"(?:\r?\n|^)SYSGEN>"

USERNAME   = "Username:"
PASSWORD   = "Password:"
WELCOME    = "Welcome to OpenVMX"

# The install-set values driven into the menu.
TARGET_DEV   = "DKA100:"
TARGET_LABEL = "WORK"
SYS_PASSWORD = "OVMXTEST1"
SCS_NODE     = "OVMXVAX"          # <= 6 chars (SCSNODE max)
SCS_ID       = "1025"

# The distribution disk's own SYSTEM password is the arch-neutral kit default
# (SYSUAF.DAT reused from distro/rootfs, same as x86_64 test_install_boot_e2e's
# login()). The install-set password above lands on the TARGET's SYSUAF, not the
# distribution disk we log into for the inspection session.
DISTRIB_SYS_PW = "MANAGER"

# The DCL interactive prompt. The echoed foreign-command "$DKA100:..." image
# spec contains "$D" (dollar-D), never "$ " (dollar-space), so a "\n$ " prompt
# match never false-fires off a command echo.
DCL_PROMPT = r"\r?\n\$ "

CMD_TO = 120     # per interactive-command console deadline


def _lit(s):
    """A pexpect pattern matching the literal string s ($ etc. escaped)."""
    return re.escape(s)


def _send(child, s):
    child.send(s + "\r")


def do_install(a, distrib_img, blank_img, boot_deadline):
    """Boot the DISTRIBUTION volume (rq1 -> DKA0:) with the BLANK target
    (rq2 -> DKA100:), drive OVMX$INSTALL.COM's PRESERVE path to install OVMX
    onto the target, then -- from an interactive DCL session with the target
    re-MOUNTed -- assert the install landed a rooted, activatable system tree.
    Returns 0 on all-pass, PROOF_FAILED otherwise."""
    import pexpect

    distrib_abs = os.path.abspath(distrib_img)
    blank_abs = os.path.abspath(blank_img)
    # Both disks writable (NO -r): rq1 = distribution (boots into the menu),
    # rq2 = blank install target. Same shape as do_sysboot's single-disk
    # vmm_args, with the second MSCP disk added.
    vmm_args = ["set rq1 ra92", "attach rq1 " + distrib_abs,
                "set rq2 ra92", "attach rq2 " + blank_abs]
    log("INSTALL: distribution volume on rq1 -> ra1 -> DKA0:, blank target on "
        "rq2 -> ra2 -> DKA100: (deadline %ds)" % boot_deadline)

    npass = [0]
    nfail = [0]

    def ok(msg):
        npass[0] += 1
        log("  PASS: %s" % msg)

    def bad(msg):
        nfail[0] += 1
        log("  FAIL: %s" % msg)

    a.dist.set_workdir(a.workdir)
    a.n_cdrom = 0
    child = a.start_simh(vmm_args)
    try:
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        # =============================================================
        # PHASE 1 -- the DISTRIBUTION disk boots STRAIGHT into the menu
        # (no login): distro/rootfs-distrib-only-vax SYSTARTUP_VMS.COM @'s
        # OVMX$INSTALL.COM. The menu text is unsolicited console output, so a
        # plain expect (NOT expect_wake -- a stray CR would be swallowed as an
        # empty INQUIRE answer) scans through the whole boot to the prompt.
        # =============================================================
        try:
            child.expect([_lit(MENU_PROMPT), USERNAME], timeout=boot_deadline)
        except (pexpect.TIMEOUT, pexpect.EOF) as e:
            bad("distribution disk never reached the install menu (%s)"
                % type(e).__name__)
            log("boot tail:\n%s" % _console_text(child)[-2000:])
            return PROOF_FAILED
        pre = _console_text(child)
        if USERNAME in pre and MENU_PROMPT not in pre:
            bad("a login prompt appeared before the install menu (should be "
                "menu-first, oracle sec3a)")
            return PROOF_FAILED
        ok("the DISTRIBUTION disk boots straight into the install menu -- no "
           "login required (oracle sec3a)")

        # =============================================================
        # PHASE 2 -- drive OVMX$INSTALL.COM's PRESERVE path (the oracle's own
        # bracketed default), EXACTLY as an operator would.
        # =============================================================
        _send(child, "1")
        child.expect(_lit(INIT_OR_PRESERVE), timeout=CMD_TO)
        ok("option 1 asks INITIALIZE-or-PRESERVE")

        _send(child, "PRESERVE")
        child.expect(_lit(ASK_TARGET), timeout=CMD_TO)
        ok("PRESERVE accepted; asks for the target device")

        _send(child, TARGET_DEV)
        child.expect(_lit(ASK_LABEL), timeout=CMD_TO)
        ok("target device accepted; asks for the volume label")

        _send(child, TARGET_LABEL)
        child.expect(_lit(ASK_OK), timeout=CMD_TO)
        gate = _console_text(child)
        if TARGET_DEV in gate:
            ok("the confirmation gate names the target device (%s)" % TARGET_DEV)
        else:
            bad("the confirmation gate does not name the target device")

        _send(child, "YES")
        # MOUNT the target for real, then PRODUCT INSTALL the real kit onto it.
        child.expect(_lit(MOUNTED), timeout=CMD_TO)
        ok("MOUNT (real mount over the executive) succeeds from inside the menu")
        try:
            child.expect(_lit(PCSI_DONE), timeout=boot_deadline)
        except (pexpect.TIMEOUT, pexpect.EOF) as e:
            bad("PRODUCT INSTALL did not reach %%PCSI-I-DONE (%s)"
                % type(e).__name__)
            log("install tail:\n%s" % _console_text(child)[-2000:])
            return PROOF_FAILED
        install_seg = _console_text(child)      # MOUNT..PCSI-DONE = the install
        if re.search(r"%PCSI-[EF]-|%MOUNT-[EF]-", install_seg):
            bad("install transcript carries a PCSI/MOUNT error despite DONE")
            log(install_seg[-1500:])
        else:
            ok("PRODUCT INSTALL reports %PCSI-I-DONE with no %PCSI-[EF]-/"
               "%MOUNT-[EF]- error in the transcript")

        # SYSTEM password (never echoed): entered, driven at AUTHORIZE against
        # the target's rooted SYSUAF via OVMX$INSTALL.COM's inline SYS$INPUT.
        child.expect(_lit(ASK_PW1), timeout=CMD_TO)
        _send(child, SYS_PASSWORD)
        child.expect(_lit(ASK_PW2), timeout=CMD_TO)
        _send(child, SYS_PASSWORD)
        # AUTHORIZE persists it (%UAF-I-SAVED). Race it against the NEXT prompt
        # so a misbehaving AUTHORIZE cannot hang the harness (SET NOON in the
        # procedure means a failed write just falls through to Enter SCSNODE).
        idx = child.expect([_lit(UAF_SAVED), _lit(ASK_SCSNODE)], timeout=boot_deadline)
        pw_seg = _console_text(child)
        if SYS_PASSWORD in pw_seg:
            bad("the SYSTEM password was ECHOED to the console -- SET "
                "TERMINAL/NOECHO did not suppress it")
        else:
            ok("the SYSTEM password was never echoed to the console")
        if idx == 0:
            ok("AUTHORIZE persisted the install-set SYSTEM password to the "
               "TARGET SYSUAF (%UAF-I-SAVED)")
            child.expect(_lit(ASK_SCSNODE), timeout=CMD_TO)
        else:
            bad("AUTHORIZE did not report %UAF-I-SAVED before the SCSNODE "
                "prompt -- the install-set password did not persist to the "
                "target SYSUAF")

        # SCSNODE / SCSSYSTEMID -> the target's own OVMXVMSSYS.PAR via SYSGEN.
        _send(child, SCS_NODE)
        child.expect(_lit(ASK_SCSID), timeout=CMD_TO)
        _send(child, SCS_ID)
        # SYSGEN.EXE is in the vax OS kit, so PRODUCT INSTALL laid it on the
        # target's rooted SYSEXE and RUN resolves it through the DEFINE/JOB
        # SYS$SYSTEM redirect -> a SYSGEN> prompt. Drive its REPL if it appears;
        # if the RUN fell through (SET NOON), the flow reaches DISMOUNT directly.
        idx = child.expect([SYSGEN_PROMPT_RE, _lit(DISMOUNTED)],
                           timeout=boot_deadline)
        if idx == 0:
            ok("SYSGEN.EXE resolves and runs against the rooted target (no "
               "%DCL-E-IVIMAGE)")
            for line in ("USE CURRENT", 'SET SCSNODE "%s"' % SCS_NODE,
                         "SET SCSSYSTEMID %s" % SCS_ID, "WRITE CURRENT", "EXIT"):
                _send(child, line)
            child.expect(_lit(DISMOUNTED), timeout=boot_deadline)
        else:
            bad("SYSGEN.EXE did not prompt (SYSGEN> never appeared) -- the SCS "
                "configuration step could not run against the target")
        ok("the procedure dismounts the target itself (%DISMOUNT-I-DISMOUNTED)")

        child.expect(_lit(INSTALL_DONE), timeout=CMD_TO)
        ok("the install reports 'installation is complete' and returns to the menu")

        # =============================================================
        # PHASE 3 -- the menu GOTOs back to its prompt. Option 8 (Execute DCL)
        # EXITs OVMX$INSTALL.COM; on netbsd-vax that returns to SYSTARTUP_VMS.COM,
        # which EXITs, and STARTUP.COM's END phase then starts JOB_CONTROL ->
        # LOGINOUT -> Username: (see the distribution SYSTARTUP header's
        # "WHERE THE FALL-THROUGH-TO-LOGIN HAPPENS DIFFERS BY SUBSTRATE" note).
        # So option 8 -> the login prompt; log into the DISTRIBUTION system and
        # inspect the just-installed TARGET from a real interactive DCL session.
        # =============================================================
        child.expect(_lit(MENU_PROMPT), timeout=boot_deadline)
        _send(child, "8")
        # LOGINOUT's OPA0: getchar RETURN-wait (vms-2213) -- HERE the CR wake
        # feed is correct (a stray CR before the prompt is discarded type-ahead),
        # unlike the INQUIRE menu above.
        try:
            expect_wake(child, [USERNAME], total_timeout=boot_deadline)
        except (pexpect.TIMEOUT, pexpect.EOF) as e:
            bad("option 8 did not fall through to a login prompt (%s)"
                % type(e).__name__)
            log("tail:\n%s" % _console_text(child)[-1500:])
            return PROOF_FAILED
        _send(child, "SYSTEM")
        child.expect(_lit(PASSWORD), timeout=CMD_TO)
        _send(child, DISTRIB_SYS_PW)
        try:
            child.expect(_lit(WELCOME), timeout=boot_deadline)
        except (pexpect.TIMEOUT, pexpect.EOF) as e:
            bad("SYSTEM could not log into the distribution system (%s)"
                % type(e).__name__)
            return PROOF_FAILED
        child.expect(DCL_PROMPT, timeout=CMD_TO)
        ok("reached an interactive DCL session on the distribution system")

        # Re-MOUNT the just-installed target for inspection.
        _send(child, "MOUNT %s %s" % (TARGET_DEV, TARGET_LABEL))
        try:
            child.expect(_lit(MOUNTED), timeout=CMD_TO)
            ok("re-MOUNT %s for inspection succeeds" % TARGET_DEV)
        except (pexpect.TIMEOUT, pexpect.EOF):
            bad("could not re-MOUNT %s for inspection" % TARGET_DEV)
            return _verdict(npass, nfail)

        # ROOTED: DCL.EXE landed at DKA100:[SYS0.SYSCOMMON.SYSEXE] (bootable).
        # "Total of [1-9]" only prints after a real successful listing; match the
        # versioned "DCL.EXE;" so the echoed command text cannot false-pass.
        _send(child, "DIRECTORY %s[SYS0.SYSCOMMON.SYSEXE]DCL.EXE" % TARGET_DEV)
        try:
            child.expect(r"Total of [1-9]", timeout=CMD_TO)
            seg = _console_text(child)
            if "DCL.EXE;" in seg:
                ok("install landed DCL.EXE at rooted "
                   "%s[SYS0.SYSCOMMON.SYSEXE]" % TARGET_DEV)
            else:
                bad("rooted %s[SYS0.SYSCOMMON.SYSEXE]DCL.EXE: 'Total of N' but "
                    "no versioned DCL.EXE; in the listing" % TARGET_DEV)
        except (pexpect.TIMEOUT, pexpect.EOF):
            bad("rooted %s[SYS0.SYSCOMMON.SYSEXE]DCL.EXE not listed "
                "(no 'Total of N')" % TARGET_DEV)

        # NEGATIVE (rooted-not-flat teeth): the OLD flat path must be ABSENT. A
        # missing rooted directory reports %RMS-E-DNF/FNF; a present file would
        # print "Total of N".
        _send(child, "DIRECTORY %s[SYSEXE]DCL.EXE" % TARGET_DEV)
        try:
            idx = child.expect(
                [r"%RMS-E-DNF", r"%RMS-E-FNF", r"%DIRECT-W-NOFILES",
                 r"no such file", r"not found", r"Total of [1-9]"],
                timeout=CMD_TO)
            if idx <= 4:
                ok("old flat %s[SYSEXE]DCL.EXE is correctly ABSENT "
                   "(rooted, not flat)" % TARGET_DEV)
            else:
                bad("flat %s[SYSEXE]DCL.EXE resolves -- install wrote the flat "
                    "(non-bootable) layout" % TARGET_DEV)
        except (pexpect.TIMEOUT, pexpect.EOF):
            bad("flat %s[SYSEXE]DCL.EXE check inconclusive (no listing, no RMS "
                "error)" % TARGET_DEV)

        # ANTI-LARP CRUX: execute the INSTALLED DCL.EXE *from the target*. A
        # foreign command forwards trailing words as argv (dcl_exec_foreign_
        # command), and DCL.EXE's argv path runs "-c <cmd>" non-interactively
        # (dcl_main.c) and EXITs -- so it produces real output instead of
        # blocking an interactive REPL on stdin. A stub / non-activatable image
        # would instead crash (Segmentation/ACCVIO) or fail to activate
        # (IVIMAGE / "cannot execute").
        _send(child, 'RUNX :== $%s[SYS0.SYSCOMMON.SYSEXE]DCL.EXE' % TARGET_DEV)
        try:
            child.expect(DCL_PROMPT, timeout=CMD_TO)
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass
        _send(child, 'RUNX -c "SHOW TIME"')
        try:
            child.expect(DCL_PROMPT, timeout=CMD_TO)
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass
        run_seg = _console_text(child)
        if re.search(r"Segmentation|cannot execute|%DCL-.*IVIMAGE|%SYSTEM-.*ACCVIO|ACCVIO",
                     run_seg):
            bad("the installed DCL.EXE crashed or failed to activate from the "
                "target: %s" % run_seg[-400:].replace("\n", " "))
        else:
            # Real output = anything beyond the echoed 'RUNX' command lines.
            body = "".join(l for l in run_seg.splitlines() if "RUNX" not in l)
            if body.strip():
                extra = " (SHOW TIME returned the year %s)" % _cur_year() \
                    if _cur_year() in run_seg else ""
                ok("the installed DCL.EXE activates FROM THE TARGET and prints "
                   "real output%s (anti-LARP)" % extra)
            else:
                bad("the installed DCL.EXE produced no output at all from the "
                    "target (anti-LARP inconclusive)")

        _send(child, "DISMOUNT %s" % TARGET_DEV)
        try:
            child.expect(_lit(DISMOUNTED), timeout=CMD_TO)
        except (pexpect.TIMEOUT, pexpect.EOF):
            pass

    except (pexpect.TIMEOUT, pexpect.EOF) as e:
        log("FAIL: unexpected console timeout/EOF driving the install (%s)"
            % type(e).__name__)
        log("console tail:\n%s" % _console_text(child)[-2000:])
        nfail[0] += 1
    finally:
        _hard_kill(child)

    return _verdict(npass, nfail)


def _cur_year():
    import datetime
    return str(datetime.datetime.now().year)


def _verdict(npass, nfail):
    log("RESULT: %d passed, %d failed" % (npass[0], nfail[0]))
    if nfail[0] == 0 and npass[0] > 0:
        log("======================================================================")
        log("  INSTALL-VAX PASSED: the OVMX/NetBSD-vax DISTRIBUTION volume booted")
        log("  straight into OVMX$INSTALL.COM, the PRESERVE path installed OVMX")
        log("  onto the BLANK target over the executive (MOUNT + PRODUCT INSTALL +")
        log("  AUTHORIZE + SYSGEN), and the installed target carries a ROOTED,")
        log("  genuinely-ACTIVATABLE system tree (DCL.EXE runs from the target;")
        log("  the flat [SYSEXE] layout is absent) -- the x86_64 install e2e,")
        log("  mirrored on the vax runtime.")
        log("======================================================================")
        return 0
    return PROOF_FAILED


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "vax")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    # The assembled boot-work disk (ovmx_init as PID 1), same as the sysboot
    # modes -- run-boot.sh assembles it (ensure_boot_install) before this runs.
    workdir = env("NETBSD_WORKDIR", "/cache/boot-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    distrib_img = env("OVMX_DISTRIB_IMG", "/cache/ovmx-distrib-vax.img")
    blank_img = env("OVMX_BLANK_IMG", "/cache/dka100-target.img")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))

    log("NetBSD %s/%s  (OVMX/NetBSD two-disk INSTALL proof; vms-d0e5 rung G)"
        % (version, arch))
    log("boot-disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("distribution:      %s" % distrib_img)
    log("blank target:      %s" % blank_img)

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s (run lab-vax install first)" % iso_path)
        return HARNESS_ERROR
    for f in (distrib_img, blank_img):
        if not os.path.isfile(f):
            log("FAIL: image not found at %s (run-boot.sh must build it first)" % f)
            return HARNESS_ERROR

    a = anita.Anita(
        dist=anita.ISO(iso_path, sets=sets),
        workdir=workdir,
        persist=True,
    )

    try:
        log("ensuring the boot-work disk is present (cache-aware install no-op)...")
        a.install()
        return do_install(a, distrib_img, blank_img, boot_deadline)
    except Exception:
        log("FAIL: unexpected error driving the install harness")
        traceback.print_exc()
        return HARNESS_ERROR
    finally:
        # drive_boot_vax keeps a module-global console child; nothing here uses
        # it, but kill it defensively if a prior import path created one.
        if getattr(db, "_con", None) is not None:
            _hard_kill(db._con.child)


def _on_term(signum, frame):
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _on_term)
    _rc = main()
    # Force an immediate exit: anita/pexpect leave a non-daemon reader thread
    # blocked on the (now-dead) SIMH child's output pipe, so a normal sys.exit
    # would hang joining it. SIMH is already terminated (do_install's finally)
    # and every write was to the attached host image files, so nothing is lost.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(_rc if isinstance(_rc, int) else 0)
