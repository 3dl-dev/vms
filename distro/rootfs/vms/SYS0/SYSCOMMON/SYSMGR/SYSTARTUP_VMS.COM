$! SYSTARTUP_VMS.COM - Site-specific system startup command procedure
$!
$! Invoked by SYS$MANAGER:STARTUP.COM at the end of system startup, after
$! the system-wide logical names are defined. This is where a system
$! manager starts the services this node should run. Edits on the system
$! disk persist across reboots.
$!
$! THIS PROCEDURE STARTS NO SERVICE, AND THAT IS DELIBERATE.
$!
$! OVMX ships no service image yet. A worked example here would have to
$! name an image that is never built, which would fail on every boot and
$! teach the reader that the failure is normal. So the mechanism is
$! documented and left unused rather than demonstrated against a service
$! that does not exist.
$!
$! HOW TO START ONE. A service is a DETACHED process with a VMS process
$! name, created by its own startup procedure:
$!
$!     $ @SYS$STARTUP:<service>_STARTUP.COM
$!
$! and that procedure runs:
$!
$!     $ RUN /DETACHED -
$!           /PROCESS_NAME=<name> -
$!           /INPUT=NL: /OUTPUT=SYS$MANAGER:<name>.LOG -
$!           SYS$SYSTEM:<image>.EXE
$!
$! The name is assigned by the EXECUTIVE as the process is created, not
$! claimed by the process afterwards, so SHOW SYSTEM lists the service
$! and $GETJPI resolves it by name from any process on the node. A
$! duplicate name in the same UIC group is refused to the caller with
$!
$!     %RUN-F-CREPRC, process creation failed
$!     -SYSTEM-F-DUPLNAM, duplicate name
$!
$! Do NOT start a service from STARTUP.EXE (PID 1). It has no way to name
$! the process, no way for a manager to disable it, and its missing-image
$! check was silent -- see the NOTE ON SERVICES in src/ovmx_init/ovmx_init.c.
$!
$ SET NOON
$!
$! ANNOUNCE THAT THIS PROCEDURE RAN, so that "the boot reached site
$! startup" is an ASSERTION and not an inference from the absence of an
$! error. tests/qemu/test_persistent_boot.sh greps the boot console for
$! the line below; without it the boot test could only observe that
$! STARTUP.COM did not complain, which is the absence-of-evidence shape
$! this project keeps getting burned by.
$!
$! NOT ORACLE-PINNED. An earlier version of this comment claimed the text
$! below matched a reference-lab capture of a real OpenVMS boot; no such
$! capture exists in ~/vax/cluster/captures, so that claim was unverifiable
$! and has been removed rather than repeated (CLAUDE.md Rule 10 -- do not
$! self-certify VMS message text). This line is OVMX's own announcement,
$! not a reproduction of any OpenVMS message text, and makes no claim to
$! be one. Pinning it to the oracle or to public OpenVMS documentation is
$! open work, not done here.
$!
$ WRITE SYS$OUTPUT "The OVMX system is now executing the site-specific startup commands."
$!
$ EXIT
