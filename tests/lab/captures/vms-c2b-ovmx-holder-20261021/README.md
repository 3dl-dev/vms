# vms-c2b + RA7 fire A: OVMX holds a lock in a VAX-mastered tree, and a real VAX hands it the tree

Pod `labc2b` in `ovmx-lab` (own pod, `requests == limits` 1500m/2Gi, image
`ovmx-vaxlab:7`), 2026-10-21 17:11-17:30 UTC, **deleted after the run**. Two
real OpenVMS VAX V7.3 nodes (VAX1 `10.2.1.1` SCSSYSTEMID 1025 CSID `00010001`,
VAX2 SCSSYSTEMID 1026 CSID `00010002`) and one booted OVMX node (OVMXE,
SCSSYSTEMID 1030, CSID `00010003`) on the pod's own `br0`.

Ran as registered in [`PLAN.md`](PLAN.md), committed in `c05f44a03` **before
the OVMX node was booted**. Build under test: `lab/vms-c2b-rmhold` at
`2de669d99`; boot artifacts from `build-boot-artifacts.yml` run `39640670084`,
whose `SOURCE_COMMIT` is that commit. `tests/lab/tools/ovmx_fire.sh` was the
only way in, so the frame-conformance gate passed for `dlm_enq_req` before the
node existed.

**The configuration is the GENERAL one, not the interim**: OVMXE runs
`LOCKDIRWT 0` and both real VAXes run `LOCKDIRWT 1` (read, not assumed:
`F$GETSYI("LOCKDIRWT")` on each console). That is the inverse of every prior
evacuation run, and it is the only arrangement in which a real VAX is both the
tree's directory node and its master.

## Part 1 -- vms-c2b: the lock OVMX holds, read on the VAX master's own database

VAX1 took the tree first with the committed `RMHOLD.MAR`
(`RMHOLD 123WRA7T`: root `RA7T` at CR, three sub-locks `S0..S2` at CW, value
block `RMLVB-0123456789` written on the down-conversion from EX), so **VAX1
mastered it**. Then OVMXE ran `RMHOLD 100-RA7T` -- the new
`tests/lab/ci6/RMHOLD.C`, the OVMX mirror of that same program.

| | VAX1's `SHOW RESOURCE/NAME=RA7T` |
|---|---|
| before OVMX asked | `CSID 00000000` (mastered here), `Lock Count 1`, granted `20000684 CR` |
| after OVMX asked | `CSID 00000000`, **`Lock Count 2`**, granted `160004A7 CR` **and** `20000684 CR` |

and the new LKB, on VAX1's own `SHOW LOCK` ([`sda-vax1.txt`](sda-vax1.txt),
verbatim):

```
Lock id:  160004A7   Granted at CR   00000000-FFFFFFFF
Resource: RA7T  Status: MSTCPY
Master copy of lock 01000349 on system 00010003
```

`01000349` is the lock id OVMXE's own `RMHOLD` printed out of its LKSB, and
`00010003` is OVMXE's CSID. **That line is this item's done-condition**: a real
VAX master's lock database carrying a lock held FOR the OVMX node, named by
OVMX's own handle, on a tree that VAX masters.

### Scoring, prediction by prediction

**P1 -- OVMX asks the right node. PASS.** Two hops, both at real VAXes
([`frames.txt`](frames.txt), trimmed frames 1-6):

```
OVMXE -> VAX2  02/01 RA7T  hash 3c256feb       the DIRECTORY node for the name
VAX2  -> OVMXE 82/01 RA7T  body[34] f8         "not here" -- the redirect
OVMXE -> VAX1  02/01 RA7T                      the MASTER the answer named
VAX1  -> OVMXE 82/01       body[34] fa, body[20:24] a7040016, body[24:28] 49030001
```

VAX2 really is the directory node: its SDA read `Status: DIRENTR`,
`CSID 00010001` before the move ([`sda-vax2.txt`](sda-vax2.txt)). No op-0x01 for
`RA7T` went to any node that is not a member.

**P2 -- the grant is real, on the VAX's own database. PASS.** Above.

**P3 -- the OVMX side agrees, from its own lock database. PARTIAL, and the gap
is reported not papered over.** The lock id OVMXE printed (`01000349`) is the
one the capture carries at body[24:28] and the one VAX1's SDA names, so the
handle is traceable end to end. But `RMHOLD`'s `$GETLKIW` report -- the part
that was to print the granted mode out of OVMX's own lock database -- failed
with `%X00000014` (`SS$_BADPARAM`) for every lock, on every run. OVMX's
`$GETLKI` refuses the six-item list `RMHOLD` asks for. **Filed as its own
item**; the granted mode is therefore evidenced on the real VAX's side only.

**P4 -- the value block crossed. PASS.** VAX1's grant answer carries VAX1's
block and its sequence in the GRANT span: `body[36:52]` ends `...23456789` and
`body[52:56]` is `01000000` -- `RMLVB-0123456789`, Seqnum 1, the block SDA shows
on VAX1.

**P5 -- the sub-locks. FAIL, and it is a real defect, pre-registered as a
possible outcome.** `RMHOLD 123-RA7T` on OVMXE reported `S0`/`S1`/`S2` granted
(lock ids `0100034B/4C/4D`), and VAX1's `SHOW RESOURCE/NAME=S0` kept
`Lock Count 1` -- its own `12000451 CW` and nothing else. The capture says why
(trimmed frames 7-12): OVMX looked each sub-resource up in the DIRECTORY as if
it were a ROOT name, under its own derived hash (`9496ebad` for `S0`), VAX2
answered `body[34] f9` (no entry), and OVMX then mastered it locally. A real VMS
node never looks a sub-resource up: a lock under a parent goes to the PARENT's
master. So OVMX reported a grant no master gave -- a double-mastering hazard in
the general configuration. **Filed as its own p0 item.** The arm continued with
a ROOT-ONLY hold, which is what the rest of this capture rests on.

**P6 -- nothing breaks. PASS.** Zero `bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic`
lines on any of the three consoles (counted, all three logs committed here),
every DCL step returned, and `F$GETSYI("CLUSTER_NODES")` read 3 on both VAXes
during the hold.

**P7 -- the bystander. PASS.** VAX2 held `RA7T` as a directory entry naming
VAX1 as master (`CSID 00010001`), not OVMXE: OVMX holding a lock did not move
the mastery.

## Part 2 -- RA7 (rd vms-6ad1) FIRE A: a real VAX hands OVMX the tree

With OVMXE holding the root CR and nothing else, `STOP HOLD1` on VAX1 ended the
old master's own holder -- the RA2/RA5/RA8 trigger. **~5 seconds later a real
V7.3 node remastered the tree onto the OVMX node**, which is the first time that
has happened anywhere. Roles are exactly the ones `PLAN.md` registered for fire
A: **O = VAX1, N = OVMXE, H = VAX2.**

```
VAX1  -> OVMXE  02/0e  RA7T          "you are the new master"   -> 82/15 by OVMXE
VAX1  -> OVMXE  02/0f  RA7T                                     -> 82/15 by OVMXE
VAX1  -> OVMXE  02/10  RA7T                                     -> 82/15 by OVMXE
VAX1  -> VAX2   02/11  RA7T          the other member is told
VAX1  -> OVMXE  02/13  RA7T          end-of-records (notification)
VAX2  -> OVMXE  02/13  RA7T          end-of-records (notification)
OVMXE -> VAX1   02/14  RA7T          ORIGINATED BY OVMX          -> 82/15 by VAX1
OVMXE -> VAX2   02/14  RA7T          ORIGINATED BY OVMX          -> 82/15 by VAX2
```

Rule W1 held exactly (an op-0x14 to each op-0x13 sender and to nobody else) and
so did rule W2 (both op-0x14s follow both op-0x13s). And the move is on the real
VAXes' own databases, not just the wire: VAX2's directory entry for `RA7T` went
from `CSID 00010001` to **`CSID 00010003`** -- OVMXE -- and VAX1 ended with no
RSB for `RA7T` at all (`%SDA-E-NORSBNAM`).

### RA7's predictions, scored for fire A only

**P1 -- the hand-over reaches OVMX's engine. PASS on every step that occurred;
the op-0x12 leg was NOT EXERCISED.** There were no `0x12` records because no
real VAX held a lock in `RA7T` at hand-over: making OVMX the CHOSEN new master
required VAX1's holder to stop and VAX2 to hold nothing (OVMX does not choose,
and with two real holders the selection could have picked the other VAX). So
this fire exercised the opening steps, both end-of-records, the adoption and the
op-0x14 -- but not a record. Stated as a limitation, not scored as a pass.

**P2 -- the answers are the real ones, by the committed program. PASS.**
`remaster_census.py --pairs | score_remaster` over this capture
([`score.txt`](score.txt)): **18 pairs, 18 byte-exact, 0 mismatched, 0
refused.** That covers OVMXE's own three `82/15` answers and the real VAXes' two
`82/15` answers to OVMX's op-0x14s.

**P3 -- the handles are the engine's. PASS for the grant OVMX served (see P8);
the adoption installed no record handles in this fire** (P1: no records).

**P4 -- THE RESIDUE IS NOT READ. PENDING -- this is fire A only.** Fire A's
op-0x14 carries the ruling's zeros across body[10:12] and body[16:40], and both
real VAXes answered `82/15` echoing the body verbatim -- **including the zero
residue span** -- with only body[8], body[9] and body[34] rewritten, which is
prediction P4c's half of the comparison. The other half needs fire B (the
`0x5A` pattern, roles rotated). **Fire B could not run on this build**: the
`OVMX_RM_RESIDUE_FILL` knob existed in no SYSGEN store and in no factory table,
so `SYSBOOT> SET` answered `%SYSGEN-E-NOSUCHP` -- dead plumbing, found by trying
to use it, fixed in `0a063563b` with a test that reds on the pre-fix store.

**P5 -- no bugcheck, no hang. PASS.** As P6 above, and no VAX DLM or CM request
to OVMXE was left unanswered -- with one honest exception, which is a REFUSAL
and not a silence: `VAX1 -> OVMXE cat 02 op 1a` (twice), which OVMX declined on
the console with `%DLM, refusing a lock message (cat 0x02 op 0x1a) from system
0x10001: it has not proved it runs this implementation`. op-0x1a is a real DLM
opcode this codec does not implement; the two real VAXes send it to each other
too (trimmed frames 33, 36), it opens no transaction (body[4:8] == 0) and is
never answered by anybody, and the remaster completed without it. The one OVMX
got names OVMX's own lock in the tree at body[20:28]. **Filed as its own item.**

**P6 -- the frame is what the grounding table says. PASS for every span the
BUILDER owns; the three spans the CONNECTION MANAGER stamps are described by the
table as the builder leaves them.** Scored with the new committed tool
`tools/cluster/pcap_to_conformance.py` feeding
`tools/cluster/frame_conformance.py`, over the two op-0x14s OVMXE really sent
([`conformance-dump.txt`](conformance-dump.txt)): of the 13 rows x 2 frames,
every row passes except three spans -- abs 72 (`body[0:4]`, the CM's send/ack
envelope), abs 76 (`body[4:8]`, the SCS transaction id) and abs 84
(`body[12:16]`, the `0x0001` + DLM rebuild sequence the CM stamps from club
state). That is 6 failing rows over the two frames; the checker prints 5 of
them because it dedupes identical failure texts and both frames fail abs 84
with the same bytes. All three are spans the codec
header already says no builder owns, and `score_remaster` pairs on body[4:8]
itself. The table cannot gate a WIRE frame until it has rows for them -- **filed
as its own item**; nothing was zeroed to make a row pass.

**P7 -- the pattern did not reach a named field. PASS (fire A).** body[8:10] is
`02 14`, body[40:44] is 0, body[44:48] is `00 00 03 04` (group 0, access mode 3,
name length 4), body[48:52] is `RA7T`, and body[128:132] is `3c256feb` -- the
directory hash OVMX **learned from the cluster's own wire** (the same value
VAX1's and VAX2's own op-0x01 frames for `RA7T` carry), never computed here.

**P8 -- OVMX really serves the tree afterwards. PASS, and this is the strongest
result in the capture.** After the hand-over, VAX2 ran `RMHOLD 100-RA7T` and its
`$ENQ` was answered by OVMXE from the LKBs the adoption installed:

```
VAX2  -> OVMXE  02/01 RA7T   body[20:28] 1802402082030014   (VAX2's EPID + lkid)
OVMXE -> VAX2   82/01        body[20:24] 4f030001  = 0100034F, the handle OVMX minted
                             body[24:28] 82030014  = VAX2's own lkid, echoed
                             body[28] 10, body[32:34] 0001, body[34] fa -- the grant
```

and VAX2's own SDA ([`sda-vax2.txt`](sda-vax2.txt)):
`SHOW RESOURCE/NAME=RA7T` -> `CSID 00010003` (**mastered on OVMXE**),
`Lock Count 1`, granted `14000382 CR`.

**One honest defect this exposed.** The adopted resource's value block is GONE:
VAX2's new grant reads `Valblk: 0`, `Seqnum 00000000`, where VAX1's original
resource carried `RMLVB-0123456789` at `Seqnum 00000001` -- the block OVMX
itself had been granted moments earlier. With no op-0x12 record to install one,
OVMX's adoption did not carry its OWN granted block into the resource it now
masters. **Filed as its own item** (beside rd vms-203, which is the sequence's
other half).

## What is in this directory

| file | what it is |
|---|---|
| `PLAN.md` | the pre-registration, committed before the fire |
| `c2b-ra7a-trimmed.pcap.gz` | 74 of the pod capture's 6,985 frames, selected by `tools/cluster/trim_lab_pcap.py`, payloads unaltered. **Gzipped on purpose**: it carries OVMX-ORIGINATED frames, and the corpus censuses (`tools/cluster/dlm_pid_cell_census.py --selftest` and friends) walk every plain `*.pcap` under `tests/lab/captures` to describe what REAL VMS senders put on the wire. A plain copy moved their figures by +1 and +10 -- i.e. OVMX's own frames would have been counted as a real VAX's. The ci.6 evacuation captures are gzipped for the same reason; the VAX-only oracle arms (RA5/RA6/RA8) are not, and belong in the census |
| `frames.txt` | every frame in that pcap, decoded, in capture order |
| `score.txt` | `remaster_census.py --pairs \| score_remaster` output |
| `conformance-dump.txt` | the OVMX-originated frames in conformance-dump format, and the checker's result |
| `sda-vax1.txt`, `sda-vax2.txt` | every `SHOW RESOURCE` / `SHOW LOCK` on each real VAX, verbatim |
| `console-vax1.log.gz`, `console-vax2.log`, `console-ovmxe.log` | all three consoles for the whole run |

The pcap trim is a RULE, not a hand-picked window, and the rule is a committed
tool (`tools/cluster/trim_lab_pcap.py`): keep every SCS-MSG frame naming `RA7T`/`S0`/`S1`/`S2` or
carrying a remaster opcode, PLUS every frame sharing a kept frame's SCS
transaction id -- which is what keeps the ANSWERS, because a `82/01` grant
overwrites the name span with the resource's value block.

## AND THEN OVMX CRASHED VAX1 -- 2.5 MINUTES LATER, read this next

Nothing above is withdrawn: every measurement in this capture was taken with
all three nodes up, `CLUSTER_NODES` 3 on both VAXes and zero bugcheck lines on
any console. But when the arm ENDED its holder on OVMX, OVMX sent VAX1 a
`$DEQ` (cat 0x02 op 0x03) naming `0x370004A7` -- the master handle VAX1 had
minted BEFORE it remastered the tree onto OVMX -- and **VAX1 took a fatal
INVLOCKID bugcheck with that value in R1**, dumped memory and rebooted; VAX2
then lost quorum.

That is `ovmx-never-crashes-a-peer` broken, and it is release-gating. The crash,
the frame, the causal chain and the contrast with the two earlier harmless DEQs
are their own capture: [`../vms-3ac-invlockid-20261021/`](../vms-3ac-invlockid-20261021/),
rd vms-3ac. Per this arm's own FAIL/STOP rule a real-VAX bugcheck takes its own
item before anything else in this lane moves, so **RA7 fire B was not run**.

## What this changes for the items

* **rd vms-c2b**: DONE by this capture -- part 1, with P5 split out as its own
  defect.
* **rd vms-6ad1 (RA7)**: fire A is DONE and clean. The arm is NOT complete: P4
  needs fire B, which the `0a063563b` fix makes possible for the first time.
* **rd vms-ed5's adoption engine**: exercised against a real V7.3 old master
  end to end, including an op-0x14 a real VAX accepted.
