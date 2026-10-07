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
$! THE GATE (INV-6, CLAUDE.md Rule 9 -- honest .COM, vms-297). Real OpenVMS runs
$! NO DECnet at boot until the node has been configured: SYSTARTUP invokes STARTNET
$! only when the Phase IV databases exist. OVMX matches that faithfully -- if the
$! local node database (SYS$SYSTEM:NETNODE_LOCAL.DAT, written by NCP SET/DEFINE
$! EXECUTOR) is absent, DECnet is not configured, so STARTNET is a SILENT no-op:
$! it launches NO NETACP and prints NOTHING to the boot console (an unconfigured
$! node running no DECnet is not an error, and must not pollute the pristine boot
$! console the vms-1fb oracle pins). The check is a pure DCL lexical (F$SEARCH),
$! NEVER a foreground image probe -- a synchronous DECNETD run whose "not
$! configured" exit DCL would render as %DCL-E-ABORT is exactly the pollution this
$! avoids (vms-a70). NETACP ALSO self-checks (defense in depth): launched detached
$! with no argv it self-sources the executor address and, finding none, logs a
$! not-configured note to SYS$MANAGER:NETACP.LOG and exits cleanly -- so even a
$! stray launch serves nothing and fabricates nothing. The configured branch is
$! armed the VMS way (rd vms-1f69): NCP SET/DEFINE EXECUTOR writes
$! SYS$SYSTEM:NETNODE_LOCAL.DAT through RMS over the Files-11 ACP, so this gate
$! sees it -- on the next boot, or when the operator runs @SYS$MANAGER:STARTNET
$! by hand after configuring:
$!     $ NCP :== $SYS$SYSTEM:NCP.EXE
$!     $ NCP SET EXECUTOR ADDRESS 1.42
$!     $ NCP SET EXECUTOR NAME OVMX
$!     $ NCP SET EXECUTOR STATE ON
$!     $ @SYS$MANAGER:STARTNET
$! (the record layout inside NETNODE_LOCAL.DAT is OVMX's documented text, not
$! the VMS binary format -- labelled in the file itself).
$!
$! INV-6 (fail-honest, no facade): NETACP opens its 0x6003 datalink on the
$! primary NIC (auto-detected -- the same NIC the executive's _NET: rides)
$! THROUGH THE EXECUTIVE: the booted DECNETD.EXE is built with the executive L2
$! backend (src/libdatalink, OVMX_DATALINK_VIA_EXECUTIVE), so the kernel owns
$! the raw socket (VMS_IOCTL_L2_OPEN) and the open is gated on the VMS PHY_IO
$! privilege -- a booted VMS process holds no CAP_NET_RAW and needs none. NETACP
$! inherits PHY_IO from its creator (SYSTEM at boot or at the console: RUN
$! /DETACHED with no /PRIVILEGES gives the detached process the creator's current
$! privileges, sys$creprc). It sources every frame from its Phase IV
$! algorithmic station address AA-00-04-00-<LE16(area*1024+node)>, which the
$! executive validates and stamps. With no NIC, or no PHY_IO, it fails honestly
$! (%DECNETD-E-NOSOCKET ... SS$_NOSUCHDEV / SS$_NOPRIV in NETACP.LOG) -- it never
$! fabricates a per-process listener that serves nothing. The SESSION side is
$! executive-backed too (an inbound object-42 connect mints RTAn: and $CREPRCs
$! LOGINOUT through /dev/vms).
$!
$! INVOCATION:
$!   @SYS$MANAGER:STARTNET
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$!
$! GATE: DECnet is configured only if the local node database exists. If not,
$! STARTNET is a faithful SILENT no-op -- launch nothing, print nothing (real VMS
$! runs no STARTNET on an unconfigured node; the boot console stays pristine).
$! F$SEARCH is a pure DCL lexical -- no image runs, so no %DCL-E-ABORT.
$ IF F$SEARCH("SYS$SYSTEM:NETNODE_LOCAL.DAT") .EQS. "" THEN GOTO no_decnet
$!
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
$! a re-run idempotent-safe. Per-process status (served / not-configured / a
$! NOSOCKET failure) goes to SYS$MANAGER:NETACP.LOG, never the console.
$ RUN /DETACHED -
       /PROCESS_NAME=NETACP -
       /INPUT=NL: /OUTPUT=SYS$MANAGER:NETACP.LOG -
       /ERROR=SYS$MANAGER:NETACP.LOG -
       SYS$SYSTEM:DECNETD.EXE
$! HONEST LAUNCH REPORT (rd vms-1f69): the announcement is made only when RUN
$! /DETACHED actually created the process. A refused $CREPRC (%RUN-F-CREPRC,
$! e.g. -SYSTEM-F-NOPRIV / -DUPLNAM) is reported with its status and NOTHING
$! claims NETACP is running. (Copy $STATUS first: a later command resets it.)
$ run_status = $STATUS
$ IF .NOT. run_status THEN GOTO run_failed
$ SAY "%DECNET-I-STARTNET, NETACP launched detached to serve inbound $ SET HOST."
$ SAY ""
$ EXIT
$!
$ run_failed:
$ SAY "%DECNET-E-STARTNET, NETACP was NOT started (RUN/DETACHED status " + F$STRING(run_status) + ")"
$ SAY ""
$ EXIT run_status
$!
$ no_decnet:
$! DECnet not configured -- nothing to do, and nothing on the console.
$ EXIT
