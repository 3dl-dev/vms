$! SYSTARTUP_VMS.COM - native-image proof variant (vms-3b3f; harness-only).
$!
$! Staged by tools/cross-alpha/build-alpha-bootimage.sh only when the
$! NATIVE_PROOF marker is present. HELLO, RETST and MAIN3 are .EXE files LINKed
$! on the lab OpenVMS Alpha V8.4 node from our own MACRO-32 sources
$! (tests/native-images/alpha/), copied here unchanged. What each must do is
$! what the same RUN did on that node (tests/lab/captures/
$! native-image-alpha-20261008/): HELLO prints through SYS$QIOW and
$! LIB$PUT_OUTPUT and leaves $STATUS %X00000001; RETST returns %X0FEDC0A9;
$! MAIN3 calls a shareable that is not installed here (MYSHR), so activation
$! fails -CLI-E-IMAGEFNF with $STATUS %X100388B2; NOTIMG is a text file, so
$! activation fails -IMGACT-F-NOTNATIVE with $STATUS %X104D8CFC.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN HELLO ==="
$ RUN SYS$SYSTEM:HELLO
$ WRITE SYS$OUTPUT "NATIVE-PROOF: HELLO-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN RETST ==="
$ RUN SYS$SYSTEM:RETST
$ WRITE SYS$OUTPUT "NATIVE-PROOF: RETST-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN MAIN3 (MYSHR not installed) ==="
$ RUN SYS$SYSTEM:MAIN3
$ WRITE SYS$OUTPUT "NATIVE-PROOF: MAIN3-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === RUN NOTIMG (a text file) ==="
$ RUN SYS$SYSTEM:NOTIMG
$ WRITE SYS$OUTPUT "NATIVE-PROOF: NOTIMG-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "NATIVE-PROOF: === END ==="
$ EXIT
