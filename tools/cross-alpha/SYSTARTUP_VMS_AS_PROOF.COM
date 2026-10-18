$! SYSTARTUP_VMS.COM - VMS-hosted cc1 -> as pipeline proof variant (vms-5303e;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the AS proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:CC1.EXE and SYS$SYSTEM:AS.EXE are GCC 14.2's compiler proper and
$! binutils 2.43's GNU as for alpha-dec-vms, both built --host=alpha-dec-vms
$! over OVMX's C RTL (32-bit DEC C programs). DCL writes the C source;
$! SYS$SYSTEM:JOINT_E2E runs CC1 and then AS the way the GCC driver does
$! (vfork + execv, wait); DCL DUMP -- a different accessor -- prints the EVAX
$! object as wrote, which the gate compares with the cross toolchain's object.
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
$! vms-e40 diagnostics: the images the launcher runs, and LINK started by DCL (which
$! reports an activation failure the vfork path swallows; no input = its usage message)
$ DIRECTORY/SIZE SYS$SYSTEM:LINK.EXE,AS.EXE,CC1.EXE
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "AS-PROOF: === DCL RUN SYS$SYSTEM:LINK (no input) ==="
$ RUN SYS$SYSTEM:LINK
$ WRITE SYS$OUTPUT "AS-PROOF: LINK-NOINPUT-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "AS-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv CC1.EXE then AS.EXE) ==="
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
$! vms-e40: the image LINK.EXE (built for this host, run by the launcher) wrote.
$ WRITE SYS$OUTPUT "AS-PROOF: === RUN HELLO.EXE (linked on OVMX) ==="
$ RUN VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "AS-PROOF: HELLO-RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "AS-PROOF: === END RUN ==="
$!
$ EXIT
