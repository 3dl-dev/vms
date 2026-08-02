# Handoff — `vms-2f3`: OVMX cannot rejoin a cluster it was removed from

**Written 2026-08-01. Read `docs/HANDOFF-vms-760.md` §0 first for the orchestrator
doctrine — it held again this session: every finding below came from agents or
from OVMX's own logs, and the orchestrator read no packet bytes at all.**

> **START AT §4e — it is the newest. §4d is the session before it and is still
> current; together they supersede §5's ordered plan, which has been executed
> through step 3.** §4e also records the `workshop` host bring-up: the lab did
> not exist there, and five blockers had to be cleared before a single run.
>
> **⚠ THE LAB IS AT `/data/training/vax`, NOT `~/vax`** — a compatibility
> symlink makes every hardcoded path in the 23 lab scripts resolve, so do not
> edit them.
>
> **⚠ THERE IS NOW A SECOND LAB, AND IT IS NOT THIS ONE.** `vms-a5c` landed
> `tests/lab/` on main (2026-08-02): a k3s StatefulSet where **one pod is one
> complete, isolated 2-node VMScluster**, cloned from the 3-node golden snapshot,
> with its own `br0`+taps inside its own pod netns. Three replicas are running.
> Scale with `kubectl -n ovmx-lab scale sts/vaxlab --replicas=N`.
>
> **What this changes for THIS investigation: nothing, deliberately.** Every
> control and negative in §1–§4e was bracketed on lab-1, and lab-2 runs a
> *different SIMH binary* (same open-simh commit `2e0d51e`, built for x86_64
> instead of the aarch64 binary lab-1 carries). **Do not mix lab-1 and lab-2 runs
> inside one comparison, and do not re-run a §3 killed hypothesis on lab-2 and
> treat the result as new evidence.** Lab-2 is for the *other* `vms-ci` items
> that were queued behind this one — `vms-ce7`, `vms-ac4`, `vms-ci.4` — and for
> any fresh line of attack whose controls are built on lab-2 from the start.
> The wire comparison that was run, and its explicit limits, are in
> `tests/lab/README.md` — it is not a byte-level fidelity proof.
>
> **§5's ordered plan is now FULLY EXECUTED — step 4 was run in §4e.4 and is
> refuted as the gate. Do not re-propose it.** §1–§4c exist so you do not re-derive
> them, in particular §3 (things that look like the answer and are not). §0 and
> §4b are kept as written on 2026-08-01 morning and are **partly superseded**:
> the join limp they describe is real but is now FIXED by `OVMX_PURE_SERVER=1`,
> and the rejoin survives that fix. §7 guardrails 18–19 are the most
> transferable thing here.

---

## 0. Status in one paragraph

**The rejoin is still refused — but the session ended somewhere better than a
narrower version of the same question. Read §4b first: OVMX's join has been
limping all along, and the rejoin is just the case where the limp stops
working.** A returning OVMX gets the full START handshake, the directory phase,
the VC bind, the config exchange, the coordinator's **`op 0x03` COMMIT** and its
**`op 0x05` lock-rebuild** — and then the coordinator never opens the barrier,
for as long as we care to wait (measured to 420 s), and all three peers log
`timed-out lost connection`. The decisive finding is that **the coordinator
stalls on OVMX in successful joins too**, and what rescues those is OVMX
retransmitting its own `op 0x02` at ~+8 s — something no real node ever does. A
real transition is over in 42–132 ms. Two genuinely frozen wire fields were
found and fixed on the way (`c302b7d`, verified live, no regression), and nine
hypotheses were killed. **A four-minute reproducer now exists that needs no lab
reset.**

| | state | run |
|---|---|---|
| fresh identity joins | **works, ~20 s** | `fresh1`, `fresh2` |
| rejoin, same identity | **REFUSED** | `ctl1`, `inc1`, `inc2`, `rej2`, `rej3` |
| quarantine is time-bounded | **REFUTED** — survives 15 h | `ctl1` |
| frozen incarnation is the cause | **REFUTED** — fixed, still refused | `inc1` |
| frozen message-time is the cause | **REFUTED** — fixed, still refused | `inc2` |
| `cat 0x01 op 0x04` is the cause | **REFUTED** — present in successes | log census |
| our window was too short | **REFUTED** — a real transition takes 42–132 ms | `rej3`, af2 |
| the bug is in the commit→barrier window | **REFUTED** — a real node sends *nothing* there | af2 |
| **OVMX's successful joins are healthy** | **REFUTED — they are rescued by our own retry** | `fresh1` vs af2 |
| the coordinator stalls before commit | **REFUTED — it acks in 0.4-0.7 ms. The silence is OURS** | `g1A`/`g1B`, §4c.1 |
| `Ref. time` (`own_admission`) is the cause | **REFUTED — real joiners send the same sentinel** | census, §4c.4 |
| the `amsg=0` / bundled `op 0x02` is the cause **of the limp** | **CONFIRMED — pure mode removes it; first reference-shaped join** | `r1A` vs `r2A`, §4c.2b |
| …and is the cause **of the rejoin refusal** | **REFUTED — pure mode joins clean and STILL cannot rejoin** | `r2A`/`r2B`, §4c.2b |
| which peer completes START first | **REFUTED — both orderings in successes and failures** | §4c.5 |
| a class-0x03 crash removal quarantines the identity | **REFUTED — a real VAX3 crash-rejoins in ~90 s** | §4c.2c ⭐ |
| a rejoin's `op 0x02` looks like a first join's | **REFUTED — 4 fields differ, 0 residuals; OVMX always sends the first-join form** | §4c.2e ⭐⭐ |
| the abort is a timeout | **REFUTED — `op 0x04 role 0x50` fires 1.2 ms after our last correct echo** | §4c.2f |
| the coordinator holds a stale CSB it compares us against | **REFUTED — VAX3 held none for us, allocated our CSID, aborted anyway** | §4d.1 |
| …and `[22:24]`/`[28:36]` are inferred | **NO LONGER — SDA names them `Found Node SYSID` / `Founding Time`** | §4d.1 ⭐ |
| **sending the rejoin form of `op 0x02` admits us** | **REFUTED — matched control, identical refusal with the form disabled** | §4d.2 ⭐⭐ |
| every refusal is the same failure | **REFUTED — TWO shapes; one never opens a transition at all** | §4d.6 ⭐⭐ |
| we address the wrong node (`Curr. coord.` rotates) | **REFUTED both ways — forcing the real coordinator still refused; a fresh identity joins via a non-coordinator** | §4d.7 ⭐ |
| a returning identity is refused | **SHARPENED — it is DROPPED, by every peer, on receipt, before any machinery runs** | §4d.8 |
| the VAXes have no oracle for their non-decisions | **REFUTED — SCACP, ANALYZE/ERROR_LOG and SDA SHOW CONNECTIONS all exist and were never used** | §4d.9 ⭐⭐⭐ |
| **the disk-discovery run is the gate (§5 step 4)** | **REFUTED — ungated, it fires on a rejoin and is IGNORED; matched kill-switch control** | §4e.4 ⭐⭐⭐ |
| we fail to INITIATE the joiner's disk run | **REFUTED — we now initiate it; the peer discards our SCS$DIRECTORY connect exactly like our `op 0x02`** | §4e.4 ⭐⭐⭐ |
| **the peer's SDA can name why it refused us** | **REFUTED — `Rej/Disconn Reason` is 0 on every CDT, on the refusal too** | §4e.3 ⭐⭐ |
| the peer holds the same connections either way | **REFUTED — a refused rejoin leaves `VMS$DISK_CL_DRVR` in `con_sent` and `SCS$DIR_LOOKUP` in `disc_sent`; a join has `MSCP$DISK` + `SCS$DIRECTORY` OPEN** | §4e.3 ⭐⭐ |
| the bug is host- or arch-specific | **REFUTED — reproduces identically on x86_64 `workshop`, bracketed 3 joins / 2 refusals** | §4e.2 |
| the two outcomes look alike below the CM layer | **REFUTED — a refused rejoin's VC is in congestion collapse; a successful one is indistinguishable from a real VAX** | §4d.9 ⭐⭐⭐ |
| the PEDRIVER collapse is an effect of the CM stall | **REFUTED — it precedes our `op 0x02` by ≥3 s; a fresh identity never degrades at all** | §4d.10 ⭐⭐⭐ |
| OVMX hears the peers' CM retransmissions | **REFUTED — `msgtype 0x7b` was discarded before any handler ran. FIXED, and it did NOT admit us** | §4d.10 ⭐⭐⭐ |

## 1. The reproducer — four minutes, no reset

This is the single most useful thing this session produced. Earlier work needed
`reset3.sh` (~6.5 min) plus a full `cycle.sh` run; this needs neither.

```bash
cd /home/baron/projects/vms
CL=/home/baron/vax/cluster
ONE=$CL/tools/oneshot.sh

# 1. a brand-new identity joins in ~20 s  (positive control — ALWAYS run it)
$CL/tools/mk_sysgen $CL/work/sysgen-X.dat OVMXX1 1230
bash $ONE ctlA $CL/work/sysgen-X.dat 130     # expect: CLUSTER_NODES=4, XITDONE=1

# 2. the SAME store, ~45 s later, is refused
bash $ONE ctlB $CL/work/sysgen-X.dat 130     # expect: waitnodes FAILED, XITDONE=0
```

`oneshot.sh` runs one join against the lab **as it stands**, captures to
`work/d94-<tag>.pcap`, logs to `work/scsd-<tag>.log`, and finishes with an SDA
`SHOW CLUSTER` dump into `work/<tag>.status`. Every fresh identity still joins
no matter how many dead CSBs have piled up, so the lab does not saturate the way
the 2026-07-30 session feared — but do reset before a run you intend to publish.
(Re-confirmed 2026-08-01 pm: `OVMXR1` and `OVMXR2` both joined in <35 s on a lab
carrying several days of dead CSBs. **If a fresh identity is refused, suspect
your harness, not the lab** — see §4c.2b.)

> ### ⚠ PASS AN ABSOLUTE STORE PATH
> `oneshot.sh` `cd`s to the OVMX repo root, so a **relative** `sysgen` path
> silently resolved to nothing and SCSD fell back to the default node name
> `"OVMX"` — five "distinct fresh identities" all went on the wire as one node
> and three results were invalid before a failing control exposed it (§4c.2b).
> Both holes are now fixed (SCSD is fatal on an unreadable named store;
> `oneshot.sh` refuses to run without a readable one), but **verify the identity
> reached the wire anyway**:
> ```bash
> strings -a work/d94-<tag>.pcap | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u
> ```

**Run the positive control immediately BEFORE each negative — not once per
session — and confirm which identity it controlled for.** Three of this
session's hypotheses would have survived without controls, and one *survived
wrongly* because the control was stale.

## 2. The one instrument that mattered

`SCSD-T-CMIN` logs **every** inbound CM message (suppressed by `OVMX_CM_QUIET`;
leave it unset). Counting inbound `(cat, op)` from OVMX's own log is cheaper
than any capture analysis and it is what localised the failure:

```
fresh2 JOINED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | op 0x05 x5 |
                op 0x0c x12 | cat 0x81 op 0x0b x12 | op 0x06 x253 | cat 0x02 op 0x0d x176
inc2  REFUSED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | op 0x05 x5 | then NOTHING
rej2  REFUSED:  op 0x14 x3 | op 0x01 x3 | op 0x03 COMMIT | no op 0x05 | then NOTHING
rej3  REFUSED:  identical to rej2 over a 420 s window
```

```bash
grep -ao 'SCSD-T-CMIN, cat 0x[0-9a-f]* op 0x[0-9a-f]*' work/scsd-<tag>.log \
  | sed 's/SCSD-T-CMIN, //' | sort | uniq -c | sort -rn
```

The peer-side oracle is SDA, not DCL — `ANALYZE/SYSTEM` → `SHOW CLUSTER`. DCL
`SHOW CLUSTER` renders an *empty table* when peers hold stale CSBs, so the
oracle degrades before it errors.

## 3. Killed hypotheses — do not re-chase any of these

1. **Time-bounded quarantine.** `cyc2` waited 420 s; `ctl1` waited **15 hours**
   on a lab left standing overnight. Still refused, with the identical
   `wait`/`long_break` CSB. There is no wait that works.
2. **The frozen incarnation timestamp.** Real, fixed, *not* the gate — §4.
3. **The frozen message timestamp.** Same: real, fixed, not the gate.
4. **`cat 0x01 op 0x04` is a rejoin-only probe we fail to answer.** A capture
   agent proposed this as the root cause and it is wrong. `cat 0x01 op 0x04
   txn=0 csum=0 smsg=11 amsg=9` arrives **identically in the successful join**
   (`scsd-fresh1.log` 11:35:37.767) and the failed one (`scsd-inc1.log`
   11:25:14.525) — same sender `08:00:2b:11:22:33`, same 190-byte frame — and
   also in `cyc1-c1`, `rm2`, `dep1/2/7/10`. The "mid-dialogue re-send of `op
   0x01`" is likewise present in successes (4 in `fresh1`, 4 in `inc1`). Both go
   unanswered in runs that reach MEMBER. **Do not write a handler for `op 0x04`
   on this evidence.**
5. **The `[22:24]` incarnation counter echo.** Correct in every failed run: the
   peers advertised 3, 4, 5, 6 across attempts and OVMX echoed each exactly.
6. **SCS sequence numbers.** Both sides restart at 1 on every attempt; the
   member's stale round-0 `send_seq` is receive-tolerated as spec §4(i).A
   requires. Not the failure.
7. **Our `op 0x02` retry.** Required and working — it is what triggers the
   coordinator's second round in a successful join. Keep it.
8. **Our test window being too short.** `rej3` ran 420 s: same census, nothing
   after the lock-rebuild.
9. **Changing one half of the identity pair.** `OVMXNU`/1222 (new name, dead
   sysid) and `OVMXC2`/1225 (dead name, new sysid) are both refused — but VSI
   *OpenVMS Cluster Systems* App. A documents that `SCSSYSTEMID` and `SCSNODE`
   may not be changed independently without rebooting the whole cluster, and
   both fail in a **different place** (never reach `STARTDONE`; 80 `STARTRX`).
   These are documented-illegal configurations, not discriminators. Do not cite
   them as evidence about the rejoin.

## 4. What DID land — two frozen fields, and the spec was wrong about one

Commit `c302b7d`. Neither fixes the rejoin; both are real bugs, both are now
honest, and one of them was one step from crashing a real VAX.

**`[66:74]` of the `0x41` START is THIS SYSTEM'S INCARNATION** — a full VMS
absolute-time quadword, not the three fields the spec described. OVMX shipped
the captured template's `bb 8e 67 7a 94 00 bc 00` = **26-JUL-2026 14:35:33.59**
on every boot, forever. Grounded four independent ways:

- SDA on VAX1 rendered OVMX's CSB as `Incarnation 26-JUL-2026 14:35:33` — the
  template bytes to the second.
- The same dump gives real peers their own boot times: VAX3 `1-AUG-2026
  00:02:21`, VAX1 `30-JUL 08:54:26` (it had not rebooted).
- After the fix VAX1 read back `Incarnation 1-AUG-2026 15:25:12`, matching the
  `0x00bc05526906b4a1` we emitted (the VAX clock runs 4 h ahead of the host).
- **Public doc**, VSI *System Management Utilities Ref. Vol. II*, SHOW CLUSTER
  SYSTEMS class: *"INCARNATION: Unique 16-digit hexadecimal number established
  when the system is booted."* Sixteen hex digits is this quadword.

> **Spec correction owed:** `cluster-protocol-spec.md` splits this into a 5-byte
> token `[66:71]`, a flag `[71]`, and a "constant `0x00bc`" `[72:74]`. It is one
> quadword. Those upper bytes only look constant because every 2026-era VMS
> timestamp ends `bc 00`.

**`[98:106]` is a second quadword — when the frame was composed.** Real peers
carry two or three distinct values inside a single capture, and one of VAX3's
matches to the second the OPCOM line it printed as it built the frame. Ours was
frozen at 26-JUL-2026 14:48:50, so every START OVMX ever sent claimed to have
been written six days earlier. Now live per frame. **Its role is not grounded
and the code does not claim one**; what is grounded is the negative — no real
node sends a stale one.

**Why this mattered beyond tidiness:** VSI *Cluster Systems* App. C.7.1
documents that a connection reestablished after `RECNXINTERVAL` without the node
having rebooted earns a **CLUEXIT bugcheck** on the surviving side. A node whose
incarnation never changes is exactly that node.

**Still frozen, deliberately not touched this session (one variable at a time):**
`Ref. time` — SYSAP body `[64:72]` of the 190-byte cat-`0x01`/op-`0x01` message
(`own_admission`, `scs_member.c`). OVMX ships the `1-JAN-2001` sentinel because
the live value is gated behind `is_member`. Real nodes carry their admission
time (VAX3 `1-AUG 00:04:40`, matching its *"is now a VAXcluster member"* console
line to 50 ms). **This is the next honesty fix and it is a candidate cause** —
see §5.

Kill-switches so the failing case stays reproducible:
`OVMX_INCARNATION_FROZEN=1`, `OVMX_MSGTIME_FROZEN=1`; `OVMX_INCARNATION_TIME`
pins the quadword. 41/41 tests green, with byte-exact assertions on both fields.

## 4b. THE REFRAME — OVMX's join has been limping all along

This landed last and it changes the shape of the problem. The af2 rejoin
specimens were decoded end to end (real identity `VX3`/1050, first join at SCA
2558, rejoins at 20170 and 33591), and compared against OVMX's own captures.

**A real transition is over in milliseconds.** From add-member request to the
last finalize frame: first join **42 ms**, rejoin-1 **132 ms**, rejoin-2
**88 ms**. From first HELLO to full barrier completion, all three are under
**2 seconds**. There is no "minutes" regime anywhere in the reference — our
130–420 s windows are 100–1000× longer than a transition ever needs.

**A real node sends `op 0x02` exactly once**, in all three transactions, and is
answered in **1–20 ms**. There is no retry cycle in the reference at all, and no
retransmission in either direction anywhere between commit and barrier-open —
every `op 0x06` in the coordinator's 257-frame lock-directory burst has a unique,
strictly increasing `smsg`, with zero gaps or repeats.

**What the returning node sends in the commit→barrier window: nothing.** All 341
frames in that window are coordinator→node, 204 bytes, `cat 0x01 op 0x06`. Flow
control is carried in-band in `smsg`/`amsg`; there is no separate ACK frame. The
returning node's only job there is to receive. So the §0 framing — "what does a
returning node owe the coordinator in that window" — has an answer, and the
answer is *nothing*. **The bug is not in that window.**

**And here is the reframe.** OVMX *always* stalls, including on joins that
succeed:

- `d94-fresh1` (**succeeded**): OVMX sends `op 0x02` once at +1.46 s → commit +
  lock-rebuild → coordinator sends `op 0x04` and a re-sent `op 0x01` → **goes
  silent for ~6.5 s**. OVMX answers neither. At **+8.0 s OVMX retransmits its
  own `op 0x02`** — the coordinator answers *that* in 2.2 ms, redoes commit and
  lock-rebuild, and this time proceeds into the 253-frame burst → barrier →
  MEMBER.
- `d94-inc1` (**failed rejoin**): identical through commit, lock-rebuild,
  `op 0x04`, re-sent `op 0x01`, silence. OVMX retries `op 0x02` at +7.76 s —
  **the coordinator never answers.**
- `d94-rej2` (**failed rejoin**): fails earlier still — commit arrives, the
  lock-rebuild round never does, retry at +7.18 s unanswered.

So OVMX's successful join is **rescued by its own retransmit** — a step no real
node ever performs. The rejoin does not introduce a new defect; it removes the
accident that was papering over an existing one. **Something OVMX does at or
before commit makes the coordinator stall, and a fresh identity forgives it
while a returning identity does not.**

This also re-frames `op 0x04` a third time. It is **not** rejoin-specific — a
full sweep of all 103 pcaps found 89 occurrences, all in 3-node captures, all
coordinator-to-all-peers broadcasts, zero in any 2-node capture. But it and the
re-sent `op 0x01` are exactly what the coordinator emits **immediately before it
stalls on us**, and a real coordinator never sends either to a real joiner. They
are plausibly the coordinator *asking us something because we already confused
it*. Grounding them is now interesting again — as a symptom, not as the gate.

## 4c. SESSION j (2026-08-01 pm) — §4b's reframe was measuring the wrong window

**Read this before §5. It supersedes §5 items 1 and 2 and corrects §4b.**

Two agents (a pre-commit byte-diff, and a grounding census of `Ref. time`) plus
four lab runs. Net: **three more hypotheses dead, including the one §5 named as
the next fix, and one of them died to a four-minute experiment after 197k tokens
of byte analysis argued for it.**

### 4c.1 The coordinator does NOT stall before commit. The silence is OURS.

§4b said "the coordinator stalls on us". Measured, in both a success and a
failure, from OVMX's own `SCSD-T-CMIN` timestamps:

| | OVMX `op 0x02` → coordinator's `cat 0x04` ack | → `op 0x03` COMMIT |
|---|---|---|
| `g1A` (**joined**) | **0.4 ms** | **1.1 ms** |
| `g1B` (**refused**) | **0.7 ms** | **2.0 ms** |

The coordinator answers instantly in *both* cases and then runs the lock
rebuild. What comes next is `cat 0x01 op 0x04` + a **re-sent `op 0x01`** — and
*then* the ~6.5 s of quiet. That quiet is **OVMX's**: the coordinator has
re-advertised and is waiting for a reply we owe it, and we produce one only when
a retry timer fires. Our own instrumentation has been saying so all along —
`SCSD-W-STRAYACK, ... arrived 6907 ms ago -- the reference acks within ~1 ms`.

**Do not describe this as a coordinator stall again.** The window to explain is
`op 0x04` + re-sent `op 0x01` → our reply, and we are the one who is late.

### 4c.2 ⚠ RETRACTED — this section was wrong, and the reason matters more than the claim

> **An earlier revision of §4c.2 claimed `OVMX_PURE_SERVER=1` was refuted by
> experiment. THAT WAS AN ARTIFACT OF A BROKEN HARNESS. The opposite is true:
> pure mode produces the first reference-shaped join OVMX has ever made.** The
> original text is kept below, struck, because the failure mode is instructive.
>
> **The harness bug.** `oneshot.sh` `cd`s to the OVMX repo root before exec'ing
> SCSD, and I passed **relative** store paths. `OVMX_SYSGEN_PATH` never resolved,
> and `resolve_node_identity()` **silently fell back to the hardcoded default
> `"OVMX"`**. Five runs meant to be five distinct fresh identities all went on
> the wire as **one node**. `grep -c OVMXC9 d94-c9A.pcap` = **0**. So every
> "fresh identity positive control" after the first was really a *rejoin of the
> same identity* — i.e. it was reproducing `vms-2f3` exactly while I read it as
> "pure mode fails" and then as "the lab has saturated". Both readings were
> wrong. The peers were never confused; they read precisely what we sent.
>
> **Both defects are now fixed** (this commit): `resolve_node_identity()` is
> fatal when `OVMX_SYSGEN_PATH` names a store it cannot read — a wrong node name
> on a live cluster wire is the INV-6 silent-fallback class that CLAUDE.md rule 9
> exists to kill, one layer up from `/dev/vms` — and `oneshot.sh` resolves the
> store to an absolute path and refuses to run without it.
>
> **Guardrail 14 caught this, and only just.** The rule says run the positive
> control in the same session as the negative one. I ran one at the *start* and
> not after, so three invalid runs accumulated before the failing control exposed
> it. **Strengthened: run the positive control immediately BEFORE each negative,
> and verify the identity actually reached the wire** —
> `strings -a work/d94-<tag>.pcap | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u`.
> A control that does not prove *which node* it controlled for is not a control.

### 4c.2b THE VALIDATED RESULT — pure mode fixes the join, and not the rejoin

Re-run with absolute paths, controls verified on the wire, all within 8 minutes
on one lab:

| run | identity | mode | result |
|---|---|---|---|
| `r1A` | `OVMXR1` (fresh) | default | **joined**, 32 s |
| `r2A` | `OVMXR2` (fresh) | `OVMX_PURE_SERVER=1` | **joined**, 27 s |
| `r2B` | `OVMXR2` **again**, +3 min | `OVMX_PURE_SERVER=1` | **REFUSED** |

And pure mode's join is **shaped like a real node's for the first time**:

| | `r1A` default | `r2A` pure |
|---|---|---|
| coordinator's `cat 0x01 op 0x04` re-advertisement | **present** | **absent** |
| peer console `proposed addition of node …` | **twice**, 9.5 s apart | **once** |
| joiner directory + MSCP disk-discovery run (`PSCLIENT`) | **0** | **33** |
| `SCSD-W-STRAYACK` 6-second-late replies | 9 | 7 |

**§4b's reframe is therefore CONFIRMED and simultaneously ANSWERED.** The join
*was* limping — the coordinator re-advertised and we replied on a retry timer —
and the limp is gone in a mode that has existed in the tree all along. The
coordinator proposes our addition **once** and it sticks, exactly as it does for
a real node. Agent A's §4c.8 checklist was right on every point; what was wrong
was my test of it.

**And the rejoin is still refused, with a valid control.** That is now a clean
separation rather than a caveat: **making the join reference-correct does not
make the rejoin work.** `vms-2f3` is not "our join is malformed". It is
coordinator-side state keyed on our identity, and §4c.3's byte-identical retry
frames say the same thing from the other direction.

**Consequences for the next session:**
1. **`OVMX_PURE_SERVER` should become the default path** — it is the grounded
   one, it removes the retry-rescue accident, and it runs the disk discovery a
   real joiner runs. That is a design change and owes the CLAUDE.md cascade; it
   is filed rather than flipped here, because `vms-2f3` should not be the item
   that silently changes the join path. See `vms-785`.
2. **§4c.8 is no longer an unexplored lead — it is implemented and it runs.**
   `PSCLIENT` fires 33 times in pure mode and the sequence completes. What is
   *not* established is whether the disk-discovery run is what removed the
   `op 0x04` re-advertisement, or whether the `op 0x02` shape did. Two variables
   moved together. Bisecting them is one cheap run each.
3. **Re-test the rejoin against the pure-mode baseline**, not the default one.
   Every earlier rejoin datum was taken on the limping path.

<details><summary>Original (wrong) §4c.2 text, kept for the reasoning</summary>

The pre-commit diff agent produced a well-evidenced root cause: OVMX bundles
`op 0x02` into the opening burst (three frames in one microsecond, leaving the
wire **0.3 ms before the coordinator's own `op 0x14` arrives**), so its
`body[2:4]` ack-msg is necessarily 0. `(smsg=3, amsg=0)` occurs **104 times,
exclusively from OVMX, and zero times in 196 real specimens**. A real joiner
sends two frames, waits, then sends `op 0x02` as `(smsg=3, amsg=2)` to the
coordinator alone. All of that is **true and grounded**.

It is also **not the gate.** `OVMX_PURE_SERVER=1` already implements exactly that
shape — 2-frame burst, `op 0x02` deferred, coordinator-only, correct `amsg`. Run
`p1A`, fresh identity, no code change:

```
12:26:00.269  CMCONFIG, add-member burst (2 frames)      smsg 1,2  amsg 0
12:26:07.144  CMCONFIG2, DEFERRED op 0x02  send_msg=4 ack_msg=2   <- reference shape
              ... and the coordinator NEVER ANSWERS IT. No ack, no COMMIT, nothing.
              census: op 0x14 x3, op 0x01 x3. That is all. XITDONE=0.
```

**The reference-correct `op 0x02` draws *less* response than the malformed one.**
A fresh identity that joins reliably in ~20 s does not join at all in pure mode.
So fixing `amsg`/ordering as specified does not unstick the dialogue, and the
checklist built on it (hold `op 0x02`, gate on `peer_max_smsg ≥ 2`, coordinator
only) is **already implemented and already tested and does not work**.

Caveat kept honest: pure mode changes several things at once, so this refutes
"the pure-server shape fixes the stall", not the narrower claim "`amsg` matters".
But the narrower claim now has no path to a fix that we have not already run.

</details>

### 4c.2c ⭐ THE MISSING SPECIMEN NOW EXISTS — a real VAX rejoins after a crash

The `vms-2f3` rd notes have said since 2026-07-31 that **no reference specimen
anywhere models a crash-detected (class-0x03) removal followed by a rejoin** —
`af2`'s departure was a *graceful* `REMOVE_NODE` (class 0x04, which runs no
barrier at all). That was the single biggest RE gap on this item. **It is closed.**

Made 2026-08-01 16:33–16:37, captured end to end:

```
16:33:42  kill -9 on VAX3's SIMH process  -- a HARD CRASH, no shutdown, no
          REMOVE_NODE message. EXACTLY how OVMX dies.
16:34:05  VAX1: timed-out lost connection to system VAX3
          VAX1: proposing reconfiguration of the VAXcluster
          VAX1: removed from VAXcluster system VAX3
          VAX1: completed VAXcluster state transition      <- class 0x03 removal
~16:35:0x VAX3 rebooted -- SAME SCSNODE "VAX3", SAME SCSSYSTEMID 1027
16:35:36  VAX3 speaking again (OPCOM traffic reaching VAX1)
16:37:39  SHOW CLUSTER on VAX1: VAX3 MEMBER
```

**A real VMS node crash-removed under an unchanged identity rejoins in ~90 s.**

Three things this settles at once:

1. **The lab is not the problem** and the CSB residue is not a permanent poison.
   The peers retire or reuse a crashed node's state as a matter of course.
2. **"Crash removal quarantines the identity" is DEAD** as an explanation. It is
   the ordinary VMS path, and it works — for a real node.
3. **The gap is ours**, and for the first time we can see what the correct
   behaviour looks like on this exact path rather than inferring it from a
   graceful-departure specimen that runs different code.

**The specimen:** `captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(4.2 MB). Compare against `work/d94-r2A.pcap` (OVMX `OVMXR2` joins, pure mode)
and `work/d94-r2B.pcap` (**same identity, 3 min later, refused**, pure mode) —
a matched success/failure pair on the good join path, which no earlier rejoin
evidence had.

**This is where the next session's effort belongs.** The question is now
concrete and has a reference answer in hand: *what does VAX3 do across a crash
and reboot that OVMX does not?*

### 4c.2d A free peer-side oracle: plain DCL renders the CSB state

Once several dead OVMX identities exist, DCL `SHOW CLUSTER` on VAX1 stops
rendering an empty table and starts naming them with their CSB states:

```
| VAX1   | VMS V7.3 | MEMBER  |
| VAX3   | VMS V7.3 | MEMBER  |
| OVMXR1 | VMX V0.1 | BRK_NON |   <- joined, killed, never retried
| OVMXR2 | VMX V0.1 | BRK_NEW |   <- joined, killed, rejoin ATTEMPTED and refused
```

**`BRK_NON` vs `BRK_NEW` is a real signal, not noise.** A refused rejoin moves
the CSB from `BRK_NON` to `BRK_NEW`: the peer **did** re-accept us into the NEW
state and we then broke while there — we are not being ignored, we are being
admitted-and-stranded. Consistent with §4c.6's SDA reading (`open`, CSID
`00000000`, no status flags, a numbered transition that never advances).
Cheaper than SDA and needs no `ANALYZE/SYSTEM`.

**And the state transition is causally confirmed, and the refusal is NOT
cumulative.** Run `r1B`: `OVMXR1` had been dead ~95 minutes and sat in a clean
`BRK_NON` — the state a freshly crash-removed node occupies, with no failed
attempt against it. Its **first** rejoin attempt was refused identically, and the
CSB moved `BRK_NON → BRK_NEW` as a direct result:

```
before r1B:  OVMXR1  BRK_NON      after r1B:  OVMXR1  BRK_NEW
```

So `BRK_NEW` is **not** a self-inflicted trap left by an earlier failed retry —
a first attempt against a pristine `BRK_NON` fails the same way, and the poison
is a consequence of the refusal rather than its cause. Two hypotheses die here:
"our first failed rejoin poisons subsequent ones" and (again, at 95 min) "the
gap was too short".

### 4c.2e ⭐⭐ THE DISCRIMINATOR — a rejoin's `op 0x02` is a different shape

**This is the strongest candidate this item has ever had. It came out of the new
crash-rejoin specimen, and I verified it independently of the agent that found
it, with my own byte scan.** Admission-frame `cat 0x01 op 0x02`, SYSAP body
offsets (`body[n]` = absolute frame offset `72+n`):

| capture | sender | `[20:22]` | `[22:24]` | `[28:36]` | `[36:40]` |
|---|---|---|---|---|---|
| `vax3-2to3` #285 — VAX3 **first join** | real | 0 | 0 | zero | 0 |
| `formation-ci1` #67 — VAX2 **first join** | real | 0 | 0 | zero | 0 |
| **crash-rejoin #1297 — VAX3 rejoin** ⭐ | real | **1** | **1025** | `004af82e3605bc00` | **9** |
| `af2-established-rejoin` #2923 | real | **1** | **1025** | `c01f673a9400bc00` | **2** |
| `af2-firsttimer` ×3 rejoins | real | **1** | **1025** | same | **2** |
| `e81-bystander-ADDITION` #2969 | real | **1** | **1025** | `e05a627a4b03bc00` | **3** |
| `r2A`, `r2B`, `r1B`, `760-MEMBER`, `e4b` | **OVMX** | **0** | **0** | **zero** | **0** |

**Zero residuals: 6 captures, 2 real joiner nodes, 3 lab generations. OVMX sends
the first-join form on every rejoin.**

What the fields are (grounded except where noted):

- **`[28:36]` is a CLUSTER-scoped founding timestamp, not our incarnation.** The
  same quadword `004af82e3605bc00` appears in the members' own `op 0x01`
  `body[28:36]`, and in the class-0x03 removal `op 0x08` that removed
  **OVMXR2** 90 minutes earlier — a node with a completely different
  incarnation. One value per lab generation. *(The agent's first reading was
  "VAX3's previous incarnation"; its own cross-check refuted that. Good.)*
- **`[22:24]` = `0x0401` = 1025 = VAX1's SCSSYSTEMID**, in both labs, where VAX1
  is CSID `00010001`. INFERRED: the founding node's system ID.
- **`[20:22]` = 1** — a boolean "I have prior cluster state".
- **`[36:40]`** — 9 / 3 / 2 across specimens. **Not determined.** Not the epoch,
  not a CSID.
- Real `op 0x02` also carries twelve `0x20` spaces at `[40:52]`; we send zeros.

**We already receive everything we need.** `r2B` #55 (VAX3 → OVMX `op 0x01`)
carries `body[28:36] = 00 4a f8 2e 36 05 bc 00` — the exact quadword the real
VAX3 quotes back. We are handed the founding time and discard it.

**Why this survives every refuted hypothesis.** The coordinator gets an
`op 0x02` claiming "no prior cluster state" while holding a CSB that says
otherwise. That one rule covers the whole corpus: fresh identity + first-join
form + no CSB → admitted; returning identity + first-join form + CSB → abort;
real node's rejoin + rejoin form + CSB → admitted in 50 ms. It also explains
§4c.3 (byte-identical frames, opposite outcomes — the deciding input is our
*constant* claim measured against the peer's *varying* state) and §3.1 (no wait
works — nothing decays).

**NOT PROVEN.** Passive capture cannot show what the coordinator evaluates. What
is proven is that the fields exist, that they separate first-join from rejoin in
every real specimen with no exceptions, and that OVMX gets them wrong every
time. **It is a candidate with a four-minute test, not a demonstrated gate — and
this item has burned three confident root causes already. Run the test.**

**Counter-evidence owed:** in `r1B` the coordinator was VAX3, booted 5 minutes
earlier, with no first-hand memory of OVMXR1 (removed at 15:04) — and it aborted
anyway. Either it inherited the dead CSB during its own admission, or the
mechanism is elsewhere. **SDA on VAX3 immediately after it rejoins, listing its
CSBs, decides this and costs nothing. Do that before building on the story.**

### 4c.2f GROUNDED — the abort message, located to the millisecond

`cat 0x01 op 0x04` **`role 0x50 class 0x02`** is the transition ABORT, broadcast
to all participants. In `d94-r1B` it fires **1.2 ms after our last correct
`op 0x05` echo** — not a timeout; the coordinator decides on state it already
holds, right after the lock rebuild and *instead of* the `op 0x06` burst. Console
confirms: `16:40:35.29 Node VAX3 aborted VAXcluster state transition`.

Census: **zero occurrences in any other capture in the corpus.** Role `0x50` is
not in spec §4(r)'s role table.

**This does NOT contradict §3.4.** The `op 0x04` that appears in successful joins
is `role 0x00 class 0x00` on a *non-`VMS$VAXcluster`* Con.ID — a different
SYSAP's opcode 4. §3.4's "do not write a handler for `op 0x04`" is correct for
that one and does not apply to role `0x50`. **We are currently blind to the
abort**: the run just looks stuck. Logging it is free and makes every future run
legible — do it first.

Bonus, and it clears a suspect: **`op 0x05` names its subject node** —
`body[20:24]` = SCSSYSTEMID, `body[28:36]` = that node's incarnation. OVMX's echo
(`r1B` #318 = `d8 04` = 1240 = OVMXR1, plus OVMXR1's live incarnation, exact) is
**byte-correct**, differing from the request only at `body[0:4]` and `body[18]`
`00→01` — the identical mutation the real VAX3 applies. Not a suspect.

### 4c.3 The refusal is coordinator-side state — proven, not inferred

The agent byte-diffed OVMX's retry `op 0x02` in the run that **succeeded**
against the one that was **refused**: they are **byte-identical** (`0b 00 0c 00`
envelope, `01 02`, `body[16:18]=0x0200`, rest zero). Same frame, opposite
outcome. **Whatever decides the rejoin is not in anything we transmit at that
moment.** Combined with §3.1 (no wait works, 15 h tested), that closes the
"find the wrong byte" family of hypotheses for this step.

### 4c.4 `Ref. time` is GROUNDED and is NOT the bug — §5 item 2 is dead

§5 named this as the next fix. It is wrong, and the evidence is clean.

`op 0x01` body `[64:72]` = `00 80 4a 3f 0e 57 9f 00` = `0x009f570e3f4a8000` =
exactly **1-JAN-2001 00:00:00**, and **that is what a real VMS node sends while
it is being admitted.** Census over every `op 0x01` in the library, split by
*role, not vendor*:

- real VAX3 `08:00:2b:11:22:33` — the sentinel **69×** (as a joiner), live 2026
  values as an established member
- real VAX2/VX3 `08:00:2b:78:56:b9` — sentinel **20×**
- VAX1 (never a joiner in this library) — **never** the sentinel, always live
- `VX3`/1050 sends the sentinel on its first join **and both rejoins**, and is
  admitted every time

The flip is exactly simultaneous with `body[12]` `0x00→0x21`. No specimen ever
shows a member-form frame with the sentinel or a joiner-form frame with a live
value. Independently corroborated by SDA (`sda-scs-extract-vax1.txt`: VAX1's
`Ref. time` = the CLUB's `Founding Time`; VAX2's = the CLUB's `Last time stamp`).

So `Ref. time` is the reporting node's **own admission** timestamp, OVMX's
`scsd.c:2818-2822` already computes it correctly, and the sentinel it ships as a
joiner is right. **Do not "fix" this field.** A full 8-byte-window scan of the
132-byte body found **no other** timestamp-shaped value: only `[28:36]`,
`[36:44]`, `[64:72]`, all three already named and handled. That closes the
`op 0x01` half of the `vms-70c` honesty debt.

Same sweep on OVMX's `op 0x01` vs a real joiner's: **byte-identical except two
bytes**, `body[44]` (0 vs 3 — "transitions seen since boot"; VX3 sends 0 on its
2nd incarnation and joins) and `body[84]` (already grounded as per-boot noise in
§4(j)). There is nothing left to find in this message.

### 4c.5 Also dead: "which peer completes START first"

Both orderings appear in successes *and* failures — `fresh2` succeeded with
`78:56:b9` first, `g1B` failed with it first; `g1A` succeeded with `11:22:33`
first, `inc2` failed with it first. Free census over the archived logs.

### 4c.6 New peer-side oracles (cheaper than any capture)

**The VMS console counts the rounds.** In a **success**, VAX3 logs
`proposed addition of node OVMX` **twice, 6.7 s apart** — the second driven by
our retry. In a **refusal**, **exactly once**, then
`aborted VAXcluster state transition`. One `grep` of `/tmp/clean-vax1-test/vax*.log`
tells you which regime you are in, with no pcap at all.

**SDA polled *during* the stall** (§5 item 3, now executed — `g1B`, polls at
+15/+35/+60/+95/+140 s):

```
87A1F540  OVMX    00000000    0     open    <NO STATUS FLAGS>
Flags: 31080001 cluster,init,qf_failed_node,quorum,transition
Last transaction code 03 | Last trans. number 7 | Last coordinator CSID 00010001
```

**Nothing moves.** Identical at +60, +95 and +140 s. A transition is open for our
admission, numbered, coordinated by VAX1, and it simply never advances — and
nothing on the peer side times it out. (The `lost connection` at the end of every
run is *us* being killed, not the peers giving up.)

**Peers never retire dead OVMX CSBs.** `OVMXC2`, `OVMXFR` and `OVMXG2` CSBs all
coexist on VAX1 simultaneously, all `state 09 wait`, most `long_break`. The
per-identity residue accumulates without bound.

### 4c.7 Confirmed working: the incarnation fix (`c302b7d`)

VAX1's CSB for a run started at host 12:25:57 reads `Incarnation 1-AUG-2026
16:25:59` (the VAX clock runs 4 h ahead). Live, to the second, no regression.

### 4c.8 Grounded but unexplored: OVMX skips the joiner's directory + MSCP run

The 1.4–4.4 s a real joiner spends between `op 0x01` and `op 0x02` is **not idle**
(§4(o) currently implies it is). `af2-firsttimer` frames 20212–20243: the joiner
opens **its own** `SCS$DIRECTORY` (it does not reuse the member's), looks up
`MSCP$TAPE` and `MSCP$DISK`, opens `MSCP$DISK`, runs 2× SET CONTROLLER
CHARACTERISTICS and the full GET-UNIT-STATUS NEXT-UNIT walk, and tears the
directory connection down. **OVMX does none of it, ever** — its only outbound
CONN-REQ to the coordinator is the `VMS$VAXcluster` one.

Not proven to be a gate (`vax3-2to3#285` has an all-zero topology body and is
acked in 0.3 ms, so the MSCP walk is not *encoded* into `op 0x02`). But it is the
largest single behavioural difference left between OVMX and a real joiner, and it
is the one thing a real node does in the window we are late in. **This is the
strongest remaining lead — see §5.**

---

## 4d. SESSION k (2026-08-01 evening) — the ordered plan, executed steps 0–3

**Read this before §5. §5's ordered plan is now DONE through step 3; what
survives it is step 4 and the new material below.** Commit `e687c6b`.
Five lab runs, one variable each, a verified control immediately before and
after. No agents were used and no packet bytes were read by anything but the
decoder scripts.

### 4d.1 ⭐ Step 0 paid off twice — and VMS names the fields itself

SDA `ANALYZE/SYSTEM` → `SHOW CLUSTER` on VAX3, taken 28 minutes after its
crash-rejoin, and on VAX1 for comparison. Both render the Cluster Block as:

```
Found Node SYSID     000000000401
Founding Time          1-AUG-2026
                         12:03:09
```

and `004af82e3605bc00` — §4c.2e's `op 0x02` `body[28:36]` from the crash-rejoin
specimen — decodes as a VMS absolute-time quadword to **exactly 1-AUG-2026
12:03:09.60**. `0x0401` = 1025 = `[22:24]`.

**So §4c.2e's two "INFERRED" fields are grounded against the peer's own oracle:
`[22:24]` is the CLUB's founding-node SCSSYSTEMID and `[28:36]` is the CLUB's
founding time.** That also kills the alternative reading (a node incarnation)
for good: it is a cluster-scoped fact with one value per lab generation.

**And we can identify the founding node from what members already send us.**
No frame names it, but the founding node is the one member whose *own admission
time* (`op 0x01 body[64:72]`, SDA's CSB `Ref. time`) **equals** the founding
time — because founding the cluster is its admission. Over `d94-r1B`: three
members, exactly one match (VAX1/1025), zero residuals, and SDA on two separate
nodes independently says `Found Node SYSID 0x0401`. This is now implemented and
it fired live in run `s1A`:
`SCSD-I-CLUFOUND, node 1025 FOUNDED this cluster`. Learned, never computed.

**The counter-evidence §4c.2e owed is settled, and it goes against the story.**
VAX3 booted at 16:35 and coordinated `r1B`'s refusal at 16:40. Its CSB list
holds `OVMXR1` (created *by* that attempt, CSID `00010008`, its `Index of next
CSID` advanced to `0009`) and **no CSB at all for `OVMXR2`**, which died before
VAX3 booted. VAX1 holds both, plus `OVMX`, all `CSID 00000000`. So VAX3 did not
inherit dead CSBs at its own admission, held nothing about OVMXR1, **allocated
us a CSID, and aborted anyway** — and all three consoles log the same three
lines in the same hundredth of a second, with no objection from VAX1 or VAX2.
The coordinator was not comparing our claim against its own memory of us.

**The decision point is now exact.** Same round in, opposite verdict out:

| | after the `op 0x05` lock-rebuild round | |
|---|---|---|
| `r2A` (**joined**) | `op 0x09` transition-open | **+27 ms** |
| `r1B` (**refused**) | `op 0x04` role 0x50 ABORT | **+1.2 ms** |

In `r1B` the coordinator sends OVMX five `op 0x05` — one naming each of the four
nodes plus the newcomer again — and VAX1/VAX2 one each naming only the newcomer.
**Every participant echoes correctly, OVMX included.** Then it aborts.

### 4d.2 ⭐⭐ Steps 2–3 shipped, and a matched control refuted them

`op 0x02` now takes the rejoin form when we hold a prior-admission record for
this identity *and* the cluster we meet carries the founding time we were
admitted to. Persisted beside the SYSGEN store (that store IS the identity the
claim is about). `OVMX_REJOIN_FORM=0` forces the old shape.

| run | identity | mode | result |
|---|---|---|---|
| `s1A` | `OVMXS1`/1242 **fresh** | pure | **JOINED, 27 s**; record written; no abort |
| `s1B` | `OVMXS1` again, +95 s | pure, **rejoin form** | **REFUSED** |
| `s1C` | `OVMXS1` again, +6 min | pure, **`REJOIN_FORM=0`** | **REFUSED, identical census** |
| `s2A` | `OVMXS2`/1243 **fresh** | pure | **JOINED, 27 s** (closing control) |

`s1B`'s `op 0x02` is byte-correct on the wire — `[20:22]=1 [22:24]=1025
[28:36]=1-AUG-2026 12:03:09` — and **drew no answer at all**. So did `s1C`'s
first-join form. Both stop at `op 0x14 ×3 | op 0x01 ×3`.

**Sending the shape a real crash-rejoining VAX sends does not get us admitted.**
That is the fourth confident root cause this item has killed, and §4c.2e called
it in advance: *a candidate with a four-minute test, not a demonstrated gate.*
The test was run. The answer is no.

**Kept anyway** (guardrail 15). Real rejoins send this form, OVMX sent the
first-join form on every rejoin it ever attempted, and that was wrong
independent of what it explains. Conditional, kill-switched, and pinned by
byte-exact tests in both directions.

> **⚠ Note what `s1B`/`s1C` do NOT license.** Both refusals died *earlier* than
> `r1B` did — no COMMIT, no lock-rebuild, no abort, so `CM-XITABORT-RECEIVED=0`
> and the new instrument had nothing to log. Do not read that as "the refusal
> moved earlier"; read it as *the refusal has more than one shape* (§2 already
> shows `rej2` without `op 0x05` and `r1B` with it). The **only** claim these two
> runs support is the one they were designed for: with one variable moved
> between them, the form makes no difference.

### 4d.3 Step 1 shipped, unexercised

`SCSD-E-XITABORT` logs `cat 0x01 op 0x04` **role 0x50** (matched on the role
slot — role 0x00 is a different SYSAP's opcode 4 and appears in successful
joins, §3.4), and the run summary reports `CM-XITABORT-RECEIVED=N`. It did not
fire in `s1B`/`s1C` because no abort was sent. **It is still worth having**: it
is the difference between "the coordinator declined us" and "nothing happened",
and until now those were indistinguishable in our logs.

### 4d.4 A new honesty bug, grounded and filed — `vms-c9f`

Every VMS quadword OVMX emits is **UTC**, and VMS absolute time is **local**.
`scs_member_vms_time_now()` adds the 1858→1970 offset to `time(NULL)` and never
converts. Peers render our incarnation exactly TZ-offset hours in the future:
`r1B` started at host 16:40:33 EDT and both VAX1 and VAX3 hold
`Incarnation 1-AUG-2026 20:40:33`, while the real nodes in the same dump sit at
08:54 / 12:05 and the CLUB's founding time is 12:03 — all cluster-local. Visible
independently in the `op 0x05` round, which names each subject and its
incarnation: three VAXes in the 12:05 era and OVMX alone at 20:40.

**Not the gate** — five first joins carrying the same 4-hour-future incarnation
were admitted normally. It is `vms-70c`-class honesty debt. Note it would be
invisible on a UTC host, so "cannot reproduce" is expected there.

### 4d.9 ⭐⭐⭐ THE VAXes DO HAVE DIAGNOSTIC LOGGING, AND IT NAMES A NEW LAYER

**Every peer-side oracle this investigation had used — OPCOM, SDA `SHOW
CLUSTER`, DCL `SHOW CLUSTER` — reports connection-manager STATE. None of them
reports a DECISION, and nobody had checked what else VAX/VMS 7.3 offers.** It
offers three things, all present in this lab and all previously untouched:

| oracle | what it gives | how |
|---|---|---|
| **`SCACP`** (SCA Control Program) | **PEDRIVER's own view of the VC and its channels** — state, channels open, ECS membership, transmit window, retransmit timeout, transmit-timeout count, per-VC error count | `MC SCACP` → `SHOW VC`, `SHOW CHANNEL` |
| **`ANALYZE/ERROR_LOG`** | `NI-SCS SUB-SYSTEM, _VAX1$PEA0: PORT HAS CLOSED VIRTUAL CIRCUIT` entries, timestamped; `/FULL` carries the entry body | `ANALYZE/ERROR_LOG/INCLUDE=PEA0/SINCE=…` |
| **SDA `SHOW CONNECTIONS`** | the CDT table per SYSAP, including a **`Rej/Disconn Reason`** field | `ANALYZE/SYSTEM` → `SHOW CONNECTIONS` |

*(There is no `PE` SDA extension on VAX 7.3, and SCACP here has only
`SET/SHOW/START/STOP` — no trace facility. `SHOW ERROR` at DCL gives the PEA0
error count, 19 at the time of writing.)*

**SCACP polled LIVE splits the rejoin from the fresh join cleanly** — new tool
`tools/scacppoll.sh`, two runs 90 seconds apart on the same lab, counters reset
when each VC opened so everything below accrued *within the run*:

| SCACP `SHOW VC`, at T+20 s | `OVMXS6` fresh — **JOINED** | `OVMXS3` rejoin — **REFUSED** | real VAX3 |
|---|---|---|---|
| VC state / channels open / ECS | Open / 1 / 1 | Open / 1 / 1 | Open / 1 / 1 |
| **VC Total Errors** | **0** | **21** | 2 |
| **Xmt:TMO** (transmit:timeout RATIO — `Infinite` = no timeouts at all) | **Infinite** | **26–31** *(one timeout per ~26 transmits)* | Infinite |
| **XmtWindow Cur/Max** | **24 / 24** | **2** / 24 | 24 / 24 |
| **ReXmt TMO** | **663 ms** | **3 000 ms — the maximum** | 678 ms |
| Channel Total Errors | **2** | **62** | 6 |

**A successful OVMX join is indistinguishable from a real VAX at this layer.**
A refused rejoin is a VC in **congestion collapse**: a transmit timeout every
~26 transmits, the window backed off from 24 to **2**, and the retransmit timer
pinned at its 3-second ceiling. (`Xmt:TMO` is a RATIO, not a count — an earlier
revision of this section read it as a count of 29. The count is not given.) And the VC's `Total Pkts` reads **686 at T+20
and 686 at T+60** — *zero VC-level traffic in forty seconds* — while the channel
counter keeps climbing on HELLOs alone.

**What a transmit timeout means is that VAX1 sent us VC messages we never
acknowledged.** Not that we sent something wrong: the peer is retransmitting
into silence and backing off. And OVMX cannot see it — `s3D` logged *fewer*
warnings than the successful `s6A` (3 `STRAYACK` vs 9 `STRAYACK` + 2
`CMUNGROUNDED`), and its setup counters are identical to the successful run's
(`START-SENT=6`, `CONNECT-RESP-SENT=3`, `DIR-LOOKUP-RESP-SENT=12`,
`CM-CONFIG-FRAMES=7` in both). Everything that diverges — `CM-RESPONSES-SENT`
0 vs 190, `CREDIT-SENT` 36 vs 656, `MSCP-SERVER-ACCEPTS` 0 vs 27 — is
downstream of not being admitted.

**THE LEAD THIS OPENS, and it was explicitly dismissed once from the wrong
side.** §3.6 retired SCS sequence numbers because *"the member's stale round-0
`send_seq` is receive-tolerated as spec §4(i).A requires. Not the failure."*
That judgment was made from OUR logs. From the peer's side SCACP now says the
opposite is happening: **the peer is retransmitting to a returning identity and
timing out.** A mechanism that fits every fact in this document is that VAX1
carries unacknowledged VC state for that identity across our death, replays it
when the VC reopens, receives no acknowledgement it accepts, and collapses the
window — so our `op 0x02` is queued behind a jammed VC and is never processed.
A fresh identity has no such state, which is why it never happens to one.

**⚠ CAUSE OR EFFECT IS NOT ESTABLISHED.** Backoff is also what you would expect
*as a consequence* of a stalled dialogue. What is established is that the two
outcomes are separable at the PEDRIVER layer, live, on a counter nobody was
reading — and that this is a layer BELOW everything §1–§4c examined. **Do not
promote it to a root cause without a run that shows the timeouts starting BEFORE
the `op 0x02` goes unanswered.** `scacppoll.sh` polls at T+20/T+60; poll at
T+5/T+8/T+12 with a packet capture running (it does not capture yet — add it)
and the ordering falls out.

### 4d.10 ⭐⭐⭐ ORDER ESTABLISHED, AND A REAL DEAFNESS BUG FOUND AND FIXED — which did NOT admit us

§4d.9 left cause-vs-effect open. `tools/scacptrace.sh` (new) closes it: it stays
*inside* SCACP and fires bare `SHOW VC` on a timer, so a sample costs one
console round-trip instead of ~23 s, and **SCACP's output header timestamps
itself** (`VAX1 PEA0 VC Summary 1-AUG-2026 18:39:27.41:`), so no markers are
needed. VAX1's clock runs **~7.5 s behind the host** (measured directly, `SHOW
TIME` against `date`) — that offset is what puts SCACP, tcpdump and the SCSD log
on one timeline.

Aligned on the moment we send `op 0x02`:

| relative to our `op 0x02` | `s3E` rejoin — REFUSED | `s7A` fresh — JOINED |
|---|---|---|
| **−3.0 s** | errors 22, **window already 8**, ReXmt **2.84 s** | 0 errors, no timeouts, window 24 |
| **+1.0 s** | **window 1, ReXmt 3.0 s (ceiling)** | window 24, ReXmt falling to 0.68 s |
| +5 → +81 s | frozen: window 2, ReXmt at ceiling | 0 errors, window 24, ReXmt settling to 0.60 s |

**The collapse precedes the add-member request by at least three seconds, and a
fresh identity never has a single transmit timeout.** So it is NOT a consequence
of `op 0x02` going unanswered. §4d.9's open question is closed.

**AND THE CAPTURE NAMED THE CAUSE OF THE TIMEOUTS.** A message-type census of the
first 12 s, refused vs joined, differs in exactly one entry: **`msgtype 0x7b`,
204 bytes, three copies — present in the rejoin, ZERO in the fresh join.**
Decoded: all three peers sending `cat 0x01 op 0x01` in **member form**
(`body[12]=0x21`), **retransmitted ~3 s apart — exactly the collapsed ReXmt
interval** — and never acknowledged by us.

**`0x7b` is the RETRANSMIT form of `0x5b`, and this codebase already knew that**
— `scs_dir.h:SCS_DIR_OPCODE_RETX`, *"its retransmit form (spec sec 4h)"*, and
the directory path accepts it. **The connection-manager path did not.** It gated
on `0x4b || 0x5b` and discarded `0x7b` before any handler ran.

That is the *same bug already fixed once*, one msgtype further along — and the
comment sitting directly above that gate names its own symptom:

> *"Gating on 0x4b alone silently DISCARDED it … so the coordinator waited
> forever for a response it was never going to get and **the CSB stayed
> 'open'**."*

**Which is exactly what SDA reported for every refused rejoin in §4d.6: state
`open`, flags `00000000`.**

**Fixed** (`SCS_MEMBER_MSGTYPE_RETX`, kill-switch `OVMX_CM_NO_RETX=1`, and the
CMIN trace now marks `(RETRANSMIT 0x7b)`). It works — run `s3F` **sees and
processes six** `0x7b` frames that every previous run was deaf to.

> ### ⚠ AND IT DID NOT ADMIT US. `s3F` was still refused.
> Fresh control `s8B` still joins in 27 s, so the change is safe and it is kept:
> we were provably deaf to every CM retransmission, on every path, and that was
> wrong regardless of what it explains (guardrail 15, for the fifth time on this
> item). **Do not record this as the fix for `vms-2f3`.**
>
> What it buys is that the peer's retransmissions are now *visible and answered*,
> and the next question is why it retransmits at all. First lead, unproven: on
> the **member's** VC our reciprocal `op 0x14`/`op 0x01` both carry `rack=10`
> while the peer's `op 0x01` is `sseq=11` — we do not acknowledge the very frame
> we are replying to, and only reach `rack=11` some 6 s later via a `cat 0x04`
> ack, after two retransmits. **Caveat that kills the easy version of this
> story:** the *joiner* CM VC's sequence/ack progression is byte-for-byte
> identical in the refused and successful runs for the first fourteen frames, so
> whatever differs is on the member VC or later. Chase it there.

### 4d.5 What is left of the ordered plan

Step 4 — **ungate the disk-discovery run** (§4c.8, `scsd.c` requires an inbound
`op 6` on `SCS_DIR_OVMX_CONID` that never arrives on a rejoin) — is the only
step not executed, and it is now the largest remaining behavioural difference
between OVMX and a real joiner. §4c.2b item 3's bisect (was it the `op 0x02`
shape or the disk-discovery run that fixed the join?) is still unrun and is one
cheap run each.

**And the shape of the question has changed.** Three sessions have now looked
for a field we get wrong and found three real ones, none of which was the gate.
**That suggestion — instrument the peer's decision rather than another byte we
send — was then acted on in the same session, and §4d.6–§4d.8 are the result.
Read those; they supersede this paragraph.**

### 4d.6 ⭐⭐ THE REFUSAL HAS TWO SHAPES, and one of them is not a refusal

**This is the most important thing in §4d and it was invisible until SDA was
polled on the right node.** `tools/stallpoll.sh` (new) polls a **chosen** node
during the stall — the node is an argument because session j's `neg.sh` only
ever asked VAX1, and in `r1B` VAX1 was not the coordinator.

| | `r1B` shape — "proposed and aborted" | `s3B` shape — "dropped on the floor" |
|---|---|---|
| our `op 0x02` | answered in 0.4 ms | **no response at all**, not even a cat-0x04 ack |
| COMMIT / lock-rebuild | both run, everyone echoes | never happen |
| transition opened | yes, then `op 0x04 role 0x50` ABORT | **never opened at all** |
| peer console | `proposed addition` → `aborted` | **completely silent** |
| our CSB on the peer | `wait` / `long_break` | **`open`, flags `00000000`** |
| the CLUB | carries `transition` | **byte-frozen for 105 s** |

The CLUB is identical at T+15, T+40, T+70 and T+105 s: `Last trans. number 24`,
`Member State Seq. Num 0011`, `Index of next CSID 000B`, flags
`cluster,tdf_valid,init,qf_failed_node,quorum` — **no `transition` bit**. And
our CSB carries **zero flags**, not even `status_rcvd`, which every long-dead
OVMX CSB on the same node does carry.

**So `s1B`, `s1C`, `s3B` and `s3C` are not "the coordinator refused us". Nothing
refused us — the add-member request was discarded and no machinery ever ran.**
Re-read every earlier rejoin datum for which shape it was before comparing any
two of them; §2's census already hints at it (`rej2` reaches COMMIT without
`op 0x05`, `r1B` runs the whole round).

### 4d.7 ⭐ `Curr. coord. CSID` is a RESULT, not an address — routing is not the gate

SDA's Cluster Block carries `Curr. coord. CSID` and **it rotates**: VAX3
(`00010007`) during `r1B`, VAX1 (`00010001`) during `s3B`/`s3C`/`s4A` after VAX1
coordinated a removal. `cm_pick_coordinator`'s heuristic ("highest DECnet node
number", ungrounded and confounded three ways by its own comment) always answers
VAX3, so it demonstrably was **not** tracking that field.

Tested in both directions, one variable each:

| run | identity | `op 0x02` sent to | SDA says coordinator is | result |
|---|---|---|---|---|
| `s3B` | `OVMXS3` rejoin | VAX3 (default) | VAX1 | REFUSED, dropped |
| `s3C` | `OVMXS3` rejoin | **VAX1** (`CFG2_PEER=1`) | VAX1 | **REFUSED, identical census** |
| `s4A` | `OVMXS4`/1245 **fresh** | VAX3 (default) | VAX1 | **JOINED, 27 s** |

`s4A` is the decisive one: a fresh identity asked a **non**-coordinator and was
admitted — and VAX3 proposed the addition, *thereby becoming* the coordinator of
that transition. **The node you ask is the node that runs it.**

**Do not chase `Curr. coord.` as a routing fix.** The ungrounded heuristic is
still owed (spec 5(z)) and d94-e15's observation is still real, but neither is
this bug, and the "a joiner must address the coordinator" model the code carried
is wrong in the direction SDA now shows. Comment corrected in `scsd.c`.

### 4d.8 What that leaves: the drop is keyed on IDENTITY, and it happens on receipt

Inside one 10-minute window on one lab, bracketed at both ends by fresh
identities joining in 27 s (`s3A` 17:44, `s4A` 17:54, `s5A` 17:57):

- the same identity was ignored by **VAX3** and, four minutes later, by **VAX1**;
- with the rejoin form and without it (`s1B`/`s1C`);
- while transmitting frames §4c.3 already proved byte-identical to ones that
  succeeded.

So the peer decides **before any transition machinery runs**, it is **not about
which peer**, and it is **not in the frame**. Combined with §3.1 (no wait works,
15 h tested) and §4c.2d (a pristine `BRK_NON` fails on its first attempt), what
is left is per-identity state on **every** peer that survives our death and
causes a returning identity's `op 0x02` to be dropped without a reply.

**The next instrument should answer "why is this CSB in `open` with no flags".**
A successful joiner's CSB must pass through states this one never reaches; the
poll that would show it needs a node whose console is logged in and a snapshot
inside the first 25 s (`s5A` attempted exactly this and returned empty because
VAX3's console had logged out — re-run it, it is 4 minutes).

**Also unrun and still cheap:** RECNXINTERVAL on the lab is **20 s** (read from
SYSGEN; also `VAXCLUSTER=2`, `EXPECTED_VOTES=1`, `LOCKDIRWT=1`,
`NISCS_MAX_PKTSZ=1498`). Every rejoin ever tested here is far beyond it, as is
the real VAX3 crash-rejoin at ~90 s, so it is not a discriminator — recorded so
nobody re-derives it.

---

## 4e. SESSION L (2026-08-02) — new host, and the first oracle that shows the peer's CDTs

**The dev seat moved to `workshop` (x86_64) and the lab had never been run
there. Most of this session was bring-up; the science at the end is one matched
pair, and it points straight at step 4 — the one step of §5's plan still unrun.**

### 4e.1 The lab did not exist on this host — five blockers, all cleared

Nothing was running: no `br0`, no SIMH, no `/tmp/clean-vax1-test`. Recorded so
nobody re-derives it on the next machine.

| blocker | resolution |
|---|---|
| `build/simh/BIN/vax` is an **aarch64** ELF | rebuilt x86_64 in a container (`ubuntu:24.04` + `libpcap-dev`), host left clean. Old binary kept as `BIN/vax.aarch64.bak`. |
| `~/projects/pcjs-vax/open-simh/BIN/vax` is x86_64 and looks like a free win | **it is not** — that build reports `no Ethernet`. Useless for this lab. Do not reach for it again. |
| SIMH rejects `USE_NETWORK=1` on Linux | that flag means *static* pcap, which upstream removed. Build with **no** flag: it picks `USE_SHARED` (dlopen) + `HAVE_TAP_NETWORK`, which is what the lab wants. |
| 23 lab scripts hardcode `/home/baron/vax` | one symlink `/home/baron/vax → /data/training/vax`. **Do not edit the scripts** — the prefix appears 29 times and every edit is a chance to desync a tool from its doc. |
| `tools/mk_sysgen` is an **aarch64 binary with no surviving source** | replaced by `tools/mk_sysgen.py` (below). |

`libpcap` matters less than it looks: the nodes attach with `at xq tap:tap1`, so
the **TAP** transport carries everything and SIMH's `libpcap not installed`
banner is cosmetic.

`reset3.sh` / `reset2.sh` need `data/d{0,1}.dsk.3node-golden.bak`, which did not
survive the migration; they were re-copied from the laptop mid-session and are
in place again. `tools/bootlab.sh` (new) brings the lab up from the **live**
disks with no golden restore, for when they are absent.

> **⚠ Two console-harness traps bit again, both already documented, both costing
> a run.** `bootlab.sh`'s first version copied `reset3.sh`'s login loop, which
> (a) greps for `Username:` — a string that also appears as a **padded field
> inside an OPCOM security-audit record**, and (b) confirms DCL by grepping for
> a **literal** it just typed, which the console echoes, so it reports success
> from a bare login prompt. Both are exactly what `tools/loginN.sh`'s header
> warns about. **Use `loginN.sh`. Never hand-roll a console login.**

### 4e.2 The bug reproduces on the new host, unchanged

Bracketed, pure mode throughout, fresh store per identity:

| run | identity | result |
|---|---|---|
| `w1A` | `OVMXW1`/1250 **fresh** | **JOINED**, `CLUSTER_NODES=4`, `XITDONE=1`, ~27 s |
| `w1B` | `OVMXW1` again, +2 min | **REFUSED** |
| `w2A` | `OVMXW2`/1251 **fresh** | **JOINED**, 27 s |
| `w1C` | `OVMXW1` again | **REFUSED**, census identical to `w1B` |
| `w3A` | `OVMXW3`/1252 **fresh** | **JOINED**, 27 s (closing control) |

Every refusal is the **"dropped on the floor"** shape of §4d.6: inbound census is
`op 0x14 ×3 | op 0x01 ×9` and *nothing else* — no COMMIT, no `op 0x05`,
`XITABORT=0`. Six of those nine `op 0x01` are the `0x7b` retransmits the
`9f98dbf` fix now hears (`RETX=6`), confirming that fix works on this host and
still does not admit us.

One CSB reading to record, because it matches **neither** shape in §4d.6's table:
`w1B`'s CSB on VAX1 was `State: 09 wait`, `Flags: 00000000`, `CSID 00000000`.
§4d.6 has `wait`/`long_break` for the aborted shape and `open`/`00000000` for the
dropped shape. This is `wait` with zero flags. Do not treat §4d.6's CSB row as a
reliable discriminator on its own — the *census* is the reliable one.

### 4e.3 ⭐⭐ THE PEER'S CDT TABLE, AND IT IS STRUCTURALLY DIFFERENT

§4d.9 listed SDA `SHOW CONNECTIONS` — with its **`Rej/Disconn Reason`** field —
as an oracle that "exists and was never used". It has now been used.

`SHOW CONNECTIONS` takes **`/NODE=name`** on VAX 7.3 (also `/SYSAP=`,
`/ADDRESS=`; verified with `HELP` on the lab). That narrows a sample to a few
lines, which is what makes it safe to poll. New tool: `tools/connpoll.sh`.

Same peer (VAX3), same instrument, one variable — the CDTs VAX3 holds **for the
joiner's identity**, sampled every 12 s through the run:

| VAX3's CDTs for the joiner | `w3A` fresh — **JOINED** | `w1C` rejoin — **REFUSED** |
|---|---|---|
| `VMS$VAXcluster` | **open** | **open** |
| `SCS$DIRECTORY` | **open** | *absent* — instead `SCS$DIR_LOOKUP` **`disc_sent`** |
| `MSCP$DISK` | **open** | *absent* — instead `VMS$DISK_CL_DRVR` **`con_sent`**, `Remote Con. ID 00000000` |
| `Rej/Disconn Reason` | 0 on every CDT | **0 on every CDT** |

Both runs show **no** CDTs in the pre-run samples and their first CDTs at the
same sample, so these were created by the run under test, not inherited.

**Two things follow, and they are different in kind.**

1. **`Rej/Disconn Reason` is zero even on the refusal.** The one oracle in this
   lab that can name a rejection says *nothing rejected us*. That is a third,
   independent confirmation of §4d.8: we are **dropped**, not declined.
2. **`con_sent` with `Remote Con. ID 00000000` is an unanswered connect
   request.** VAX3's `VMS$DISK_CL_DRVR` sent us a CONNECT and sat in `con_sent`
   for the rest of the run. On the successful join that SYSAP pairing does not
   hang — VAX3 instead ends up with `MSCP$DISK` **open**.

Our own counters agree, and the agreement is sharp because everything *else* is
identical: `START-SENT=6`, `CONNECT-RESP-SENT=3`, `DIR-LOOKUP-RESP-SENT=12`,
`CM-CONFIG-FRAMES=7`, `HELLO-SENT=66` in **both** runs — while
**`MSCP-SERVER-ACCEPTS-SENT` is 39 when joined and 0 when refused**.

> ### ⚠ CAUSE OR EFFECT IS NOT ESTABLISHED — this is §4d.9's trap, again
> §4d.9 already saw `MSCP-SERVER-ACCEPTS 0 vs 27` and correctly filed it as
> "downstream of not being admitted", and nothing here refutes that reading: in
> `w3A` the MSCP traffic happens **after** admission. The 12 s sampling cadence
> is far too coarse to order `con_sent` against our `op 0x02`. **Do not promote
> this to the root cause.** What is new is only that the peer holds a
> *structurally different CDT set*, with a connect request to us left unanswered
> — which is the same behavioural gap §4c.8 identified from the wire, now seen
> from the peer's own tables.

**Why this still matters: it is independent motivation for step 4**, the only
step of §5's ordered plan never executed. §4c.8 said OVMX never does the
joiner's directory + MSCP run and called it "the largest single behavioural
difference left"; §4d.5 says `scsd.c` gates it on an inbound `op 6` that never
arrives on a rejoin. The peer's CDT table now shows a disk-class connection
hanging on us during exactly that window. **Run step 4 next**, and settle the
ordering while doing it: sample at 2–3 s cadence (the `/NODE=` query is cheap
enough) so `con_sent`'s arrival can be timed against our `op 0x02`.

---

### 4e.4 ⭐⭐⭐ STEP 4 IS EXECUTED — the disk-discovery run is ungated, and it does NOT admit us

**The last unrun step of §5's ordered plan has been run. It is refuted as the
gate — the fifth confident candidate this item has killed — and the way it fails
says more than the change itself.**

**Where the gate actually was.** §4d.5 cites `scsd.c:2280–2305`; line numbers have
drifted, and it is at **`scsd.c:2466–2484`**. The pure-server disk-CLIENT machine
(our own `SCS$DIRECTORY` connect → `MSCP$DISK` lookup → SCC/GUS walk) has exactly
**two** entry points, and both require `ps->psc_credit_done`, which is set in
exactly **one** place: an inbound `op 6` addressed to `SCS_DIR_OVMX_CONID`.
Grounded on this host before changing anything — `SCSD-I-PSCLIENT` fires **33
times on a join and 0 times on a rejoin**.

**The change.** A timer-driven fallback in the main loop: once the CM config
exchange has completed and the `op 6` still has not arrived after
`OVMX_DISKRUN_GATE_MS` (default 2000 ms, inside the 1.4–4.4 s window §4c.8 shows
a real joiner using), open our own `SCS$DIRECTORY` client connect anyway. New
counter `PSC-UNGATED`, new log line `SCSD-I-PSCUNGATE`, kill-switch
**`OVMX_NO_DISKRUN_UNGATE=1`**.

**It is genuinely additive.** On both fresh joins in the bracket, `PSC-UNGATED=0`
— the `op 6` arrives and the original trigger wins the race, so the new path
never executes on a working join.

| run | identity | ungate | result | `PSC-UNGATED` | inbound `op 0x01` | `0x7b` retx |
|---|---|---|---|---|---|---|
| `w4A` | `OVMXW4`/1253 **fresh** | on | **JOINED** | 0 | — | — |
| `w4B` | `OVMXW4` rejoin | **on** | **REFUSED** | **3** | **3** | **0** |
| `w4C` | `OVMXW4` rejoin | **off** (kill-switch) | **REFUSED** | 0 | **9** | **6** |
| `w5A` | `OVMXW5`/1254 **fresh** | on | **JOINED** (closing control) | 0 | — | — |

**HOW it fails is the finding.** The run *starts* — three `PSCUNGATE`, one per
peer, the first time OVMX has ever opened its own directory connection on a
rejoin — and then **never advances past step 1**. `PSC_DIR_CONNECT` is
retransmitted six times and is never accepted by anybody. The peer's CDT table
is **unchanged** from §4e.3's refusal: `VMS$DISK_CL_DRVR` still `con_sent` with
`Remote Con. ID 00000000`, `SCS$DIR_LOOKUP` still `disc_sent`,
`Rej/Disconn Reason` still 0.

**So the defect is not that OVMX fails to INITIATE the disk-discovery run.** We
now initiate it and the peer ignores it. Our `SCS$DIRECTORY` CONNECT-REQUEST is
dropped exactly the way our `op 0x02` is dropped. **This strengthens §4d.8 rather
than competing with it:** a returning identity is not being declined at the
connection manager — *every* new connection it attempts, to any SYSAP, is
discarded on receipt.

> ### ⭐ One real, matched-control-attributable behaviour change — and do not overread it
> `w4B` vs `w4C` is one variable, same identity, same lab, back to back: with the
> disk run started the peers **stop retransmitting** their `op 0x01`
> (`0x7b` retransmits 6 → 0, inbound `op 0x01` 9 → 3). That is attributable to
> the ungate and to nothing else, and §4d.10 established those retransmits exist
> because the peer is transmitting into silence.
>
> **What it does NOT establish is that this is progress.** "The peer stopped
> retransmitting because we finally answered" and "the peer stopped
> retransmitting because it gave up sooner" both fit. Deciding it needs SCACP
> (`tools/scacptrace.sh`) across the same matched pair — if the ungate is real
> improvement the transmit window should stop collapsing.

**KEPT** (guardrail 15, for the sixth time on this item). OVMX skipping the
joiner's disk-discovery run was a real behavioural divergence from every real
joiner, documented since §4c.8, and it was wrong independent of what it explains.
Conditional, kill-switched, additive, and proven not to regress a fresh join by
two controls in the same session.

**The ordered plan of §5 is now fully executed.** Nothing in it remains unrun.

---

## 5. ⚠ WHERE TO START NEXT SESSION

> **⚠ THE ORDERED PLAN BELOW IS NOW FULLY EXECUTED — steps 0–3 in §4d, step 4
> in §4e.4. Every step has been run and none was the gate.**
> Step 0 grounded two fields and settled its own counter-evidence; steps 1–3
> shipped in `e687c6b`; step 2 was then REFUTED as the gate by a matched
> control. **Only step 4 (ungate the disk-discovery run) is unrun.** The rest of
> this section is kept for its reasoning, not as instructions.
>
> **§5 items 1 and 2 below are SUPERSEDED by §4c.** Item 2 (`Ref. time`) is dead
> — do not spend a commit on it. Item 1's framing ("why does the coordinator
> stall") is wrong — the silence is ours. Item 3 has been executed; its result is
> §4c.6. **Start from §4c.8, then item 4.**
>
> **UPDATE — the join half is DONE and the two outcomes are now separated by
> experiment, not by guess (§4c.2b).** `OVMX_PURE_SERVER=1` produces a
> reference-shaped join: the coordinator proposes our addition **once**, never
> re-advertises with `cat 0x01 op 0x04`, and the disk-discovery run executes. The
> same identity, three minutes later, is **still refused**. So:
>
> 1. **Baseline everything on pure mode from now on.** Every rejoin datum in this
>    document older than `r2B` was taken on the limping default path.
> 2. **`vms-785`** — promote pure mode to the default, with the design cascade.
> 3. **Bisect what actually fixed the join** — the `op 0x02` shape or the
>    disk-discovery run. They moved together. One cheap run each.
> 4. **Then attack the rejoin as its own defect — and START FROM §4c.2c.** The
>    missing reference specimen now exists: a real VAX3, `kill -9`'d exactly as
>    OVMX dies, crash-removed class-0x03, rebooted under an unchanged
>    SCSNODE/SCSSYSTEMID, **rejoined in ~90 s**
>    (`captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`). Diff it against
>    the matched OVMX pair `d94-r2A` (joined) / `d94-r2B` (same identity, refused,
>    both pure mode). The question is no longer open-ended — there is a reference
>    answer on this exact path for the first time.
>
> ### ⭐ THE ORDERED PLAN — change ONE thing per run, control immediately before each
>
> 0. **SDA on VAX3 right after it rejoins**, listing its CSBs. Free, and it
>    decides §4c.2e's counter-evidence before you write any code.
> 1. **Log `cat 0x01 op 0x04 role 0x50 class 0x02` as ABORT** (§4c.2f). `txn=0`
>    so per §4(r) it is not answered — but fail the join honestly instead of
>    hanging. Pure observability, costs nothing, makes every later run legible.
> 2. **Send the rejoin form of `op 0x02`** (§4c.2e): `[20:22]=1`,
>    `[22:24]`=founding node's SCSSYSTEMID, `[28:36]`=**copy verbatim** from any
>    member's `op 0x01` `body[28:36]` (we already parse it), `[40:52]`=twelve
>    `0x20` spaces. `[36:40]` unknown — leave 0 and vary it second.
>    **MUST be conditional**: a genuinely fresh identity has to keep sending
>    zeros, because real first joins do.
> 3. **Persist "I have been admitted to cluster `<founding-time>`" across a
>    restart**, keyed on the sysgen store — a rebooted VAX3 has it and a
>    restarted OVMX does not. Without this, step 2 cannot fire on the run that
>    matters. *(Note this is the same statefulness `vms-e6c` is about, from the
>    opposite direction: e6c says don't keep membership state you've lost; this
>    says do keep the fact that you once had it. Reconcile them deliberately.)*
> 4. **Ungate the disk-discovery run** from the member's DISC-REQ
>    (`scsd.c:2280–2305` requires an inbound `op 6` on `SCS_DIR_OVMX_CONID`,
>    which **never arrives on a rejoin** — `r2B`: 0 of 3 peers, `PSCLIENT`=0).
> 5. **Do NOT chase**: the incarnation (VAX3's moved 162 ms and it still
>    rejoined — an SIMH stored-clock artifact, which is itself the proof that
>    "the incarnation moved" is not the key), `[36:38]` (already correct),
>    the `op 0x05` echo (byte-correct), the HELLO (identical before and after),
>    `Ref. time` (§4c.4), or `op 0x04 role 0x00` (a different SYSAP).
>
> **Spec debt owed:** §4(r) role table gains `0x50` = transition ABORT;
> §4(j)/§4(o) gain the `op 0x02` rejoin field map and the `op 0x05` subject-node
> decode; §4(g) must state the offset convention — **the START is 120 bytes
> absolute / 106 payload, and `body[n]` = absolute `72+n`.** §4's `[66:74]` and
> `[98:106]` are PAYLOAD-relative; absolute they are `[80:88]` and `[112:120]`.
> That ambiguity has already cost one agent a wrong turn.
>
> **A note on method, since this item keeps punishing the same mistake.** Three
> agent analyses on `vms-2f3` produced confident, well-evidenced root causes.
> One was refuted by a `grep`, one by a four-minute run, and one turned out to be
> right after a four-minute run refuted my refutation. Every time, the resolution
> came from the lab or from a peer-side oracle, not from more bytes. **Get the
> oracle to answer before you get the agent to explain.**

**Historical framing, kept for the reasoning only:**

1. **Find what OVMX does at or before `op 0x02` that a real joiner does not.**
   Diff OVMX's pre-commit exchange against `VX3`'s in af2, frame by frame, in
   both directions — the two paths already differ in *coordinator behaviour* by
   the time commit lands, so the cause is upstream of it. Concretely: what does
   the coordinator have from `VX3` at the moment it answers `op 0x02` in 1–20 ms
   that it does not have from OVMX, given it instead emits `op 0x04` + a re-sent
   `op 0x01` and waits? A re-sent `op 0x01` reads like *"answer the cluster-
   parameters question again"* — which points at our `op 0x01` reply being
   unsatisfying, and note that `Ref. time` in that very reply is `1-JAN-2001`.
2. **Make `Ref. time` live** (§4, `own_admission`) (§4, `own_admission`). It is the last known frozen
   field, it is what a peer reads as *"when did this node join this cluster"*,
   and a returning node advertising `1-JAN-2001` is claiming to have joined 25
   years ago — plausibly enough for a coordinator to decline to promote it. One
   commit, one reproducer run.
3. **Ask the peer, don't infer it.** Session i's method note earned itself again
   here: SDA answered in one query what two capture analyses could not. During
   a refused rejoin the CLUB carries `transition` (`31080001`) while our CSB
   sits in `wait` with **no CSID and no status flags**, whereas a properly
   removed node reads `long_break,removed`. A transition *is* open for our
   admission and never progresses. Poll SDA **during** the stall, repeatedly,
   and watch which field moves.
4. **`vms-70c` is now partly paid and partly exposed.** Three replayed constants
   turned out to be timestamps; two are fixed. Anything else in
   `scs_start_tmpl` / `member_params_tmpl` that decodes as a plausible VMS
   quadword should be audited the same way — the honesty debt and the rejoin bug
   turned out to be the same defect class.

## 6. Lab tooling added this session

| script | what it does |
|---|---|
| `tools/cycle2.sh` | `cycle.sh` plus an **SDA CSB oracle** before the first join, after every cycle, and at the end; `PER_CYCLE_SYSID=1` for per-cycle identity; `CYCLE<N>_ENV` for per-cycle env; `SKIP_RESET=1`. |
|  `tools/oneshot.sh` | one join against the lab as it stands + SDA dump. **The four-minute loop.** Worth moving into `tools/`. |
|  `tools/probe.sh` | drive any VAX console, capture between markers. |
| `tools/stallpoll.sh` | **one join + SDA polled on a CHOSEN node DURING the stall.** The node is an argument on purpose: session j polled VAX1, which was not the coordinator in `r1B`, so the one node whose state decided the outcome was never asked. This is what separated the two refusal shapes (§4d.6). |
| `tools/scacppoll.sh` | **one join + SCACP (`SHOW VC` / `SHOW CHANNEL`) polled on a chosen node DURING the run.** This is PEDRIVER's own view, a layer below everything §1–§4c examined, and it is what separated a refused rejoin from a successful join on live counters (§4d.9). **Does not capture packets yet — add tcpdump before using it to establish ordering.** |
| `tools/bootlab.sh` | **boot the 3-node lab from the LIVE disks, no golden restore.** For a host where `d{0,1}.dsk.3node-golden.bak` is absent (they did not survive the 2026-08-01 migration and were re-copied later). Uses `loginN.sh`, deliberately — see §4e.1. |
| `tools/mk_sysgen.py` | **x86_64 replacement for the aarch64 `mk_sysgen` binary**, whose source is lost. Patches SCSNODE/SCSSYSTEMID into a known-good template store (the one from `s8A`/`s8B`, both of which joined) rather than regenerating, so every other field is byte-identical to a store proven to work. Rejects names >6 chars and deletes any stale `.cluster` prior-admission sidecar, so a "fresh" identity is really fresh. |
| `tools/connpoll.sh` | **SDA `SHOW CONNECTIONS/NODE=<id>` sampled on a chosen peer THROUGHOUT a run** — the CDTs that peer holds for our identity, and their `Rej/Disconn Reason`. The only oracle here that can name a rejection rather than describe a silence (§4e.3). Stays INSIDE SDA and uses `/NODE=` to keep a sample small; the first version drove `ANALYZE/SYSTEM…EXIT` per sample and overran the console input buffer during the OPCOM flood, losing 3 of 4 snapshots. |
| `tools/scacptrace.sh` | **high-cadence SCACP + packet capture.** Stays INSIDE SCACP and fires bare `SHOW VC` on a timer (one console round-trip per sample instead of ~23 s), and relies on SCACP's self-timestamping header instead of markers. This is what established ORDER (§4d.10). VAX1's clock runs ~7.5 s behind the host — measure it with `SHOW TIME` vs `date` before correlating. |

Run tags session j (part 2): `r1A` `r2A` joined, `r1B` `r2B` refused, `vax3crash` = the real-VAX crash-rejoin specimen. **Last SCSSYSTEMID used: 1241.**
Run tags session k: `s1A` `s2A` `s3A` `s4A` `s5A` joined (fresh, pure); `s1B` refused (rejoin form), `s1C` refused (`OVMX_REJOIN_FORM=0`); `s3B` refused (SDA-polled on VAX3), `s3C` refused (`OVMX_CFG2_PEER=1`, forced to the real coordinator); `s3D` refused / `s6A` joined = the matched SCACP pair (§4d.9); `s3E` refused / `s7A` joined = the high-cadence ORDERING pair (§4d.10); `s3F` refused WITH the 0x7b fix, `s8B` fresh joined with it. **Last SCSSYSTEMID used: 1249.**
Run tags session L (workshop): `w1A` `w2A` `w3A` joined (fresh, pure); `w1B` `w1C` refused (`OVMXW1` rejoin). `w1C`/`w3A` are the matched CDT pair (§4e.3); `w2A` is VOID as an instrumented run — its console overran (§4e.1) — but valid as a join. **Last SCSSYSTEMID used: 1252.**

Run tags session i: `ctl1`, `inc1`, `inc2`, `fresh1`, `fresh2`, `keyB`, `keyC`,
`rej2`, `rej3`. Session j: `g1A` (joined), `g1B` (refused, SDA-polled), `p1A`
(pure-server, refused). **Last SCSSYSTEMID used: 1232.**

Session j also added `$CLAUDE_JOB_DIR/tmp/neg.sh` — a negative run that polls SDA
at +15/+35/+60/+95/+140 s *during* the stall instead of only at the end. Worth
folding into `tools/` as `stallpoll.sh`; the end-of-run SDA dump that `oneshot.sh`
takes cannot show that nothing moves.

## 7. Guardrails earned here

13. **Verify an agent's wire claim against a cheaper oracle before you build on
    it.** The `op 0x04` root-cause was refuted in one `grep` of our own log,
    after an agent had spent 160k tokens arriving at it. Agents are for reading
    bytes, not for deciding what is true.
14. **Run the positive control in the same session as the negative one.** "It is
    refused" means nothing without "and a fresh identity joined 20 seconds
    later, on this same lab, with this same binary."
15. **A fix that is right can leave the bug unfixed.** Two frozen fields were
    genuine defects, grounded and documented, and neither was the gate. Fix them
    anyway — and do not let the disappointment relabel them as noise.
16. **Before implementing an agent's checklist, check whether it is already
    behind an env var.** The pre-commit agent spent 197k tokens deriving a
    correct, well-evidenced `op 0x02` shape. `OVMX_PURE_SERVER=1` had implemented
    it months ago. One four-minute run refuted the whole checklist — *and* told
    us something new (the correct shape draws no reply at all), which no amount
    of further capture reading would have. **`grep` the codebase for the fix
    before you write it, and run the experiment before you commit it.** This is
    guardrail 13 with the cost inverted: cheap oracles beat expensive analysis
    not only for *refuting* claims but for *acting* on them.
18. **A positive control must prove WHICH identity it controlled for.** Guardrail
    14 said run the control in the same session. Not enough: I ran one at the
    start, then three runs whose "fresh identities" never reached the wire at all
    because a relative `OVMX_SYSGEN_PATH` silently fell back to the default node
    name. Three invalid results accumulated, and I wrote a refutation on them.
    **Run the control immediately before each negative, and verify the identity
    on the wire** — `strings -a work/d94-<tag>.pcap | grep -oE 'OVMX[A-Z0-9]{2}'`.
19. **Suspect the harness before the theory when a control fails.** A failing
    positive control means *the experiment is broken*, and "the lab saturated" is
    the seductive wrong answer because it explains the data and requires nothing
    of you. The lab was fine. Check what you actually put on the wire first.
20. **A control that runs AFTER the negative is worth as much as one before.**
    §4d's four runs bracket the experiment: a fresh identity joined 95 s before
    `s1B` and again 4 minutes after `s1C`. Without the closing one, "both
    refusals look the same" could still have meant "the lab stopped admitting
    anyone halfway through". Bracket the experiment, do not merely precede it.
21. **When a change you believe in fails, run it against its own kill-switch
    before you interpret the failure.** `s1B` refused, and the tempting reading
    was the same one that was wrong in §4c.2 — *the reference-correct shape
    draws less response*. `s1C` (`OVMX_REJOIN_FORM=0`, same identity, same lab,
    one variable) produced an identical census and reduced that to nothing. Ship
    every wire change with the switch that turns it off, and use it in the same
    session.
17. **Measure the window you are actually naming.** §4b asserted a coordinator
    stall for a whole session. The coordinator's response latency was 0.4 ms and
    had been in our own logs the entire time; the 6.5 s belonged to the next
    window and to us. Timestamp the specific edge before naming who is late.
