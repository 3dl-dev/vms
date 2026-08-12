$! OVMX$INSTALL.COM - the OVMX faithful installation menu procedure (vms-dcf)
$!
$! WHY THIS NAME. The Alpha oracle's own install procedure name was never
$! observed on the console (the menu prints, it never names the .COM file
$! driving it), so there is nothing to pin an authentic VMS name to
$! (CLAUDE.md Rule 8) -- this is an OVMX-labeled name for an OVMX-authored
$! procedure, not a claimed-authentic VMS filename.
$!
$! PINNED TO: docs/design-vms-faithful-install.md sec2/sec3.3 and
$! docs/oracle/installation-media-vax73-alpha84.md sec3a (menu text,
$! question order, the INITIALIZE/PRESERVE/confirmation flow -- OpenVMS
$! Alpha V8.4, 2026-08-07 capture) and sec5 (first-boot behavior, vms-490,
$! 2026-08-12 capture). VAX (sec1-3) is history per sec4 -- OVMX is 64-bit,
$! Alpha is the model.
$!
$! MENU CONTENT: the oracle's 9-item Alpha menu is reproduced ONLY where
$! OVMX has the underlying facility -- an item OVMX cannot honestly offer
$! is OMITTED, never faked (CLAUDE.md Rule 10):
$!   1) kept  - PRODUCT INSTALL VMS exists (vms-df9).
$!   2) OMIT  - no layered-product catalog to display.
$!   3) OMIT  - no generic layered-product installer (only the one PARTS
$!              demo kit, staged at SYS$UPDATE: outside this menu).
$!   4) kept  - PRODUCT SHOW PRODUCT exists (vms-df9).
$!   5) OMIT  - no PRODUCT RECONFIGURE.
$!   6) OMIT  - no PRODUCT REMOVE.
$!   7) OMIT  - no patch/PAK/recovery-data facility.
$!   8) kept  - DCL is the shell already running this procedure; option 8
$!              just returns to it (see DO_DCL).
$!   9) kept  - SYSMAN SHUTDOWN NODE exists as a DCL path, though see the
$!              NOTE ON SHUTDOWN below -- it is a PRE-EXISTING facade
$!              (print-only, does not halt anything), a gap this item
$!              found but does not own or fix.
$! The item numbers themselves are the oracle's own numbers for the kept
$! items, not renumbered 1-4, so the menu stays legible against sec3a's
$! transcript instead of implying a parity the omissions don't have.
$!
$! FOUR PREREQUISITE GAPS FOUND WHILE BUILDING AND GROUND-SOURCING THIS
$! PROCEDURE, ALL FILED/REPORTED (not fixed here -- CONSTRAINTS: pure DCL,
$! no bash escape, and no silent workaround):
$!
$!   (a) INITIALIZE <device>: <label> (cmd_initialize, dcl_cmd_misc.c) does
$!       NOT resolve the VMS device name to its backing block device the
$!       way MOUNT does (vms_kif_disk_resolve) -- it forwards the LITERAL
$!       string "DKA100:" to INITIALIZE.EXE, which does not recognize VMS
$!       device syntax and (ground-sourced locally, host build):
$!           $ INITIALIZE DKA100: LABEL
$!       creates and formats a REGULAR FILE literally named "DKA100:" in
$!       the process's current directory and reports
$!       %INIT-I-COMPLETE -- a silent fake success on the real target
$!       (Rule 9's exact defect class), never touching the intended block
$!       device. The INITIALIZE branch below is written to the real oracle
$!       shape and will work once this is fixed; until then it cannot be
$!       proven end-to-end (see tests/qemu/test_install_menu.sh, which
$!       proves the PRESERVE branch instead -- PRESERVE is the oracle's
$!       own bracketed default).
$!
$!   (b) There is no SYS$INPUT-as-procedure-file mechanism for a RUN'd
$!       image (dcl_activate_image/cmd_run does not dup2 the child's stdin
$!       from a DEFINE'd SYS$INPUT or from the .COM file's own read
$!       position) -- the classic VMS idiom of feeding a utility's REPL
$!       (AUTHORIZE, SYSGEN) from lines inside the calling procedure does
$!       not work in OVMX. AUTHORIZE.EXE and SYSGEN.EXE are also pure
$!       REPLs (argc/argv ignored). Below, this procedure sets up the
$!       target and terminal state and then RUNs the utility interactively
$!       -- the operator (or, in the ground-source test, the console
$!       driver) types the utility's own commands at its own prompt. This
$!       is real DCL and a real utility, never faked, but it means the
$!       install session cannot fully automate those two steps yet.
$!
$!   (c) AUTHORIZE.EXE, forked via dcl_exec_utility(), never learns about a
$!       device MOUNTed by its DCL parent -- only SYSDISK is registered in
$!       its own per-process vmsfs device table at its own startup. The
$!       DEFINE/JOB SYS$SYSTEM redirect below DOES correctly retarget the
$!       LOGICAL (confirmed live: SHOW LOGICAL SYS$SYSTEM shows the target
$!       after the DEFINE), but AUTHORIZE's own file layer cannot then open
$!       anything through it -- it reports %UAF-E-NOSUCHUSER for an
$!       account that genuinely exists on the target.
$!
$!   (d) Independently of (c): plain RMS/POSIX file CREATE on a REAL
$!       vmsfs.ko-mounted volume fails outright -- ground-sourced with
$!       `CREATE DKA0:[SYSEXE]PROBE.TXT` on the DISTRIBUTION disk itself
$!       (no target, no redirection at all), which returns %RMS-E-CRE.
$!       SET PASSWORD (an in-process DCL builtin, so it does not hit gap
$!       (c)) fails the SAME way, via sysuaf_write_record()'s own
$!       fopen()+rename(), reporting %UAF-E-WRITEFAIL even for the
$!       caller's own account on its own disk.
$!       tests/libvms/test_sysuaf_write_veracity.c's own header explains
$!       why this was never caught: "WHY THIS IS A HOST TEST, NOT A
$!       tests/qemu ONE" -- the real vmsfs.ko create path had never been
$!       ground-source tested before this item.
$!
$!   Net effect of (c)+(d): NEITHER of OVMX's two password-write
$!   mechanisms can currently write a password to ANY SYSUAF.DAT on a real
$!   mounted vmsfs volume -- not just the cross-device target case this
$!   procedure needs. The AUTHORIZE-against-target design below is correct
$!   or forward-compatible; it will work once (c) and (d) are fixed.
$!
$ SET NOON
$!
$MENU:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    You can install or reconfigure the OVMX operating"
$ WRITE SYS$OUTPUT "    system, or execute DCL commands and procedures to"
$ WRITE SYS$OUTPUT "    perform ""standalone"" tasks."
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "        1)  Install or reconfigure OVMX"
$ WRITE SYS$OUTPUT "        4)  Show installed products"
$ WRITE SYS$OUTPUT "        8)  Execute DCL commands and procedures"
$ WRITE SYS$OUTPUT "        9)  Shut down this system"
$ WRITE SYS$OUTPUT ""
$ OVMX_CHOICE = ""
$ INQUIRE/NOPUNCTUATION OVMX_CHOICE "OVMX$INSTALL Option"
$ IF OVMX_CHOICE .EQS. "1" THEN GOTO DO_INSTALL
$ IF OVMX_CHOICE .EQS. "4" THEN GOTO DO_SHOW
$ IF OVMX_CHOICE .EQS. "8" THEN GOTO DO_DCL
$ IF OVMX_CHOICE .EQS. "9" THEN GOTO DO_SHUTDOWN
$ WRITE SYS$OUTPUT "%OVMX-E-IVCHOICE, ""''OVMX_CHOICE'"" is not a valid choice"
$ GOTO MENU
$!
$DO_SHOW:
$ PRODUCT SHOW PRODUCT
$ GOTO MENU
$!
$DO_DCL:
$! Real Alpha option 8 is "execute DCL commands and procedures" -- DCL is
$! already the shell running this menu, so honoring that choice means
$! returning to it, not building a second, invented DCL-inside-DCL loop.
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "Returning control to DCL. @SYS$MANAGER:OVMX$INSTALL to return to this menu."
$ EXIT
$!
$DO_SHUTDOWN:
$! See the NOTE ON SHUTDOWN in the file header: SYSMAN SHUTDOWN NODE is a
$! pre-existing print-only facade (found, not fixed, by this item) -- it
$! does not halt the emulated machine. Driven honestly anyway: real
$! SYSMAN, real DCL, nothing invented here.
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "At the SYSMAN> prompt, type: SHUTDOWN NODE  then  EXIT"
$ RUN SYS$SYSTEM:SYSMAN.EXE
$ EXIT
$!
$DO_INSTALL:
$ WRITE SYS$OUTPUT ""
$ OVMX_MODE = "PRESERVE"
$ INQUIRE/NOPUNCTUATION OVMX_MODE "Do you want to INITIALIZE or to PRESERVE? [PRESERVE]"
$ IF OVMX_MODE .EQS. "" THEN OVMX_MODE = "PRESERVE"
$ IF (OVMX_MODE .NES. "INITIALIZE") .AND. (OVMX_MODE .NES. "PRESERVE") -
   THEN GOTO DO_INSTALL
$!
$ASK_TARGET:
$ OVMX_TARGET = ""
$ INQUIRE/NOPUNCTUATION OVMX_TARGET "Enter device name for target disk: (? for choices)"
$ IF OVMX_TARGET .EQS. "?" THEN SHOW DEVICE
$ IF OVMX_TARGET .EQS. "?" THEN GOTO ASK_TARGET
$ IF OVMX_TARGET .EQS. "" THEN GOTO ASK_TARGET
$!
$ OVMX_LABEL = "OVMXSYS"
$ INQUIRE/NOPUNCTUATION OVMX_LABEL "Enter volume label for target system disk: [OVMXSYS]"
$ IF OVMX_LABEL .EQS. "" THEN OVMX_LABEL = "OVMXSYS"
$!
$ WRITE SYS$OUTPUT ""
$ IF OVMX_MODE .EQS. "INITIALIZE" THEN -
   WRITE SYS$OUTPUT "    This will INITIALIZE ''OVMX_TARGET' -- ALL EXISTING DATA ON ''OVMX_TARGET' WILL BE ERASED -- and label it ''OVMX_LABEL'."
$ IF OVMX_MODE .EQS. "PRESERVE" THEN -
   WRITE SYS$OUTPUT "    This will install OVMX onto ''OVMX_TARGET', preserving its existing volume (expected label ''OVMX_LABEL')."
$ OVMX_OK = ""
$ INQUIRE/NOPUNCTUATION OVMX_OK "Is this OK? (Yes/No)"
$ IF OVMX_OK .NES. "YES" THEN GOTO MENU
$!
$ IF OVMX_MODE .NES. "INITIALIZE" THEN GOTO SKIP_INIT
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Initializing target...."
$ INITIALIZE 'OVMX_TARGET' 'OVMX_LABEL'
$ IF $SEVERITY .NE. 1 THEN GOTO INSTALL_FAILED
$SKIP_INIT:
$!
$ WRITE SYS$OUTPUT "    Mounting target...."
$ MOUNT 'OVMX_TARGET' 'OVMX_LABEL'
$ IF $SEVERITY .NE. 1 THEN GOTO INSTALL_FAILED
$!
$ WRITE SYS$OUTPUT "    Installing OVMX...."
$ PRODUCT INSTALL VMS /SOURCE=SYS$UPDATE:OVMX-OS.KIT /DESTINATION='OVMX_TARGET'
$ IF $SEVERITY .NE. 1 THEN GOTO INSTALL_FAILED_MOUNTED
$!
$! ---------------------------------------------------------------------
$! System configuration -- SYSTEM password (never echoed) and SCSNODE /
$! SCSSYSTEMID, written to the TARGET, not the distribution disk (sec3a).
$! ---------------------------------------------------------------------
$ WRITE SYS$OUTPUT ""
$PASSWORD_RETRY:
$ SET TERMINAL/NOECHO
$ OVMX_PW1 = ""
$ READ SYS$COMMAND OVMX_PW1 /PROMPT="Password for SYSTEM account: "
$ WRITE SYS$OUTPUT ""
$ OVMX_PW2 = ""
$ READ SYS$COMMAND OVMX_PW2 /PROMPT="Reenter password for verification: "
$ WRITE SYS$OUTPUT ""
$ SET TERMINAL/ECHO
$ IF OVMX_PW1 .EQS. OVMX_PW2 THEN GOTO PASSWORD_OK
$ WRITE SYS$OUTPUT "%OVMX-E-PWMISMATCH, passwords do not match - try again"
$ GOTO PASSWORD_RETRY
$PASSWORD_OK:
$!
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "Configuring the SYSTEM account on the target. At the UAF> prompt (input"
$ WRITE SYS$OUTPUT "is not echoed), type exactly:"
$ WRITE SYS$OUTPUT "    MODIFY SYSTEM/PASSWORD=<the password just entered above>"
$ WRITE SYS$OUTPUT "    EXIT"
$ DEFINE/JOB SYS$SYSTEM 'OVMX_TARGET'[SYSEXE]
$ SET TERMINAL/NOECHO
$ RUN SYS$SYSTEM:AUTHORIZE.EXE
$ SET TERMINAL/ECHO
$ DEFINE/JOB SYS$SYSTEM SYS$SYSROOT:[SYSEXE]
$!
$ OVMX_SCSNODE = ""
$ INQUIRE/NOPUNCTUATION OVMX_SCSNODE "Enter SCSNODE:"
$ OVMX_SCSID = ""
$ INQUIRE/NOPUNCTUATION OVMX_SCSID "Enter SCSSYSTEMID:"
$ IF (OVMX_SCSNODE .EQS. "") .OR. (OVMX_SCSID .EQS. "") THEN GOTO SKIP_SCS
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "Configuring SCSNODE/SCSSYSTEMID on the target. At the SYSGEN> prompt, type:"
$ WRITE SYS$OUTPUT "    USE CURRENT"
$ WRITE SYS$OUTPUT "    SET SCSNODE ""''OVMX_SCSNODE'"""
$ WRITE SYS$OUTPUT "    SET SCSSYSTEMID ''OVMX_SCSID'"
$ WRITE SYS$OUTPUT "    WRITE CURRENT"
$ WRITE SYS$OUTPUT "    EXIT"
$ DEFINE/JOB SYS$SYSTEM 'OVMX_TARGET'[SYSEXE]
$ RUN SYS$SYSTEM:SYSGEN.EXE
$ DEFINE/JOB SYS$SYSTEM SYS$SYSROOT:[SYSEXE]
$SKIP_SCS:
$!
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Dismounting target...."
$ DISMOUNT 'OVMX_TARGET'
$!
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "The installation is complete. Reboot on ''OVMX_TARGET' to use the new system."
$ GOTO MENU
$!
$INSTALL_FAILED_MOUNTED:
$ DISMOUNT 'OVMX_TARGET'
$INSTALL_FAILED:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "%OVMX-E-INSTFAIL, installation did not complete - see the error above"
$ GOTO MENU
$!
$ EXIT
