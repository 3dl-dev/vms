# vms-874f: a VAX-only 3-node departure, and the op-0x08 failover rebuild

`ra1b-departure.pcap.gz` (7,994 frames, 06:47:37.339 to 06:47:43.461 UTC, 2026-10-11).

## Provenance

The RA1/RA1b arm of `../vms-380-remaster-arms-20261011` was run in pod `rm380` on three real
OpenVMS VAX V7.3 nodes (VAX1 SYS0, VAX2 SYS1, VAX3 SYS2) with no OVMX node present. The
holders are `RMHOLD.MAR` from that directory. SDA ground truth taken before the departure is
in `../vms-380-remaster-arms-20261011/RA1b-truth.txt`. VAX1 masters `RMT1`. VAX2 holds it at
PR with subs S0 and S1 at CR. VAX3 holds it at CR with subs S0, S1 and S2 at CW. VAX1 runs
`SHUTDOWN` with `REMOVE_NODE`.

The whole br0 capture is `/lab/k8s-labs/rm380/ra1b.pcap` on the ovmx-lab PVC (55,853 frames,
06:42:32 to 06:49:22). This file is a window of it cut by time alone. It keeps EVERY 0x6007
frame from 06:47:36.9 to 06:47:44.0, with payloads unaltered. Nothing is selected by opcode
or name, so every remaster episode is kept whole and each census groups episodes exactly as
it would from the full capture. The wire is silent from 06:47:36.555 to 06:47:37.339. The first
op-0x0e is at 06:47:37.339 and the last op-0x08 answer is at 06:47:42. After 06:47:43.461 the
next frame is at 06:47:44.456, and no cat-0x02/0x82 frame appears before 06:48:00.

Addresses: VAX1 sends as `aa:00:04:00:01:04`, VAX2 as `08:00:2b:b8:36:ea` and VAX3 as
`08:00:2b:96:2a:c4`. VAX2 and VAX3 were identified from their own RMT1 lkids
(`10000387` and `0E00049B` in the truth file).

## What the frames show

1. **Shutdown remaster (06:47:37 to 06:47:41).** VAX1 completes 271 hand-overs, 270 to VAX2
   and 1 to VAX3: CACHE$ (143), F11B$ (126), SYS$_ (1) and VCC$ (1) only, through the full 0e/0f/10/11/12/13/14 sequence.
   RMT1 is **not** remastered, although both survivors hold it. In two episodes VAX1 sends its
   op-0x13 and then stops transmitting (its last frame is #6483), so the new master's op-0x14
   reaches only the survivor. This is the same shape as the three W1 exceptions the corpus
   already had in `heldout-E4`.
2. **Directory re-registration.** After VAX1 leaves, the survivors re-register their own roots
   with the new directory: 194 op-0x0d, each answered 82/0d. All of them precede the first
   op-0x08.
3. **Failover rebuild (06:47:42).** This is 416 cat-0x02 op-0x08 records: 395 ROOT and 21 SUB,
   with 177 sent VAX2 to VAX3 and 239 sent VAX3 to VAX2. Every one is answered by an 82/01,
   paired by transaction. The RMT1 tree, in wire order:

   | frame | from > to | lock | body[24:28] own lkid | [30]/[31] | body[36:40] | body[40:44] | answer |
   |---|---|---|---|---|---|---|---|
   | 7787 | VAX2 > VAX3 | RMT1 | 10000387 | 03/03 PR | 0 | 0 | **f9**: VAX2 now masters RMT1 |
   | 7941 | VAX3 > VAX2 | RMT1 | 0E00049B | 01/01 CR | 0 | 0 | **fa**: body[20:24] = 01000563, body[36:52] = `RMLVB-0123456789` |
   | 7946 | VAX3 > VAX2 | S2 | 010004A6 | 02/02 CW | 0E00049B | 01000563 | fa, 01000564 |
   | 7948 | VAX3 > VAX2 | S1 | 010004A5 | 02/02 CW | 0E00049B | 01000563 | fa, 01000565 |
   | 7950 | VAX3 > VAX2 | S0 | 33000498 | 02/02 CW | 0E00049B | 01000563 | fa, 01000566 |

   Every VAX3 lkid and mode matches the SDA truth. On the wire the lkids are little-endian.
   VAX2 sends no SUB records for its own S0 and S1: it was told f9, so it is the master and its
   subs are local. VAX3 is RMT1's directory, by the (hash>>16)&1 split measured below, so it
   sends its ROOT straight to the master it has just installed.

## The rules, as `tools/cluster/dlm_rebuild08_census.py --selftest` pins them

The corpus holds 888 op-0x08 records in ten captures. Every sender is a real VAX.

> **Update, rd vms-2fa (2026-11-02).** `../vms-2fa-rebuild-step-20261102` adds three captures.
> The corpus is now 1,966 records (1,590 ROOT, 376 SUB), and the census selftest pins the
> current figures. The figures below are the 2026-10-11 corpus. Every rule still holds except
> two statements:
>
> - "body[30] is the higher" is false for one record: arm1's CACHE$ ROOT has body[30] 0 and
>   body[31] 3. All 24 records that differ are system locks.
> - The 0xfa body[28] exceptions are no longer all in RA1b. The arms add 0x14 ×11 and 0x18 ×4.
>
> The new captures also ground **when** a survivor sends: after barrier release 5 if its own
> LOCKDIRWT is non-zero, and after release 6 if it is zero.

- **ROOT and SUB records.** A SUB has a non-zero body[36:44]. There are 550 ROOT records and
  338 SUB records. Each SUB's body[36:40] is the sender's own lkid for a ROOT it sent earlier,
  and its body[40:44] is that ROOT's answer body[20:24]. This holds for all 338. The SUB goes
  to the same node after that answer arrives, and only when that answer was 0xfa.
- **Which cells are fields:**
  - body[24:28] is the holder's own lkid. The answer echoes it in 888 of 888.
  - body[30] and body[31] carry the mode. They are equal in 880. In the 8 that differ (system
    locks) body[30] is the higher; which one is the granted mode is **not grounded**.
  - body[44:48+] is the resource identity (group, access mode, length, name).
  - body[88:108] is the value block and sequence. A 0xfa ROOT answer returns it at
    body[36:56] (387 of 398).
  - body[128:132] on a ROOT is refhash of its own identity (550 of 550). On a SUB it is neither
    its own name's hash nor its parent's, the same finding `dlm_sublock_census.py` has for the
    op-0x01 sub request.
- **Which cells are residue:**
  - body[20:24] in the request matches a value the sender had just received in 529 of 550 ROOT
    and 319 of 338 SUB records. It is PID-form (0x20xxxxxx) in 68 of them, including the RMT1
    records. The answer echoes it on f9 and overwrites it on fa.
  - body[28:30] and body[32:36] differ by resource family. They are not named here.
- **The answer** is always cat 0x82 op 0x01:
  - **0xf9** (149 ROOT answers): the requester now masters the tree, and body[20:24] is echoed.
  - **0xfa** (398 ROOT, 338 SUB): a master exists. body[20:24] is the master's lkid for the
    holder's lock, and body[28] is 0x10 in 726 of 736. The exceptions are 0x14 ×5 and 0x18 ×3,
    all in RA1b.
- **Who answers, in RA1b.** VAX2 gives every f9 for (hash>>16)&1 = 0 (48) and VAX3 every f9 for
  parity 1 (85). Of the 262 fa ROOT answers, 141 come from the node that is both directory and
  master. The other 121 answer the directory node itself, which is addressing the master it has
  just answered f9. That is the RMT1 case.
- **SYS$SYS_ID** at a formation barrier (rd vms-629, four samples) is the same message: a ROOT
  record of the node's own lock (lkid 01000000, EX/EX, exec mode), answered f9.
- **Phase.** No op-0x0d lies inside any capture's op-0x08 run.

## Topology: what is covered and what is NOT

| capture | cluster | departure | survivors | LOCKDIRWT | op-0x08 senders | records (ROOT/SUB) |
|---|---|---|---|---|---|---|
| this one | 3 VAX | VAX1 SHUTDOWN REMOVE_NODE | 2 | equal (vms-380 RA3) | both survivors | 416 (395/21) |
| `vms-cab-op0f-20261010/rule2/heldout-E4` | 3 VAX | VAX1 SHUTDOWN REMOVE_NODE | 2 | 0/1/0 | 1 | 137 (135/2) |
| `.../heldout-E5`, `heldout-E5b` | 3 VAX | VAX2 hung and was removed by the survivors | 2 | 0/0/0 | 1 | 158 (6/152), 6 (6/0) |
| `vms-b36-cnxmgrerr-20260925/oracle/oracle-3node-fault-f1` | 3 VAX | VAX3 killed (crash) | 2 | not recorded | 1 | 164 (1/163) |
| `vms-ci6-evac-20261010/run16` | VAX1, VAX2, OVMX | VAX1 SHUTDOWN REMOVE_NODE | 2 (OVMX is the only directory) | 0/0/1 | VAX2 | 3 (3/0); the answers are OVMX-built and are excluded |
| `vms-629`, `vms-6d3d`, `vms-cab heldout1/2` | 2 VAX formation or join | none | n/a | n/a | 1 | 1 each, SYS$SYS_ID |

**NOT covered:**
- **Three or more survivors.** With two survivors the destination is always "the other node",
  so this corpus cannot separate *addressed to the directory* from *addressed to the master*
  except in RA1b's sender-is-directory case. What a directory that is neither master nor
  requester answers (a redirect?) has **no instance**.
- **A sub of a sub.** Every SUB's parent is a ROOT.
- **A real answer from OVMX's side.** No capture has a real VAX answering an OVMX-sent op-0x08,
  because OVMX has never sent one.
- **An Alpha sender.**
- **The meaning of body[30] versus body[31]** when the two differ.

## Correction recorded

The codec header's comment at `VMS_DLM_WIREOP_DIR_LOOKUP_TR`
(`src/kernel-core/vms_cluster_codec_dlm.h`) called op 0x08 only a root lookup sent at a
barrier. It now names op 0x08 as the failover rebuild record and points here.
