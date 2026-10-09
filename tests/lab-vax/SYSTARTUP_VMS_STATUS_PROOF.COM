$! SYSTARTUP_VMS.COM - $STATUS proof variant (rd vms-b869; harness-only).
$!
$! The VAX analogue of tools/cross-alpha/SYSTARTUP_VMS_NATIVE_PROOF.COM.
$! Staged in place of the Decision-A SYSTARTUP_VMS.COM only by
$! tests/lab-vax/run-boot.sh status-gate (stage_sysvol.sh --status-proof);
$! never shipped. Still Decision A: no INSTALL ADD.
$!
$! Each RUN must leave this exact $STATUS (asserted by the run-boot.sh function
$! assert_status_proof against the SIMH console):
$!   STSNORM - SYS$EXIT(SS$_NORMAL)    $STATUS %X00000001
$!   STSCOND - SYS$EXIT(%X0FEDC0A9)    $STATUS %X0FEDC0A9
$! (tests/netbsd/guest/status_exit.c, built as Decision-A OVMX/VAX images.)
$!
$! ADDING AN IMAGE (e.g. a native VAX IHD .EXE LINKed on real VAX/VMS, from
$! tests/native-images/vax/): copy it into the STATUS_PROOF_IMAGES dir
$! run-boot.sh stages into SYS$SYSTEM, add a three-line block below
$! shaped exactly like the STSCOND one (RUN header, RUN, STATUS line),
$! and add a "<NAME>|<%Xvalue>|<line the image prints>" row to
$! STATUS_PROOF_EXPECT in run-boot.sh. Keep the END line last.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "STATUS-PROOF: === RUN STSNORM ==="
$ RUN SYS$SYSTEM:STSNORM
$ WRITE SYS$OUTPUT "STATUS-PROOF: STSNORM-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "STATUS-PROOF: === RUN STSCOND ==="
$ RUN SYS$SYSTEM:STSCOND
$ WRITE SYS$OUTPUT "STATUS-PROOF: STSCOND-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "STATUS-PROOF: === END ==="
$ EXIT
