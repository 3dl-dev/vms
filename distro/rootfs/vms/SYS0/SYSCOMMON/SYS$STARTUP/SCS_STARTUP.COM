$! SCS_STARTUP.COM - starts SCSD, the SCS cluster connection manager, as a
$! detached system process (vms-5ad, child of vms-110b).
$!
$! Invoked from SYS$STARTUP:VMS$VMS.DAT, a registered CONFIG-phase STDRV
$! component (mirrors JOB_CONTROL_STARTUP.COM's END-phase registration --
$! see that file's header for the full RUN_COMPONENTS mechanism). This is
$! the boot-time half of vms-5ad; the self-configuring half lives in
$! src/vmsscs/scsd.c's BOOT-CLUSTER MODE (reached when SCSD.EXE is invoked
$! with no argv at all -- exactly what RUN /DETACHED gives it below).
$!
$! WHY NO ARGUMENTS. OVMX's RUN /DETACHED passes NO argv to the image --
$! dcl_cmd_process.c's qualifier table has no /PARAMETERS -- so this
$! procedure cannot hand SCSD.EXE --iface/--connect/etc. the way the
$! reference lab's hand-run probe does. SCSD.EXE therefore self-configures
$! on a bare invocation (argc==1):
$!
$!   1. THE VAXCLUSTER SELF-GATE. SCSD reads VAXCLUSTER from the same
$!      SYSGEN store (SYS$SYSTEM:OVMXVMSSYS.PAR) every other cluster
$!      identity param comes from (sysgen_read_param(), scsd.c's existing
$!      resolve_scssystemid()/resolve_alloclass() path). VAXCLUSTER==0 (the
$!      shipped default -- this node is standalone) makes SCSD print one
$!      honest line and exit 0: a cluster connection manager choosing not
$!      to run on a standalone node is correct behaviour, not a failure.
$!      Any nonzero VAXCLUSTER means the operator wants this node in a
$!      cluster, so SCSD runs persistently until $ STOP or a reboot.
$!
$!   2. ETH0: PRIMARY-NIC SELF-RESOLUTION. When it does run, SCSD resolves
$!      the primary cluster NIC itself -- the SAME non-loopback Ethernet
$!      net device the executive's device table (src/kernel-core/
$!      vms_devtab.c) surfaces to VMS programs as ETH0: (exec_netdev_primary,
$!      src/kernel-core/exec_kbackend.h sec 11) -- via its own userspace
$!      twin of that rule (scs_datalink_primary_iface()). This names the
$!      VMS device family the executive already established, never a Linux
$!      interface name on this surface -- exactly as JOB_CONTROL_STARTUP.COM
$!      above names OPA0:, the VMS console device, never a host tty path.
$!
$! WHY /OUTPUT AND /ERROR NAME OPA0:, THE OPERATOR CONSOLE. Cluster state
$! transitions are OPCOM/operator-console events on real VMS, so SCSD's
$! SCSD-I-* startup lines belong on OPA0: -- the same device JOB_CONTROL_
$! STARTUP.COM names, resolved the same way (ovmx_console_terminal_path). A
$! private log file is NOT an option here: a detached process's /OUTPUT is
$! created by $CREPRC with a raw open() (src/libvms/syssvc/sys_process.c),
$! which cannot create a file on the ODS-2 system disk the executive ACP
$! serves -- SYS$MANAGER:SCSD.OUT comes back %RMS-E-FNF (the file is never
$! made). Wiring $CREPRC /OUTPUT through RMS/the ACP so a detached process
$! CAN log to an ODS-2 file is a real gap, tracked separately (vms-7eb
$! follow-up); it does not gate cluster boot. The few SCSD-I lines may
$! interleave with STARTUP.COM's CONFIG-phase console output, exactly as
$! cluster messages interleave during a real VMS boot -- an accepted
$! reality, not a defect, and each line is a single flushed write.
$!
$ SET NOON
$ RUN /DETACHED -
      /PROCESS_NAME=SCS_SERVER -
      /OUTPUT=OPA0: /ERROR=OPA0: -
      SYS$SYSTEM:SCSD.EXE
$ EXIT
