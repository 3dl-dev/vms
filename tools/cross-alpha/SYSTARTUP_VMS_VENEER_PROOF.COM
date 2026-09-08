$! SYSTARTUP_VMS.COM - CRTL->RMS VENEER un-fakeable proof variant
$! (vms-f49, rung 4 of the vms-b4f ladder; harness-only).
$!
$! Harness variant of the Alpha static-bootstrap SYS$MANAGER:SYSTARTUP_VMS.COM,
$! staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the VENEER proof artifacts are present (LIBVMSRMS$SHR.EXE alongside
$! the joint image); the product rootfs-alpha file is never modified.
$!
$! WHAT THIS PROVES (the anti-fabrication payoff, INV-6). SYS$SYSTEM:JOINT_E2E is
$! the alpha-dec-vms GCC-port image built with the CRTL->RMS stdio veneer wired
$! in (JOINT_CRTL_RMS_VENEER=1): its decc$fopen/fwrite/fclose bind to the
$! crtl_rms_stdio.c veneer (ovmx_crtl_* -> sys$create/$put/$close ->
$! LIBVMSRMS$SHR -> ioctl(/dev/vms) -> the Files-11 ACP), NOT to musl-POSIX. It
$! writes PORTTEST.DAT through that path on the live OVMX/Alpha executive.
$!
$! Then -- and THIS is the un-fakeable part -- an INDEPENDENT reader inspects the
$! volume through a DIFFERENT accessor than the writing process's CRTL/RMS
$! handle: DCL DIRECTORY/FULL, which runs its OWN sys$parse+sys$search over the
$! Files-11 ACP directory (dcl_cmd_file.c cmd_directory -> from_acp) and prints
$! the GENUINE ODS-2 File ID (num,seq,rvn) the directory search returned. A
$! POSIX/ramfs write cannot appear in the ACP directory at all: it draws
$! %DIRECT-W-NOFILES with NO File ID line. So "PORTTEST.DAT;1 ... File ID: (n,..)"
$! here is proof the veneer's fopen genuinely landed the file on the real ODS-2
$! volume -- something a same-CRTL fwrite->fread round-trip (which a ramfs
$! satisfies identically) can never establish.
$!
$! The fully-qualified spec VDA0:[SYSTMP]PORTTEST.DAT (device + directory) is
$! deliberate, matching the host-arch proof's VDA0:[OVMXDIR]VENEER.DAT: the port
$! image RUN has no default device/directory in this STDRV process context, so a
$! bare name cannot be resolved by RMS/Files-11 (vms-f49). The writer (veneer
$! sys$create) and this independent DIRECTORY reader use the identical qualified
$! spec, so they agree on the target. If the executive/RMS write path is wrong,
$! the write reaches nothing and DIRECTORY reports %DIRECT-W-NOFILES -- the gate
$! reds. That is the whole point: the un-fakeable ODS-2 landing is what is proven.
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "VENEER-PROOF: === MILESTONE: RUN JOINT_E2E (veneer crtl_rms writes PORTTEST.DAT via sys$create) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "VENEER-PROOF: WRITE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL VDA0:[SYSTMP]PORTTEST.DAT (a DIFFERENT accessor over the ACP) ==="
$ DIRECTORY/FULL VDA0:[SYSTMP]PORTTEST.DAT
$ WRITE SYS$OUTPUT "VENEER-PROOF: DIR-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "VENEER-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
