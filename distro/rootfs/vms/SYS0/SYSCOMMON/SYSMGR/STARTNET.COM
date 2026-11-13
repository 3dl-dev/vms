$! STARTNET.COM - DECnet Phase IV startup for OVMX (rd vms-a70 direction B).
$!
$! On real OpenVMS, @SYS$MANAGER:STARTNET starts DECnet Phase IV -- it brings up
$! the network ancillary control process (NETACP), which drives the routing
$! datalink AND serves inbound connects to the Session Control OBJECTS the node
$! offers (CTERM = object 42, the target of a remote $ SET HOST). This procedure
$! is the OVMX equivalent: NCP SET EXECUTOR STATE ON creates SYS$SYSTEM:NETACP.EXE
$! detached, which sources its executor address from the node's DECnet configuration
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
$! NEVER a foreground image probe -- a synchronous NETACP run whose "not
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
$! THROUGH THE EXECUTIVE: the booted NETACP.EXE is built with the executive L2
$! backend (src/libdatalink, OVMX_DATALINK_VIA_EXECUTIVE), so the kernel owns
$! the raw socket (VMS_IOCTL_L2_OPEN) and the open is gated on the VMS PHY_IO
$! privilege -- a booted VMS process holds no CAP_NET_RAW and needs none. NETACP
$! inherits PHY_IO from its creator (SYSTEM at boot or at the console: NCP's
$! $CREPRC passes no privilege mask, so the detached process gets the creator's
$! current privileges). It sources every frame from its Phase IV
$! algorithmic station address AA-00-04-00-<LE16(area*1024+node)>, which the
$! executive validates and stamps. With no NIC, or no PHY_IO, it fails honestly
$! (%NETACP-E-NOSOCKET ... SS$_NOSUCHDEV / SS$_NOPRIV in NETACP.LOG) -- it never
$! fabricates a per-process listener that serves nothing. The SESSION side is
$! executive-backed too (an inbound object-42 connect mints RTAn: and $CREPRCs
$! LOGINOUT through /dev/vms).
$!
$! INVOCATION:
$!   @SYS$MANAGER:STARTNET
$!
$ SET NOON
$!
$! GATE: DECnet is configured only if the local node database exists. If not,
$! STARTNET is a faithful SILENT no-op -- launch nothing, print nothing (real VMS
$! runs no STARTNET on an unconfigured node; the boot console stays pristine).
$! F$SEARCH is a pure DCL lexical -- no image runs, so no %DCL-E-ABORT.
$ IF F$SEARCH("SYS$SYSTEM:NETNODE_LOCAL.DAT") .EQS. "" THEN GOTO no_decnet
$!
$! THE CONSOLE (rd vms-158d). A real VMS node's console shows only what VMS
$! itself prints for this step: a "%RUN-S-PROC_ID, identification of created
$! process is ..." and then NETACP's OPCOM message "DECnet starting"
$! (tests/lab/captures/vms-52d-0b01-oracle-20261013/armA-vax1-console.log).
$! This procedure prints no banner and no launch line of its own: NETACP
$! reports "DECnet starting" to OPCOM ($SNDOPR, SYS$MANAGER:OPERATOR.LOG) once
$! its datalink is open. NCP's $CREPRC prints no PROC_ID line (the VAX's comes
$! from a RUN its startup does; the one-line difference is recorded on the S3
$! PR), and a refused launch is NCP's own %NCP-F-OPEFAI.
$! THE NETWORK STARTS WITH NCP (rd vms-0a7 S3): SET EXECUTOR STATE ON is what
$! turns the network on. NCP $CREPRCs NETACP -- SYS$SYSTEM:NETACP.EXE, DETACHED,
$! process name NETACP, /INPUT=NL: and SYS$OUTPUT = SYS$ERROR =
$! SYS$MANAGER:NETACP.LOG -- with NO parameters: NETACP reads its executor from
$! the executor database, its LAN unit from the executive, and serves inbound
$! object 42 and object 17 by default. It is a persistent master listener, so
$! it is detached and never blocks system startup. The EXECUTIVE assigns the
$! process name NETACP as it creates the process, so SHOW SYSTEM lists it; the
$! network already on (a NETACP of that name running) leaves STATE ON a no-op.
$! NCP runs with this procedure's (SYSTEM's) privileges, which NETACP inherits:
$! PHY_IO for its datalink. Per-process status (served / not-configured / a
$! NOSOCKET failure) goes to SYS$MANAGER:NETACP.LOG, never the console.
$ NCP := $SYS$SYSTEM:NCP.EXE
$ NCP SET EXECUTOR STATE ON
$! HONEST LAUNCH REPORT (rd vms-1f69): NETACP announces "DECnet starting" to
$! OPCOM only once it is really running. A refused $CREPRC is NCP's
$! %NCP-F-OPEFAI with the system's reason (e.g. -SYSTEM-F-NOPRIV) and NOTHING
$! claims NETACP is running. (Copy $STATUS first: a later command resets it.)
$ run_status = $STATUS
$ IF .NOT. run_status THEN GOTO run_failed
$ EXIT
$!
$ run_failed:
$! NCP has already said why, in its own words (%NCP-F-OPEFAI); the status goes
$! back to the caller unchanged and nothing claims NETACP is running.
$ EXIT run_status
$!
$ no_decnet:
$! DECnet not configured -- nothing to do, and nothing on the console.
$ EXIT
