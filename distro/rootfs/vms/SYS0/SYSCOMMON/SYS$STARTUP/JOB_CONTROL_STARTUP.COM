$! JOB_CONTROL_STARTUP.COM - creates JOB_CONTROL, the console-session process
$!
$! Invoked from SYS$STARTUP:VMS$VMS.DAT, a registered END-phase STDRV
$! component (vms-2a9, docs/design-boot-faithful.md §3.8/§4.5) -- no longer
$! hardcoded into SYS$MANAGER:SYSTARTUP_VMS.COM (vms-8d2's original landing;
$! see that file's own header for why the direct call moved). This is the
$! FIRST real worked example of the "HOW TO START ONE" mechanism
$! SYSTARTUP_VMS.COM documents (vms-47b): a DETACHED process with a VMS
$! process name, so SHOW SYSTEM lists it and $GETJPI resolves it by name
$! from any other process on the node.
$!
$! ORDERING NOTE (vms-2a9). END is the LAST of STDRV's nine phases, which
$! runs after LPMAIN and therefore after SYSTARTUP_VMS.COM's Priority 1
$! shareable-image INSTALL ADD registrations -- so JOB_CONTROL.EXE's own
$! dynamic activation still resolves its dependencies at Priority 1, exactly
$! as it did when this procedure was called directly from SYSTARTUP_VMS.COM.
$! END was not the first phase tried: CONFIG (much earlier) was, on the
$! reasoning that a core OS service belongs before the layered-product/site
$! phases -- but a real QEMU boot (tests/qemu/test_boot_conformance.sh)
$! showed that starting this early interleaves JOB_CONTROL's own LOGINOUT
$! session's "Username:" prompt with STARTUP.COM's still-running console
$! output. END reproduces this project's already-tested-safe original
$! ordering (JOB_CONTROL was, in practice, the last real action of the
$! LPMAIN phase before this item) while still routing creation through the
$! declarative component mechanism VMS$VMS.DAT exists to drive -- see that
$! file's own header for the full account.
$!
$! WHY THIS EXISTS AT ALL (design-init-scope.md §1 row 6 / §2's "wrong
$! component" finding). On OpenVMS, the interactive console session is
$! created by JOB_CONTROL, not by the SYSINIT-equivalent bootstrap process.
$! STARTUP.EXE (PID 1) used to run that loop itself, inline -- real function,
$! wrong owner. JOB_CONTROL.EXE now holds it, and this procedure is what
$! creates JOB_CONTROL, exactly the way any other OVMX service is created.
$!
$! WHY /INPUT, /OUTPUT AND /ERROR NAME OPA0:, THE VMS CONSOLE (vms-948).
$! A DETACHED process has no controlling terminal by definition -- $CREPRC
$! (src/libvms/syssvc/sys_process.c) attaches an unsupplied stream to the
$! null device for one. JOB_CONTROL needs the REAL console to do its job, so
$! this procedure supplies it explicitly, and names it the VMS way: OPA0:,
$! the operator console (src/kernel-core/vms_devtab.c). This procedure spells
$! a VMS device, never a substrate path.
$!
$! HOW OPA0: RESOLVES. RUN/DETACHED's /INPUT=/OUTPUT=/ERROR= qualifiers resolve
$! through dcl_resolve_path() (src/vmsdcl/dcl_filespec.c). It recognises the
$! console-terminal aliases (OPA0:/_OPA0:/TT:/TT0:) via the SINGLE resolver
$! ovmx_console_terminal_path() (src/libvms/include/ovmx_console.h) -- the same
$! source sys$assign() uses -- and yields the console's backing device before
$! the ':'-bearing spec could fall into the vmsfs disk translator. The Linux
$! device that backs OPA0: (measured vms-1c57) lives ONLY inside that resolver
$! and never appears on this VMS surface. JOB_CONTROL's own login children take
$! a channel to OPA0: the VMS way ($ASSIGN + $GETDVI, vms-d0b) once they are
$! running; this creation-time redirection names the same device.
$!
$! No quoting: OPA0: contains no '/', so it tokenises as a normal qualifier
$! value (the quoting that a leading-slash Linux path once forced is gone).
$!
$ SET NOON
$ RUN /DETACHED -
      /PROCESS_NAME=JOB_CONTROL -
      /INPUT=OPA0: /OUTPUT=OPA0: /ERROR=OPA0: -
      SYS$SYSTEM:JOB_CONTROL.EXE
$ EXIT
