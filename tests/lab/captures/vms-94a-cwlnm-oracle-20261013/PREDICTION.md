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
