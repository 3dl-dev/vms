$! SYSTARTUP_VMS_SHR_ACTIVATION_PROOF.COM - P4 shareable-activation proof
$! variant (rd vms-d4a, epic vms-404 P4), harness-only.
$!
$! Harness variant of the netbsd-vax Decision-A SYS$MANAGER:SYSTARTUP_VMS.COM
$! (distro/rootfs-vax/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM), staged
$! onto the ODS-2 volume by tools/cross-vax/run-shr-activation-vax.sh ONLY
$! for this gate; the product rootfs-vax file is never modified. Same
$! shape as the Alpha JOINT-E2E proof variant
$! (tools/cross-alpha/SYSTARTUP_VMS_JOINT_PROOF.COM, rd vms-157).
$!
$! MILESTONE (rd vms-d4a): during STDRV a DCL RUN activates the REAL elf32-vax
$! CONSUMER.EXE on the live OVMX/NetBSD-vax executive. IMGACT.EXE
$! (PT_INTERP=SYS$SYSEXE:IMGACT.EXE) reads the image AND its SINGLE `.vms$imp`
$! producer -- the SHIPPED LIBVMS$SHR.EXE (rd vms-c7f7) -- OVER THE FILES-11
$! ACP from the MOUNTED ODS-2 volume. CONSUMER's freestanding _start runs its
$! body, which calls the REAL purdy_s_hash universal cross-shareable into
$! LIBVMS$SHR.EXE and prints the value-sensitive result on THREE channels --
$! PRIMARY a direct write to the /dev/console DEVICE (which survives the fork()+
$! execve() child's unwired fd 1/2, the measured failure of the prior cut), plus
$! a bare write(2) and a C-RTL printf() fallback -- then _exit()s with 0 IFF the
$! value was golden. DCL maps that exit to $STATUS (0 -> SS$_NORMAL %X00000001,
$! nonzero -> %DCL-E-ABORT), so the STATUS line this .COM writes below is a
$! SECOND, console-proven golden witness independent of the text channels.
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "VAX-SHR-ACT-PROOF: === RUN CONSUMER (IMGACT resolves .vms$sv; cross-shareable purdy_s_hash call into the shipped LIBVMS$SHR.EXE) ==="
$ RUN SYS$SYSTEM:CONSUMER
$ WRITE SYS$OUTPUT "VAX-SHR-ACT-PROOF: STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$ EXIT
