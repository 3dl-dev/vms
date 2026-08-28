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
$! JOB_CONTROL (vms-8d2) -- the console-session process -- is NOT started
$! here. It is a registered STDRV component (SYS$STARTUP:VMS$VMS.DAT, END
$! phase), created by STARTUP.COM's own RUN_COMPONENTS after this file and
$! everything else STDRV runs has finished -- identical to the Linux path
$! (vms-2a9). See SYS$STARTUP:VMS$VMS.DAT's header for why END, not here.
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
$! WHERE THE FALL-THROUGH-TO-LOGIN HAPPENS DIFFERS BY SUBSTRATE, THE EFFECT
$! DOES NOT. On x86_64 SYSTARTUP itself starts JOB_CONTROL
$! (@SYS$STARTUP:JOB_CONTROL_STARTUP.COM) and the menu block sits just before
$! that call, so when the operator finishes (the menu procedure EXITs)
$! execution falls straight through to the login. On netbsd-vax JOB_CONTROL
$! is NOT started from this file -- it is a registered STDRV END-phase
$! component that STARTUP.COM's RUN_COMPONENTS creates AFTER this file returns
$! (see this file's own JOB_CONTROL note above). So here the menu returns,
$! this file EXITs, and STARTUP.COM then starts JOB_CONTROL one layer out --
$! the distribution disk still offers a normal console login after the menu.
$! ---------------------------------------------------------------------
$!
$ @SYS$MANAGER:OVMX$INSTALL.COM
$!
$ EXIT
