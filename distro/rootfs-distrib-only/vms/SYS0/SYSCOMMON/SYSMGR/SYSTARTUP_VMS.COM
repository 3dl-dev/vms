$! SYSTARTUP_VMS.COM - Site-specific system startup command procedure
$! DISTRIBUTION-DISK VARIANT (vms-dcf, docs/design-vms-faithful-install.md
$! sec3.3 "the distribution disk differs from an installed system disk
$! ONLY IN ITS PAYLOAD"): identical to the installed-target's
$! distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM except for the
$! one block marked below that runs the install menu. distro/Dockerfile.
$! bootable stages THIS file onto ovmx-distrib.img only, AFTER the kit-stage
$! step has already captured the plain (non-install-invoking) version above
$! into ovmx-os.kit -- so a PRODUCT INSTALL'd target inherits the plain one,
$! never this one (sec3.3's "two variants staged at mastering").
$!
$! Invoked by SYS$MANAGER:STARTUP.COM at the end of system startup, after
$! the system-wide logical names are defined. This is where a system
$! manager starts the services this node should run. Edits on the system
$! disk persist across reboots.
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
$! claimed by the process afterwards, so SHOW SYSTEM lists the service
$! and $GETJPI resolves it by name from any process on the node. A
$! duplicate name in the same UIC group is refused to the caller with
$!
$!     %RUN-F-CREPRC, process creation failed
$!     -SYSTEM-F-DUPLNAM, duplicate name
$!
$! Do NOT start a service from STARTUP.EXE (PID 1). It has no way to name
$! the process, no way for a manager to disable it, and its missing-image
$! check was silent -- see the NOTE ON SERVICES in src/ovmx_init/ovmx_init.c.
$!
$ SET NOON
$!
$! REGISTER THE SHAREABLE IMAGES every dynamically-activated OVMX binary
$! depends on with INSTALL (bead vms-913.7, docs/design-image-activation.md
$! section 6). This is Priority 1 of IMGACT.EXE's SONAME search order
$! (src/imgact/imgact.c load_needed()): a hit here is an O(1) mmap'd hash
$! lookup with no filesystem search at all; a system that never runs this
$! still boots, because Priority 2 (the hardcoded SYS$SHARE fallback) always
$! finds the same files by scanning the directory instead. INSTALL ADD is
$! idempotent (re-adding an installed image updates its entry in place), so
$! running this on every boot -- not just the first -- is the VMS way and
$! matches the real SYSTARTUP_VMS.COM's own STARTUP.COM pattern.
$!
$! This list is exactly the shareable set the fat initramfs actually ships
$! in SYS$SHARE (distro/Dockerfile.bootable's "9 VMS-native LINK.EXE
$! artifacts" gate: these 7 shareables + DCL.EXE + LOGINOUT.EXE in SYSEXE).
$! LIBVMSQUEUE$SHR.EXE (src/vmsqueue) is deliberately NOT here: it is built
$! by the ordinary CMake add_library() graph, not the VMS-native LINK.EXE
$! graph, and is not copied into the fat initramfs -- an INSTALL ADD for it
$! would fail with %INSTALL-E-FILNOTFND on every boot.
$!
$ INSTALL ADD SYS$SHARE:DECC$SHR.EXE          /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMSSYS$SHR.EXE     /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMS$SHR.EXE        /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMSPROCESS$SHR.EXE /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMSLNM$SHR.EXE     /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMSFS$SHR.EXE      /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBVMSRMS$SHR.EXE     /OPEN /SHARED /HEADER_RESIDENT
$!
$! ---------------------------------------------------------------------
$! THE ONE BLOCK THAT DIFFERS FROM THE INSTALLED TARGET'S SYSTARTUP_VMS.COM:
$! run the install menu here, before JOB_CONTROL/login -- matching the
$! oracle (docs/oracle/installation-media-vax73-alpha84.md sec3a), where
$! the Alpha media's menu appears right after boot, no login required.
$! When the operator picks option 8 or the menu procedure otherwise
$! returns control (EXIT), execution falls through to JOB_CONTROL below,
$! so the distribution disk still offers a normal login too.
$!
$ @SYS$MANAGER:OVMX$INSTALL.COM
$! ---------------------------------------------------------------------
$!
$! START JOB_CONTROL (vms-8d2) -- the console-session process, and the first
$! real worked example of the mechanism documented above. Started AFTER the
$! shareable-image registration so its own dynamic activation finds every
$! dependency at Priority 1 rather than falling through to the Priority 2
$! directory scan. See SYS$STARTUP:JOB_CONTROL_STARTUP.COM for why it is
$! started with explicit /INPUT /OUTPUT /ERROR pointed at the console.
$!
$ @SYS$STARTUP:JOB_CONTROL_STARTUP.COM
$!
$! ANNOUNCE THAT THIS PROCEDURE RAN, so that "the boot reached site
$! startup" is an ASSERTION and not an inference from the absence of an
$! error. tests/qemu/test_persistent_boot.sh greps the boot console for
$! the line below; without it the boot test could only observe that
$! STARTUP.COM did not complain, which is the absence-of-evidence shape
$! this project keeps getting burned by.
$!
$! NOT ORACLE-PINNED. An earlier version of this comment claimed the text
$! below matched a reference-lab capture of a real OpenVMS boot. No such
$! capture exists: `grep -rl "STDRV\|site-specific" ~/vax/cluster/captures/`
$! (re-run at round 6, 2026-07-31) returns nothing, so that claim was
$! unverifiable and was removed rather than repeated. Whether this wording
$! matches, should match, or should differ from what a real OpenVMS boot
$! prints is UNVERIFIED IN EITHER DIRECTION -- neither claimed nor denied
$! here (CLAUDE.md Rule 10: an authenticity claim about VMS message text is
$! either oracle-pinned with the capture recorded, or it does not appear).
$! Pinning it to the oracle or to public OpenVMS documentation is open
$! work, not done here.
$!
$ WRITE SYS$OUTPUT "The OVMX system is now executing the site-specific startup commands."
$!
$ EXIT
