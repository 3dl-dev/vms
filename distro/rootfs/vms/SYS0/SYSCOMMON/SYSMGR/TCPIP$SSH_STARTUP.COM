$! TCPIP$SSH_STARTUP.COM - Start the OVMX SSH service
$!
$! Invoked by SYS$MANAGER:SYSTARTUP_VMS.COM during system startup.
$! Creates the SSH listener as a detached process named TCPIP$SSH.
$!
$! To disable the SSH service, comment out the line that invokes this
$! procedure in SYS$MANAGER:SYSTARTUP_VMS.COM. STARTUP.EXE does not start
$! this service and has no knowledge of it.
$!
$ SET NOON
$!
$! The service image is not guaranteed to be present: a system disk may
$! have been installed without it. Report that and stop -- do NOT continue
$! silently, or an operator sees a normal boot with no SSH.
$ IF F$SEARCH("SYS$SYSTEM:VMSSSHD.EXE") .EQS. "" THEN GOTO no_image
$!
$ RUN/DETACHED SYS$SYSTEM:VMSSSHD.EXE /PROCESS_NAME=TCPIP$SSH -
      /OUTPUT=SYS$MANAGER:TCPIP$SSH.LOG /ERROR=SYS$MANAGER:TCPIP$SSH.LOG
$ WRITE SYS$OUTPUT "%TCPIP$SSH-I-STARTUP, SSH service started as TCPIP$SSH"
$ EXIT
$!
$ no_image:
$ WRITE SYS$OUTPUT "%TCPIP$SSH-F-NOIMAGE, SYS$SYSTEM:VMSSSHD.EXE not found - SSH service not started"
$ EXIT
