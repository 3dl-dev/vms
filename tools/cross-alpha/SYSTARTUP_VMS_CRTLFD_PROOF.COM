$! SYSTARTUP_VMS.COM - C RTL file layer over RMS proof variant (vms-b90;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the CRTLFD proof marker is present (a crtl_fd_test.c build); the
$! product rootfs-alpha file is never modified.
$!
$! SYS$SYSTEM:JOINT_E2E is a 32-bit DEC C program built against the RMS-backed
$! DECC$SHR, whose C RTL file calls (open/read/write/lseek/fstat/close and all of
$! stdio) are served by RMS from the syscall funnel (src/vmsrms/crtl_rms_fd.c).
$!
$! Both directions are checked by a DIFFERENT accessor than the image's C RTL:
$!   - BEFORE the image runs, DCL OPEN/WRITE writes VDA0:[SYSTMP]CFDIN.TXT, a
$!     record file DCL owns; the image must read its records back as lines.
$!   - AFTER the image runs, DCL TYPE (RMS $OPEN/$GET in DCL's own image) prints
$!     VDA0:[SYSTMP]CFDOUT.TXT, the stream file the image wrote with fputs; its
$!     lines must appear on the console exactly. A write that went anywhere but
$!     the ODS-2 volume (a ramfs, a kernel file) cannot be TYPEd here.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: === DCL writes the record file the image reads ==="
$ OPEN/WRITE CFDIN VDA0:[SYSTMP]CFDIN.TXT
$ WRITE CFDIN "WRITTEN BY DCL"
$ WRITE CFDIN "SECOND RECORD"
$ CLOSE CFDIN
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$!
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: === MILESTONE: RUN JOINT_E2E (C RTL file layer over RMS) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: === INDEPENDENT READER: DCL TYPE of the file the image wrote ==="
$ TYPE VDA0:[SYSTMP]CFDOUT.TXT
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: TYPE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "CRTLFD-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
