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
$! The bare filespec PORTTEST.DAT is deliberate: the port image RUN executes in
$! THIS STDRV process context, so its write and the DIRECTORY below resolve
$! against the identical process default -- whatever that default is, writer and
$! independent reader agree on it. If the LLP64 width fix (vms-1fc) were wrong,
$! the veneer's ioctl(/dev/vms) pointer would truncate, the write would reach
$! nothing, and DIRECTORY would report %DIRECT-W-NOFILES -- the gate reds. That
$! is the whole point: Part A (width) is VALIDATED by Part B (this reader).
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "VENEER-PROOF: === MILESTONE: RUN JOINT_E2E (veneer crtl_rms writes PORTTEST.DAT via sys$create) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "VENEER-PROOF: WRITE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ==="
$ DIRECTORY/FULL PORTTEST.DAT
$ WRITE SYS$OUTPUT "VENEER-PROOF: DIR-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "VENEER-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
