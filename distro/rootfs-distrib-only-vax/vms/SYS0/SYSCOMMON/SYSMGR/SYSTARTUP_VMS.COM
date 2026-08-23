$! SYSTARTUP_VMS.COM - Site-specific system startup (netbsd-vax DISTRIBUTION variant)
$!
$! DISTRIBUTION-DISK VARIANT (vms-4834 rung F, docs/design-vax-installer.md
$! sec5.6 / table row "SYSTARTUP_VMS.COM (distribution disk)"). This is the
$! netbsd-vax mirror of the x86_64 distribution-only file
$! (distro/rootfs-distrib-only/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM,
$! vms-dcf). It is BYTE-FOR-BYTE the vax Decision-A installed-target variant
$! (distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM) EXCEPT for
$! the one block near the end that runs the install menu -- exactly the
$! "the distribution disk differs from an installed system disk ONLY IN ITS
$! PAYLOAD" contract the x86_64 distribution file states
$! (design-vms-faithful-install.md sec3.3). tests/lab-vax/stage_sysvol.sh's
$! --distribution mode stages THIS file (instead of the plain Decision-A one)
$! onto ovmx-distrib-vax.img; the plain Decision-A file stays on an INSTALL'd
$! target so a product-installed system reaches Username: with no menu.
$!
$! DECISION-A VARIANT (vms-d9c, epic vms-8e8; Decision A = vms-42d). This is
$! the SYS$MANAGER:SYSTARTUP_VMS.COM that ships on an OVMX/NetBSD-vax system
$! volume. It is a DELIBERATE variant of the Linux one
$! (distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM), NOT a copy --
$! the differences are the INSTALL ADD block (dropped, explained below) and
$! the install-menu invocation (added, near the end).
$!
$! Invoked by SYS$MANAGER:STARTUP.COM at the LPMAIN phase, after the
$! system-wide logical names are defined. This is where a system manager
$! starts the services this node should run. Edits on the system disk
$! persist across reboots.
$!
$! HOW TO START ONE. A service is a DETACHED process with a VMS process
$! name, created by its own startup procedure:
$!
$!     $ @SYS$STARTUP:<service>_STARTUP.COM
$!
$! and that procedure runs:
$!
$!     $ RUN /DETACHED -
$!           /PROCESS_NAME=<name> -
$!           /INPUT=NL: /OUTPUT=SYS$MANAGER:<name>.LOG -
$!           SYS$SYSTEM:<image>.EXE
$!
$! The name is assigned by the EXECUTIVE as the process is created, not
$! claimed by the process afterwards, so SHOW SYSTEM lists the service and
$! $GETJPI resolves it by name from any process on the node.
$!
$ SET NOON
$!
$! ----------------------------------------------------------------------
$! NO INSTALL ADD SYS$SHARE:*$SHR.EXE BLOCK ON NETBSD-VAX (Decision A).
$!
$! The Linux SYSTARTUP_VMS.COM registers seven OVMX shareable images with
$! INSTALL here (DECC$SHR.EXE, LIBVMSSYS$SHR.EXE, LIBVMS$SHR.EXE,
$! LIBVMSPROCESS$SHR.EXE, LIBVMSLNM$SHR.EXE, LIBVMSFS$SHR.EXE,
$! LIBVMSRMS$SHR.EXE) so IMGACT.EXE's SONAME search finds them at Priority 1.
$!
$! On netbsd-vax NONE of that applies (docs/design-p4-netbsd-vax-boot.md
$! sec4.3, Decision A / vms-42d):
$!
$!   1. There are NO OVMX shareables. The vax boot images (DCL.EXE,
$!      PROVISION.EXE, LOGINOUT.EXE, JOB_CONTROL.EXE) are ORDINARY
$!      ELF32-vax dynamic executables STATICALLY linked against the OVMX
$!      RTL and activated by NetBSD's own /usr/libexec/ld.elf_so -- the
$!      *$SHR.EXE files this block names DO NOT EXIST on the volume, so
$!      every INSTALL ADD would fail %INSTALL-E-FILNOTFND on every boot.
$!
$!   2. There is no INSTALL.EXE / IMGACT.EXE in the netbsd-vax boot
$!      cross-build set (build-boot-images-vax.sh) -- INSTALL execs
$!      INSTALL.EXE, which is not on the volume, so the verb itself is
$!      unavailable, not just its targets.
$!
$! So the whole INSTALL ADD block is OMITTED here, on purpose. This is NOT
$! a shortcut hiding a missing feature: dynamic-linking resolution on this
$! substrate is ld.elf_so's job (the images' NEEDED set is ordinary NetBSD
$! libc/libpthread/libm/libatomic, resolved from the NetBSD root), and
$! there is no OVMX shareable for INSTALL to accelerate. Blindly copying
$! the Linux block would red the boot on the first INSTALL ADD.
$! ----------------------------------------------------------------------
$!
$! JOB_CONTROL (vms-8d2) -- the console-session process -- IS started from
$! this file on the DISTRIBUTION boot (vms-422, Wall 6b), by the direct
$! @SYS$STARTUP:JOB_CONTROL_STARTUP.COM call near the end -- CONVERGED with
$! the x86_64 distribution file (distro/rootfs-distrib-only/.../
$! SYSTARTUP_VMS.COM line 96), which starts it the same way. The intent is
$! that this file must NOT defer to the "one layer out" STDRV END-phase
$! component: JOB_CONTROL_STARTUP.COM's RUN/DETACHED is a $CREPRC that is
$! meant to stamp the child's UIC/username/privileges through the executive
$! (sys$creprc -> vms_pcb_set_identity, src/libvms/syssvc/sys_process.c), so a
$! JOB_CONTROL $CREPRC'd DIRECTLY from THIS procedure would inherit
$! STARTUP.COM's genuine SYSTEM identity ([1,4], stamped by PROVISION.EXE at
$! boot and reported as "%OVMX-I-EXEC, system identity SYSTEM [1,4]
$! established by the executive"); LOGINOUT would then continue that identity
$! (vms_kif_register_continue, ovmx_job_control.c) and read the World-denied
$! SYSUAF.DAT.
$!
$! MEASURED STATUS ON netbsd-vax (vms-422, real SIMH, tests/lab-vax/
$! run-boot.sh install): this convergence is NECESSARY but NOT YET SUFFICIENT.
$! The direct call DOES fire and DOES create JOB_CONTROL ("%RUN-S-PROC_ID ...
$! 0000006A" right after the menu returns), yet the interactive SYSTEM login
$! STILL fails "User authorization failure" -- even though STARTUP.COM holds a
$! genuine SYSTEM [1,4] (the %OVMX-I-EXEC line above). So the residual gap is
$! NOT this .COM's control flow: it is the EXECUTIVE identity model on this
$! substrate not propagating STARTUP.COM's SYSTEM [1,4] through the
$! $CREPRC(JOB_CONTROL) -> register_continue(LOGINOUT) -> ACP SYSUAF-read
$! chain. That half is tracked separately (executive identity model); do NOT
$! "fix" it by weakening the SYSUAF World-deny or faking a login success.
$!
$! SYS$STARTUP:VMS$VMS.DAT still carries the END-phase JOB_CONTROL
$! registration (the arch-independent rootfs tree is staged verbatim, exactly
$! as the x86_64 install-media image keeps it); that END attempt now finds
$! JOB_CONTROL already running and is refused %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM,
$! swallowed by SET NOON -- benign, and identical to the x86_64 distribution
$! boot.
$!
$! THE SITE-STARTUP ANNOUNCEMENT is printed by STARTUP.COM's RUN_SITE_STARTUP
$! subroutine (its single owner, vms-3ab8), immediately before it invokes
$! this file -- so it is deliberately NOT written here.
$!
$! ---------------------------------------------------------------------
$! THE ONE BLOCK THAT DIFFERS FROM THE INSTALLED-TARGET (Decision-A)
$! SYSTARTUP_VMS.COM: run the install menu here, before this file EXITs.
$! This mirrors the x86_64 distribution-only file's single added block
$! (distro/rootfs-distrib-only/.../SYSTARTUP_VMS.COM) and matches the oracle
$! (docs/oracle/installation-media-vax73-alpha84.md sec3a), where the media's
$! menu appears right after boot, no login required.
$!
$! THE MENU BLOCK SITS JUST BEFORE THE JOB_CONTROL CALL, EXACTLY AS ON x86_64
$! (vms-422 convergence). When the operator finishes (the menu procedure
$! EXITs, e.g. option 8 "Execute DCL commands") execution falls straight
$! through to @SYS$STARTUP:JOB_CONTROL_STARTUP.COM below, which $CREPRCs
$! JOB_CONTROL from THIS procedure so it inherits STARTUP.COM's SYSTEM
$! identity -- the login the "one layer out" STDRV END-phase path could not
$! reach on netbsd-vax (see this file's JOB_CONTROL note above). The
$! distribution disk offers a normal console login after the menu.
$! ---------------------------------------------------------------------
$!
$ @SYS$MANAGER:OVMX$INSTALL.COM
$! ---------------------------------------------------------------------
$!
$! START JOB_CONTROL (vms-8d2, vms-422) -- the console-session process --
$! $CREPRC'd DIRECTLY from this procedure (the intent: inherit STARTUP.COM's
$! genuine SYSTEM identity [1,4]/SYSPRV so LOGINOUT can continue it to read the
$! World-denied SYSUAF.DAT). CONVERGED with the x86_64 distribution file's
$! line 96 (distro/rootfs-distrib-only/.../SYSTARTUP_VMS.COM). See the
$! JOB_CONTROL note above for the MEASURED netbsd-vax status: the call fires
$! and creates JOB_CONTROL, but the SYSUAF read at interactive login is still
$! denied pending the executive identity-model fix -- this .COM is the correct
$! rung, not the whole fix. No INSTALL ADD precedes it here (Decision A:
$! netbsd-vax has no OVMX shareables and no INSTALL.EXE -- see the NO INSTALL
$! ADD block above); JOB_CONTROL.EXE's own ELF32-vax dependencies resolve
$! through ld.elf_so, not INSTALL.
$!
$ @SYS$STARTUP:JOB_CONTROL_STARTUP.COM
$!
$ EXIT
