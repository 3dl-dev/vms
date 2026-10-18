$! SYSTARTUP_VMS.COM - VMS-hosted cc1 -> as -> LINK -> RUN proof variant (vms-e40;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the LNK proof marker is present; the product rootfs-alpha file is
$! never modified.
$! SYS$SYSTEM:CC1.EXE, AS.EXE and LINK.EXE are GCC 14.2 cc1, binutils 2.43 gas and OVMX's
$! linker, all built --host=alpha-dec-vms over OVMX's C RTL. DCL writes the C source;
$! SYS$SYSTEM:JOINT_E2E runs the three stages (vfork + execv, wait); DCL then RUNs the
$! HELLO.EXE the linker wrote.
$!
$ SET NOON
$ WRITE SYS$OUTPUT "LNK-PROOF: === DCL writes the C source ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "static int sq (int x) { return x * x; }"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""hello from cc1 on OVMX %d\n"", sq (7));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ WRITE SYS$OUTPUT "LNK-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "LNK-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv CC1, AS, LINK) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "LNK-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "LNK-PROOF: === DIRECTORY (what the three stages left behind) ==="
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "LNK-PROOF: === RUN THE LINKED PROGRAM: RUN HELLO.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "LNK-PROOF: HELLO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "LNK-PROOF: === END ==="
$!
$ EXIT
