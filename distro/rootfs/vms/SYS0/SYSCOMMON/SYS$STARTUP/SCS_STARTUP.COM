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
$! WHY /OUTPUT AND /ERROR NAME A LOG, NOT OPA0:. Unlike JOB_CONTROL (which
$! OWNS the console login loop and therefore needs the real console),
$! SCSD's boot-time run log has no interactive purpose -- it exists so an
$! operator can `TYPE SYS$MANAGER:SCSD.OUT` after the fact. Routing it to
$! OPA0: instead would interleave SCSD's own SCSD-I-* startup lines with
$! whatever STARTUP.COM (CONFIG phase runs early, alongside SYCONFIG.COM)
$! is still writing to that same console -- the EXACT garbled-output defect
$! JOB_CONTROL_STARTUP.COM's header describes for a different phase. A log
$! file sidesteps the interleave entirely rather than needing a late phase
$! to avoid it, which is what lets SCS run at CONFIG (see VMS$VMS.DAT's own
$! comment on why CONFIG is right for this component) instead of END.
$!
$ SET NOON
$ RUN /DETACHED -
      /PROCESS_NAME=SCS_SERVER -
      /OUTPUT=SYS$MANAGER:SCSD.OUT /ERROR=SYS$MANAGER:SCSD.OUT -
      SYS$SYSTEM:SCSD.EXE
$ EXIT
