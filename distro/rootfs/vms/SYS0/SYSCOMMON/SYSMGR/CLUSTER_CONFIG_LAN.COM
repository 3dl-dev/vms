$! CLUSTER_CONFIG_LAN.COM - OpenVMX cluster configuration procedure, LAN (vms-258)
$!
$! THE VMS-CANON FRONT DOOR for authoring a node's cluster identity. Before
$! this procedure the only documented way to configure OVMX clustering was to
$! hand-edit the binary OVMXVMSSYS.PAR store (or point OVMX_SYSGEN_PATH at one)
$! -- Linux plumbing that no VMScluster operator would ever be told to touch.
$! This is the operator surface a VMS admin expects: an interactive
$!   $ @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM
$! that drives SYSGEN to author SCSNODE / SCSSYSTEMID / VOTES / ... into
$! SYS$SYSTEM:OVMXVMSSYS.PAR and is adopted on the next reboot.
$!
$! PINNED TO: docs/design-cluster-config-authoring.md (the vms-098 epic; this is
$! rung R4's operator front-end, the honest V0.6 subset of the vms-9c6c
$! capstone). The name CLUSTER_CONFIG_LAN.COM is the public VMS name for the
$! LAN (Ethernet) configuration procedure -- OVMX clusters over raw-Ethernet
$! SCA, so the LAN variant is the faithful one. The QUESTION FLOW and menu
$! verbs are derived only from the public VSI OpenVMS Cluster Systems manual
$! (SYS$MANAGER procedures) plus lab observation (CLAUDE.md Rule 8); OVMX
$! authors all .COM content, and no VSI/HPE source or binary was consulted.
$!
$! MENU CONTENT (CLAUDE.md Rule 10 -- reproduce a canonical verb ONLY where
$! OVMX has the underlying facility; an item OVMX cannot honestly perform prints
$! an honest "not available at this edition", it is never faked):
$!   ADD    - kept.  Enables clustering on THIS standalone node and authors its
$!            identity. (Real VMS ADD also provisions *remote* satellite roots;
$!            OVMX cannot boot a satellite, so ADD here configures the LOCAL
$!            node only -- stated plainly at the prompt.)
$!   CHANGE - kept.  Reconfigures THIS node's cluster characteristics.
$!   REMOVE - not available as a working action.  Removing a *remote* member's
$!            root from this node is not offered; a node leaves by CHANGE with
$!            VAXCLUSTER 0 then reboot.
$!   CREATE a duplicate system disk / satellite roots - not available; no
$!            satellite/duplicate-disk facility at this edition.
$!
$! HONESTY (CLAUDE.md INV-6). Every parameter this procedure authors goes into
$! SYS$SYSTEM:OVMXVMSSYS.PAR, and STARTUP.EXE loads the whole set into the
$! EXECUTIVE at the next boot (VMS_IOCTL_SYSGEN_LOAD) before it starts the
$! cluster (VMS_IOCTL_CLUSTER_START) -- there is no cluster daemon reading
$! this file for itself any more. What each one actually does:
$!
$!   SCSNODE / SCSSYSTEMID -- this node's real wire identity. Effectual: the
$!            executive's port and connection manager put them on the wire,
$!            and CLUSTER_START is REFUSED (SS$_BADPARAM) if VAXCLUSTER is
$!            nonzero with no SCSNODE authored.
$!   VAXCLUSTER -- the boot-time decision, applied by the executive:
$!            0 (the shipped default) brings up NO cluster port at all, so no
$!            HELLO leaves this node; 1 joins if a cluster is present, else
$!            reports STANDALONE; 2 forms or joins and prints "waiting to
$!            form or join an OpenVMS Cluster" on OPA0: until it can.
$!   RECNXINTERVAL / TIMVCFAIL -- effectual: the executive's reconnect loop
$!            and its circuit-failure detector use them.
$!   VOTES / EXPECTED_VOTES -- authored, persisted, and really read: the
$!            connection manager computes CEVOTES and QUORUM from them and
$!            SHOW CLUSTER/CLUSTER displays the result. But nothing yet
$!            BLOCKS on lost quorum -- the arithmetic is tracking only.
$!   ALLOCLASS -- authored and persisted; reported, not yet acted on.
$!
$! This procedure records all of them and says so at the prompt -- it never
$! claims VOTES/EXPECTED_VOTES/ALLOCLASS gate anything. See the Cluster
$! Systems manual, "Not yet available at V0.6".
$!
$! MECHANISM. SYSGEN is driven NON-INTERACTIVELY via inline SYS$INPUT, the same
$! idiom OVMX$INSTALL.COM proved (vms-597): the non-$ data lines after RUN
$! SYS$SYSTEM:SYSGEN.EXE become SYSGEN's SYS$INPUT, and DCL performs apostrophe
$! substitution on them, so the answers entered above expand before SYSGEN reads
$! each line. USE CURRENT loads the live OVMXVMSSYS.PAR; WRITE CURRENT mints a
$! fresh version over the executive ACP (vms-46c) -- the same file the booted
$! system reads its identity from.
$!
$! DCL NOTE. This interpreter does not substitute a bare symbol passed as a
$! lexical argument, so every F$INTEGER/F$LENGTH here takes the apostrophe-
$! substituted VALUE -- F$INTEGER("''SYM'") -- not F$INTEGER(SYM). Verified in
$! tests/dcl/test_cluster_config_lan.sh.
$!
$ SET NOON
$!
$MENU:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "                    Cluster Configuration Procedure"
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "        1. ADD this node to a cluster"
$ WRITE SYS$OUTPUT "        2. CHANGE this node's cluster characteristics"
$ WRITE SYS$OUTPUT "        3. REMOVE a node from the cluster"
$ WRITE SYS$OUTPUT "        4. CREATE a duplicate system disk"
$ WRITE SYS$OUTPUT "        5. EXIT from this procedure"
$ WRITE SYS$OUTPUT ""
$ CC_OPT = ""
$ INQUIRE CC_OPT "Enter choice [1]"
$ IF CC_OPT .EQS. "" THEN CC_OPT = "1"
$ IF CC_OPT .EQS. "1" THEN GOTO ADD
$ IF CC_OPT .EQS. "2" THEN GOTO CHANGE
$ IF CC_OPT .EQS. "3" THEN GOTO REMOVE
$ IF CC_OPT .EQS. "4" THEN GOTO CREATE
$ IF CC_OPT .EQS. "5" THEN GOTO DONE
$ IF CC_OPT .EQS. "EXIT" THEN GOTO DONE
$ IF CC_OPT .EQS. "QUIT" THEN GOTO DONE
$ WRITE SYS$OUTPUT "%CLUSTER_CONFIG-E-IVCHOICE, ""''CC_OPT'"" is not a valid choice"
$ GOTO MENU
$!
$! ---------------------------------------------------------------- ADD / CHANGE
$! ADD and CHANGE share the identity-authoring routine; ADD additionally enables
$! cluster participation (VAXCLUSTER=2) on a node that may be standalone today.
$ADD:
$ CC_ACTION = "ADD"
$ CC_ENABLE = "2"
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    ADD configures THIS node as a VMScluster member. (OpenVMX cannot"
$ WRITE SYS$OUTPUT "    provision a remote satellite node; this authors the local node only.)"
$ GOTO CONFIGURE
$!
$CHANGE:
$ CC_ACTION = "CHANGE"
$ CC_ENABLE = ""
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    CHANGE reconfigures this node's cluster characteristics."
$ GOTO CONFIGURE
$!
$CONFIGURE:
$ WRITE SYS$OUTPUT ""
$! --- SCSNODE (effectual: read by the cluster daemon, sets the wire name) ---
$CFG_NODE:
$ CC_NODE = ""
$ INQUIRE CC_NODE "    What is the node's SCSNODE name"
$ IF CC_NODE .EQS. "" THEN GOTO CFG_NODE
$ IF F$LENGTH("''CC_NODE'") .LE. 6 THEN GOTO CFG_ID
$ WRITE SYS$OUTPUT "%CLUSTER_CONFIG-E-BADNODE, SCSNODE is limited to 6 characters"
$ GOTO CFG_NODE
$! --- SCSSYSTEMID (effectual: read by the cluster daemon, sets the wire id) ---
$CFG_ID:
$ CC_ID = ""
$ INQUIRE CC_ID "    What is the node's SCSSYSTEMID"
$ IF F$INTEGER("''CC_ID'") .GT. 0 THEN GOTO CFG_ID_OK
$ WRITE SYS$OUTPUT "%CLUSTER_CONFIG-E-BADID, SCSSYSTEMID must be a positive integer"
$ GOTO CFG_ID
$CFG_ID_OK:
$ CC_ID = F$INTEGER("''CC_ID'")
$! --- ALLOCLASS (read & reported only) ---
$ CC_ALLO = ""
$ INQUIRE CC_ALLO "    Allocation class for shared cluster devices [0]"
$ IF CC_ALLO .EQS. "" THEN CC_ALLO = "0"
$ CC_ALLO = F$INTEGER("''CC_ALLO'")
$! --- VOTES / EXPECTED_VOTES (authored, not yet enforced at V0.6) ---
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Note: at this OpenVMX edition VOTES/EXPECTED_VOTES are recorded in the"
$ WRITE SYS$OUTPUT "    parameter file but not yet enforced -- the node joins non-voting and"
$ WRITE SYS$OUTPUT "    quorum is computed-and-logged only. See the Cluster Systems manual."
$ CC_VOTES = ""
$ INQUIRE CC_VOTES "    Votes this node contributes [1]"
$ IF CC_VOTES .EQS. "" THEN CC_VOTES = "1"
$ CC_VOTES = F$INTEGER("''CC_VOTES'")
$ CC_EXP = ""
$ INQUIRE CC_EXP "    Expected total cluster votes [''CC_VOTES']"
$ IF CC_EXP .EQS. "" THEN CC_EXP = "''CC_VOTES'"
$ CC_EXP = F$INTEGER("''CC_EXP'")
$!
$! ---------------------------------------------------------------- confirm
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    The following will be written to SYS$SYSTEM:OVMXVMSSYS.PAR:"
$ WRITE SYS$OUTPUT "        SCSNODE         = ''CC_NODE'"
$ WRITE SYS$OUTPUT "        SCSSYSTEMID     = ''CC_ID'"
$ WRITE SYS$OUTPUT "        ALLOCLASS       = ''CC_ALLO'"
$ WRITE SYS$OUTPUT "        VOTES           = ''CC_VOTES'"
$ WRITE SYS$OUTPUT "        EXPECTED_VOTES  = ''CC_EXP'"
$ IF CC_ENABLE .NES. "" THEN WRITE SYS$OUTPUT "        VAXCLUSTER      = ''CC_ENABLE'  (cluster participation enabled)"
$ WRITE SYS$OUTPUT ""
$ CC_OK = ""
$ INQUIRE CC_OK "Is this correct? (Y/N) [Y]"
$ IF CC_OK .EQS. "" THEN CC_OK = "Y"
$ IF CC_OK .EQS. "Y" THEN GOTO WRITE_IT
$ IF CC_OK .EQS. "YES" THEN GOTO WRITE_IT
$ GOTO ABORTED
$!
$! ---------------------------------------------------------------- write via SYSGEN
$WRITE_IT:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Writing OVMXVMSSYS.PAR...."
$ IF CC_ENABLE .EQS. "" THEN GOTO WRITE_NOENABLE
$ RUN SYS$SYSTEM:SYSGEN.EXE
USE CURRENT
SET SCSNODE "''CC_NODE'"
SET SCSSYSTEMID 'CC_ID'
SET ALLOCLASS 'CC_ALLO'
SET VOTES 'CC_VOTES'
SET EXPECTED_VOTES 'CC_EXP'
SET VAXCLUSTER 'CC_ENABLE'
WRITE CURRENT
EXIT
$ GOTO WROTE
$!
$WRITE_NOENABLE:
$ RUN SYS$SYSTEM:SYSGEN.EXE
USE CURRENT
SET SCSNODE "''CC_NODE'"
SET SCSSYSTEMID 'CC_ID'
SET ALLOCLASS 'CC_ALLO'
SET VOTES 'CC_VOTES'
SET EXPECTED_VOTES 'CC_EXP'
WRITE CURRENT
EXIT
$!
$WROTE:
$ IF .NOT. $STATUS THEN GOTO WRITE_FAILED
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    ''CC_ACTION' complete. The new cluster identity is written to"
$ WRITE SYS$OUTPUT "    SYS$SYSTEM:OVMXVMSSYS.PAR and takes effect on the next reboot."
$ WRITE SYS$OUTPUT "    After rebooting, confirm membership with  $ SHOW CLUSTER"
$ GOTO MENU
$!
$WRITE_FAILED:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "%CLUSTER_CONFIG-E-WRITEFAIL, SYSGEN could not write OVMXVMSSYS.PAR - see the error above"
$ GOTO MENU
$!
$ABORTED:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "%CLUSTER_CONFIG-I-NOCHANGE, no change made"
$ GOTO MENU
$!
$! ---------------------------------------------------------------- REMOVE
$REMOVE:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Removing a remote member's root is not available at this OpenVMX"
$ WRITE SYS$OUTPUT "    edition. To take THIS node out of the cluster, choose CHANGE and set"
$ WRITE SYS$OUTPUT "    VAXCLUSTER to 0, then reboot."
$ GOTO MENU
$!
$! ---------------------------------------------------------------- CREATE
$CREATE:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Creating a duplicate system disk is not available at this OpenVMX"
$ WRITE SYS$OUTPUT "    edition (no satellite/duplicate-disk facility). See the Cluster"
$ WRITE SYS$OUTPUT "    Systems manual, ""Not yet available at V0.6""."
$ GOTO MENU
$!
$DONE:
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    Exiting the cluster configuration procedure."
$ EXIT
