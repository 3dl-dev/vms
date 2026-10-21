# vms-3ac: OVMX crashed a real VAX -- INVLOCKID from a $DEQ naming the old master's handle

**This is `ovmx-never-crashes-a-peer` broken, in the lab, which is where it has
to be found.** Pod `labc2b` in `ovmx-lab`, 2026-10-21, build
`lab/vms-c2b-rmhold` at `2de669d99` (vms-380's codec + vms-ed5's adoption engine
+ this lane's `RMHOLD`). The hand-over that preceded it is the capture next
door: [`../vms-c2b-ovmx-holder-20261021/`](../vms-c2b-ovmx-holder-20261021/).

## What the real VAX did

`console-vax1-bugcheck.txt`, verbatim from VAX1's own console:

```
**** Fatal BUG CHECK, version = V7.3     INVLOCKID, Invalid lock id

    Crash CPU: 00        Primary CPU: 00
    Current process = NULL

	R0 = 00002124
	R1 = 370004A7
	...
	PC = 87BCA0FC
	PSL= 04080001
```

then a memory dump, `?06 HLT INST`, and a reboot. VAX2 followed with
`lost connection to system VAX1`, `removed from VAXcluster system VAX1`,
`quorum lost, blocking activity`.

## The frame OVMX sent, and why R1 is in it

`R1 = 370004A7` is a lock id, and it is OVMX's frame that carried it. Frame
`#14091` of the pod capture, `OVMXE -> VAX1`, **cat 0x02 op 0x03 -- a $DEQ**:

```
body[ 16] 00 00 00 00 a7 04 00 37 4e 03 00 01 00 00 01 00
                      ^ body[20:24] = 0x370004A7   ^ body[24:28] = 0x0100034E
```

* `0x370004A7` is the master handle **VAX1 itself minted** for OVMX's root lock
  in tree `RA7T`, back when VAX1 still mastered that tree (its grant answer
  carried `body[20:24] = a7040037`).
* `0x0100034E` is OVMX's own lock id for the same lock.
* By the time this DEQ arrived, **VAX1 had no RSB for `RA7T` at all** -- its own
  SDA said `%SDA-E-NORSBNAM` -- because ~2.5 minutes earlier VAX1 had
  **remastered the tree onto OVMX** (the RA7 fire-A hand-over). An unresolvable
  lock id is what `INVLOCKID` is for.

The two earlier DEQs in the same run were harmless, and the contrast is the
whole diagnosis (`deq-frames.txt`):

| frame | OVMX's DEQ names | mastered by, at that moment | outcome |
|---|---|---|---|
| `#2562` | `0x160004A7` | VAX1 | fine |
| `#4214` | `0x2B000375` | VAX1 | fine |
| `#4503..#4549` | -- | **the tree moves to OVMXE** (0e/0f/10, two 0x13s, OVMX's two 0x14s) | fine |
| `#14091` | `0x370004A7` | **OVMXE** -- VAX1 masters nothing here any more | **VAX1 bugchecks** |

## The causal chain, every step off committed evidence

1. VAX1 mastered `RA7T`. OVMX's `$ENQ` was granted and OVMX's **proxy LKB**
   recorded VAX1 as the master with handle `0x370004A7`.
2. VAX1's own holder stopped and VAX1 **remastered the tree onto OVMX**. OVMX
   masters `RA7T` from then on (VAX2's SDA: `CSID 00010003`).
3. OVMX's own lock in that tree **stayed a proxy pointing at VAX1**. Nothing in
   the adoption converted this node's own proxy LKBs for the adopted resource
   into local master-side LKBs.
4. The holder image was aborted; rundown released that lock; the proxy release
   path did what a proxy release does -- sent a DEQ **to the recorded master**,
   naming the handle that master had minted before the tree moved.
5. VAX1 bugchecked.

## Why this is release-gating and not exotic

It fires on the **first release of any lock this node held in a tree it has
since been handed** -- which is the ci.6 evacuation sequence itself: a workload
process exits after its tree has been remastered. Nothing unusual was done to
provoke it; the arm simply ended its holder.

## What a fix has to do (for the item, not decided here)

The repair belongs in the **adoption**, not in the DEQ path: when the engine
installs the master-side RSB for a tree this node is handed, every LOCAL lock
this node already holds on that resource must stop being a proxy -- its master
is THIS node now and its release is local. Independently, the release path
should refuse to address a master CSID the node's own mastery state no longer
supports: fail-honest, never fatal-to-the-peer.

## Files

| file | what it is |
|---|---|
| `console-vax1-bugcheck.txt` | VAX1's console from the SDA line before the crash through the reboot, verbatim |
| `invlockid-trimmed.pcap.gz` | 518 of the pod capture's 14,825 frames: everything the OVMX node was party to, plus every frame naming `RA7T`, by the rule in `trimrule.py`. Payloads unaltered |
| `deq-frames.txt` | the three DEQs and the remaster between them, decoded |
| `trimrule.py` | the trim rule, so the selection is reproducible |
