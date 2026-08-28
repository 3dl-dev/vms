$! SYSTARTUP_VMS.COM - JOINT-E2E GCC-port-image proof variant (vms-157, harness-only)
$!
$! Harness variant of the Alpha static-bootstrap SYS$MANAGER:SYSTARTUP_VMS.COM,
$! staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the joint-e2e proof artifacts are present; the product rootfs-alpha
$! file is never modified.
$!
$! MILESTONE (vms-157): during STDRV a DCL RUN activates the REAL alpha-dec-vms
$! GCC-port image SYS$SYSTEM:JOINT_E2E on the live OVMX/Alpha executive. IMGACT
$! (PT_INTERP=/run/ovmx-boot/IMGACT.EXE, where PID 1 stages IMGACT.EXE) reads the
$! image AND its .vms$imp producers (DECC$SHR.EXE -> LIBOTS_SHR.EXE) OVER THE
$! FILES-11 ACP from the MOUNTED ODS-2 volume. crt0 -> decc$main -> main runs,
$! prints "OVMX crt0 join: activated, argc=1", and returns the sentinel 3.
$!
$! CONTROL (requirement: prove DCL RUN's fork-path status mapping is real, not a
$! rationalization): JOINT_E2E_OK is the same image but main returns 0. A 0
$! return must read success-class $STATUS; the sentinel-3 return must read
$! %X...03 (surfaced in the %DCL-E-ABORT message -- the DCL-RUN fork-path maps a
$! nonzero POSIX child exit to SS$_ABORT; the raw value 3 is still shown).
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: === CONTROL: RUN JOINT_E2E_OK (main returns 0) ==="
$ RUN SYS$SYSTEM:JOINT_E2E_OK
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: CONTROL-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: === MILESTONE: RUN JOINT_E2E (main returns sentinel 3) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ EXIT
