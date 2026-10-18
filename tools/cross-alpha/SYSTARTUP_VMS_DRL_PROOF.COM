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
$ WRITE SYS$OUTPUT "DRL-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv XGCC.EXE -o HELLO.EXE) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "DRL-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === DIRECTORY (what the driver left behind) ==="
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.EXE ==="
$ DUMP VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE LINKED PROGRAM: RUN HELLO.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: HELLO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === END ==="
$!
$ EXIT
