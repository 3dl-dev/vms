$! SYSTARTUP_VMS.COM - Site-specific system startup
$!
$! Invoked by SYS$MANAGER:STARTUP.COM at the end of system startup.
$! This is where a system manager starts site services. Edits to this file
$! on the system disk persist across reboots.
$!
$! Each service is started by its own startup procedure, and each of those
$! creates the service as a DETACHED process (RUN/DETACHED). STARTUP.EXE
$! (PID 1) starts no services itself -- it runs STARTUP.COM and nothing
$! more. To add a service, add its startup procedure here.
$!
$ SET NOON
$!
$! --- SSH remote access ---
$! Comment out the next line to disable the SSH service.
$ @SYS$STARTUP:TCPIP$SSH_STARTUP.COM
$!
$ EXIT
