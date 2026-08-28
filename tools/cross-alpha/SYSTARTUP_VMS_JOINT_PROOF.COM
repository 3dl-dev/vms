$! SYSTARTUP_VMS.COM - JOINT-E2E GCC-port-image proof variant (vms-157, harness-only)
$!
$! Harness variant of the Alpha static-bootstrap SYS$MANAGER:SYSTARTUP_VMS.COM
$! (distro/rootfs-alpha/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM). It is
$! staged onto the ODS-2 system volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the joint-e2e proof artifacts are present; the product rootfs-alpha
$! file is never modified.
$!
$! WHAT IT PROVES (path B, the faithful milestone -- vms-157): during the STDRV
$! LPMAIN phase, a DCL `RUN` activates the REAL alpha-dec-vms GCC-port image
$! SYS$SYSTEM:JOINT_E2E on the live OVMX/Alpha executive. IMGACT (PT_INTERP)
$! reads the image AND its .vms$imp producers (DECC$SHR.EXE -> LIBOTS_SHR.EXE)
$! OVER THE FILES-11 ACP from the MOUNTED ODS-2 volume -- not the initramfs. The
$! port image's crt0 -> decc$main -> main() runs and returns its sentinel (3),
$! which C$_EXIT1 folds into the image exit status RUN records in $STATUS.
$!
$! The DIRECTORY probes below pin, when the RUN does NOT reach main, WHETHER the
$! producers are (a) mis-PLACED for IMGACT's ACP SYS$SHARE lookup or (b) present
$! but with ODS-2 record attributes IMGACT cannot read (compared against DCL.EXE,
$! a known-good ACP-read image). SET NOON keeps every probe non-fatal.
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: === producer placement + record attributes (IMGACT reads these over the ACP) ==="
$ SHOW LOGICAL SYS$SHARE
$ DIRECTORY SYS$COMMON:[SYSLIB]
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: --- DECC$SHR.EXE full attributes ---"
$ DIRECTORY/FULL SYS$COMMON:[SYSLIB]DECC$SHR.EXE
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: --- LIBOTS_SHR.EXE full attributes ---"
$ DIRECTORY/FULL SYS$COMMON:[SYSLIB]LIBOTS_SHR.EXE
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: --- known-good ACP-read image DCL.EXE full attributes (compare) ---"
$ DIRECTORY/FULL SYS$SYSTEM:DCL.EXE
$!
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: activating SYS$SYSTEM:JOINT_E2E via IMGACT over the mounted ODS-2 ACP"
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "JOINT-E2E-PROOF: STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ EXIT
