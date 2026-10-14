# vms-c55: what the frames should show — registered BEFORE the capture is read

Written 2026-10-14 18:30Z, before openvmx-site runs **38614087787** (`b-first`) and
**38615151215** (`all-at-once`, `c-first-legacy`) finished. Both are preflight+capture runs
of `release-verify-v0.7-10` with every 0x6007 frame recorded, from branch
`work/demo-capture-cases`.

## What is already measured (not predicted)

- The crash is `LOCKMGRERR` on the real VAX at **PC 80A07362** with the same register
  signature in both occurrences — `R0=1, R1=0x84 (132), R4=80828780, R7=0x34, R8=8082B000` —
  differing only in `Current process` (NULL in 38578886393 page-order, STARTUP in
  38605304613 b-first). **R1 = 132 is the SYSAP body length**, so the VAX is very likely
  holding a received 132-byte message when it bugchecks.
- The pcjs TX byte-deletion is fixed and every OVMX node is V0.7-10, so the corrupt-frame
  explanation is gone.
- Across the eight cases of the clean run, `%DLM, refusing a lock message…` and the crash
  correlate **1/1** (b-first only). Over both crashing runs the refusal is **2/2**.
- `%DLM, a directory lookup arrived for a resource this node's own weight vector does not
  direct here: answered from this node's entries` appears in **3** of the clean run's eight
  cases and in only one of the two crashes, so it is **not** common to both.
- The two OVMX nodes are mutually "ours": the software-version token is
  `OVMX_CLUSTER_SW_VERSION "VMX V0.7"`, which tracks MAJOR.MINOR only, so V0.7-7 Node B and
  V0.7-10 Node A advertised the same token. RULE C refusals in the demo are therefore
  refusals of **the real VAX's** messages, never of the other OVMX node's.

## Predictions

- **P1.** The last frame VAXC receives before it stops transmitting is from an OVMX node and
  is a **cat-0x02 or cat-0x82 DLM frame**, not a cat-0x01 connection-manager frame.
- **P2.** The frame OVMX refused (the console line) is from VAXC and is one of the opcodes
  no OVMX path serves: **0x05 (BLKAST), 0x0d (rebuild record), 0x0e, or a remaster
  0x10/0x11/0x12/0x13**. It is NOT 0x01/0x03/0x06/0x07, which the directory and mixed-held
  paths do serve.
- **P3.** VAXC sends **nothing** between the refusal and its death: the refusal is not what
  it is reacting to, because a VMS node that gets no answer waits (rd vms-db1, rd vms-52d —
  measured hangs, never crashes). What kills it is a frame OVMX **sent**.
- **P4.** In the window, OVMX sends VAXC at least one frame whose (cat, op) is NOT covered by
  `tests/cluster/conformance/frame-grounding.tsv` — the table has rows for eight types
  (`cm_0b01_ans cm_commit cm_lnm_ans cm_lnm_dbreq cm_model dlm_dir_remove mscp_cmd
  scs_conreq_mscp`) and the DLM **directory answer** (0xf9/0xf8), the **grant/deny** reply
  and the op-0x01/0x07 **requests** are all absent from it.
- **P5.** If a directory answer is in that window, it carries status **0xf9 "you master it"**
  for a name OVMX holds no entry for — the `no entry -> the requester becomes the master`
  arm of `vms_dlm_dir_lookup` — and in the crashing case OVMX was NOT the directory node its
  own weight vector names for that resource.

## What each outcome means

- P3 **confirmed** and P4/P5 confirmed → the defect is an OVMX-originated frame, and the
  first suspect is an assignment OVMX had no authority to make (answering "you master it"
  from an empty table for a resource it does not direct). Fix: never mint a mastery
  assignment for a name this node is not the directory for; not answering is the honest
  floor, and it is what a misaddressed lookup already gets when the table is full.
- P3 **falsified** (VAXC transmits after the refusal, then dies) → the refusal IS in the
  causal chain and the question becomes what the VAX does when its own lock message is
  dropped, which no capture in this lane has yet shown.
- P1 falsified (the last frame is cat-0x01 or a HELLO) → the trigger is not a lock message
  at all, and the register dump's `R1=132` reading is wrong.

## How it will be read

`lastframes.py <frames.jsonl> VAXC 40` prints the frames up to VAXC's last transmission and
dumps, in full, every 132-byte body addressed to it in that window. Each OVMX-originated
body is then checked field by field against `frame-grounding.tsv` where a row exists, and
against the per-op census (`tools/cluster/`, the lab corpus) where one does not.

## P6, added 18:33Z — still before the capture was read

The refused opcode is most likely a **remaster message (0x10/0x11/0x12/0x13)**, and
b-first is the order that produces one.

Reasoning, all of it from code and captures already in hand:

- The receive door handles, above RULE C: op 0x0f (acked), op 0x0d (the barrier owns the
  grounded echo -- 227 and 117 of them were answered to VAXC in a NON-crashing b-first,
  so 0x0d is not the refused frame), the directory role, and the master role for a lock
  this node already holds for that system. Below it: everything else.
- `src/kernel-core/vms_cluster_codec_dlm.h` defines no 0x10, 0x11, 0x12, 0x13 or 0x14
  opcode at all, and nothing in `src/` outside `vms_lock.c` calls the adoption engine
  (rd vms-380 progress note, verified on 277c57ccf). So a real VAX's remaster messages
  reach the door, match no branch, and are refused -- the one message class OVMX is
  guaranteed to drop.
- A dropped ENQ or lookup makes a VMS node WAIT (measured twice: rd vms-db1, rd vms-52d).
  A dropped remaster record is different in kind: the sender has already moved mastery of
  that tree and is waiting for the handover to complete. A lock manager with a half-moved
  tree has an internal inconsistency, and LOCKMGRERR is what it raises for one.
- `R1 = 0x84` (132) in both crashes is the SYSAP body length, consistent with the VAX
  bugchecking while holding a received 132-byte lock message.

If P6 holds, the P0's fix and rd vms-380 are the same work, and the minimum honest step is
that OVMX must not silently drop a remaster message: either it takes part (vms-380 rungs
a-c, answering each 0x12 with the handle its own engine minted) or it declines the role in
a way the sender can act on. Which of those is available depends on what the capture shows
the VAX doing next, and on whether any real capture grounds a refusal.

## P7-P9, added 18:35Z — measured baseline, and two more hypotheses, still before reading

**Measured, from the NON-crashing V0.7-10 b-first capture (run 38582830937):**
`tools/cluster/capture_conformance.py` says OVMXB sent VAXC **652** frames and OVMXA
**256**; of those, **1** and **4** are of a type `frame-grounding.tsv` covers. The rest --
651 and 252 -- are of types with **no grounding row at all**, the largest classes being
`82/01` (306 across both nodes) and `82/0d` (227). Every one of those 306 `82/01` frames
carries **body[34] = 0xf9, "YOU MASTER IT"**: zero grants, zero redirects. So the dominant
thing OVMX says to a real VAX's lock manager is an assignment of mastery, and the
conformance gate checks none of it.

- **P7.** In the crashing run OVMX also sends VAXC a stream of `82/01` `0xf9` answers, and
  the fatal frame is one of them or the frame immediately after one.
- **P8.** The fatal `0xf9` names a resource for which the VAX (or the other OVMX node)
  already holds a master -- the two-master hole from the directory side. Checkable: the
  same resource identity appears earlier in the capture in a lookup answered by someone
  else, or in an op-0x0d rebuild record naming another master.
- **P9.** The fatal answer is sent **while a cluster transition is in flight**. Davis
  p. 6-33 has a real cluster DISCARD its directory information at Phase 1; OVMX's
  `dlm_arm_dir_lookup` has no transition check and answers from its own table throughout,
  so an answer that was true before the transition can be asserted during one. Checkable:
  bracket the frame between the cat-0x01 transition frames in the same capture.

P8 and P9 are not exclusive: the second is a mechanism by which the first becomes true.

## Scored before the crashing capture: the directory answer is NOT the suspect

Measured while the fires ran, on the NON-crashing V0.7-10 b-first capture and the 20,366
op-0x01 / 6,175 directory-answer corpus in `tests/lab/captures/*/*.pcap`:

- **The re-ask is not a tell.** VAXC re-sends one (name, requester handle) lookup up to 36
  times at OVMX, which looked like "the answer is not being accepted". Real VAX-to-VAX
  traffic does the same thing: 20,366 lookups over 6,225 distinct (name, handle) pairs, one
  of them re-sent **3,000** times. OVMX's 36 is inside the real distribution. **Hypothesis
  dropped.**
- **The zeroed cells are residue-shaped, and P8/P9's premise weakens.** In 5,161 paired real
  request/0xf9-answer exchanges, `[28:32]` equals the request's bytes in **10** of them, so
  it is not an echo; grouped by sender-and-recipient inside ONE capture it takes **295
  distinct values**, one of which is `0x8797e880` -- an S0 kernel address in the same
  `87xxxxxx` range as the op-0x14 residue RA6 measured. `[30]`, `[32:34]`, `[35]` and
  `[36:40]` behave the same way. So OVMX zeroing them is what the rd vms-42de ruling
  prescribes for residue, and a real VAX swallowed **306** such answers in one non-crashing
  run. The directory answer is therefore an unlikely trigger -- and it stays the largest
  UNGROUNDED thing OVMX says to a real lock manager, which is a separate problem.

That leaves **P6** (a refused remaster message) as the standing hypothesis, and the refusal
itself as the only signal that correlates 2/2 with the crash.

# ============================ RESULT ============================

The crash reproduced WITH frames: openvmx-site run **38616543784**, case `b-first`, tag
V0.7-10, `capture=true` (the second of three fires; the first and third did not crash).
`VAXC.console.log` has the bugcheck, `crash-frames-204.jsonl` every 204-byte frame of the
run (373 of 1,245; the rest are HELLOs and sub-204 traffic).

## The whole DLM conversation of that run is four frames

```
3.701  OVMXA -> VAXC   02/04   directory-entry REMOVAL, name RMS$4F.1.0, hash 650412f2
3.713  VAXC  -> OVMXA  02/19   an opcode that occurs NOWHERE in this repo's corpus
3.730  OVMXA -> VAXC   02/01   a lookup for RMS$4F.1.0, hash 650412f2 -- the SAME resource
                               it has just told the VAX to forget -- with body[20:24] = 0
3.743  VAXC  -> OVMXA  82/01   body[34] = 0xfe
       ...VAXC transmits once more and bugchecks LOCKMGRERR, PC 80A07362...
```

## Scoring

- **P1 CONFIRMED.** The last DLM frames are cat-0x02/0x82, not connection-manager frames.
- **P2 PARTLY CONFIRMED, and better than registered.** The refused message is `02/19`, an
  opcode this codec does not implement -- and a census of every committed pcap finds
  **0 occurrences of cat-0x02 op-0x19 anywhere**. It is a message the VAX sends only to
  OVMX, 12 ms after OVMX's directory removal.
- **P3 CONFIRMED.** VAXC sends nothing between the refusal and its death except the `82/01`
  answer to OVMX's own request. It is not reacting to silence.
- **P4 CONFIRMED, and measured.** `tools/cluster/capture_conformance.py`: of 908 frames the
  two OVMX nodes sent VAXC in one run, **5** are of a type `frame-grounding.tsv` covers.
  Neither the directory answer, the rebuild echo, nor the op-0x01 request has a row.
- **P5/P8/P9 DROPPED, measured, before the crash capture was read** (see the section above):
  the zeroed cells of OVMX's directory answer are residue-shaped in the real corpus (295
  distinct values between one sender/recipient pair, one of them an S0 kernel address), and
  a real VAX swallowed 306 of OVMX's answers in a non-crashing run.
- **P6 NOT CONFIRMED.** No remaster opcode (0x10-0x14) appears in the crashing run.
- **P7 FALSIFIED.** There is no stream of 0xf9 answers in the crashing run; the whole run
  carries four DLM frames.

## What the corpus says about the shape OVMX used

`body[20:24]` is the MASTER's lock id (`VMS_OFF_DLM_MASTER_LKID`, frame 92), not the
requester's -- the requester's is `body[24:28]`, and OVMX filled that one correctly. A
request with `body[20:24] = 0` is a **bare directory lookup**: "I do not know who masters
this."

Over every committed pcap, 20,366 op-0x01 requests:

| sender | addressed to a known master | **bare lookup (master = 0)** |
|---|---|---|
| real VAX | 20,307 | **15 (0.07%)** |
| OVMX | 7 | **37 (84%)** |

and the answers they draw:

| request shape | answers |
|---|---|
| addressed | `fa` x12,106, `f9` x5,131, `f8` x1,007, `fb` x39, `fc` x15 -- **never `fe`** |
| bare lookup | `f9` x32, **`fe` x10**, `fa` x3, `f8` x1 |

**`0xfe` answers a bare lookup and nothing else** -- 10 of 10 occurrences in the corpus --
and the crashing run's last answer is a `0xfe`.

## The two OVMX-side facts this names

1. **OVMX asks where a real VAX tells.** A real VMS node resolves the directory itself and
   addresses the master; the bare lookup is 0.07% of its traffic and 84% of OVMX's. The
   one reply status that exists only for that shape is the one the VAX sent before dying.
2. **OVMX removed a directory entry and then immediately asked about the same resource.**
   Same name, same hash, 29 ms apart. The removal says "forget this, I no longer master
   it"; the lookup asks that same directory to resolve it again. A directory database
   mid-delete being asked to resolve the deleted entry is an inconsistency of exactly the
   kind `LOCKMGRERR` reports.

Not yet known, and NOT to be guessed: what `02/19` is, and whether the VAX dies on its own
cleanup of the removal or on the lookup that followed it. The next arm should re-fire
b-first with the refusal diagnostic that names the opcode (PR #1714) and with the removal
suppressed, which is a one-line experiment that separates the two.
