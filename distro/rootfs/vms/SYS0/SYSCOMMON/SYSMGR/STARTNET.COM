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
$! THE GATE lives in NETACP, not here (INV-6, CLAUDE.md Rule 9 -- honest .COM,
$! vms-297). DECnet is served ONLY when an executor address is configured, exactly
$! as real VMS runs no DECnet until NCP DEFINE EXECUTOR ADDRESS has. This procedure
$! does not FOREGROUND-probe the config: a synchronous image whose "not configured"
$! exit DCL would announce as %DCL-E-ABORT would pollute the boot console (and it
$! is not an error). Instead NETACP self-checks: launched detached with no argv, it
$! self-sources the executor address and, finding none, logs %DECNETD-I-NOCONFIG to
$! SYS$MANAGER:NETACP.LOG and exits cleanly -- it does NOT serve, and nothing is
$! fabricated. With an address configured it serves inbound object 42.
$!
$! INV-6 (fail-honest, no facade): NETACP opens its 0x6003 datalink as a raw
$! AF_PACKET socket on the primary NIC (auto-detected -- the same NIC the
$! executive's _NET: rides; src/libdatalink/ovmx_datalink.c), so DECNETD.EXE
$! needs CAP_NET_RAW (it has it as a SYSTEM-booted detached process). With no
$! NIC / no privilege it fails honestly (%DECNETD-E-NOSOCKET in NETACP.LOG) --
$! it never fabricates a per-process listener that serves nothing. The SESSION
$! side IS executive-backed (an inbound object-42 connect mints RTAn: and
$! $CREPRCs LOGINOUT through /dev/vms); moving DECnet's L2 into the executive
$! (as the cluster wire did, vms_pe.c) is a later rung under vms-30e, not here.
$!
$! INVOCATION:
$!   @SYS$MANAGER:STARTNET
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$ SAY ""
$ SAY "        OVMX DECnet Phase IV -- Startup (STARTNET)"
$ SAY ""
$! NETACP is a persistent master listener -- it must run as a DETACHED process,
$! NOT a foreground RUN that would BLOCK system startup forever. VMS RUN passes
$! the image NO argv (an image parameter only), so DECNETD self-sources its
$! executor address, auto-detects its NIC, and serves inbound object 42 by
$! default -- the same shape TCPIP$STARTUP uses to start TCPIP$INETD, which reads
$! its own service DB. The EXECUTIVE assigns the process name NETACP as it creates
$! the process, so SHOW SYSTEM lists it and $GETJPI resolves it by name; a
$! duplicate is refused to the caller (%RUN-F-CREPRC / -SYSTEM-F-DUPLNAM), making
$! a re-run idempotent-safe. Status (served / %DECNETD-I-NOCONFIG / a NOSOCKET
$! failure) goes to SYS$MANAGER:NETACP.LOG, never the console.
$ RUN /DETACHED -
       /PROCESS_NAME=NETACP -
       /INPUT=NL: /OUTPUT=SYS$MANAGER:NETACP.LOG -
       /ERROR=SYS$MANAGER:NETACP.LOG -
       SYS$SYSTEM:DECNETD.EXE
$ SAY "%DECNET-I-STARTNET, NETACP launched detached -- it serves inbound $ SET HOST"
$ SAY "  when a DECnet executor address is configured (NCP SET EXECUTOR ADDRESS);"
$ SAY "  otherwise it logs %DECNETD-I-NOCONFIG to SYS$MANAGER:NETACP.LOG and stops."
$ SAY ""
$ EXIT
