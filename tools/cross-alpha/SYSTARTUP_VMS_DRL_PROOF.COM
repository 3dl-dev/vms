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
$ WRITE SYS$OUTPUT "DRL-PROOF: === END ==="
$!
$ EXIT
