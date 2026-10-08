$! EVACWL.COM - assemble, link, and (optionally) run EVACWL.MAR (vms-06c).
$!
$! Usage:
$!   $ @EVACWL                  build only
$!   $ @EVACWL RUN              build, then run with no arguments
$!                              (direct EX, loop forever)
$!   $ @EVACWL RUN STANDBY      build, then run "STANDBY" (NL then CONVERT
$!                              to EX, loop forever)
$!   $ @EVACWL RUN n            build, then run with a record-count limit
$!   $ @EVACWL RUN STANDBY n    build, then run STANDBY with a count limit
$!
$ ON ERROR THEN GOTO FIN
$ WRITE SYS$OUTPUT "EVACWL.COM: assembling EVACWL.MAR"
$ MACRO EVACWL
$ WRITE SYS$OUTPUT "EVACWL.COM: linking EVACWL.OBJ"
$ LINK EVACWL
$ IF P1 .EQS. "" THEN GOTO FIN
$ IF P1 .NES. "RUN" THEN GOTO FIN
$! A foreign-command symbol, not RUN: EVACWL.MAR reads its STANDBY/count
$! arguments via LIB$GET_FOREIGN, which only sees a command TAIL when the
$! image is invoked as a foreign command verb (DLMLOOP.COM's own convention,
$! tests/lab/captures/vms-8219-dir-role-20261004/DLMLOOP.COM) -- plain RUN
$! passes the image no command line at all.
$ EVACWL :== $SYS$DISK:[]EVACWL.EXE
$ WRITE SYS$OUTPUT "EVACWL.COM: running EVACWL ''P2' ''P3'"
$ EVACWL 'P2' 'P3'
$ FIN:
$ EXIT
