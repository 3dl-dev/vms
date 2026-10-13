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
