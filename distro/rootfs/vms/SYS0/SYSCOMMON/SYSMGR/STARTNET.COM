$! STARTNET.COM - DECnet Phase IV startup for OVMX (rd vms-a70 direction B).
$!
$! On real OpenVMS, @SYS$MANAGER:STARTNET starts DECnet Phase IV -- it brings up
$! the network ancillary control process (NETACP), which drives the routing
$! datalink AND serves inbound connects to the Session Control OBJECTS the node
$! offers (CTERM = object 42, the target of a remote $ SET HOST). This procedure
$! is the OVMX equivalent: it starts SYS$SYSTEM:DECNETD.EXE detached as NETACP,
$! which self-sources its executor address from the node's DECnet configuration
$! and serves inbound $ SET HOST by default (an object-42 connect reaches
$! LOGINOUT on an executive-minted RTAn:; the remote user authenticates fresh).
$!
$! NAME (CLAUDE.md Rule 8): STARTNET.COM is the observable public name of the
$! Phase IV DECnet startup procedure; its INTERNAL byte-for-byte content was
$! never read from a VSI original and is not reproduced -- this is OVMX's own
$! procedure, structured like SYS$STARTUP:TCPIP$STARTUP.COM (its sibling network
$! layered product), which it deliberately mirrors.
$!
$! REGISTERED at the LPBETA phase in SYS$STARTUP:VMS$VMS.DAT (the STDRV driver
$! runs it there -- LPBETA is after LPMAIN's INSTALL ADDs, which NETACP's
$! shareable dependencies need, and before END's console login); it can also be
$! @-invoked by hand after NCP configures the executor address.
$!
$! GATE (INV-6, CLAUDE.md Rule 9 -- the honest-.COM discipline, vms-297): DECnet
$! is started ONLY when an executor address is configured, exactly as real VMS
$! starts nothing until NETCONFIG/NCP DEFINE EXECUTOR ADDRESS has run. There is
$! NO unconditional "success" here: DECNETD --show-executor self-sources the
$! configured address and exits SUCCESS only when one exists, else
$! %DECNETD-E-NOADDRESS, and this procedure branches on $SEVERITY. A node with no
$! DECnet address simply does not start NETACP, and says so.
$!
$! INV-6 (executive-backed, no facade): NETACP opens its 0x6003 datalink over the
$! executive (VMS_IOCTL_L2_OPEN on the primary NIC that _NET: rides). With no
$! executive or no NIC, DECNETD.EXE fails honestly (a genuine %DECNETD-E-NOSOCKET
$! in NETACP.LOG) -- it never fabricates a per-process listener that serves
$! nothing.
$!
$! INVOCATION:
$!   @SYS$MANAGER:STARTNET
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$ SAY ""
$ SAY "        OVMX DECnet Phase IV -- Startup (STARTNET)"
$ SAY ""
$!
$ DECNETD := $SYS$SYSTEM:DECNETD.EXE
$!
$! Is a DECnet executor address configured on this node? NCP SET/DEFINE EXECUTOR
$! ADDRESS writes it; DECNETD --show-executor SELF-SOURCES it (opening no socket,
$! needing no privilege) and reports the executor/circuit summary. Its exit
$! status is the gate: SUCCESS iff an address is configured (vms-a70).
$ DECNETD --show-executor
$ SEV = $SEVERITY
$ IF SEV .NE. 1 THEN GOTO no_decnet
$!
$ SAY ""
$ SAY "%DECNET-I-STARTNET, starting the network ancillary control process (NETACP)"
$! NETACP is a persistent master listener -- it must run as a DETACHED process,
$! NOT a foreground RUN that would BLOCK system startup forever. VMS RUN passes
$! the image NO argv (an image parameter only), so DECNETD self-sources its
$! executor address and serves inbound object 42 by default -- the same shape
$! TCPIP$STARTUP uses to start TCPIP$INETD, which reads its own service DB. The
$! EXECUTIVE assigns the process name NETACP as it creates the process, so SHOW
$! SYSTEM lists it and $GETJPI resolves it by name; a duplicate is refused to the
$! caller (%RUN-F-CREPRC / -SYSTEM-F-DUPLNAM), making a re-run idempotent-safe.
$! I/O goes to SYS$MANAGER:NETACP.LOG (never the console).
$ RUN /DETACHED -
       /PROCESS_NAME=NETACP -
       /INPUT=NL: /OUTPUT=SYS$MANAGER:NETACP.LOG -
       /ERROR=SYS$MANAGER:NETACP.LOG -
       SYS$SYSTEM:DECNETD.EXE
$ SAY "%DECNET-I-NETACPSTARTED, NETACP started detached -- inbound $ SET HOST served"
$ SAY ""
$ EXIT
$!
$ no_decnet:
$ SAY ""
$ SAY "%DECNET-I-NOTCONFIGURED, DECnet is not configured on this node -- NETACP not started"
$ SAY "  Configure it with:   $ MCR NCP SET EXECUTOR ADDRESS area.node"
$ SAY "  then re-run:          $ @SYS$MANAGER:STARTNET"
$ SAY ""
$ EXIT
