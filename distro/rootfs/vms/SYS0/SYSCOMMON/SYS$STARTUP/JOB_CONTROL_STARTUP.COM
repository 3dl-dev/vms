$! JOB_CONTROL_STARTUP.COM - creates JOB_CONTROL, the console-session process
$!
$! Invoked from SYS$MANAGER:SYSTARTUP_VMS.COM (vms-8d2, docs/design-init-
$! scope.md §2/§5, docs/design-boot-faithful.md §4.5). This is the FIRST
$! real worked example of the "HOW TO START ONE" mechanism SYSTARTUP_VMS.COM
$! documents (vms-47b): a DETACHED process with a VMS process name, so
$! SHOW SYSTEM lists it and $GETJPI resolves it by name from any other
$! process on the node.
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
$ SET NOON
$ RUN /DETACHED -
      /PROCESS_NAME=JOB_CONTROL -
      /INPUT="/dev/console" /OUTPUT="/dev/console" /ERROR="/dev/console" -
      SYS$SYSTEM:JOB_CONTROL.EXE
$ EXIT
