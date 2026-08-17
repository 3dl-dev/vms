$! SYSTARTUP_VMS.COM - Site-specific system startup (Alpha static-bootstrap variant)
$!
$! STATIC-BOOTSTRAP VARIANT (vms-3f6, epic vms-8954, OVMX-on-Alpha). This is
$! the SYS$MANAGER:SYSTARTUP_VMS.COM that ships on the Alpha cross-built
$! bootstrap system volume (tools/cross-alpha/build-alpha-bootimage.sh). It is
$! a DELIBERATE variant of the Linux one
$! (distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM), NOT a copy --
$! the difference is the INSTALL ADD block, explained below. It follows the
$! same reasoning as the netbsd-vax variant (distro/rootfs-vax/..., Decision A
$! / vms-42d / vms-d9c).
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
$! NO INSTALL ADD SYS$SHARE:*$SHR.EXE BLOCK ON THE ALPHA STATIC BOOTSTRAP.
$!
$! The Linux SYSTARTUP_VMS.COM registers seven OVMX shareable images with
$! INSTALL here (DECC$SHR.EXE, LIBVMSSYS$SHR.EXE, LIBVMS$SHR.EXE,
$! LIBVMSPROCESS$SHR.EXE, LIBVMSLNM$SHR.EXE, LIBVMSFS$SHR.EXE,
$! LIBVMSRMS$SHR.EXE) so IMGACT.EXE's SONAME search finds them at Priority 1.
$!
$! On the Alpha cross-built bootstrap NONE of that applies. The Alpha boot +
$! login chain images (STARTUP.EXE, PROVISION.EXE, JOB_CONTROL.EXE,
$! LOGINOUT.EXE, DCL.EXE and the utilities) are STATIC EM_ALPHA executables
$! -- the Alpha cross-build produces no VMS-native LINK.EXE symbol-vector
$! graph (build-alpha-bootimage.sh: "OVMX_LINK_NATIVE off ... alpha-linux-gnu
$! is not aarch64/x86_64-musl"), so there are NO OVMX *$SHR.EXE shareables on
$! the volume at all. Every INSTALL ADD above would therefore fail
$! %INSTALL-E-FILNOTFND on every boot (SET NOON keeps that non-fatal, but it
$! is seven lines of misleading error noise for a registration that can never
$! succeed and that nothing on this substrate needs).
$!
$! Unlike netbsd-vax, INSTALL.EXE itself IS present in the Alpha boot set, so
$! the VERB works here -- only its targets are absent. The whole INSTALL ADD
$! block is omitted anyway: a statically linked image resolves every symbol at
$! link time and activates with no IMGACT SONAME search, so there is nothing
$! for a Priority 1 known-file entry to accelerate. This is NOT a shortcut
$! hiding a missing feature; it is the honest configuration for a static
$! bootstrap. When the Alpha port grows a VMS-native LINK.EXE graph and ships
$! real shareables, this variant is retired in favour of the Linux block.
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
$ EXIT
