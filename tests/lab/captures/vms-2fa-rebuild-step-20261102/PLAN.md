# vms-2fa: in which barrier step does a survivor send its op-0x08 records?

**Registered 2026-11-02, BEFORE the pod exists and before any byte of this capture is taken.**
Scored by `rebuild_step.py` in this directory, committed with this plan, not by eye.

## The question

When a lock tree's master leaves, every surviving holder re-sends its locks in that tree as
cat-0x02 op-0x08 records (rd vms-874f). The corpus puts all 887 real records after the
release (cat-0x01 op-0x0c, step index body[16]) of step 5 or step 6, each answered before the
next release. OVMX sent one after release 12 and a real VAX bugchecked LOCKMGRERR (rd
vms-bcb). Which of 5 or 6 does lab33a's topology take, and what selects it?

## Step A: the desk diff (`rebuild_step.py --selftest .`)

The barrier frames themselves do not separate the two. Every transition runs 12 steps. A
release carries its step at body[16] and nothing else that is stable: body[20:72] is buffer
residue (device and resource names) that differs from frame to frame. Two things do differ:

| capture | departure | op-0x0d run closed by release | sender's LOCKDIRWT | records after release |
|---|---|---|---|---|
| RA1b | SHUTDOWN REMOVE_NODE | 5 | 1 (equal weights), both survivors | 5 |
| b36 f1 | node killed | 5 | 1 (image default) | 5 |
| 6d3d coldform | formation | none | 1 (image default) | 5 |
| E4 | SHUTDOWN REMOVE_NODE, LDW 0/1/0 | none | 0 (VAX3) | 6 |
| E5, E5b | hung node removed, LDW 0/0/0 | 5 | 0 | 6 |
| ci6 run16 | SHUTDOWN REMOVE_NODE | none | 0 | 6 |
| cab heldout1/2 | formation | none | 0 | 6 |

So:

- **The transition type does not select it.** Clean and fault removals both occur at 5
  (RA1b, f1) and at 6 (E4, E5).
- **The directory-vector change does not select it.** E5 re-registers its directory (op-0x0d
  closed by release 5) exactly as RA1b does, yet its records go after release 6.
- **The coordinator's weight does not select it.** In E4 the coordinator is VAX2 (LDW 1, the
  directory that every op-0x0d of the epoch-3 formation went to), and the records still go
  after release 6.
- **The sender's own LOCKDIRWT separates all 887.** LDW > 0: after release 5. LDW 0: after
  release 6. Membership in the directory does not, because in E5 (0/0/0) every node is a
  directory node (op-0x0d flows both ways) and the records still go after 6.

When the op-0x0d run exists, it always lies between releases 4 and 5. So step 5 is the
directory re-registration step, a weighted node rebuilds in step 6 (after release 5), and a
weight-0 node rebuilds in step 7 (after release 6). In every case the sender holds its own
next barrier frame (participant: its next 01/0b step report; coordinator: its next release)
until its last record is answered.

**H1 (the candidate):** a survivor sends its op-0x08 records after the release of step
`5 if its own LOCKDIRWT > 0 else 6`, and holds its next step report or release until every
record is answered.

**Rivals:**

- **H2:** "5 when the directory vector changes." E5 already contradicts it, and arm 1 tests
  it again.
- **H7:** "5 only when every survivor has LOCKDIRWT > 0." With two survivors, H7 and H1 agree
  on every record that reaches the wire. A weighted sender facing a weight-0 peer is the only
  directory, so its records are local. Separating them needs 3 or more survivors, which is
  not in this plan and not in lab33a's topology.

## The arms

VAX ONLY. No OVMX node, so OVMX emits nothing at a real VAX. Own pod `lab2fa` in `ovmx-lab`
(requests == limits, `ovmx-vaxlab:7`), deleted after the run. vax1 (SYS0) and vax2 (SYS1)
come from the entrypoint. vax3 (SYS2) comes from `tests/lab/tools/oracle_stage.sh node vax3
conv`. The holder is `RMHOLD.MAR` from `vms-50d-ra5-14cells-20261013/`. Each node's
LOCKDIRWT is read on its console with `F$GETSYI("LOCKDIRWT")` before each arm.

- **Arm 1 (lab33a's topology, VAX3 in OVMXE's role): LDW 1/1/0.** At vax3's SYSBOOT> enter
  `SET LOCKDIRWT 0`. VAX1 runs `RMH 110WDEPT`, so it masters DEPT. VAX3 runs `RMH 111-DEPT`,
  which takes root CR and one CR sub, S0. Take SDA SHOW RESOURCE/LOCK on all three nodes.
  Start the capture, then on VAX1 run SHUTDOWN with REMOVE_NODE. Take SDA on VAX2 and VAX3
  afterwards.
- **Arm 3 (E4 control): LDW 0/1/0.** Reboot VAX1 conversationally with `SET LOCKDIRWT 0`.
  Keep VAX3 at 0. Repeat the holders and the departure.
- **Arm 2 (the lever, flipped in the same pod): LDW 1/1/1.** Reboot VAX1 normally (LDW 1).
  Run SHUTDOWN with REMOVE_NODE on VAX3, then reboot it normally (LDW 1). Repeat arm 1's
  holders and departure. This arm runs only if the pod is healthy after arm 3. If it does not
  run, the report says so.

No fault-removal arm. Step A shows the transition type does not move the step.

## Predictions, each scored PASS/FAIL by `rebuild_step.py`

Arm 1 (1/1/0):

- **P1.** Every VAX3 op-0x08 record follows the release of step **6** (`rel_first ==
  rel_last == 6`). H2 predicts 5.
- **P2.** The departure transition carries an op-0x0d run, closed by release 5
  (`dir_step == 5`). VAX1's weight leaves the directory vector.
- **P3.** Every VAX3 record is answered 82/01 before release 7 (`closed`), and VAX3's next
  own barrier frame after its last answer is its step-7 report or release 7 (`held`).
- **P4.** VAX3's DEPT ROOT record goes to VAX2, the only weighted survivor, so its directory.
  The answer is 0xf9: VAX3 becomes the master. No SUB record for S0 is sent, because the sub
  is local to the new master. After the transition, SDA on VAX3 shows DEPT mastered locally
  with the root CR and S0.
- **P5.** VAX2 puts no op-0x08 on the wire. As the only directory, its own records are local.
- **P6.** No node bugchecks (no LOCKMGRERR, CNXMGRERR or INVLOCKID), and VAX2 and VAX3 stay a
  2-node cluster.

Arm 3 (0/1/0):

- **P7.** No op-0x0d in the departure transition (`dir_step == None`). Every VAX3 record
  follows release 6, `closed` and `held`.

Arm 2 (1/1/1):

- **P8.** Every VAX3 record follows release **5**, and so does every VAX2 record if VAX2
  sends any. All are `closed` and `held`. This is the flip: the same pod and the same holders,
  with only VAX3's LOCKDIRWT changed. A record after release 6 here falsifies H1.

If P1 or P8 fails, H1 is wrong and OVMX's sender stays dark (rd vms-bcb's closed `rb_window`).

## What OVMX's sender gate would test if H1 holds

The executive's own LOCKDIRWT (the SYSGEN value it advertises, which is already in its
directory weight vector) selects the release: 5 if non-zero, else 6. The rebuild window opens
on receipt (participant) or emission (coordinator) of that release in the departure's
transition. The node's next step report (participant) or next release (coordinator) is held
until every record is answered. It is never a frame-count constant and never "after the
transition".

## Addendum, registered 2026-11-02 11:20Z, after arms 1 and 3 and before arm 2

Arm 3 (0/1/0, SHUTDOWN) carried no op-0x08. VAX1, at LOCKDIRWT 0, remastered DEPV to VAX3 at
shutdown (op-0x0e..0x14, frames 3855-3873), so there was nothing left to rebuild. It was re-run
as **arm 3b**: the same 0/1/0 weights, with VAX1 removed by stopping its simulator (`^E`).

Arm 2 runs as **0/1/1** with fault removal. A normal reboot of VAX1 came back with LOCKDIRWT 0,
as read by `F$GETSYI`. VAX3 was rebooted conversationally with `SET LOCKDIRWT 1`. Holders: VAX1
`RMH 110WDEPX`, `110WDEPY`, `110WDEPZ`; VAX3 `RMH 111-DEPX`, `111-DEPY`, `111-DEPZ`. VAX1 is
stopped by `^E`. This sharpens the separation:

- VAX1's weight is 0, so the directory vector does not change. H2 predicts release 6.
- H1 predicts release **5**, because VAX3's own weight is 1.

P8 is unchanged: every VAX3 record follows release 5, and each is `closed` and `held`.

## Addendum 2, registered 2026-11-02 11:30Z, before arm 2 fires

The 0/1/1 holders did not stay on VAX1. SDA shows DEPX, DEPY and DEPZ each mastered on VAX3,
with VAX1's locks held as process copies (CSID 00010007). A departing VAX1 would therefore leave
no tree to rebuild. A conversational SET appears to persist: VAX1 came back at 0 after its arm-3
conversational boot. So VAX1 is rebooted conversationally with `SET LOCKDIRWT 1`, which makes
arm 2 **1/1/1**, the RA1b weights, in this pod. Holders: VAX1 `RMH 110WDEPQ`; VAX3
`RMH 111-DEPQ`. VAX1 is removed by `^E`. P8 is unchanged: every VAX3 record follows release 5.
Set against arm 3b (0/1/0, fault removal, release 6), the only difference is the weights.
