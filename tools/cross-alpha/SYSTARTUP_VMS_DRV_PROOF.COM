$! SYSTARTUP_VMS.COM - VMS-hosted driver (xgcc) proof variant (vms-1eb;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the DRV proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:XGCC.EXE is the GCC 14.2 driver for alpha-dec-vms; CC1.EXE and AS.EXE
$! (binutils 2.43) sit beside it, all built --host=alpha-dec-vms over OVMX's C RTL.
$! DCL writes the C source; SYS$SYSTEM:JOINT_E2E runs XGCC -c (vfork + execv, wait);
$! the driver runs cc1 and as itself. DCL DUMP -- a different accessor -- prints the
$! EVAX object it wrote, which the gate compares with the cross driver's object.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "AS-PROOF: === DCL writes the C source ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "static int sq (int x) { return x * x; }"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""hello from cc1 on OVMX %d\n"", sq (7));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ WRITE SYS$OUTPUT "AS-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$!
$ WRITE SYS$OUTPUT "AS-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv XGCC.EXE -c) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "AS-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "AS-PROOF: === DIRECTORY (what the two stages left behind) ==="
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ DIRECTORY/SIZE VDA0:[000000]
$ DIRECTORY/SIZE SYS$LOGIN
$ WRITE SYS$OUTPUT "AS-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.OBJ ==="
$ DUMP VDA0:[SYSTMP]HELLO.OBJ
$ WRITE SYS$OUTPUT "AS-PROOF: DUMP-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "AS-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
