$! SYSTARTUP_VMS.COM - native-image proof variant (vms-3b3f; harness-only).
$!
$! Staged by tools/cross-alpha/build-alpha-bootimage.sh only when the
$! NATIVE_PROOF marker is present. HELLO, RETST, CSTDIO, MAIN3, MAIN4 and
$! MYSHR/MYSHRV2 are images LINKed on the lab OpenVMS Alpha V8.4 node from our
$! own MACRO-32 sources (tests/native-images/alpha/), copied here unchanged.
$! What each RUN must do is what the same RUN did on that node
$! (tests/lab/captures/native-image-alpha-20261008/):
$!   HELLO  - SYS$QIOW + LIB$PUT_OUTPUT lines, $STATUS %X00000001   (rung 1)
$!   RETST  - $STATUS %X0FEDC0A9 (its main's return value)          (rung 1)
$!   CSTDIO - DECC$SHR puts + printf lines, $STATUS %X00000001       (rung 2)
$!   MAIN3  - two modules + MYSHR (SYS$SHARE), three lines, %X00000001 (rung 3)
$!   MAIN4  - linked against MYSHR 2.0: against SYS$SHARE's MYSHR 1.0 it is
$!            refused -SYSTEM-F-SHRIDMISMAT %X100020BC; with MYSHR defined
$!            to MYSHRV2 (2.0) it runs; defined to a missing file it is
$!            refused -CLI-E-IMAGEFNF %X100388B2                      (rung 3)
$!   NOTIMG - a text file: -IMGACT-F-NOTNATIVE %X104D8CFC
$!
$ SET NOON
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN HELLO ==="
$ RUN SYS$SYSTEM:HELLO
$ WRITE SYS$OUTPUT "NATIVE-PROOF: HELLO-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN RETST ==="
$ RUN SYS$SYSTEM:RETST
$ WRITE SYS$OUTPUT "NATIVE-PROOF: RETST-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN CSTDIO ==="
$ RUN SYS$SYSTEM:CSTDIO
$ WRITE SYS$OUTPUT "NATIVE-PROOF: CSTDIO-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN MAIN3 ==="
$ RUN SYS$SYSTEM:MAIN3
$ WRITE SYS$OUTPUT "NATIVE-PROOF: MAIN3-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN MAIN4 (MYSHR 1.0 in SYS$SHARE) ==="
$ RUN SYS$SYSTEM:MAIN4
$ WRITE SYS$OUTPUT "NATIVE-PROOF: MAIN4V1-STATUS=''$STATUS'"
$ DEFINE MYSHR SYS$SYSTEM:MYSHRV2
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN MAIN4 (MYSHR = SYS$SYSTEM:MYSHRV2) ==="
$ RUN SYS$SYSTEM:MAIN4
$ WRITE SYS$OUTPUT "NATIVE-PROOF: MAIN4V2-STATUS=''$STATUS'"
$ DEFINE MYSHR SYS$SYSTEM:NOSUCH
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN MAIN4 (MYSHR = SYS$SYSTEM:NOSUCH) ==="
$ RUN SYS$SYSTEM:MAIN4
$ WRITE SYS$OUTPUT "NATIVE-PROOF: MAIN4NONE-STATUS=''$STATUS'"
$ DEASSIGN MYSHR
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN NOTIMG (a text file) ==="
$ RUN SYS$SYSTEM:NOTIMG
$ WRITE SYS$OUTPUT "NATIVE-PROOF: NOTIMG-STATUS=''$STATUS'"
$!
$! The semantic-oracle probes LINKed on the lab Alpha V8.4 node (the very
$! programs docs/oracle/semantics/<family>/alpha84.txt was printed by), run
$! unchanged; each prints its own "=== SEMPROBE <family> BEGIN/END ===".
$! One RUN per image in tests/native-images/alpha/sp/ (the gate checks the
$! two lists agree).
$ RUN SYS$SYSTEM:SP_EF
$ RUN SYS$SYSTEM:SP_FAO
$ RUN SYS$SYSTEM:SP_TT
$ RUN SYS$SYSTEM:SP_IO
$ RUN SYS$SYSTEM:SP_LNM
$ RUN SYS$SYSTEM:SP_INFO
$ RUN SYS$SYSTEM:SP_LOCK
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === END ==="
$ EXIT
