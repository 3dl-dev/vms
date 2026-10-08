$! SYSTARTUP_VMS.COM - VMS-hosted cc1 proof variant (vms-9a63; harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the CC1 proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:CC1.EXE is GCC 14.2's compiler proper for alpha-dec-vms, built
$! --host=alpha-dec-vms over OVMX's C RTL (a 32-bit DEC C program). DCL writes
$! the C source; SYS$SYSTEM:JOINT_E2E runs CC1.EXE on it the way the GCC driver
$! does (vfork + execv); DCL TYPE -- a different accessor -- prints the assembly
$! cc1 wrote, which the gate compares with the cross compiler's output.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "CC1-PROOF: === DCL writes the C source ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "static int sq (int x) { return x * x; }"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""hello from cc1 on OVMX %d\n"", sq (7));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ WRITE SYS$OUTPUT "CC1-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$!
$ WRITE SYS$OUTPUT "CC1-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv CC1.EXE) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "CC1-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "CC1-PROOF: === INDEPENDENT READER: DCL TYPE HELLO.S ==="
$ TYPE VDA0:[SYSTMP]HELLO.S
$ WRITE SYS$OUTPUT "CC1-PROOF: TYPE-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "CC1-PROOF: === END INDEPENDENT READER ==="
$!
$ EXIT
