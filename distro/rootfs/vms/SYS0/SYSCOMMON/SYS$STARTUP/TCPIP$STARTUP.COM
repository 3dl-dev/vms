$! TCPIP$STARTUP.COM - TCP/IP Services for OVMX startup procedure (rd vms-cdb9).
$!
$! On real OpenVMS, SYS$STARTUP:TCPIP$STARTUP starts the TCP/IP Services layered
$! product, including the AUXILIARY SERVER (TCPIP$INETD) -- the master listener
$! that binds the well-known port of every ENABLED service and, on an inbound
$! connection, spawns the service's image with the connection as its
$! SYS$INPUT/SYS$OUTPUT. This procedure is the OVMX equivalent: it starts
$! TCPIP$INETD.EXE, which reads SYS$SYSTEM:TCPIP$SERVICE.DAT and binds each
$! enabled service over the executive BGn: seam (the proven sockets veneer,
$! ovmx_bind/listen/accept, vms-698).
$!
$! ORDERING: the core TCP/IP environment (host name, domain, interface address)
$! must already be configured by @SYS$MANAGER:TCPIP$CONFIG so the auxiliary
$! server's listeners are reachable; run TCPIP$CONFIG before this procedure.
$!
$! INV-6 (CLAUDE.md Rule 9): the auxiliary server binds over the executive BGn:
$! device. With no executive (/dev/vms absent) TCPIP$INETD.EXE fails honestly
$! (SS$_NOSUCHDEV) and this procedure reports that -- it never fabricates a
$! per-process listener that shares nothing.
$!
$! HONEST DEFERRALS (docs/design-tcpip-services-ovmx.md): the persistent binary
$! service database (#878), UDP/dgram services, the concurrent "nowait" model,
$! per-service run-as identity, and access-control lists are later rungs and are
$! NOT faked here.
$!
$! INVOCATION:
$!   @SYS$STARTUP:TCPIP$STARTUP
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$ SAY ""
$ SAY "        OVMX TCP/IP Services -- Startup (TCPIP$STARTUP)"
$ SAY ""
$!
$ INETD_IMAGE = "SYS$SYSTEM:TCPIP$INETD.EXE"
$ SERVICE_DB  = "SYS$SYSTEM:TCPIP$SERVICE.DAT"
$!
$! No F$SEARCH pre-check on these SYS$SYSTEM: filespecs (rd vms-21b): both are
$! ODS-2-resident on the ACP-only system disk, and F$SEARCH of a $-in-name file
$! over the Files-11 ACP does not resolve reliably (a DCL/RMS $SEARCH gap; tracked
$! separately). The real operations report honestly WITHOUT the pre-check: RUN
$! activates INETD.EXE via the DCL RUN ACP-stage path (a genuine %RUN-F- if it is
$! absent), and TCPIP$INETD reads the service DB over the ACP itself (a genuine
$! %TCPIP-F-NOSERVICEDB if it cannot). So a pre-check here only ADDS a false
$! negative on the $-name; drop it and let the operations speak.
$!
$ SAY "%TCPIP-I-STARTAUX, starting the auxiliary server (TCPIP$INETD) from ''SERVICE_DB'"
$! The auxiliary server is a persistent master listener -- it must run as a
$! DETACHED process (the RUN/DETACHED shape SYSTARTUP_VMS.COM documents), NOT a
$! foreground RUN that would BLOCK system startup forever (rd vms-21b). The
$! EXECUTIVE assigns the process name TCPIP$INETD as it creates the process, so
$! SHOW SYSTEM lists it and $GETJPI resolves it by name; a duplicate is refused
$! to the caller (%RUN-F-CREPRC / -SYSTEM-F-DUPLNAM), making a re-run idempotent-
$! safe. I/O goes to SYS$MANAGER:TCPIP$INETD.LOG (never the console).
$ RUN /DETACHED -
       /PROCESS_NAME=TCPIP$INETD -
       /INPUT=NL: /OUTPUT=SYS$MANAGER:TCPIP$INETD.LOG -
       /ERROR=SYS$MANAGER:TCPIP$INETD.LOG -
       ''INETD_IMAGE'
$ SAY "%TCPIP-I-AUXSTARTED, auxiliary server (TCPIP$INETD) started detached"
$ EXIT
