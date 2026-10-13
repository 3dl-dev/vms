# vms-94a: pre-registered before the OVMX fire (2026-10-13)

## The layout under test (grounded in README.md, before this fire)

A cat-0x0b op-0x04 names a buffer. The recipient REQUEST DATAs it, applies the record to its
own logical-name tables, and then answers 0x8b/0x04. The record holds the system service's
argument list: $CRELNM (fn 1), $CRELNT (fn 2) or $DELLNM (fn 3).

## Fire F2 (pod lab52dfb, build work/vms-94a-cwlnm on lab/int-52d)

The topology is fire F1's: OVMXE (1030, EXPECTED_VOTES 1) founds alone, and VAX1 and VAX2
(golden V7.3, ASYM_MSCP) boot 300 s later.

- G1: every cat-0x0b op-0x04 a VAX sends OVMXE is pulled (one SCA 0x08 from OVMXE per
  request) and answered with 0x8b/0x04. That holds for its startup `$CRELNT`s
  (`CLU$ICC_ORBS_<node>`, `ICC$REGISTRY_TABLE`, `LNM$CLUSTER_INTRUSION_TABLE`). OVMXE logs
  no unroutable frame and no "was not answered".
- G2: both VAXes finish startup (`job terminated` on each console, then `Username:`), with
  CN = 3 and 0 bugchecks.
- G3: a `DEFINE/TABLE=LNM$SYSCLUSTER` typed on a VAX completes with no error, reaches
  OVMXE as a $CRELNM, and is answered.

If G2 fails, the capture's `unans.py` names the next request the VAX waits on.

## Fire F2 result (artifacts run 38350552165, 6f752786d)

- G1 **held**. All 5 cat-0x0b op-0x04 a VAX sent OVMXE (VAX1 4, VAX2 1) were pulled and
  answered 0x8b/0x04. OVMXE logged no unroutable frame and no "was not answered".
- G2 **failed in part**. VAX2 finished startup (`job terminated`), which F1 never reached.
  VAX1 stopped. Its DECnet start changed its station address (08002b390ec9 to aa0004000104),
  and its next kind-6 MOUNT close to OVMXE drew a REQUEST DATA addressed to the OLD
  station. Block frames were never ringed, so the lost frame left a sequence hole, the
  circuit failed on TIMVCFAIL, and the close went unanswered. That is rd vms-c1d (PR #1669),
  not a logical-name fault. 0 bugchecks.
- G3 was not reached.

Files: `f2-ovmx-first-cm.pcap` (cat 0x01/0x06/0x0b), `f2-cwlnm.pcap` (the logical-name
traffic with its block transfers), `f2-*-console.log`.

## Fire F3 (pod lab52dfc, build lab/int-94a = this branch + vms-c1d), registered before it runs

The topology and the predictions are F2's: G1, G2 and G3. G2 additionally requires that VAX1's
post-DECnet MOUNT close is answered.

## Fire F3 result (artifacts run 38354422537, lab/int-94a a1175d61f)

The topology was OVMX first: OVMXE founded alone, and VAX1 and VAX2 booted 300 s later.

- G1 **held**. Every 0b/04 to OVMXE was pulled and answered.
- G2 **held**. Both VAXes finished startup and each offers `Username:` (VAX/VMS V7.3 node
  VAX1, node VAX2). VAX1 reads `CN_3`. `unans.py` finds no unanswered request in the whole
  capture, and there were 0 bugchecks. VAX1's DECnet address change happened as in F2, and
  its circuit to OVMXE held.
- G3 **held**. `DEFINE/TABLE=LNM$SYSCLUSTER OVMX_F3 "FROM_VAX1"` typed on VAX1 completed with
  no error. It reached OVMXE as a $CRELNM 0b/04 (LNM$SYSCLUSTER_TABLE, OVMX_F3 [super] =
  "FROM_VAX1") and was answered 2 ms later.

Files: `f3-ovmx-first-cm.pcap`, `f3-cwlnm.pcap`, `f3-*-console.log`.

## Fire G (pod lab94ag, build lab/int-94a with the database pull), registered before it runs

The topology is VAX first. VAX1 and VAX2 (golden V7.3) form, and on VAX1
`DEFINE/TABLE=LNM$SYSCLUSTER OVMX_G1 "VAL_A","VAL_B"` and
`DEFINE/TABLE=LNM$SYSCLUSTER/EXECUTIVE_MODE OVMX_G2 "VAL_C"` are typed. Then OVMXE boots
and joins (EXPECTED_VOTES 3).

- H1: once a member, OVMXE sends exactly one 0x0b/0x05, to VAX1 (the lowest SCSSYSTEMID,
  1025). VAX1 answers 0x8b/0x05 and pushes 0x0b/0x06. OVMXE REQUEST DATAs it and answers
  0x8b/0x04, logging no "was not answered".
- H2: on OVMXE, `SHOW LOGICAL/TABLE=LNM$SYSCLUSTER` lists OVMX_G1 = ("VAL_A","VAL_B")
  [super] and OVMX_G2 = "VAL_C" [exec]. These are names OVMX never saw propagated one by
  one, only in the pushed database.
- H3: a third `DEFINE/TABLE=LNM$SYSCLUSTER OVMX_G3 "LATE"` on VAX1 after OVMXE joined
  reaches OVMXE as a 0b/04 and shows up in the same listing.
- H4: on OVMXE, `DEFINE/TABLE=LNM$SYSCLUSTER OVMX_G4 "X"` is refused with
  %SYSTEM-F-UNSUPPORTED, because origination is not built and VAX1 must not diverge.
  VAX1 never sees an OVMX_G4.
- H5: 0 bugchecks, CN = 3 on VAX1.

## Fire G result (artifacts /tmp/evac/art-int94b, lab/int-94a 5357c045)

- H1 **held**. OVMXE sent one 0b/05, to VAX1 (send 3). VAX1 answered 8b/05 (its send 3) and
  pushed 0b/06 (its send 4, 1876 bytes). OVMXE pulled the database and answered 8b/04 (its
  send 4). Nothing "was not answered". `g1-cwlnm.pcap` has the six frames.
- H2 **held**, checked with F$TRNLNM rather than SHOW LOGICAL. On OVMXE,
  `F$TRNLNM("OVMX_G1","LNM$SYSCLUSTER")` with indexes 0 and 1 gave VAL_A and VAL_B, and
  OVMX_G2 gave VAL_C. These two names reached OVMX only in the pushed database.
- H3 **not run**. VAX1 crashed (H5) before OVMX_G3 could be typed.
- H4 **failed as registered, but held in substance**. On OVMXE,
  `DEFINE/TABLE=LNM$SYSCLUSTER OVMX_G4 "X"` was refused at DCL's parser with %DCL-W-IVQUAL,
  because OVMX's DEFINE has no /TABLE. It never reached the executive's SS$_UNSUPPORTED check.
  VAX1 saw no OVMX_G4. The DCL gap is tracked as rd vms-aae.
- H5 **FAILED**. VAX1 took a fatal CNXMGRERR bugcheck (`g1-vax1-console.log`) at OVMXE's
  86/00. `g1-vax1-ovmxe-cm.pcap` (`trimcm.py`) shows OVMXE's send-msg# going from 4 to 6.
  Cause: the emit guard's view of what the peer had sent left out category 0x0b. A DLM frame
  acking VAX1's send 4 was therefore refused as ack-unbacked, and it had already taken send 5.
  Fixed in rd vms-cd8. G is re-run on an integration build that includes it.

Oracle captured on VAX1 during G (`g1-vax1-console.log`): LNM$SYSTEM is a search list,
"LNM$SYSTEM_TABLE" then "LNM$SYSCLUSTER". LNM$SYSCLUSTER is "LNM$SYSCLUSTER_TABLE"
[kernel,no_alias] in LNM$SYSTEM_DIRECTORY.

## Demo topology (pods labdemo1, labdemo2; build lab/int-94c = this branch + vms-c1d + vms-cd8), registered before they run

These runs reproduce the web demo's three nodes: two OVMX nodes and one real VAX, which is Node C.
- Node C is the golden V7.3 VAX1 (1025).
- OVMXA (1021) and OVMXB (1022) are x86 OVMX nodes. In the browser demo, Node B is the OVMX-VAX
  build. The lab has no rail for that build, so this run uses x86 for B.
- The OVMX SCSSYSTEMIDs sit below 1025 so that, as in the demo (1987/1988/1989), the real VAX
  has the highest ID.
- Group 1 is the lab's group. The demo uses 257.

As in the demo grader, the VAX is never logged in to.

**DC1, C first.** VAX1 boots conversationally with `SET EXPECTED_VOTES 1` and forms alone.
OVMXA then joins (EXPECTED_VOTES 3), and after it OVMXB (EXPECTED_VOTES 3).
- D1: VAX1 forms and offers `Username:`.
- D2: when each OVMX node becomes a member, it sends one 0b/05 to VAX1 and pulls the 0b/06 it
  gets back.
- D3: VAX1 reads CN=3 (SHOW CLUSTER on an OVMX node lists all three) and offers `Username:` again.
- D4: no request is left unanswered (`unans.py`), no CM dialogue from an OVMX node to VAX1
  skips a send-msg# (`trimcm.py`), and there are 0 bugchecks.

**DC2, C last.** VAX1 is held at `SYSBOOT>`. OVMXA founds alone (EXPECTED_VOTES 1) and OVMXB
joins (EXPECTED_VOTES 3). Then VAX1 continues with the golden parameters.
- L1: VAX1 joins the cluster the OVMX nodes formed, without forming its own, reaches CN=3 and
  offers `Username:`.
- L2: VAX1's 0b/01 to each OVMX node is answered 8b/01, with [28:32] and [36:40] = 1, because
  both became members first.
- L3: VAX1 sends no 0b/05 to an OVMX node. This is grounded on F3: in an OVMX-first cluster,
  the first VAX to join sent none. If it does send one, OVMX has no answer for it yet, and L1
  fails on a startup hang.
- L4: as D4.

## DC1 result (pod labdemo1, artifacts run 38364208322, lab/int-94c e64baabb8)

VAX1 did not boot conversationally. The pod's entrypoint booted it straight from the golden
parameters, and it formed alone ("proposing formation of a VAXcluster").

- D1 **held**. VAX1 formed and finished startup.
- D2 **held**. OVMXA sent 0b/05 to VAX1 (its send 96), and VAX1 answered 8b/05 and pushed
  0b/06; OVMXA pulled it and answered 8b/04. OVMXB did the same (send 97 → 8b/04 send 98).
  `dc1-cwlnm.pcap` has the 12 frames.
- D3 **held**. SHOW CLUSTER on OVMXB lists OVMXB, OVMXA and VAX1, all MEMBER. VAX1's console
  shows it proposed and completed the addition of OVMXA and then of OVMXB. Pressing Return
  after the run got `VAX/VMS V7.3 node VAX1` and `Username:`. Nobody logged in.
- D4 **held for the VAX, failed between the OVMX nodes**.
  - VAX side: no request from or to VAX1 was left unanswered. Neither OVMX→VAX1 dialogue
    skips a send-msg# (391 and 372 messages). 0 bugchecks.
  - OVMX side: OVMXB sent OVMXA five cat-0x02 (DLM) op-0x01 requests (sends 3 to 7, 2 s
    apart, during OVMXB's join). OVMXA answered none of them (`dc1-ovmx-ovmx-dlm.pcap`).
    Tracked as its own item.

## DC2 amendment (registered before DC2 runs)

The pod cannot hold VAX1 at `SYSBOOT>`, because its entrypoint boots VAX1 straight away. So in
DC2:
- VAX1 is halted at `sim>` (^E) before it reaches SYSINIT, and is not used again.
- Node C is VAX2 (root SYS1, golden parameters), booted by `oracle_stage.sh node vax2` after
  OVMXA has founded and OVMXB has joined.
- L1 to L4 stand as registered, with VAX2 in place of VAX1.

## DC2 result (pod labdemo2, lab/int-94c e64baabb8)

VAX1 was halted at `sim>` during its ROM boot. OVMXA founded, and OVMXB joined with OVMXA
coordinating; SHOW CLUSTER on OVMXB listed both as MEMBER. Then VAX2 (Node C) booted.

- L1 **FAILED**. VAX2 opened connections to both OVMX nodes and sent its membership request
  (01/02) to OVMXB. OVMXB, as coordinator, relayed it to OVMXA (01/12, 08:10:53). OVMXA never
  answered, so OVMXB never proposed the addition. VAX2 kept sending "membership request to
  system OVMXB" (63 times in 12 minutes) and was never admitted. There was no bugcheck.
  Cause: OVMXA had only ever coordinated, never received a transition open, so it held no
  transition class to answer the relay with (rd vms-6ec). `dc2-cat01.pcap` has every cat-0x01
  frame.
- L2, L3 and L4 were not reached. VAX2 sent no cat-0x0b, and no frame went to it that the
  gate does not cover.

## DC1b and DC2b (lab/int-94c + vms-6ec), registered before they run

Both orders run again on one build that carries the vms-6ec fix. DC1b repeats DC1, and D1 to
D4 stand. DC2b repeats DC2 as amended, and L1 to L4 stand. In addition, for DC2b:
- L5: OVMXA answers OVMXB's 01/12 relay of VAX2 with 81/12, body[17] = 0x02, the class it
  drove when it admitted OVMXB.

## DC2b result (pod labdemo4, lab/int-94c b92a55bd1 = vms-6ec's class fix only)

- L1 **FAILED again**, and L5 failed. OVMXB relayed VAX2 to OVMXA (01/12, 08:50:14), and
  OVMXA again sent no 81/12. VAX2 was not admitted, and there was no bugcheck. OVMXA's CNXTRACE
  (`dc2b-ovmxa-console.log`) shows why: the founder's join sits in ADVERTISE, where a relay is
  an empty cell (`ev=RX_TR_RELAY detail=EMPTY-CELL`). The class fix was needed but not
  enough. vms-6ec now also answers the relay from ADVERTISE when this node is a committed
  member.

## DC2c (lab/int-94c + all of vms-6ec), registered before it runs

DC2's procedure, as amended, with L1 to L5 standing.

## DC2c result (pod labdemo5, lab/int-94c 7946dcbfd = all of vms-6ec)

- L1 **held**. VAX2 asked OVMXB, was admitted ("now a VAXcluster member -- system VAX2"), and
  finished startup. SHOW CLUSTER on OVMXA lists OVMXA, OVMXB and VAX2, all MEMBER. Pressing
  Return gets `VAX/VMS V7.3 node VAX2` and `Username:`. Nobody logged in.
- L2 **held**. Both 8b/01 answers carry [28:32] = [36:40] = 1.
- L3 **held**. VAX2 sent no 0b/05.
- L4 **held for the VAX**: 0 bugchecks, no send-msg# gap from either OVMX node to VAX2, and
  every request VAX2 sent was answered (each 0b/04 by both nodes, `dc2c-cwlnm.pcap`). The one
  unanswered message is OVMXB→OVMXA 02/03, a DLM release, which is one-way on V0.7-8 as well
  (rd vms-e09).
- L5 **held**. OVMXA answered OVMXB's relay 1 ms later with 81/12, body[17] = 0x02.

OVMXA's console also logged `%SCS-W-DIRNOANSWER` and `%SCS-W-CONNTIMO` every 36 s after the
admission.

## DC1b result (pod labdemo6, lab/int-94c 7946dcbfd)

- D1, D2 and D3 **held**, as in DC1. Each OVMX node pulled VAX1's database (`dc1b-cwlnm.pcap`,
  12 frames). SHOW CLUSTER on OVMXB lists all three nodes as MEMBER. VAX1 offers `Username:`.
  0 bugchecks.
- D4 **held for the VAX**: no send-msg# gap from either OVMX node to VAX1. Between the OVMX
  nodes it failed again: OVMXB's five 02/01 to OVMXA during its join went unanswered. OVMXA's
  CNXTRACE (`dc1b-ovmxa-console.log`) says it received 5 requests and declined 5, so the DLM
  arm refused them; nothing was lost. An all-OVMX 2-node cluster on main and on V0.7-8 answers
  every 02/01 (rd vms-e09).
