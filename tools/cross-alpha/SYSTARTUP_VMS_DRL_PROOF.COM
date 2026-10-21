$! SYSTARTUP_VMS.COM - VMS-hosted xgcc compile + link proof variant (vms-65a;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the DRVLNK proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:XGCC.EXE is the GCC 14.2 driver for alpha-dec-vms; CC1.EXE, AS.EXE,
$! LD.EXE (the port's wrapper around the VMS LINK command) and LINK.EXE sit beside it,
$! all built --host=alpha-dec-vms over OVMX's C RTL. DCL writes the C source;
$! SYS$SYSTEM:JOINT_E2E runs `xgcc ... -o HELLO.EXE` (vfork + execv, wait); the driver
$! runs cc1, as and ld, and ld runs DCL LINK. DCL then DUMPs and RUNs HELLO.EXE.
$!
$ SET NOON
$ SET DEFAULT VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "DRL-PROOF: === DCL writes the C source ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "static int sq (int x) { return x * x; }"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""hello from cc1 on OVMX %d\n"", sq (7));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO2.C
$ WRITE HC "#include <stdio.h>"
$ WRITE HC "#include <stdlib.h>"
$ WRITE HC "#include <string.h>"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  char *p = malloc (16);"
$ WRITE HC "  strcpy (p, ""headers"");"
$ WRITE HC "  printf (""hello2 %s %d\n"", p, (int) strlen (p));"
$ WRITE HC "  free (p);"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$! the system headers and the compiler's own, where the driver looks by default
$! (/usr/include -> USR:[INCLUDE], /gnu/lib/gcc/... -> GNU:[LIB.GCC...]): concealed rooted logicals
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED USR SYS$COMMON:[USR.]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED GNU SYS$COMMON:[GNU.]
$ SHOW LOGICAL USR
$ SHOW LOGICAL GNU
$ DIRECTORY USR:[INCLUDE]STDIO.H
$ DIRECTORY USR:[INCLUDE.SYS]TYPES.H
$ DIRECTORY GNU:[LIB.GCC.ALPHA-DEC-VMS.14_2_0.INCLUDE]STDDEF.H
$ WRITE SYS$OUTPUT "DRL-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv XGCC.EXE -o HELLO.EXE) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "DRL-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === DIRECTORY (what the driver left behind) ==="
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "DRL-PROOF: === the options file ld wrote ==="
$ TYPE VDA0:[SYSTMP]HELLO.OPT_TMPFILE
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.EXE ==="
$ DUMP VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE LINKED PROGRAM: RUN HELLO.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: HELLO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP HELLO2.EXE ==="
$ DUMP VDA0:[SYSTMP]HELLO2.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE HEADER PROGRAM: RUN HELLO2.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO2.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: HELLO2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$! A native BUILD: the compiler and the archiver as DCL foreign commands, the way a
$! command procedure (or MMK) runs them -- two sources compiled, an object library made
$! with the port's ar wrapper (which runs LIBRARY), a program linked against it.
$ WRITE SYS$OUTPUT "DRL-PROOF: === native build: GCC and AR as foreign commands ==="
$ GCC := $SYS$SYSTEM:XGCC.EXE
$ AR := $SYS$SYSTEM:AR.EXE
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOA.C
$ WRITE HC "int two_a (int x) { return x + 3; }"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOB.C
$ WRITE HC "int two_b (int x) { return x * 4; }"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOMAIN.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "int two_a (int);"
$ WRITE HC "int two_b (int);"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""two %d %d\n"", two_a (4), two_b (4));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ GCC -BSYS$SYSTEM: -nostdinc -x c -c VDA0:[SYSTMP]TWOA.C -o VDA0:[SYSTMP]TWOA.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-CC1-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ GCC -BSYS$SYSTEM: -nostdinc -x c -c VDA0:[SYSTMP]TWOB.C -o VDA0:[SYSTMP]TWOB.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-CC2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$! the port's ar (config/vms/vms-ar.c) takes key letters ("rc", the AR_FLAGS a Makefile passes) or separate -r -c; it rejects a joined "-rc"
$ AR rc VDA0:[SYSTMP]LIBTWO.A VDA0:[SYSTMP]TWOA.O VDA0:[SYSTMP]TWOB.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-AR-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ LIBRARY/LIST VDA0:[SYSTMP]LIBTWO.A
$ GCC -BSYS$SYSTEM: -L/SYS$SYSTEM -nostdinc -x c VDA0:[SYSTMP]TWOMAIN.C -o VDA0:[SYSTMP]TWO.EXE -x none VDA0:[SYSTMP]LIBTWO.A
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-LINK-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP TWO.EXE ==="
$ DUMP VDA0:[SYSTMP]TWO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP3-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE BUILT PROGRAM: RUN TWO.EXE ==="
$ RUN VDA0:[SYSTMP]TWO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: TWO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === END ==="
$!
$ EXIT
