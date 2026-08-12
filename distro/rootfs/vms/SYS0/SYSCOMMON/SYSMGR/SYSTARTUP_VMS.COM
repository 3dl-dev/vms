$! SYSTARTUP_VMS.COM - Site-specific system startup command procedure
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
$! JOB_CONTROL is the worked example of the RUN/DETACHED shape above, but
$! it is no longer STARTED from this file (see below) -- it is registered
$! as a STDRV component in SYS$STARTUP:VMS$VMS.DAT instead (vms-2a9), at
$! the END phase (after this file runs, preserving the same console-safe
$! ordering this file's own hardcoded call used to produce -- see
$! VMS$VMS.DAT's header for why). A layered-product or site-specific
$! service still belongs here.
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
$! JOB_CONTROL (vms-8d2) -- the console-session process -- is NO LONGER
$! started here (vms-2a9, docs/design-boot-faithful.md sec3.8/sec4.5). It
$! is now a registered STDRV component (SYS$STARTUP:VMS$VMS.DAT, END
$! phase -- the LAST of the nine), created by STARTUP.COM's own
$! RUN_COMPONENTS subroutine after this file, and everything else STDRV
$! runs, has already finished. That preserves this file's own former
$! ordering exactly (JOB_CONTROL used to be the last real action of this
$! procedure) rather than moving JOB_CONTROL's creation earlier: an
$! earlier phase was tried first and measurably interleaved JOB_CONTROL's
$! LOGINOUT "Username:" prompt with this file's own INSTALL ADD console
$! output (see VMS$VMS.DAT's header for the full account). Because END
$! still runs after the Priority 1 INSTALL ADD table above, JOB_CONTROL's
$! own dynamic activation still resolves its dependencies at Priority 1,
$! unchanged from before this item.
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
