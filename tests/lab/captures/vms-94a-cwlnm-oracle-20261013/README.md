# vms-94a: clusterwide logical names on the wire (cat 0x0b)

This is a real-VAX-only oracle: pod `lab94a`, image `ovmx-vaxlab:7`, golden 3-node volume.
VAX1 and VAX2 booted together. With VAX1 logged in, the commands below were typed, and then
VAX3 booted late. Every 0x6007 frame was captured. `lnm1-cwlnm.pcap` keeps the cat-0x0b
messages and the SCA block transfers they cause (`trimlnm.py`). `armB-cwlnm.pcap` does the
same for vms-52d's arm B.

## The commands (VAX1, SYSTEM)

| tag | command |
|---|---|
| D1 | `DEFINE/TABLE=LNM$SYSCLUSTER OVMXT1 "A"` |
| D2 | `DEFINE/TABLE=LNM$SYSCLUSTER OVMXTEST_LONGNAME_2 "VALUE_TWO_LONGER"` |
| D3 | `DEFINE/TABLE=LNM$SYSCLUSTER OVMXT3 "X","YY","ZZZ"` |
| D4 | `DEFINE/TABLE=LNM$SYSCLUSTER/EXECUTIVE_MODE/TRANSLATION_ATTRIBUTES=CONCEALED OVMXT4 "DUA0:[A.]"` |
| D5 | `DEFINE/TABLE=LNM$SYSCLUSTER OVMXT1 "B"` (supersede) |
| D6 | `DEASSIGN/TABLE=LNM$SYSCLUSTER OVMXT3` |
| D7 | `DEFINE/TABLE=LNM$SYSCLUSTER/NAME_ATTRIBUTES=(NO_ALIAS) OVMXT5 "P1"/T=TERMINAL,"P2"/T=CONCEALED,"P3"` |
| D8 | `CREATE/NAME_TABLE/PARENT_TABLE=LNM$CLUSTER_TABLE/PROTECTION=(S:RWCD,O:RWCD,G:R,W:R) OVMX_CTAB` |
| D9c | `DEFINE/TABLE=OVMX_CTAB OVMXT6 "IN_CTAB"` |
| D10 | `DEASSIGN/TABLE=LNM$SYSCLUSTER/EXECUTIVE_MODE OVMXT4` |
| D11 | `DEASSIGN/TABLE=LNM$SYSCLUSTER/ALL` |

Each command produced exactly one cat-0x0b op-0x04 to every other member.

## The protocol

1. The request names a buffer. body[16:20] is the requester's buffer name, body[20:24] the
   recipient's own Con.ID (the same descriptor a cat-0x06 close carries), body[24:28] a
   per-requester sequence number, and body[32:36] the record length.
2. **Every** real recipient REQUEST DATAs the record (SCA 0x08, then DATA 0x0d) before it
   answers (`xfer04.py`).
3. The answer is 0x8b/0x04 with the requester's txn/token. Past the header, real answers
   hold only leftover buffer contents. Two are entirely zero (`rm380-cat0b.pcap`, both
   recipients at 137.965).

## The record (`lnmrec.py` decodes all 20; each matches its command)

| rec | meaning |
|---|---|
| [0:2] | the record's own length |
| [2:4] | 1 $CRELNM, 2 $CRELNT, 3 $DELLNM (5 is the startup database push, below) |
| [4:8] | a base address B |
| [8:12] | n, the argument count: 5, 9, 3 |
| [12:12+4n] | the argument pointers, each B-relative (0 = omitted) |

The arguments are the service's own, in its documented order. A descriptor is {u32 length,
u32 pointer}. The item list is {u16 buflen, u16 code, u32 bufaddr, u32 retlen}, ended by a
zero longword: code 3 LNM$_ATTRIBUTES (applying to the strings after it), code 2 LNM$_STRING.
Ground truth checks:
- D7 carries attr 1 (NO_ALIAS) and the item list ATTR 0, ATTR 0x200, "P1", ATTR 0x100, "P2",
  ATTR 0, "P3".
- D11 is a $DELLNM whose lognam pointer is 0.
- D8 is a $CRELNT with promsk 0x8000ee00, which is (S:RWCD,O:RWCD,G:R,W:R).

At startup each VAX creates `LNM$CLUSTER_INTRUSION_TABLE` (under LNM$SYSCLUSTER_TABLE,
kernel), `ICC$REGISTRY_TABLE` and `CLU$ICC_ORBS_<node>` (under LNM$CLUSTER_TABLE, exec),
each as one op 0x04 to every member. That is what fire F1's VAX2 sent OVMXE.

## Not yet grounded (rd vms-94a follow-ons)

- A booting member's database pull. It sends op 0x05 to one member, which pushes the whole
  table set back with op 0x06 as fn-5 records (484 and 2000 bytes here), answered 0x8b/0x04.
  A VAX booting into an OVMX-founded cluster did not send OVMX an op 0x05 in F1.
- OVMX originating these operations, so that its own $CRELNM on LNM$SYSCLUSTER reaches the
  VAXes.
