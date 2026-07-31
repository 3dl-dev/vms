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
$! ORACLE-PINNED, with one deliberate substitution. On the reference lab
$! (VAX2, OpenVMS VAX V7.3, 2026-07-31) a booting system announces site
$! startup with exactly:
$!
$!     %STDRV-I-STARTUP, OpenVMS startup begun at 30-JUL-2026 21:02:32.56
$!     The OpenVMS VAX system is now executing the site-specific startup commands.
$!
$! OVMX prints the second of those two lines with the product name
$! changed -- OVMX is not OpenVMS and must never present itself as
$! OpenVMS (INV-0). The %STDRV-I- line is NOT reproduced: STDRV is the
$! VMS startup driver, an image OVMX does not have, and a facility
$! prefix borrowed from a component that does not exist would be a
$! claim about OVMX that is not true. OVMX also prints this line from
$! the site procedure rather than from its caller, because the point of
$! the line here is to prove THIS procedure was invoked.
$!
$ WRITE SYS$OUTPUT "The OVMX system is now executing the site-specific startup commands."
$!
$ EXIT
