$! SYSTARTUP_VMS.COM - VMS-ABI RMS proof variant (vms-692; harness-only).
$!
$! Staged by tools/cross-alpha/build-alpha-bootimage.sh only when the VMSABI
$! proof marker is present (a vmsabi_rms_test.c build). SYS$SYSTEM:JOINT_E2E is
$! a 32-bit DEC C program that calls SYS$PARSE/SYS$SEARCH with a VMS-layout
$! FAB/NAM and prints the File ID it got for SYS$SYSTEM:JOINT_E2E.EXE; DCL
$! DIRECTORY/FULL -- DCL's own RMS, a different accessor -- then prints the File
$! ID the directory holds for the same file. The two must agree.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "VMSABI-PROOF: === MILESTONE: RUN JOINT_E2E ($PARSE/$SEARCH, VMS layout) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "VMSABI-PROOF: RUN-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "VMSABI-PROOF: === INDEPENDENT READER: DIRECTORY/FULL ==="
$ DIRECTORY/FULL SYS$SYSTEM:JOINT_E2E.EXE
$ WRITE SYS$OUTPUT "VMSABI-PROOF: === END INDEPENDENT READER ==="
$ EXIT
