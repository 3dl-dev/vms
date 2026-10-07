# rd vms-8219 — the lock-directory role a real VAX expects of an OVMX member

With every member at the V7.3 default LOCKDIRWT 0 (rd vms-fcb), Davis p. 6-32
makes every member a directory node, OVMX included. A real VAX therefore sends
an OVMX member directory lookups (op-0x01), entry removals (named op-0x04) and
rebuild registrations (op-0x0d). On the stall rig (`/lab/run-e88/runs/R-15`) the
real VAX's two `F11B$a<VAXCSYS>` lookups to OVMXA went unanswered: RULE C
declined them, and the VAX process asking was left waiting on its lock.

## The oracle

These come from a private three-node OpenVMS VAX V7.3 cluster in which VAX1 was the
only directory node (LOCKDIRWT 3/0/0, L1, full pcap
`/lab/k8s-labs/dlmlab/L1/L1.pcap`) plus the L2 weighted run. Both are described in
`../vms-4fb-dir-hash-20261004/README.md`.

| directory traffic | count (L1) | answered? | shape |
|---|---|---|---|
| op-0x01 lookup, outcome "requester masters it" | 4960 | 0x82/0x01, body[34]=**0xf9** | request echoed; body[0:4], body[8], body[28:39] rewritten |
| op-0x01 lookup, outcome "master is X" | 36 | 0x82/0x01, body[34]=**0xf8** | same, with X's CSID at body[28:32] |
| op-0x01, directory is also master | 8459 | 0x82/0x01, body[34]=0xfa | the master role, **not this rung** |
| named op-0x04, master removes its entry | 4880 | never | tracks the 0xf9 count |
| op-0x0d registration (L2) | 434 | grounded echo (barrier) | all root resources, all addressed to `entry[(hash>>16) mod n]` |

In a real answer, everything in body[28:39] other than the outcome and the CSID
holds the directory node's stale buffer: kernel addresses and text that differ
from one frame to the next. OVMX writes zeros there.

`dirrole-L1L2.pcap` contains the removal and registration specimens (`pick2.py`).
The lookups and answers are in `../vms-4fb-dir-hash-20261004/dirhash-L1.pcap`.

## The lab proof (2026-10-05, own rig `/lab/run-dlm`, a copy of the run-e88 stall rig)

The rig was a real V7.3 VAX (VAXC, LOCKDIRWT 0) plus two booted OVMX members, run with
no stall. After both OVMX nodes reached MEMBER, the VAX logged in and ran `DLMLOOP.COM`:
`$ENQ` EX followed by `$DEQ` on 40 fresh root names (`scenD.sh`).

| build | VAX interactive login | DLMLOOP | VAX lookups to OVMX left unanswered | bugchecks |
|---|---|---|---|---|
| main + vms-fcb (no directory role) | **never completes** (LOGINOUT blocked) | never starts | 4 (`F11B$aVAXCSYS…` ×2, `LNM$CWLOGICALS`, `F11B$a…`) | 0 |
| + vms-4fb + vms-8219 | completes | **40 / 40** `$ENQ`+`$DEQ` | **0** (OVMXA answered 77/77 0xf9, OVMXB every lookup) | 0 |

Without the directory role, a real VAX in a cluster with OVMX members cannot log a user
in. The reason is that its own system-disk allocation locks and cluster-wide logical name
locks are directed at OVMX members, and those members never answered.
