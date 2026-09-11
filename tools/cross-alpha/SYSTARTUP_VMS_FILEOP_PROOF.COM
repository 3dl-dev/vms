$! SYSTARTUP_VMS.COM - CRTL->RMS FILE-OP veneer un-fakeable proof variant
$! (vms-3320, extends the vms-f49 rung-4 veneer proof to the file-op family;
$! harness-only).
$!
$! Harness variant of the Alpha static-bootstrap SYS$MANAGER:SYSTARTUP_VMS.COM,
$! staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the FILEOP proof marker is present (a crtl_rms3_test.c build); the
$! product rootfs-alpha file is never modified.
$!
$! WHAT THIS PROVES (the anti-fabrication payoff, INV-6). SYS$SYSTEM:JOINT_E2E is
$! the alpha-dec-vms GCC-port image built with the CRTL->RMS veneer wired in
$! (JOINT_CRTL_RMS_VENEER=1): its decc$open/creat/unlink/rename/opendir/readdir/
$! closedir bind, BY SYMBOL-VECTOR INDEX, to the crtl_rms_stdio.c veneer
$! (ovmx_crtl_* -> sys$create/$open/$erase/$rename/$parse/$search ->
$! LIBVMSRMS$SHR -> ioctl(/dev/vms) -> the Files-11 ACP), NOT to musl-POSIX. It
$! creats FOPCRE.DAT, creats+unlinks FOPDEL.DAT, and creats+renames
$! FOPSRC.DAT -> FOPDST.DAT, all on the live OVMX/Alpha executive, LEAVING
$! FOPCRE.DAT + FOPDST.DAT behind for this reader.
$!
$! Then -- the un-fakeable part -- an INDEPENDENT reader inspects the volume
$! through a DIFFERENT accessor than the writer's CRTL handle: DCL DIRECTORY,
$! which runs its OWN sys$parse+sys$search over the Files-11 ACP directory
$! (dcl_cmd_file.c cmd_directory -> from_acp) and prints the GENUINE ODS-2 File
$! ID the directory search returned. A POSIX/ramfs write cannot appear in the
$! ACP directory at all (it draws %DIRECT-W-NOFILES, NO File ID). So:
$!   - FOPCRE.DAT + FOPDST.DAT present WITH File IDs  => creat + rename landed;
$!   - FOPDEL.DAT + FOPSRC.DAT %DIRECT-W-NOFILES      => unlink + rename-away worked.
$! The renamed FOPDST.DAT keeping a genuine File ID proves the ACP MODIFY!M_MOVE
$! re-link (the same-File-ID atomicity is proven exhaustively on the host-arch
$! layer, tests/qemu/test_syssvc_crtl_rms_veneer.c).
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "FILEOP-PROOF: === MILESTONE: RUN JOINT_E2E (veneer file-ops over sys$create/$erase/$rename) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "FILEOP-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "FILEOP-PROOF: === INDEPENDENT READER: DIRECTORY over the ACP (a DIFFERENT accessor) ==="
$ WRITE SYS$OUTPUT "FILEOP-PROOF: --- created file FOPCRE.DAT (must be present) ---"
$ DIRECTORY/FULL VDA0:[SYSTMP]FOPCRE.DAT
$ WRITE SYS$OUTPUT "FILEOP-PROOF: CRE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "FILEOP-PROOF: --- renamed target FOPDST.DAT (must be present, genuine File ID) ---"
$ DIRECTORY/FULL VDA0:[SYSTMP]FOPDST.DAT
$ WRITE SYS$OUTPUT "FILEOP-PROOF: DST-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "FILEOP-PROOF: --- unlinked file FOPDEL.DAT (must be GONE) ---"
$ DIRECTORY VDA0:[SYSTMP]FOPDEL.DAT
$ WRITE SYS$OUTPUT "FILEOP-PROOF: DEL-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "FILEOP-PROOF: --- rename source FOPSRC.DAT (must be GONE) ---"
$ DIRECTORY VDA0:[SYSTMP]FOPSRC.DAT
$ WRITE SYS$OUTPUT "FILEOP-PROOF: SRC-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "FILEOP-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
