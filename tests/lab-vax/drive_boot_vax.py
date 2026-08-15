#!/usr/bin/env python3
#
# drive_boot_vax.py - vms-7b1 boot-disk assembly + unattended boot proof
# (parent vms-c99, epic vms-8e8): assemble a BOOTABLE OVMX/NetBSD-vax system
# disk from the OVMX build and boot it UNATTENDED under SIMH with ovmx_init
# (STARTUP.EXE) as PID 1 (init), reaching the boot milestone:
#
#     kernel up
#       -> vms.kmod  loaded, /dev/vms live     (executive attached)
#       -> OpenVMX product banner emitted       (SYSBOOT hand-over)
#       -> vmsfs.kmod loaded, OVMX ODS-2 system disk (DKA0:) mounted
#
# This is the VAX capstone-minus-one: it proves ovmx_init RUNS AS INIT on real
# NetBSD/vax under SIMH and drives its own NetBSD boot seam (ovmx_boot_netbsd.c).
# Reaching a full DCL prompt is the SEPARATE capstone (vms-d59); this driver
# stops at the banner+mount milestone and lets ovmx_init halt honestly at its
# installed-system gate (SYS$SYSTEM:DCL.EXE absent), which is exactly the
# fail-honest behaviour design-init-scope.md §1 requires and the concrete
# report of what userspace is still missing for boot-to-DCL.
#
# It builds ON the proven vax runtime substrate (tests/lab-vax): the custom
# GENERIC+MODULAR /netbsd kernel already on the cached disk (vax GENERIC omits
# `options MODULAR'), the loadable elf32-vax vms.kmod + vmsfs.kmod, and the
# mastered OVMX ODS-2 volume. Its two peers PROVE the pieces in isolation:
# drive_devvms_vax.py (/dev/vms PING) and drive_vmsfs_vax.py (ODS-2 mount+read);
# THIS one wires them together UNDER ovmx_init as PID 1.
#
# TOOLING, NOT A RUNTIME (CLAUDE.md Rule 9): booting a real NetBSD/vax under
# SIMH to run the real ovmx_init + real kernel modules against a real /dev/vms
# is exactly what tests/qemu/ does for the Linux path. SIMH is emulation;
# nothing runs on the host.
#
# MODES (OVMX_MODE):
#   install-boot  ONE-TIME assembly onto an ISOLATED copy of the cached disk
#                 (never the shared wd0.img the devvms/vmsfs proofs boot): boot
#                 single-user with the ORIGINAL NetBSD init (a shell), install
#                 STARTUP.EXE as /sbin/init, place vms.kmod + vmsfs.kmod in the
#                 kernel module_path, capture the executive's char major and
#                 pre-create /dev/vms, MAKEDEV the ra1 system-disk node, create
#                 the boot mount points, then halt.
#   prove         Boot that assembled disk single-user with the mastered ODS-2
#                 volume on rq1 (-> ra1 -> DKA0:). The kernel execs /sbin/init =
#                 ovmx_init as PID 1; assert the milestone lines appear on the
#                 console. UNATTENDED: no shell is driven -- ovmx_init IS init.
#   negctl        Same as prove but WITHOUT the ODS-2 volume attached: the mount
#                 cannot happen, so the "%OVMX-I-MOUNTED" milestone line MUST NOT
#                 appear -- proving the mount assertion has teeth (Rule 7).
#
# The whole run is bounded by run-boot.sh's hard `timeout`, and every in-guest
# command here has its own console deadline, so nothing hangs.

import os
import sys
import signal
import subprocess
import traceback

import anita

sys.path.insert(0, os.environ.get("OVMX_NETBSD_DIR", "/netbsd"))
import netbsd_console


# The milestone lines ovmx_init emits, in the order the flagless NetBSD boot
# path produces them (src/ovmx_init/ovmx_init.c bare_metal_init / main).
MS_SYSKRNL = "OVMX/NetBSD"                                   # SYSKRNL identity
MS_EXEC    = "VMS executive attached on /dev/vms"           # /dev/vms open
MS_BANNER  = "OpenVMS-compatible"                           # product banner
MS_MOUNTED = "system disk DKA0: mounted"                    # ODS-2 mounted

# vms-d9c: the milestones PAST the mount, on a real installed OVMX SYSTEM volume
# (the sysboot mode). require_installed_system() halts %OVMX-F-SYSINIT with this
# detail when SYS$SYSTEM:DCL.EXE is absent (the flat/test volume); on a MASTERED
# system volume it passes and the boot reaches print_stdrv_begun() (MS_STDRV),
# emitted IMMEDIATELY before run_startup() execs SYS$SYSTEM:PROVISION.EXE.
MS_INSTALLED_GATE_FAIL = "is not an installed OVMX system volume"
MS_STDRV               = "%STDRV-I-STARTUP"
# The honest post-STDRV outcomes: PROVISION.EXE either activates + reaches a
# login prompt, or its activation FAILS (the exec-from-vmsfs gap -- the vax
# vmsfs mount's VOP_GETPAGES is genfs_eopnotsupp, so demand-paging PROVISION.EXE
# off the mounted ODS-2 volume cannot succeed yet), surfacing as %OVMX-E-NOIMG
# (child) and/or %OVMX-F-EXECINIT (PID 1's halt after PROVISION returns nonzero).
MS_PROVISION_LOGIN   = "Username:"
MS_PROVISION_NOIMG   = "%OVMX-E-NOIMG"
MS_PROVISION_HALT    = "%OVMX-F-EXECINIT"
# vms-72da: the logical-name-layer boundary. Before the NetBSD executive gained
# the logical-name facility (vms.kmod's LNM$SYSTEM arena + DEFINE/DELETE/GETSCOPE
# ioctls + the read-only arena mmap), PROVISION.EXE ran but could not CREATE the
# system logicals SYS$STARTUP/SYS$LOGIN/SYS$UPDATE, so STARTUP looped on
# %DCL-E-LNMFAIL -> %RMS-E-FNF (VMS$PHASES.DAT) -> %DCL-E-IVLOGNAM (~22,263x).
# Seeing LNMFAIL/IVLOGNAM now is a REGRESSION of that facility; NOT seeing it (the
# boot reaches Username: or the next honest gap) is the vms-72da milestone.
MS_PROVISION_LNMFAIL = "%DCL-E-LNMFAIL"
MS_PROVISION_IVLOGNAM = "%DCL-E-IVLOGNAM"
# vms-63a (#536, on main): PROVISION.EXE DEMAND-PAGES off the mounted ODS-2
# volume and RUNS -- the executive then establishes its SYSTEM identity. This
# line is the deterministic proof PROVISION.EXE activated + ran (the boundary the
# vmsfs vnode pager closed). Its ABSENCE is the stale-volume / pager regression
# (PROVISION.EXE "missing" -> %OVMX-F-EXECINIT). No brackets -> literal regex.
MS_PROVISION_RUNNING = r"system identity SYSTEM"
# rd vms-e7a: positive forward-progress evidence PAST provision_ownership() --
# STARTUP.COM's own announcement (distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/
# STARTUP.COM) that the STDRV phase is running SYSTARTUP_VMS.COM. Reaching
# this line means DCL is actively EXECUTING commands, not merely that no
# error was seen -- corroboration the coordinator asked for alongside the
# OWNER-flood check (that check alone cannot rule out provision_ownership()
# being skipped entirely).
MS_STARTUP_EXECUTING = "The OVMX system is now executing the site-specific startup commands"
# rd vms-e7a: the RW-ODS2 OWNER-path regression signal. provision_ownership()
# runs IMMEDIATELY after MS_PROVISION_RUNNING (ovmx_provision.c calls it last,
# right before exec'ing STARTUP), so it is chronologically the FIRST thing to
# appear in the STEP-2 window below -- before Username:/LNMFAIL/IVLOGNAM/etc.
# Before this fix it emitted ~28 of these (one per file in the system tree,
# lchown(2) failing with genfs_eopnotsupp on the read-only ODS-2 mount); after
# it, this string must NEVER appear. Its absence is verified against the
# window's FULL captured text (child.before), not a race against the other
# STEP-2 patterns -- see do_sysboot()'s STEP 2 below.
MS_PROVISION_OWNER_WARN = "%OVMX-W-OWNER"
# LIMITATION (do not remove this check, but do not trust it ALONE): absence of
# this string proves PROVISION reported no ERROR -- it does NOT by itself rule
# out a write VOP that silently no-ops (the exact INV-6 failure class), since
# lchown(2) would report success either way. The POSITIVE proof -- that the
# volume's raw on-disk bytes actually changed during this boot -- is a
# sha256sum of SYSVOL_IMG taken before/after this SIMH session, done on the
# HOST in run-boot.sh's `sysboot' case (rq1 is attached WITHOUT `-r', so real
# guest writes land in that host file). This console check and that hash diff
# are deliberately two independent signals: this one localizes a regression to
# the OWNER call sites specifically; the hash diff is what actually rules out
# a fake/no-op write.


def log(msg):
    print("[drive_boot_vax] %s" % msg, flush=True)


def env(name, default=None):
    v = os.environ.get(name)
    return v if (v is not None and v != "") else default


_con = None


def _hard_kill(child):
    """SIGKILL a SIMH pexpect child by pid -- a single syscall that cannot hang.
    pexpect's terminate() empirically does NOT reliably kill SIMH here (it stays
    alive while a non-daemon reader thread blocks the interpreter), so we do not
    depend on it; the caller then os._exit()s, which drops the reader thread."""
    try:
        if child is not None and child.pid is not None:
            os.kill(child.pid, signal.SIGKILL)
    except Exception:
        pass


def console(child):
    global _con
    if _con is None or _con.child is not child:
        _con = netbsd_console.NetBSDConsole(child, logfn=log)
    return _con


def run(child, cmd, timeout, echo=True):
    return console(child).run(cmd, timeout, echo)


def _console_text(child):
    """Best-effort read of child.before as a plain str -- NEVER raises, NEVER
    returns anything but a str. A timed-out or errored expect() can leave
    `.before` as None (or, per anita's own broken-sentinel handling noted in
    the STEP-1/STEP-2 comments below, something else unexpected); a bare
    `child.before or ""` looked safe by inspection but still crashed the
    harness in CI (rd vms-e7a: post_identity_text.count() on None) -- so
    every reader of console text after an expect() that CAN fail goes through
    this helper instead of touching child.before directly."""
    try:
        raw = child.before
    except Exception:
        return ""
    if raw is None:
        return ""
    if isinstance(raw, bytes):
        return raw.decode("utf-8", "replace")
    if isinstance(raw, str):
        return raw
    return str(raw)


def build_source_iso(artifacts_dir, out_iso, required):
    """Bundle the boot deliverables (the WHOLE artifacts dir) into an ISO9660
    image attached as a second CD (rq2): STARTUP.EXE (-> /sbin/init), the
    loadable vms.kmod + vmsfs.kmod (-> the kernel module_path), and the custom
    MODULAR kernel netbsd-OVMX (-> /netbsd, for install-kernel). `required' is
    the subset this mode must have present."""
    for f in required:
        p = os.path.join(artifacts_dir, f)
        if not os.path.isfile(p):
            raise RuntimeError("missing boot artifact: %s" % p)
    if os.path.exists(out_iso):
        os.remove(out_iso)
    cmd = ["genisoimage", "-quiet", "-J", "-r", "-V", "OVMXBOOT",
           "-o", out_iso, artifacts_dir]
    log("building boot-artifact ISO: %s" % " ".join(cmd))
    subprocess.check_call(cmd)
    log("boot-artifact ISO built: %s (%d bytes)" % (out_iso, os.path.getsize(out_iso)))


def do_install_kernel(a, artifacts_dir, src_iso, boot_deadline, cmd_timeout):
    """Swap the custom GENERIC+MODULAR kernel (netbsd-OVMX) in as /netbsd on the
    (shared) NetBSD/vax disk -- vax GENERIC omits `options MODULAR', so no module
    can load until this runs. Idempotent via run-boot.sh's marker; boots the
    current kernel single-user and copies the MODULAR kernel over /netbsd,
    keeping the original as /netbsd.GENERIC. Same operation the devvms/vmsfs
    drivers perform on this shared disk."""
    build_source_iso(artifacts_dir, src_iso, ("netbsd-OVMX",))
    src_abs = os.path.abspath(src_iso)
    vmm_args = ["set rq2 cdrom", "attach -r rq2 " + src_abs]
    log("booting single-user to swap in the MODULAR kernel (deadline %ds)..."
        % boot_deadline)
    child, con = start_single_user(a, vmm_args, boot_deadline, cmd_timeout)
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
                  "cp /mnt/netbsd-OVMX /netbsd.new && mv /netbsd.new /netbsd && "
                  "sync && umount /mnt && ls -l /netbsd /netbsd.GENERIC",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not install the MODULAR kernel onto /netbsd")
        return 30
    run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
    log("OK: installed MODULAR kernel as /netbsd; next boot has modules(9)")
    return 0


def start_single_user(a, vmm_args, boot_deadline, cmd_timeout):
    """Reuse anita's start_simh() to spawn SIMH with the disk + our extra
    drives, then drive the KA655 ROM to boot SINGLE-USER (R5 = RB_SINGLE) and
    bring up a writable root shell with a unique prompt. Returns (child, con)."""
    import pexpect  # noqa: F401  (import proves availability early)
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
    child.sendline("PATH=/sbin:/bin:/usr/sbin:/usr/bin; export PATH")
    child.expect(r"# ")
    child.sendline("fsck -y / 2>&1 | tail -1")
    child.expect(r"# ", timeout=cmd_timeout)
    child.sendline("mount -u -w /")
    child.expect(r"# ")
    child.sendline("stty -echo 2>/dev/null; true")
    child.expect(r"# ")
    con = console(child)
    con.set_unique_prompt()
    log("single-user root shell ready on NetBSD/vax (writable /)")
    return child, con


def do_install_boot(a, artifacts_dir, src_iso, boot_deadline, cmd_timeout):
    """Assemble the bootable disk: install STARTUP.EXE as /sbin/init, place the
    modules in the module_path, capture the executive major + pre-create
    /dev/vms, MAKEDEV ra1, create the boot mount points. Idempotent."""
    build_source_iso(artifacts_dir, src_iso,
                     ("STARTUP.EXE", "vms.kmod", "vmsfs.kmod"))
    src_abs = os.path.abspath(src_iso)
    vmm_args = ["set rq2 cdrom", "attach -r rq2 " + src_abs]
    log("booting the isolated boot-disk copy SINGLE-USER with the artifact CD "
        "on rq2 (deadline %ds)..." % boot_deadline)
    child, con = start_single_user(a, vmm_args, boot_deadline, cmd_timeout)

    rc, out = run(child, "uname -srm; sysctl -n kern.securelevel", cmd_timeout)
    log("guest: %s" % " | ".join(out.split()))

    # 1. Stage the deliverables off the OVMX CD (NetBSD/vax names MSCP CD-ROMs
    #    racd0/racd1). Decide on rc (the real $?), never on echoed output.
    rc, out = run(child,
                  "mkdir -p /root/ovmx; ok=; "
                  "for dev in /dev/racd0[a-z] /dev/racd1[a-z] "
                  "/dev/cd0[a-z] /dev/cd1[a-z]; do "
                  "  test -e $dev || continue; "
                  "  if mount_cd9660 $dev /mnt 2>/dev/null; then "
                  "    if test -f /mnt/STARTUP.EXE; then ok=$dev; "
                  "      cp /mnt/STARTUP.EXE /mnt/vms.kmod /mnt/vmsfs.kmod /root/ovmx/ && "
                  "      umount /mnt && break; "
                  "    else umount /mnt 2>/dev/null; fi; "
                  "  fi; done; "
                  "echo \"OVMX boot CD = $ok\"; ls -l /root/ovmx; "
                  "test -f /root/ovmx/STARTUP.EXE && test -f /root/ovmx/vms.kmod "
                  "&& test -f /root/ovmx/vmsfs.kmod",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not stage the boot artifacts from the OVMX CD")
        return 10
    log("OK: staged STARTUP.EXE + vms.kmod + vmsfs.kmod")

    # 2. Place the modules in the kernel module_path so ovmx_init's bare-name
    #    modctl(MODCTL_LOAD, "vms"/"vmsfs") resolves them the standard way -- the
    #    authentic way an installed NetBSD module is found.
    rc, out = run(child,
                  "MP=`sysctl -n kern.module.path | cut -d: -f1`; "
                  "echo \"module_path=$MP\"; "
                  "mkdir -p \"$MP/vms\" \"$MP/vmsfs\" && "
                  "cp /root/ovmx/vms.kmod \"$MP/vms/vms.kmod\" && "
                  "cp /root/ovmx/vmsfs.kmod \"$MP/vmsfs/vmsfs.kmod\" && "
                  "ls -l \"$MP/vms/vms.kmod\" \"$MP/vmsfs/vmsfs.kmod\"",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not place vms.kmod/vmsfs.kmod in the module_path")
        return 11
    log("OK: modules placed in the kernel module_path")

    # 3. Verify BARE-NAME modload works (exactly ovmx_init's path) for BOTH
    #    modules, and capture the executive's dynamically-assigned char major so
    #    /dev/vms can be pre-created (NetBSD has no devfs; ovmx_init recreates it
    #    at boot too, but a read-only root at boot needs the node already there).
    rc, out = run(child, "modload vms && echo LOADED_VMS", cmd_timeout)
    if rc != 0:
        log("FAIL: bare-name `modload vms' FAILED -- ovmx_init's modctl load "
            "would fail identically. Console output above.")
        return 12
    rc, out = run(child,
                  "MAJ=`dmesg | sed -n "
                  "'s/.*vms: registered, char major \\([0-9][0-9]*\\).*/\\1/p'"
                  " | tail -1`; echo \"vms_major=$MAJ\"; "
                  "test -n \"$MAJ\" && rm -f /dev/vms && "
                  "mknod /dev/vms c $MAJ 0 && chmod 666 /dev/vms && "
                  "ls -l /dev/vms && test -c /dev/vms",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not capture the vms char major / pre-create /dev/vms")
        return 13
    run(child, "modunload vms 2>/dev/null; true", cmd_timeout)

    rc, out = run(child, "modload vmsfs && echo LOADED_VMSFS", cmd_timeout)
    if rc != 0:
        log("FAIL: bare-name `modload vmsfs' FAILED -- ovmx_init's vmsfs load "
            "would fail identically.")
        return 14
    run(child, "modunload vmsfs 2>/dev/null; true", cmd_timeout)
    log("OK: both modules bare-name modload cleanly; /dev/vms pre-created")

    # 4. Create the system-disk device node (ra1 = the ODS-2 volume on rq1) and
    #    the boot mount points ovmx_init's seam expects (all pre-created so the
    #    read-only-root boot needs no writes).
    rc, out = run(child,
                  "cd /dev && sh MAKEDEV ra1 2>/dev/null; cd /; "
                  "mkdir -p /vms /proc /dev/pts /dev/shm && "
                  "ls -ld /vms /proc /dev/pts /dev/shm; ls -l /dev/ra1c && test -b /dev/ra1c",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not MAKEDEV ra1 / create the boot mount points")
        return 15
    log("OK: /dev/ra1c (DKA0:) node + boot mount points created")

    # 5. Install STARTUP.EXE as /sbin/init (keep the NetBSD init as a backup).
    #    LAST, so the running assembly shell keeps its NetBSD init for this
    #    session; the NEXT boot execs ovmx_init.
    # NetBSD's /sbin/init is mode r-xr-xr-x (no write bit), so a bare `mv' onto
    # it prompts ("override ...?") and silently does NOT replace it. rm the
    # target first (root can, /sbin is writable), then move the new image into
    # place -- no unwritable target to prompt about. Assert the installed size
    # matches STARTUP.EXE so a skipped replace is caught, not fooled.
    rc, out = run(child,
                  "test -f /sbin/init.netbsd || cp -p /sbin/init /sbin/init.netbsd; "
                  "cp /root/ovmx/STARTUP.EXE /sbin/init.tmp && chmod 755 /sbin/init.tmp && "
                  "rm -f /sbin/init && mv /sbin/init.tmp /sbin/init && "
                  "want=`wc -c < /root/ovmx/STARTUP.EXE`; got=`wc -c < /sbin/init`; "
                  "echo \"init_bytes want=$want got=$got\"; "
                  "ls -l /sbin/init /sbin/init.netbsd; ldd /sbin/init; "
                  "test \"$want\" = \"$got\"",
                  cmd_timeout)
    if rc != 0:
        log("FAIL: could not install STARTUP.EXE as /sbin/init (size mismatch "
            "means the replace was skipped)")
        return 16
    if "not found" in out:
        log("FAIL: /sbin/init (STARTUP.EXE) has an unresolved shared library "
            "on the guest -- ld.elf_so could not activate PID 1:\n%s" % out)
        return 17
    log("OK: STARTUP.EXE installed as /sbin/init; all shared libs resolve")

    # Flush to the disk image and remount root read-only (clean-unmount
    # equivalent), so an abrupt SIMH teardown cannot lose the assembly.
    run(child, "sync; mount -u -r / 2>/dev/null; sync", cmd_timeout)
    log("OK: assembly flushed to disk; the next boot runs ovmx_init as PID 1")
    return 0


def do_prove(a, ods2_img, negctl, boot_deadline):
    """Boot the assembled disk single-user (securelevel 0 -> ovmx_init's modctl
    loads are permitted) with the ODS-2 volume on rq1. The kernel execs
    /sbin/init = ovmx_init as PID 1; watch the console for the milestone lines.
    In negctl the ODS-2 volume is NOT attached, so the mount cannot happen and
    the MOUNTED milestone MUST NOT appear."""
    import pexpect

    if negctl:
        vmm_args = []
        log("NEGATIVE CONTROL: booting WITHOUT the ODS-2 volume -- the "
            "'%s' milestone MUST NOT appear" % MS_MOUNTED)
    else:
        ods2_abs = os.path.abspath(ods2_img)
        vmm_args = ["set rq1 ra92", "attach rq1 " + ods2_abs]
        log("booting the assembled disk: ovmx_init as PID 1, ODS-2 system disk "
            "on rq1 -> ra1 -> DKA0: (deadline %ds)..." % boot_deadline)

    a.dist.set_workdir(a.workdir)
    a.n_cdrom = 0
    child = a.start_simh(vmm_args)
    seen = {}
    try:
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        # ovmx_init IS init: no shell, no login. Watch the console for the
        # milestone lines in order. Each expect has the boot deadline; a
        # milestone that never arrives times out -> honest failure.
        want_order = [("syskrnl", MS_SYSKRNL), ("exec", MS_EXEC),
                      ("banner", MS_BANNER)]
        if not negctl:
            want_order.append(("mounted", MS_MOUNTED))

        try:
            for key, pat in want_order:
                child.expect(pat, timeout=boot_deadline)
                seen[key] = True
                log("MILESTONE: saw %r" % pat)
        except pexpect.TIMEOUT:
            log("timed out waiting for a boot milestone; saw=%s" % sorted(seen))
        except pexpect.EOF:
            log("SIMH exited before a boot milestone appeared; saw=%s"
                % sorted(seen))

        if negctl:
            # TEETH: with no ODS-2 volume the mount MUST fail, so the MOUNTED
            # line must NOT appear. Race MOUNTED against the honest mount-fail
            # halt -- if MOUNTED wins, the gate has no teeth (recorded in `seen'
            # and caught by main); if the halt wins, MOUNTED was correctly
            # absent.
            try:
                # No pexpect.EOF in the list (anita's expect wrapper crashes on a
                # matched EOF/TIMEOUT sentinel -- see STEP 1/2); EOF is caught below.
                idx = child.expect([MS_MOUNTED, r"%OVMX-F-SYSINIT",
                                    r"%OVMX-F-EXECINIT"], timeout=300)
                if idx == 0:
                    seen["mounted"] = True
                    log("post-milestone: MOUNTED appeared with NO ODS-2 volume "
                        "(the mount gate has NO teeth)")
                else:
                    log("post-milestone: mount correctly FAILED without an "
                        "ODS-2 volume; MOUNTED did not appear (teeth confirmed)")
            except (pexpect.TIMEOUT, pexpect.EOF, Exception):
                log("post-milestone: no MOUNTED and no halt within window (or SIMH "
                    "exited); treating MOUNTED as absent")
        else:
            # For the record + the "what's missing for DCL" report: capture the
            # honest post-milestone halt (require_installed_system: DCL absent).
            try:
                child.expect([r"%OVMX-F-SYSINIT", r"%OVMX-F-EXECINIT",
                              r"%OVMX-E-", r">>>"], timeout=180)
                log("post-milestone console: ovmx_init reached its honest "
                    "installed-system halt (expected -- DCL is vms-d59)")
            except Exception:
                pass
    finally:
        _hard_kill(child)

    return seen


def do_sysboot(a, sysvol_img, negctl, boot_deadline):
    """vms-d9c: boot the assembled vms-7b1 disk (ovmx_init as PID 1) with a real
    MASTERED OVMX SYSTEM volume on rq1 (-> ra1 -> DKA0:), and assert the boot
    proceeds PAST ovmx_init's installed-system gate and reaches the point where
    PID 1 execs SYS$SYSTEM:PROVISION.EXE.

    The pre-mount milestones (executive attach, banner, MOUNTED) are identical
    to do_prove -- the difference is what the mounted volume CARRIES. On the
    mastered system volume, require_installed_system() finds SYS$SYSTEM:DCL.EXE
    at the rooted [SYS0.SYSCOMMON.SYSEXE] path and does NOT halt; the boot then
    reaches print_stdrv_begun() (MS_STDRV), emitted immediately before
    run_startup() execs PROVISION.EXE. Reaching MS_STDRV is the deterministic
    proof that the boot got past the gate and is at the PROVISION.EXE exec.

    negctl boots the SAME disk with the FLAT test volume (mkimage_vmsfs's
    HELLO.TXT-only volume, no DCL.EXE): require_installed_system() MUST halt
    %OVMX-F-SYSINIT (DCL.EXE absent), and MS_STDRV must NOT appear -- proving it
    is specifically the mastered system volume that carries the boot past the
    gate (the gate has teeth, Rule 7).

    PAST MS_STDRV (rd vms-72da, tightened now that the vmsfs vnode pager #536 and
    the netbsd-vax logical-name facility have landed): the run now REQUIRES that
    PROVISION.EXE demand-pages + RUNS (MS_PROVISION_RUNNING -- the executive
    establishes SYSTEM identity) and that the logical-name layer lets it CLEAR the
    old %DCL-E-LNMFAIL / %DCL-E-IVLOGNAM loop -- reaching Username: (the DCL
    capstone, vms-d59) or the next honest gap (e.g. RW-ODS2, vms-e7a). A boot that
    halts at %OVMX-F-EXECINIT (PROVISION.EXE missing -- the stale-volume boundary)
    or loops on LNMFAIL is a FAILURE, not the "expected boundary" it used to be.
    Nothing is faked into a login the boot did not reach (INV-6)."""
    import pexpect

    ods2_abs = os.path.abspath(sysvol_img)
    vmm_args = ["set rq1 ra92", "attach rq1 " + ods2_abs]
    if negctl:
        log("SYSBOOT NEGATIVE CONTROL: booting with the FLAT test volume (no "
            "SYS$SYSTEM:DCL.EXE) -- the installed-system gate MUST halt and "
            "'%s' MUST NOT appear" % MS_STDRV)
    else:
        log("SYSBOOT: booting the assembled disk with the MASTERED OVMX system "
            "volume on rq1 -> ra1 -> DKA0: (deadline %ds); expecting the boot "
            "to pass the installed-system gate and reach the PROVISION.EXE exec"
            % boot_deadline)

    a.dist.set_workdir(a.workdir)
    a.n_cdrom = 0
    child = a.start_simh(vmm_args)
    seen = {}
    try:
        child.timeout = boot_deadline
        child.expect(r">>>")
        child.send("B/R5:2 DUA0\r")

        # Pre-mount milestones, same order as do_prove.
        for key, pat in [("syskrnl", MS_SYSKRNL), ("exec", MS_EXEC),
                         ("banner", MS_BANNER), ("mounted", MS_MOUNTED)]:
            try:
                child.expect(pat, timeout=boot_deadline)
                seen[key] = True
                log("MILESTONE: saw %r" % pat)
            except (pexpect.TIMEOUT, pexpect.EOF):
                log("did not reach milestone %r; saw=%s" % (pat, sorted(seen)))
                return seen

        # The decisive race: does the installed-system gate HALT (DCL.EXE
        # absent), or does the boot reach %STDRV-I-STARTUP (past the gate,
        # about to exec PROVISION.EXE)?
        try:
            # No pexpect.EOF in the list (anita crashes on a matched EOF sentinel);
            # EOF/TIMEOUT are caught below and reported as an unresolved gate.
            idx = child.expect([MS_STDRV, MS_INSTALLED_GATE_FAIL],
                               timeout=boot_deadline)
            if idx == 0:
                seen["stdrv"] = True
                log("MILESTONE: saw %r -- PAST the installed-system gate; PID 1 "
                    "is at the SYS$SYSTEM:PROVISION.EXE exec" % MS_STDRV)
            else:
                seen["installed_gate_fail"] = True
                log("saw %r -- the installed-system gate HALTED (DCL.EXE absent)"
                    % MS_INSTALLED_GATE_FAIL)
        except (pexpect.TIMEOUT, pexpect.EOF, Exception) as e:
            log("gate did not resolve (%s: SIMH exited or timed out); saw=%s"
                % (type(e).__name__, sorted(seen)))
            return seen

        # Past the gate. STEP 1 -- REQUIRE PROVISION.EXE demand-pages + RUNS: the
        # executive establishes SYSTEM identity (MS_PROVISION_RUNNING). If instead
        # PID 1 halts at %OVMX-F-EXECINIT (PROVISION.EXE missing) or %OVMX-E-NOIMG
        # (activation failed), PROVISION never ran and the logical-name layer is
        # never reached -- a FAILURE (not the old "expected exec-from-vmsfs
        # boundary", which #536's pager closed).
        if seen.get("stdrv"):
            try:
                # NOTE: pexpect.EOF/TIMEOUT are NOT in the pattern list -- anita's
                # expect() wrapper unconditionally calls self.match.group(0) after
                # every expect, which raises AttributeError when a TIMEOUT/EOF
                # SENTINEL is the match (anita.py). So we let TIMEOUT/EOF RAISE and
                # catch them below -- every list entry is a real regex whose match
                # yields a re.Match with .group().
                idx = child.expect([MS_PROVISION_RUNNING, MS_PROVISION_HALT,
                                    MS_PROVISION_NOIMG], timeout=300)
                if idx == 0:
                    seen["provision_running"] = True
                    log("MILESTONE: %r -- PROVISION.EXE demand-paged off the ODS-2 "
                        "volume and RUNS; executive established SYSTEM identity"
                        % MS_PROVISION_RUNNING)
                else:
                    why = {1: "%OVMX-F-EXECINIT (SYS$SYSTEM:PROVISION.EXE did not resolve)",
                           2: "%OVMX-E-NOIMG (activation failed)"}.get(idx, "unknown")
                    seen["provision_outcome"] = "DID NOT RUN: " + why
                    tail = _console_text(child)[-1500:]
                    # HONEST diagnosis (rd vms-72da): the volume + vnode pager are
                    # FINE -- require_installed_system() already resolved DCL.EXE via
                    # the DEVICE-TABLE path (DKA0:[SYS0.SYSCOMMON.SYSEXE], no logical),
                    # and run-boot.sh's content gate proved PROVISION.EXE + OVMXVMSSYS.PAR
                    # are ON the mastered volume. read_boot_parameters (OVMXVMSSYS.PAR)
                    # and run_startup (PROVISION.EXE) resolve the "SYS$SYSTEM:" LOGICAL;
                    # a halt here means the executive LOGICAL-NAME layer did NOT resolve
                    # SYS$SYSTEM on this substrate (the LNM$SYSTEM arena define/translate
                    # roundtrip), NOT a volume/pager problem.
                    log("PROVISION.EXE did NOT run: %s. The volume + pager are fine "
                        "(DCL.EXE resolved at the gate via the device-table path, and "
                        "PROVISION.EXE/OVMXVMSSYS.PAR are on the mastered volume) -- so "
                        "the failure is executive SYS$SYSTEM LOGICAL-NAME resolution "
                        "(LNM$SYSTEM arena roundtrip), the vms-72da target. tail:\n%s"
                        % (why, tail))
                    return seen
            except (pexpect.TIMEOUT, pexpect.EOF, Exception) as e:
                seen["provision_outcome"] = ("DID NOT RUN: %s before SYSTEM identity"
                                             % type(e).__name__)
                log("PROVISION.EXE produced no SYSTEM-identity line within the "
                    "window (%s) -- treated as DID NOT RUN" % type(e).__name__)
                return seen

        # STEP 2 -- PROVISION runs; does the logical-name layer let it CLEAR the
        # LNMFAIL/IVLOGNAM loop? Reaching Username: is the DCL capstone; a distinct
        # honest boundary AFTER identity with NO LNMFAIL is the vms-72da win ("logicals
        # create, stops next at Z"); a LNMFAIL/IVLOGNAM outcome is a REGRESSION.
        if seen.get("provision_running"):
            try:
                # Same anita caveat as STEP 1: NO pexpect.EOF/TIMEOUT in the list.
                # This is pure SCOUTING -- the vms-72da PASS is already decided by
                # provision_running (SYSTEM identity) + absence of LNMFAIL; reaching
                # Username: is the SEPARATE vms-d59 capstone, not gated here.
                idx = child.expect([MS_PROVISION_LOGIN, MS_PROVISION_LNMFAIL,
                                    MS_PROVISION_IVLOGNAM, MS_PROVISION_HALT,
                                    MS_PROVISION_NOIMG], timeout=300)
                if idx == 0:
                    seen["login"] = True
                    seen["provision_outcome"] = "LOGIN(Username:) -- DCL capstone (vms-d59)"
                    log("CAPSTONE: reached Username: -- PROVISION created the system "
                        "logicals and STARTUP ran to LOGINOUT (vms-72da clears "
                        "LNMFAIL; DCL login capstone vms-d59)")
                elif idx in (1, 2):
                    seen["lnmfail"] = True
                    seen["provision_outcome"] = "LNMFAIL/IVLOGNAM loop (vms-72da REGRESSION)"
                    log("REGRESSION: PROVISION runs but still hits %DCL-E-LNMFAIL / "
                        "%DCL-E-IVLOGNAM -- the netbsd-vax logical-name facility did "
                        "NOT create the system logicals (vms-72da FAIL)")
                else:
                    seen["next_gap"] = True
                    seen["provision_outcome"] = "PAST LNMFAIL -> next honest gap (%OVMX-F/E after identity)"
                    log("PAST LNMFAIL: PROVISION created the system logicals (no "
                        "LNMFAIL/IVLOGNAM after identity); STARTUP advanced to the "
                        "next honest gap (e.g. RW-ODS2 ownership, vms-e7a)")
                tail = _console_text(child)[-1500:]
                if tail:
                    log("post-identity console tail:\n%s" % tail)
            except (pexpect.TIMEOUT, pexpect.EOF, Exception) as e:
                # PROVISION ran (SYSTEM identity) and NO LNMFAIL/IVLOGNAM appeared
                # within the window -- the old bug emitted ~22,263 LNMFAILs here, so
                # their ABSENCE is the proof the loop is cleared. Not reaching
                # Username: within the window is the honest next gap (typically the
                # %OVMX-W-OWNER RW-ODS2 flood, vms-e7a), NOT a failure.
                seen["next_gap"] = True
                seen["provision_outcome"] = ("PAST LNMFAIL -> next gap (%s, no LNMFAIL, no login yet)"
                                             % type(e).__name__)
                tail = _console_text(child)[-1500:]
                log("PROVISION ran + SYSTEM identity established; no LNMFAIL/IVLOGNAM "
                    "within the window (%s) -- past the logical-name loop, stopped at "
                    "the next gap (Username: is the separate vms-d59 capstone). tail:\n%s"
                    % (type(e).__name__, tail))

            # rd vms-e7a: the OWNER-flood check. child.before holds everything
            # consumed by the STEP-2 expect() above, i.e. exactly the console
            # text between MS_PROVISION_RUNNING and whichever pattern matched
            # (or the timeout) -- the window provision_ownership() runs in.
            # Count, don't just detect: the OLD bug was a ~28-line FLOOD (one
            # per file/dir in the system tree), so a nonzero count is the
            # regression signal whether it is 1 or 28.
            post_identity_text = _console_text(child)
            owner_warn_count = post_identity_text.count(MS_PROVISION_OWNER_WARN)
            seen["owner_warn_count"] = owner_warn_count
            if owner_warn_count:
                log("OWNER-FLOOD: %d occurrence(s) of %s after SYSTEM identity -- "
                    "RW-ODS2 ownership stamping is FAILING (vms-e7a not fixed / "
                    "regressed): PROVISION could not lchown(2) files on the "
                    "mounted system volume" % (owner_warn_count, MS_PROVISION_OWNER_WARN))
            else:
                log("OK: no %s warnings after SYSTEM identity -- PROVISION's "
                    "provision_ownership() stamped UIC file ownership on the "
                    "RW-ODS2 volume with NO failures (vms-e7a)" % MS_PROVISION_OWNER_WARN)

            # rd vms-e7a: positive corroboration -- did STARTUP.COM actually
            # EXECUTE (not just "no LNMFAIL")? owner_warn_count==0 alone cannot
            # rule out provision_ownership() being skipped entirely (silence
            # looks identical either way); this is a second, independent
            # forward-progress signal in the SAME captured window.
            seen["startup_executing"] = MS_STARTUP_EXECUTING in post_identity_text
            if seen["startup_executing"]:
                log("OK: %r seen -- STARTUP.COM is actively EXECUTING commands "
                    "(SYSTARTUP_VMS.COM running), not just an absence of errors"
                    % MS_STARTUP_EXECUTING)
            else:
                log("NOTE: %r not seen within this window -- STARTUP may not "
                    "have reached the STDRV announcement yet (or the window "
                    "ended first at LOGIN/LNMFAIL/HALT/NOIMG)" % MS_STARTUP_EXECUTING)
    finally:
        _hard_kill(child)

    return seen


def main():
    version = env("NETBSD_VERSION", "10.1")
    arch = env("NETBSD_ARCH", "vax")
    iso_path = env("OVMX_VAX_ISO", "/cache/NetBSD-%s-vax.iso" % version)
    # The ISOLATED boot-disk copy's workdir (NEVER the shared anita-work), set by
    # run-boot.sh so the shared devvms/vmsfs disk is never mutated.
    workdir = env("NETBSD_WORKDIR", "/cache/boot-work")
    sets = env("SETS", "kern-GENERIC,base,etc").split(",")

    artifacts_dir = env("OVMX_ARTIFACTS", "/artifacts")
    src_iso = env("OVMX_SRC_ISO", "/tmp/ovmx-vax-boot.iso")
    ods2_img = env("OVMX_ODS2_IMG", "/cache/ovmx-ods2-vax.img")
    sysvol_img = env("OVMX_SYSVOL_IMG", "/cache/ovmx-sysvol-vax.img")

    boot_deadline = int(env("NETBSD_BOOT_DEADLINE", "1800"))
    cmd_timeout = int(env("NETBSD_CMD_TIMEOUT", "600"))

    mode = env("OVMX_MODE", "prove")

    log("NetBSD %s/%s  (OVMX/NetBSD bootable system disk; ovmx_init as PID 1, "
        "vms-7b1)" % (version, arch))
    log("boot-disk workdir: %s   iso: %s" % (workdir, iso_path))
    log("artifacts:         %s   ods2: %s" % (artifacts_dir, ods2_img))

    if not os.path.isfile(iso_path):
        log("FAIL: install ISO not found at %s (run lab-vax install first)" % iso_path)
        return 3

    a = anita.Anita(
        dist=anita.ISO(iso_path, sets=sets),
        workdir=workdir,
        persist=True,
    )

    child = None
    try:
        log("ensuring the target disk is present (cache-aware install no-op)...")
        a.install()

        if mode == "install-kernel":
            return do_install_kernel(a, artifacts_dir, src_iso, boot_deadline, cmd_timeout)

        if mode == "install-boot":
            return do_install_boot(a, artifacts_dir, src_iso, boot_deadline, cmd_timeout)

        if mode in ("sysboot", "sysboot-negctl"):
            sb_negctl = (mode == "sysboot-negctl")
            # negctl reuses the FLAT test volume (no DCL.EXE); the positive
            # case uses the mastered SYSTEM volume.
            vol = ods2_img if sb_negctl else sysvol_img
            if not os.path.isfile(vol):
                log("FAIL: volume image not found at %s (master it first)" % vol)
                return 4
            seen = do_sysboot(a, vol, sb_negctl, boot_deadline)

            # Pre-mount milestones are required in BOTH cases -- a run that never
            # even mounts cannot conclude anything about the installed-system
            # gate.
            if not (seen.get("syskrnl") and seen.get("exec")
                    and seen.get("banner") and seen.get("mounted")):
                log("SYSBOOT INCONCLUSIVE: did not reach the pre-gate milestones "
                    "(executive attach + banner + MOUNTED); saw=%s" % sorted(seen))
                return 21

            if sb_negctl:
                # Teeth: the flat volume MUST halt the installed-system gate and
                # MUST NOT reach %STDRV-I-STARTUP.
                if seen.get("stdrv"):
                    log("SYSBOOT NEGATIVE CONTROL DID NOT FAIL: reached "
                        "%STDRV-I-STARTUP with a volume that has no "
                        "SYS$SYSTEM:DCL.EXE -- the installed-system gate has no "
                        "teeth")
                    return 20
                if not seen.get("installed_gate_fail"):
                    log("SYSBOOT NEGATIVE CONTROL INCONCLUSIVE: the flat volume "
                        "neither halted the gate nor reached STDRV; saw=%s"
                        % sorted(seen))
                    return 22
                log("PASS: sysboot negative control -- the flat volume (no "
                    "DCL.EXE) correctly HALTED the installed-system gate and "
                    "never reached %STDRV-I-STARTUP (the gate has teeth)")
                return 0

            # Positive: the mastered system volume MUST carry the boot PAST the
            # gate to %STDRV-I-STARTUP (i.e. to the PROVISION.EXE exec).
            if seen.get("installed_gate_fail"):
                log("FAIL: the mastered system volume HALTED the installed-"
                    "system gate (DCL.EXE not found at the rooted "
                    "[SYS0.SYSCOMMON.SYSEXE] path) -- the volume did not master "
                    "or the vax vmsfs mount cannot resolve the subdirectory path")
                return 1
            if not seen.get("stdrv"):
                log("FAIL: mounted the system volume but did not reach "
                    "%STDRV-I-STARTUP; saw=%s" % sorted(seen))
                return 1
            # TIGHTENED (rd vms-72da): STDRV alone is NO LONGER a pass. PROVISION.EXE
            # must demand-page + RUN (the vmsfs pager #536 closed that boundary), so
            # a halt at %OVMX-F-EXECINIT (PROVISION.EXE missing -- the stale-volume
            # false-green) now FAILS.
            if not seen.get("provision_running"):
                log("FAIL: reached %%STDRV-I-STARTUP but PROVISION.EXE did NOT run "
                    "(no SYSTEM-identity line) -- outcome: %s. DCL.EXE resolved at the "
                    "gate (device-table path) and run-boot.sh content-gated PROVISION.EXE "
                    "onto the volume, so this is executive SYS$SYSTEM LOGICAL-NAME "
                    "resolution failing (the LNM$SYSTEM define/translate arena roundtrip "
                    "on this substrate) -- the vms-72da target, NOT a volume/pager fault."
                    % seen.get("provision_outcome", "unknown"))
                return 1
            # PROVISION runs -> the logical-name layer must let it CLEAR the LNMFAIL
            # loop (vms-72da). A LNMFAIL/IVLOGNAM outcome is a hard FAIL.
            if seen.get("lnmfail"):
                log("FAIL: PROVISION.EXE runs (SYSTEM identity established) but the "
                    "logical-name layer did NOT create the system logicals -- it "
                    "loops on %%DCL-E-LNMFAIL / %%DCL-E-IVLOGNAM (vms-72da not fixed)")
                return 1
            # PASS BAR (rd vms-e7a): the RW-ODS2 OWNER path must be clean -- ZERO
            # %OVMX-W-OWNER warnings after SYSTEM identity. Before this fix
            # PROVISION's provision_ownership() hit ~28 of these (lchown(2)
            # failing on a read-only ODS-2 mount with no write VOPs); their
            # presence now is a hard FAIL, not a tolerated warning, because
            # fixing exactly this is what vms-e7a delivers.
            if seen.get("owner_warn_count"):
                log("FAIL: %d %%OVMX-W-OWNER warning(s) after SYSTEM identity -- "
                    "PROVISION could not stamp UIC ownership on the RW-ODS2 volume "
                    "(vms-e7a regression / not fixed)" % seen["owner_warn_count"])
                return 1
            # SECOND HALF of the vms-e7a pass bar: OWNER-flood absence alone
            # cannot rule out provision_ownership() being skipped entirely
            # (silence looks identical either way -- INV-6). Require the
            # positive signal too: STARTUP.COM actually EXECUTING commands.
            if not seen.get("startup_executing"):
                log("FAIL: %r was never seen -- STARTUP.COM did not visibly "
                    "execute within the window (owner-flood absence alone is "
                    "not proof of a real write; need this positive signal too)"
                    % MS_STARTUP_EXECUTING)
                return 1
            # PASS BAR (rd vms-72da): PROVISION.EXE ran (the specific
            # "%OVMX-I-EXEC, system identity SYSTEM" marker) AND no LNMFAIL. That is
            # this proof's deliverable -- the executive LNM$SYSTEM define/translate
            # roundtrip created the system logicals. Reaching Username: is the
            # SEPARATE DCL login capstone (vms-d59), captured as scouting, NOT gated
            # here. This still has teeth: it FAILS if PROVISION does not run
            # (provision_running unset) or the LNM loop regresses (lnmfail set).
            log("======================================================================")
            log("  SYSBOOT PASSED: ovmx_init booted as PID 1 on NetBSD/vax, mounted the")
            log("  MASTERED OVMX ODS-2 system volume READ-WRITE, passed the installed-")
            log("  system gate, PROVISION.EXE DEMAND-PAGED + RAN (SYSTEM identity")
            log("  established), and stamped UIC file ownership on the RW-ODS2 volume")
            log("  with ZERO %%OVMX-W-OWNER warnings -- and STARTUP.COM was seen ACTIVELY")
            log("  EXECUTING commands, not merely an absence of errors (rd vms-e7a).")
            if seen.get("login"):
                log("  *** DCL CAPSTONE (vms-d59): the boot CLEARED the logical-name")
                log("  *** layer (vms-72da) and reached Username:. ***")
            else:
                log("  The logical-name layer is CLEARED (vms-72da): PROVISION created")
                log("  the system logicals with no %DCL-E-LNMFAIL, and STARTUP advanced")
                log("  to the next honest gap -- outcome: %s"
                    % seen.get("provision_outcome", "next gap"))
            log("======================================================================")
            return 0

        negctl = (mode == "negctl")
        seen = do_prove(a, ods2_img, negctl, boot_deadline)

        if negctl:
            # Teeth: the mount cannot happen -> MOUNTED must be ABSENT, while the
            # pre-mount milestones (executive attach + banner) still appear.
            if seen.get("mounted"):
                log("NEGATIVE CONTROL DID NOT FAIL: the MOUNTED milestone "
                    "appeared with no ODS-2 volume -- the mount assertion has "
                    "no teeth")
                return 20
            if not (seen.get("syskrnl") and seen.get("exec") and seen.get("banner")):
                log("NEGATIVE CONTROL INCONCLUSIVE: ovmx_init did not even reach "
                    "the pre-mount milestones (executive attach + banner); "
                    "cannot conclude the mount assertion is what reddened")
                return 21
            log("PASS: negative control -- ovmx_init attached the executive and "
                "emitted the banner, but the MOUNTED milestone correctly did "
                "NOT appear without an ODS-2 system disk")
            return 0

        need = ["syskrnl", "exec", "banner", "mounted"]
        missing = [k for k in need if not seen.get(k)]
        if missing:
            log("FAIL: boot did not reach every milestone; missing=%s" % missing)
            return 1
        log("======================================================================")
        log("  BOOT-VAX PASSED: ovmx_init ran as PID 1 on NetBSD/vax under SIMH,")
        log("  attached the executive on /dev/vms, emitted the OpenVMX banner,")
        log("  and mounted the OVMX ODS-2 system disk (DKA0:) -- UNATTENDED.")
        log("======================================================================")
        return 0

    except Exception:
        log("FAIL: unexpected error driving the harness")
        traceback.print_exc()
        return 2
    finally:
        _hard_kill(child)
        if _con is not None:
            _hard_kill(_con.child)


def _on_term(signum, frame):
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _on_term)
    _rc = main()
    # Force an immediate process exit. anita/pexpect leave a non-daemon reader
    # thread blocked on the (now-dead) SIMH child's output pipe, so a normal
    # `sys.exit' would hang at interpreter shutdown waiting to join it -- the
    # container would then sit until run-boot.sh's hard session timeout. SIMH is
    # already terminated (main's finally) and every disk write was flushed in
    # the guest (sync + remount-ro), so nothing is lost by exiting now.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(_rc if isinstance(_rc, int) else 0)
