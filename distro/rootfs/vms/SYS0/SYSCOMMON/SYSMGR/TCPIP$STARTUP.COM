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
$ IF F$SEARCH(SERVICE_DB) .EQS. ""
$ THEN
$     SAY "%TCPIP-W-NOSERVICEDB, service database ''SERVICE_DB' not found -- no services started"
$     EXIT
$ ENDIF
$!
$ IF F$SEARCH(INETD_IMAGE) .EQS. ""
$ THEN
$     SAY "%TCPIP-W-NOINETD, auxiliary server image ''INETD_IMAGE' not present -- TCP/IP services not started"
$     SAY "                  (build tools/vms_tcpip_inetd -> TCPIP$INETD.EXE and stage it in SYS$SYSTEM)"
$     EXIT
$ ENDIF
$!
$ SAY "%TCPIP-I-STARTAUX, starting the auxiliary server (TCPIP$INETD) from ''SERVICE_DB'"
$ RUN ''INETD_IMAGE'
$ SAY "%TCPIP-I-AUXDONE, auxiliary server exited"
$ EXIT
