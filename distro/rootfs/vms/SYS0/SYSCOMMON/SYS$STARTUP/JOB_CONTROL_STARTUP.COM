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
$! WHY /INPUT, /OUTPUT AND /ERROR NAME A LINUX PATH, NOT A VMS FILESPEC.
$! A DETACHED process has no controlling terminal by definition -- $CREPRC
$! (src/libvms/syssvc/sys_process.c) attaches an unsupplied stream to the
$! null device for one. JOB_CONTROL needs the REAL console to do its job, so
$! this procedure supplies it explicitly. The VMS filespec for the console is
$! OPA0: (src/kernel/vms_devtab.c) -- but RUN/DETACHED's /INPUT=/OUTPUT=
$! /ERROR= qualifiers resolve through dcl_resolve_path() (src/vmsdcl/
$! dcl_filespec.c), which sends a spec with a ':' or '[' through the vmsfs
$! disk-backed translator, not through the SEPARATE terminal-alias resolution
$! sys$assign() implements (src/libvms/syssvc/sys_assign.c) for TT:/TT0:/
$! OPA0:/_OPA0:. That resolver's own comment names the Linux device behind
$! OPA0: directly: /dev/console (measured, vms-1c57 -- NOT /dev/tty, which
$! resolves to the CALLING process's own controlling terminal and nothing in
$! OVMX ever establishes one). dcl_resolve_path() already passes an absolute
$! Linux path through unchanged, so this procedure spells the same physical
$! device sys_assign.c already names, rather than extending the VMS-filespec
$! resolver to understand terminal aliases -- a change out of this item's
$! scope. JOB_CONTROL's own login children still take a channel to OPA0: the
$! VMS way ($ASSIGN + $GETDVI, vms-d0b) once they are running; only THIS
$! procedure's creation-time redirection is spelled as a Linux path.
$!
$! WHY THE PATH IS QUOTED. DCL's lexer (src/vmsdcl/dcl_lexer.c) treats
$! EVERY '/' as the start of a new qualifier, unconditionally -- including
$! one that appears inside a qualifier's own unquoted value. An unquoted
$! /INPUT=/dev/console therefore does not tokenize as "qualifier INPUT,
$! value /dev/console" at all: it tokenizes as three separate qualifiers
$! (INPUT with no value, then bogus DEV and CONSOLE), which the parser
$! neither refuses nor honours as intended. Quoting forces the lexer's
$! quoted-string branch (checked before the qualifier branch), which reads
$! the whole value, slashes included, as one token -- the DCL parser
$! explicitly accepts TOK_STRING for a qualifier value
$! (src/vmsdcl/dcl_parser.c parse_qualifier).
$!
$! WHY /UIC=[1,4] AND /PRIVILEGES=(ALL) (vms-69e / vms-d31d). Until vms-d31d,
$! RUN's /UIC was refused outright (OVMX$_NOPRCUIC) and /PRIVILEGES was
$! silently dropped, so passing them here would have been a lie DCL accepted
$! and threw away -- JOB_CONTROL could only ever inherit whatever identity
$! STARTUP.COM's own $CREPRC call happened to carry. vms-d31d closed that:
$! $CREPRC now stamps a requested uic/prvadr onto the CREATED process's
$! executive row (vms_kif_setident, src/libvms/syssvc/sys_process.c), so
$! what RUN passes here is what every other process and the Files-11
$! reference monitor sees. [1,4] is SYSTEM's UIC; SYSTEM's Authorized
$! privileges, captured verbatim off the real reference lab's own SHOW
$! PROCESS/PRIVILEGES (docs/oracle/vax73-privileges.md §4, "User: SYSTEM"),
$! are the full 39-entry grid -- i.e. ALL. JOB_CONTROL is the process that
$! $CREPRCs every interactive LOGINOUT, and LOGINOUT must read the
$! World-denied SYSUAF.DAT (SYSPRV/BYPASS at minimum) -- explicit here
$! rather than leaning on inheritance from whatever $CREPRC'd this
$! procedure, and correct for x86_64/VAX/Alpha alike (this file is the
$! shared, arch-independent rootfs tree).
$!
$ SET NOON
$ RUN /DETACHED -
      /PROCESS_NAME=JOB_CONTROL -
      /UIC=[1,4] -
      /PRIVILEGES=(ALL) -
      /INPUT="/dev/console" /OUTPUT="/dev/console" /ERROR="/dev/console" -
      SYS$SYSTEM:JOB_CONTROL.EXE
$ EXIT
