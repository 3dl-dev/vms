$! SYSTARTUP_VMS.COM - cc1 BUILT ON OVMX proof variant (rd vms-465; harness-only).
$!
$! Staged onto the system volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the CC1B_PROOF marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! GCCSRC:[SRC.GCC-14^.2^.0...] is GCC 14.2's source tree, GCCSRC:[BLD...] the sources and
$! headers its build generates, GCCSRC:[HOST...] GMP/MPFR/MPC as installed.
$! GCCSRC:[000000]BUILD_CC1.COM (gen-cc1-build.py) runs every compile GCC's own
$! Makefiles run for cc1 with the native driver (XGCC.EXE / GXX.EXE -> CC1.EXE /
$! CC1PLUS.EXE, AS.EXE), makes the libraries with AR.EXE (LIBRARY) and links
$! GCCOBJ:[CC1]cc1.exe with GXX.EXE (ld -> LINK). Then the cc1 built here
$! compiles HELLO.C, and DCL TYPE shows what it wrote.
$!
$ SET NOON
$ SET DEFAULT VDA0:[SYSTMP]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED USR SYS$COMMON:[USR.]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED GNU SYS$COMMON:[GNU.]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED GCCSRC SYS$COMMON:[GCCSRC.]
$ CREATE/DIRECTORY SYS$COMMON:[GCCOBJ]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED GCCOBJ SYS$COMMON:[GCCOBJ.]
$! the C RTL hands RMS names with '+' (c^+^+config.h) when this is defined, as the DEC C RTL does
$ DEFINE/SYSTEM DECC$EFS_CHARSET ENABLE
$! the trees as the ACP sees them: a file deep in the largest source directory
$ DIRECTORY GCCSRC:[000000]
$ DIRECTORY GCCSRC:[SRC.GCC-14^.2^.0]
$ DIRECTORY/SIZE GCCSRC:[SRC.GCC-14^.2^.0.GCC]GIMPLE-MATCH-HEAD.CC
$ DIRECTORY/SIZE GCCSRC:[SRC.GCC-14^.2^.0.GCC.C-FAMILY]C-COMMON.CC
$ DIRECTORY/SIZE GCCSRC:[SRC.GCC-14^.2^.0.GCC.CONFIG.VMS]XM-VMS.H
$ DIRECTORY/SIZE GCCSRC:[BLD.GCC]CONFIG.H
$! a machine description the generated insn-*.cc name by #line, from their directory (vms-2eb)
$ DIRECTORY/SIZE GCCSRC:[SRC.GCC-14^.2^.0.GCC.CONFIG.ALPHA]ALPHA.MD
$ WRITE SYS$OUTPUT "CC1B-PROOF: === BUILD: @GCCSRC:[000000]BUILD_CC1 ==="
$ @GCCSRC:[000000]BUILD_CC1
$ WRITE SYS$OUTPUT "CC1B-PROOF: BUILD-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$! what the ACP holds under GCCOBJ (the objects a CI shard carries off its volume)
$ DIRECTORY/SIZE=ALL GCCOBJ:[*]
$ DIRECTORY/SIZE GCCOBJ:[CC1]CC1.EXE
$!
$! The cc1 built here, as a foreign command; the arguments are quoted so DCL passes
$! them as written (the same arguments the cross cc1 wrote the reference with).
$ CC1N := $GCCOBJ:[CC1]CC1.EXE
$ COPY GCCSRC:[000000]HELLO.C VDA0:[SYSTMP]HELLO.C
$ WRITE SYS$OUTPUT "CC1B-PROOF: === the built cc1 compiles HELLO.C ==="
$ CC1N "-quiet" "-nostdinc" "-dumpbase" "hello.c" "VDA0:[SYSTMP]HELLO.C" "-o" "VDA0:[SYSTMP]HELLO.S"
$ WRITE SYS$OUTPUT "CC1B-PROOF: CC1-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "CC1B-PROOF: === INDEPENDENT READER: DCL TYPE HELLO.S ==="
$ TYPE VDA0:[SYSTMP]HELLO.S
$ WRITE SYS$OUTPUT "CC1B-PROOF: TYPE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ CC1N "-quiet" "-nostdinc" "-O2" "-dumpbase" "hello.c" "VDA0:[SYSTMP]HELLO.C" "-o" "VDA0:[SYSTMP]HELLO2.S"
$ WRITE SYS$OUTPUT "CC1B-PROOF: CC1O2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "CC1B-PROOF: === INDEPENDENT READER: DCL TYPE HELLO2.S ==="
$ TYPE VDA0:[SYSTMP]HELLO2.S
$ WRITE SYS$OUTPUT "CC1B-PROOF: TYPE2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "CC1B-PROOF: === END ==="
$ EXIT
