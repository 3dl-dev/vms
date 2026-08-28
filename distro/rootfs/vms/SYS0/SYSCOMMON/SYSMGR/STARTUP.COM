$! STARTUP.COM - OVMX System Startup Command Procedure (STDRV)
$!
$! Runs after kernel modules and system disk mount. There is no logical
$! name daemon to wait for: on VMS the logical name tables are
$! executive-resident, not a service (vms-a4b) -- see the NOTE ON
$! SERVICES in src/ovmx_init/ovmx_init.c.
$! Edits to this file on the system disk persist across reboots.
$!
$! WHAT RUNS THIS PROCEDURE, AND UNDER WHOSE IDENTITY (vms-9b7).
$!
$! PID 1 (STARTUP.EXE) execs SYS$SYSTEM:PROVISION.EXE, which:
$!   1. reads SYSUAF's SYSTEM record -- through libvms's ONE reader;
$!   2. asks the EXECUTIVE to stamp that identity on itself, and halts the
$!      boot if there is no SYSTEM record or the executive refuses;
$!   3. provisions the accounts' home directories and the system tree's
$!      ownership;
$!   4. execs DCL.EXE on THIS FILE, in the SAME process.
$!
$! So this procedure -- and everything it invokes below -- runs under
$! SYSTEM's identity as the executive recorded it, which is what OpenVMS
$! does: STARTUP runs under username SYSTEM.
$!
$! PID 1 DOES NOT READ SYSUAF.DAT and must not start. It is statically linked,
$! so when it did, it carried two hand-rolled 512-byte parsers of its own --
$! two of five independent parsers of one format, whose disagreement over the
$! line limit could leave the system with no readable SYSTEM record and no
$! boot. Account provisioning and system identity are system management, and
$! system management belongs on this path.
$!
$! --------------------------------------------------------------------
$! THIS PROCEDURE IS STDRV -- A PHASED STARTUP DRIVER (vms-21a,
$! docs/design-boot-faithful.md sec3.2/3.3), not a flat script. It:
$!
$!   1. defines SYS$STARTUP (below) and the other system-wide logicals;
$!   2. reads the nine startup PHASES from SYS$STARTUP:VMS$PHASES.DAT
$!      (OVMX-authored; the phase NAMES are OBSERVED OpenVMS Alpha V8.4
$!      boot behavior, CLAUDE.md Rule 8 -- a data file naming phases was
$!      read, no VSI procedure content was);
$!   3. for each phase, in order, runs every component procedure the
$!      OVMX-DEFINED database SYS$STARTUP:VMS$VMS.DAT registers for it
$!      (RUN_COMPONENTS, below -- only the FILE NAME is observed; the line
$!      format inside it is an OVMX invention, labeled at its own point of
$!      definition in ovmx_layout.h). A phase with nothing registered runs
$!      empty and prints nothing -- that is honest, not a bug;
$!   4. at the CONFIG phase, additionally runs SYS$MANAGER:SYCONFIG.COM;
$!      at BASEENVIRON, SYS$MANAGER:SYLOGICALS.COM; at LPMAIN,
$!      SYS$MANAGER:SYSTARTUP_VMS.COM -- the three documented OpenVMS
$!      site-customization files (VSI OpenVMS System Manager's Manual,
$!      "Customizing Startup with Site-Specific Files"). This phase
$!      assignment is this project's own reading of that public
$!      documentation, not an oracle capture -- flagged here rather than
$!      silently presented as measured.
$!
$! This REPLACES SYS$MANAGER:OVMX.CONF and SYLOGICALS.CONF (both deleted,
$! vms-21a, docs/design-boot-faithful.md sec2.4): a Unix KEY=VALUE config
$! file is not how VMS configures a boot. NODE_NAME lives in the SYSGEN
$! parameter SCSNODE, already read at boot by read_boot_parameters()
$! (vms-b6a7); VERSION_LIMIT and DEFAULT_PROTECTION are SYSGEN parameters /
$! SET commands a site now places in SYCONFIG.COM, once OVMX has a SYSGEN
$! parameter or SET command for them -- neither exists yet, so neither
$! config key had anywhere honest to go and both are simply gone, not
$! ported (OVMX.CONF had ZERO code readers on main; verified again here).
$!
$ SET NOON
$!
$! (vms-dec) No "SHOW TIME" here: it emitted a stray bare date line in the
$! middle of the boot, and the boot identification banner already prints the
$! date. Real VMS site-startup has no SHOW TIME, and the oracle boot capture
$! (docs/design-boot-faithful.md sec3.5) shows none.
$!
$! Define system-wide logical names.
$!
$! SYS$STARTUP is the measured SEARCH LIST (docs/design-boot-faithful.md
$! sec3.2): "SYS$SYSROOT:[SYS$STARTUP]" then "SYS$MANAGER" -- a multi-value
$! logical, exactly what vms-420 built DEFINE/SYSTEM support for. OVMX does
$! not model SYS$SYSROOT as reaching SYSCOMMON on its own (lnm_defaults.c's
$! lnm_setup_defaults() seeds SYS$MANAGER the same flattened way, straight
$! off SYS$SYSDEVICE, rather than via SYS$SYSROOT:[SYSMGR]), so the first
$! element here is OVMX's flattened equivalent of the real directory
$! (ovmx_layout.h VMS_SYS_STARTUP) rather than the oracle's literal
$! "SYS$SYSROOT:[SYS$STARTUP]" text -- SHOW LOGICAL SYS$STARTUP will show
$! the OVMX path, not the VSI one, and that divergence is intentional and
$! documented here rather than silent.
$ DEFINE/SYSTEM SYS$STARTUP SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYS$STARTUP],SYS$MANAGER
$ DEFINE/SYSTEM SYS$LOGIN SYS$MANAGER
$!
$! SYS$UPDATE -- the standard VMS home for pre-PCSI layered-product install
$! kits and their SETUP.COM procedures (e.g. SYS$UPDATE:PARTS_SETUP.COM,
$! vms-977/vms-2579). It is NO LONGER (re)defined here: lnm_setup_defaults()
$! (src/vmslnm/lnm_defaults.c) now seeds SYS$UPDATE = SYS$SYSROOT:[SYSUPD],
$! COMPOSED onto the concealed rooted SYS$SYSROOT search list exactly like
$! SYS$MANAGER/SYS$SYSTEM. The old flatten override here --
$! "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]" -- was a stale pre-SYS$SYSROOT
$! workaround that DEFEATED that composition, making SHOW LOGICAL and the
$! DIRECTORY header render the flattened physical path instead of the
$! authentic "[SYSUPD]". Dropped so SYS$UPDATE resolves the same rooted way
$! as every other SYS$xxx directory logical (vms-9344).
$!
$! Login banners are NOT set here -- LOGINOUT reads SYS$ANNOUNCE and
$! SYS$WELCOME, which a manager defines in SYS$MANAGER:SYLOGICALS.COM (run
$! at the BASEENVIRON phase, below). The startup banner itself is
$! displayed by STARTUP.EXE from the system identity module, so there is
$! exactly one place that knows the version.
$!
$! Run the phase driver. Every startup phase after this line -- component
$! procedures and the three site files -- happens inside RUN_PHASES.
$ GOSUB RUN_PHASES
$!
$ EXIT
$!
$! ======================================================================
$! RUN_PHASES - STDRV's phase loop.
$!
$! Reads SYS$STARTUP:VMS$PHASES.DAT one phase name per line (READ signals
$! end-of-file as SS$_ENDOFFILE, decimal 2160 -- checked via $STATUS since
$! this DCL has no READ/END_OF_FILE= qualifier). For each phase: run its
$! registered components (RUN_COMPONENTS), then the one site file (if any)
$! documented for that phase.
$! ======================================================================
$ RUN_PHASES:
$!  FIX C (vms-af26, boot-speed): read the component database SYS$STARTUP:
$!  VMS$VMS.DAT ONCE into DCL symbols here, before the phase loop, instead of
$!  re-OPEN/READ-ing it inside RUN_COMPONENTS for every one of the nine phases
$!  (nine full opens+scans of the same file per boot). LOAD_COMPONENTS caches
$!  each registration line's phase name and procedure filespec into the indexed
$!  symbols COMPONENT_PHASE_n / COMPONENT_PROC_n (n = 1..COMPONENT_COUNT);
$!  RUN_COMPONENTS then walks those symbols. Behaviour is identical -- the same
$!  procedures run for the same phases, in file order -- only the redundant
$!  re-reads are gone.
$   GOSUB LOAD_COMPONENTS
$   OPEN/READ PHASE_FILE SYS$STARTUP:VMS$PHASES.DAT
$ PHASE_LOOP:
$   READ PHASE_FILE PHASE_NAME
$   IF $STATUS .EQ. 2160 THEN GOTO PHASE_LOOP_DONE
$   GOSUB RUN_COMPONENTS
$   IF PHASE_NAME .EQS. "CONFIG" THEN @SYS$MANAGER:SYCONFIG.COM
$   IF PHASE_NAME .EQS. "BASEENVIRON" THEN @SYS$MANAGER:SYLOGICALS.COM
$   IF PHASE_NAME .EQS. "LPMAIN" THEN GOSUB RUN_SITE_STARTUP
$   GOTO PHASE_LOOP
$ PHASE_LOOP_DONE:
$   CLOSE PHASE_FILE
$   RETURN
$!
$! RUN_SITE_STARTUP - the LPMAIN-phase site-specific commands (the role
$! SYSTARTUP_VMS.COM already had before vms-21a; unchanged here, only its
$! call site moved into the phase driver). This subroutine is the SINGLE
$! owner of the "executing the site-specific startup commands" announcement:
$! SYSTARTUP_VMS.COM (both the installed and the distribution-media variants)
$! used to WRITE the same line too, so a real boot printed it TWICE -- vms-3ab8
$! removed those duplicate WRITEs, leaving this one. The tests that assert the
$! line appears (tests/qemu/test_persistent_boot.sh, test_startup_phase_
$! driver.sh, test_job_control_*.sh) still find it: it is still printed here,
$! exactly once.
$!
$! NOT ORACLE-PINNED. Whether this wording matches, should match, or should
$! differ from what a real OpenVMS boot prints is UNVERIFIED IN EITHER
$! DIRECTION -- no reference-lab capture of the line exists (checked at vms-21a
$! round 6). Pinning it to the oracle or to public OpenVMS documentation is
$! open work; it is neither claimed nor denied here (CLAUDE.md Rule 10: an
$! authenticity claim about VMS message text is either oracle-pinned with the
$! capture recorded, or it does not appear).
$ RUN_SITE_STARTUP:
$   WRITE SYS$OUTPUT "The OVMX system is now executing the site-specific startup commands."
$   @SYS$MANAGER:SYSTARTUP_VMS.COM
$   RETURN
$!
$! LOAD_COMPONENTS - read SYS$STARTUP:VMS$VMS.DAT ONCE (FIX C, vms-af26) and
$! cache each registration into COMPONENT_PHASE_n / COMPONENT_PROC_n. Called
$! once from RUN_PHASES before the phase loop. See SYS$STARTUP:VMS$VMS.DAT's
$! own header for the OVMX-defined line format (Rule 8: this format is OVMX's
$! invention, not VMS-authentic). The same skip rules RUN_COMPONENTS applied
$! per phase are applied once here: blank lines and lines beginning with "!"
$! are ignored, so only real registrations are cached.
$ LOAD_COMPONENTS:
$   COMPONENT_COUNT = 0
$   OPEN/READ COMPONENT_FILE SYS$STARTUP:VMS$VMS.DAT
$ LOAD_LOOP:
$   READ COMPONENT_FILE COMPONENT_LINE
$   IF $STATUS .EQ. 2160 THEN GOTO LOAD_LOOP_DONE
$   IF COMPONENT_LINE .EQS. "" THEN GOTO LOAD_LOOP
$   COMPONENT_FIRST_CHAR = F$EXTRACT(0,1,COMPONENT_LINE)
$   IF COMPONENT_FIRST_CHAR .EQS. "!" THEN GOTO LOAD_LOOP
$   COMPONENT_COUNT = COMPONENT_COUNT + 1
$   COMPONENT_PHASE_'COMPONENT_COUNT' = F$ELEMENT(0," ",'COMPONENT_LINE')
$   COMPONENT_PROC_'COMPONENT_COUNT' = F$ELEMENT(1," ",'COMPONENT_LINE')
$   GOTO LOAD_LOOP
$ LOAD_LOOP_DONE:
$   CLOSE COMPONENT_FILE
$   RETURN
$!
$! RUN_COMPONENTS - run every component registered for the CURRENT phase
$! (PHASE_NAME, set by RUN_PHASES above). Walks the symbols LOAD_COMPONENTS
$! cached from SYS$STARTUP:VMS$VMS.DAT -- no file re-open (FIX C, vms-af26).
$! A phase with no matching registration runs nothing and prints nothing --
$! an empty phase is honest, not a shortcut.
$ RUN_COMPONENTS:
$   COMPONENT_I = 0
$ COMPONENT_LOOP:
$   COMPONENT_I = COMPONENT_I + 1
$   IF COMPONENT_I .GT. COMPONENT_COUNT THEN GOTO COMPONENT_LOOP_DONE
$   IF COMPONENT_PHASE_'COMPONENT_I' .NES. PHASE_NAME THEN GOTO COMPONENT_LOOP
$   COMPONENT_PROC = COMPONENT_PROC_'COMPONENT_I'
$   IF COMPONENT_PROC .NES. "" THEN @'COMPONENT_PROC'
$   GOTO COMPONENT_LOOP
$ COMPONENT_LOOP_DONE:
$   RETURN
