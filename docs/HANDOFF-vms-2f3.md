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
> refuted as the gate. Do not re-propose it.** **§4f–§4g are newer still: §4f proves the
> deciding state is the CLUSTER's, not ours; §4g shows the peer asks for our disk
> server, is told yes, and declines to connect. **START AT §4k, then §4j — they are
> the newest, and between them they correct §4h, §4c.8 and the reading of §4g.
> §4(i).B's incarnation semantics are CONFIRMED, not corrected — see §4k.9, and
> note that §4k.3's first version of that result was retracted as an offset error.** Then §4h and §4i for background. The CDT layer is
> excluded for everyone; the peer's CSB shows the rejoin attempt itself clearing
> `removed`/`status_rcvd` and zeroing the CSID; and §4i found a real ack bug that
> explains the PEDRIVER collapse but is NOT the gate.
>
> ## ⭐⭐⭐ START AT §4L — IT IS THE NEWEST AND THE SHARPEST
>
> **A bracketed triple on one lab-2 pod (fresh join → same-identity rejoin →
> fresh join), with the peer's CSB sampled every 5 s DURING each attempt, reduces
> the whole bug to one flag word.** At T+5 s all three runs are identical on the
> peer — new CSB, `01 open`, `CSID 00000000`, **the same CDT address** — and the
> only difference is:
>
> | | admitted | **refused** |
> |---|---|---|
> | CSB flags | `02040000 status_rcvd,vcc` | **`00000000` — nothing set** |
>
> **The peer builds the same structures for a refused rejoin as for a successful
> join, then never sets `vcc` or `status_rcvd`.** That is the single question now.
>
> **§4L.2 also REFUTES §4j.2's OVMX column**: a refused rejoin DOES get a new CSB
> and DOES get a CDT. §4j reached the opposite conclusion by comparing a live
> real-node readmission against OVMX identities sampled *after their processes had
> exited* — see guardrail 22. §4j's real-node half still stands.
>
> **§4j is the real-node control.** A SIGKILLed real VAX leaves a CSB *identical*
> to a dead OVMX identity's — so our surviving CSB is not the asymmetry (§3 item
> 10), and CSID zeroing is normal readmission, not damage.
>
> **§4k decodes the reference rejoin nobody had decoded, and it reverses the
> frame.** On a REJOIN the peer initiates everything — the VC, the directory, the
> `VMS$VAXcluster` connect, the first config message — and the returning node only
> answers. The refused OVMX rejoin completes that whole round-1 exchange and then
> the peer **never tears down its directory connection**, so round 2 never opens.
> That teardown is the divergence point, and it is peer-side.** §1–§4c exist so you do not re-derive
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
| our node-status reply differs on a rejoin | **REFUTED — byte-identical, 0 of 132 bytes across 4 frames** | §4i.1 ⭐⭐ |
| the `0x48` credit-return is enough to stop the peers retransmitting | **REFUTED — OVMX sent `acked_seq=11` three times and they retransmitted anyway; only a SEQUENCED `recv_ack` stops them** | §4i.2 ⭐⭐ |
| §4e.4's RETX 6→0 might be the peers giving up sooner | **RESOLVED — it is progress; the ungate supplies the sequenced frames that carry `recv_ack`** | §4i.3 ⭐ |
| **the refusal is a stale quarantine CSB predating the attempt** | **REFUTED — a died-and-never-returned identity's CSB looks NORMAL (`removed,status_rcvd`, CSID intact)** | §4h ⭐⭐⭐ |
| **the rejoin attempt itself degrades the peer's CSB** | **GROUNDED — it clears `removed`+`status_rcvd`, zeroes the CSID, and sticks in `wait`; exact 2/3 split, no exceptions** | §4h ⭐⭐⭐ |
| the peer holds a stale `VMS$DISK_CL_DRVR` CDT across our death | **REFUTED — 3 dead OVMX identities leave ZERO CDTs; live control shows 4** | §4g.6 ⭐⭐ |
| the peer's `MSCP$DISK` connect arrives and we discard it (5th deafness bug) | **REFUTED — it never arrives; the gate at scsd.c:4617 already accepts 0x4b/0x5b/0x7b** | §4g.3 ⭐⭐⭐ |
| **the peer declines to connect to a disk server it just confirmed exists** | **GROUNDED — absent from the peer's OWN send_seq numbering, so nothing was dropped** | §4g.2 ⭐⭐⭐ |
| the disk connection is downstream of admission | **REFUTED — it precedes `XITDONE` in all 5 joined runs measured** | §4g.1 ⭐⭐ |
| OVMX-directed traffic collapse shows targeted silence | **REFUTED — peer↔peer traffic falls MORE (9.5x vs 8.6x)** | §4g.4 |
| **OVMX's own persisted state causes the refusal** | **REFUTED — the same identity + sidecar joins a virgin cluster in 15 s** | §4f.2 ⭐⭐⭐ |
| the deciding state might be in the peer's CDT table | **REFUTED — a departed REAL node leaves zero CDTs; nine empty samples** | §4f.3 ⭐⭐⭐ |
| **the refusal is per-cluster, peer-side state** | **CONFIRMED — positively, for the first time; one cluster refuses while another admits** | §4f.2 ⭐⭐⭐ |
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
10. **OVMX's surviving CSB on the peer is the asymmetry.** Killed by the
    real-node control (§4j.1): a SIGKILLed real VAX2 leaves a CSB whose state,
    flag word (`06040005 long_break,removed,status_rcvd,send_status` `vcc`),
    absent CDT and retained CSID are **identical** to a dead OVMX identity's, in
    the same dump from the same peer — and it rejoins anyway. Holding a CSB is
    normal. Related and also dead: **CSID zeroing at the rejoin attempt is
    damage** — the real node's readmitted CSB carries `CSID 00000000` too
    (§4j.3, which corrects §4h).
11. **Anything at all about the START `[22:24]` incarnation counter.** Settled
    against the reference for the first time in §4k.9, after two wrong readings:
    peers advertise `2` to a returning real VAX3 (abs 92, directed HELLOs only)
    and `2` to a returning OVMX identity, and **both echo `2`**. OVMX already
    emits the reference value on the failing path — the field is **identical**
    in the successful real rejoin and the refused OVMX one. It is neither a
    candidate delta nor an oracle for the CSB decision, and the counter is
    incremented at channel re-formation, upstream of the admission decision.
    The adjacent quadword at abs 80..87 is not a boot stamp either (4.5 h stale
    across the reboot). **Do not touch this field.** Note `[22:24]` is
    payload-relative = **abs 36**; reading it at abs 22 produces a constant `1`
    and a wrong conclusion, which is exactly what happened once already.
12. **The peer's connect to OUR `MSCP$DISK` is a precondition for admission.**
    Killed by §4k.6: in the reference rejoin that connect arrives 7.47 s *after*
    the membership request and is refused 9 times over 90 s while the node is
    already a member. **But mind the direction** — the returning node's own
    client walk *to the peer's* `MSCP$DISK` does precede `op 0x02`, and that one
    is real. The two are constantly confused; §4k.6 has the table.

13. **The directory-response msgtype mirror (§4M).** OVMX echoed the request's
    msgtype onto its lookup response. **Not the gate** — and note §4M.12: the
    "a real VAX always answers `0x4b`" justification is REFUTED for this frame
    (a real VAX mirrors `0x5b` about half the time); the live candidate is
    `0x4b` if present / `0x5b` if not. `N1B`/`N1C`/`N1D`
    answer `0x4b` and are refused three times, between two joining controls.
    The surviving observation, corrected in §4M.9, is a **transition**: in a
    refused rejoin the peer's `MSCP$DISK` lookups stay `0x5b` for all four,
    while every join reaches `0x4b` by the second. A join CAN carry a leading
    `0x5b` (`N3A`). Useful free oracle; the causal reading is dead.
21. **The masquerade / incarnation-match test (§4M.30/§4M.31).** Documented in
    *VAXcluster Principles* p.2-21: same SCSSYSTEMID + same SCSNODE + a Path
    Block already queued => the 64-bit incarnations must match or **VC formation
    is abandoned**. **Refuted twice.** `Z1B` presented a wire-verified identical
    incarnation and was refused between joining controls; and the VC
    demonstrably FORMS on every refusal (the peer opens connections, runs 8
    lookups and completes `op8`->`op9` over it), so formation is not being
    abandoned at all. The rule is real; it is not our gate.
20. **`OVMX_DISKLESS=1` on a rejoin (§4M.24).** Never tried in 13 sessions
    despite a source comment describing exactly the stuck disk connect SDA
    shows. **Refused** (`T1C`), switch verified engaged (`MSCP$DISK`
    affirmatives 4 -> 0), between two joining controls.
19. **OVMX performing its own disconnect call (§4M.23/§4M.24).** Documented as
    required by DTJ v1n5 p.25 and implemented; our `op6` reaches the wire
    well-formed and **the peer never answers it with `op7`** and retransmits
    `op8` again. Refused (`U1B`, `N1I`) with the fix verified engaged, between
    joining controls. Kept as opt-in.
18. **Answering a retransmit with a fresh sequence number (§4M.20).** Real, and
    a defect §4M.14 itself created — the peer replayed `op8 send_seq=12` and we
    answered 12/13/14. Fixed (`scs_retx_reply_seq`, 11 assertions), **verified
    replayed as 12/12/12 on the wire, and not the gate** (`N1G`/`R1B` refused
    between joining controls).
17. **Our `op7`/`op9` carrying the retransmit msgtype `0x7b` (§4M.19).** Real,
    also created by §4M.14's `memcpy`. Fixed, **verified as `mt=4b` on the
    wire, and not the gate.** Its closing inference — "VAX1 did not accept our
    `op9`" — is **refuted** by §4M.21: the reply is now provably perfect and the
    peer still retransmits.
16. **The `0x7b` credit-handshake deafness (§4M.14/§4M.17).** `scsd.c:2525`
    discarded every retransmitted `op6`/`op8`; third instance of one defect, on
    the gate the source labels as gating admission. **Real, fixed, kept — and
    not the gate:** `N1F` (an identity already refused 4×) was refused again
    with it, behind a joining control. Predicted by the timing — VAX1's
    retransmit lands at +2.26 s, two seconds after the `op6` divergence.
15. **"OVMX is deaf to `cat 0x04` and that is the gate" (§4M.13).** `scsd.c`
    never dispatches on a `cat 0x04` opcode — **and neither does a real node.**
    Across 175 `cat 0x04` frames sent to the rejoining VAX3 there is never a
    same-opcode reply; the category is absorbed by the ack field alone. OVMX's
    non-dispatch is correct. Related and also dead: the peer's `op 0x04` to us
    is byte-conformant with what the reference's returning node received and
    joined on.
14. **The peer's `cat 0x04` ack opcode (§4M.7).** Claimed as `op 0x00` on a
    join vs `op 0x04`/`op 0x06` on a rejoin, from one run. **Refuted the same
    session:** `N3A` JOINED with `op 0x04`; `N1D`/`N1E` were REFUSED with
    `op 0x00`. Varies freely across both outcomes. This is §3 item 4's mistake
    one category over, and `scsd.c:2804` warns about exactly it.

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

> ### ⚠ SUPERSEDED IN PART (`vms-096` then `vms-ebb`, 2026-08-05) — read before quoting the two paragraphs above
> Two statements above describe a tree that no longer exists, and both are left
> in place as the record of what was measured then:
> - **"exactly two entry points, and both require `ps->psc_credit_done`"** — on
>   the integrated tree the `op 6` handler that set that flag sat inside an
>   `if (cm_op == 8)` branch and was unreachable, so `vms-096` deleted the flag,
>   its writer and the immediate trigger together. **There is ONE entry point**,
>   `scsd_diskrun_ungate_tick()`.
> - **"`SCSD-I-PSCLIENT` fires 33 times on a join and 0 times on a rejoin"** was
>   measured on `worktree-760-active-directory`, where that handler really was
>   reachable. It does not describe the tree in `main` and must not be requoted
>   as if it did.
>
> `vms-ebb` ran the bracket the deletion left owing (spec §4(O.2), lab-2
> `vaxlab-1`, three pure-server arms with the control between them) and **RULED:
> one trigger stays.** It also refutes a tempting reading of the deletion — the
> peer *does* initiate a p. 2-26 teardown of our `SCS$DIRECTORY` server
> connection, twice per run in 3 of 3 arms, so the immediate trigger's signal is
> live; it is simply not worth a second entry point, because the control arm
> with disk discovery **entirely suppressed** joined on the same schedule. Every
> arm there is a FIRST join: **whether that `op 6` arrives on a REJOIN — the case
> §4e.4 is about — is `vms-449`'s bracket and is still open.**

---

## 4f. SESSION L part 2 — LAB-2, and the two experiments lab-1 could never run

**`vms-a5c`'s k3s lab (`tests/lab/`, one pod = one isolated 2-node VMScluster)
made two experiments cheap that were previously impossible: presenting a refused
identity to a cluster that has never seen it, and killing a real VAX to watch it
rejoin. Both were run. Both answered.**

Every comparison below is **lab-2 only**, per the mixing rule at the top of this
file and in `tests/lab/README.md`. Nothing here is compared against a lab-1 run.

### 4f.1 Lab-2 reproduces the bug — so the reproducer is now disposable

| run | identity | pod | result |
|---|---|---|---|
| `L1` | `OVMXL1`/1301 **fresh** | A (`vaxlab-0`) | **JOINED**, `CLUSTER_NODES=3`, 15 s |
| `L1b` | `OVMXL1` again, +3 min | A | **REFUSED** |

A lab-2 cluster is two nodes, so an OVMX join is `CLUSTER_NODES=3`. The refusal
reproduces on a completely independent cluster running a *different* SIMH binary.
**Each pod is now both a reproducer and a virgin cluster**, which is what makes
§4f.2 possible.

> **⚠ The prior-admission sidecar is part of the identity and it lives beside the
> store INSIDE the pod.** `lab2run.sh` re-copies the store per run, so without
> explicitly pulling `<store>.cluster` back to the host afterwards, every run is
> a first join and the rejoin can never be tested. The runner does this now, and
> logs the byte count each way. If a "rejoin" on lab-2 joins cleanly, check this
> before believing it.

### 4f.2 ⭐⭐⭐ THE DISCRIMINATOR — the refusal is the CLUSTER's state, not ours

`OVMXL1` was refused by pod A at 01:23. Three minutes later the **same store, the
same binary, and the same prior-admission sidecar carried across** were presented
to pod B — a cluster that had never seen this identity.

| run | identity | cluster | result |
|---|---|---|---|
| `L1b` | `OVMXL1` (has been admitted before) | pod A | **REFUSED** |
| `L1c` | `OVMXL1`, **sidecar carried** | **pod B — virgin** | **JOINED, 15 s** |
| `L2` | `OVMXL2`/1302 **fresh** | pod A | **JOINED** — pod A still admits anyone |

**This is the first POSITIVE proof of §4d.8.** Until now "per-identity state on
every peer, surviving our death" was inferred from absences — not in the frame,
not about which peer, no wait works. It is now demonstrated directly: identical
binary, identical identity, identical persisted state, and one cluster refuses
while another admits within three minutes of each other.

**Two things are killed by this.**

1. **OVMX's own persisted state is NOT the cause.** The `.cluster` prior-admission
   record travelled with the identity and pod B admitted it anyway. Any theory of
   the form "we behave differently once we know we have joined before" is dead.
2. **`L2` closes the obvious escape.** Pod A had not simply stopped admitting
   nodes; it admitted a fresh identity immediately after refusing `OVMXL1`.

### 4f.3 ⭐⭐⭐ WHAT A REAL REJOIN LOOKS LIKE FROM THE PEER — the missing reference

The archive's one real-rejoin specimen (§4c.2c) is **wire-only**. Nobody had ever
watched a real node rejoin through SDA's CDT table — the exact layer where §4e.3
characterised OVMX's refusal. `tools/lab2rejoin.sh` (new) does it on a disposable
pod: VAX2 is **SIGKILLed** (not shut down — that would be a class-0x04 graceful
departure, a different case), left dead 81 s, then rebooted under an unchanged
SCSNODE/SCSSYSTEMID while VAX1 stays parked inside SDA sampling
`SHOW CONNECTIONS/NODE=VAX2`.

The whole cycle, from VAX1's console (`R1`, no console overrun):

```
VAX2 healthy      CDTs: VMS$DISK_CL_DRVR, MSCP$DISK, SCA$TRANSPORT,
                        VMS$VAXcluster            -- all open
%CNXMAN, lost connection to system VAX2
%CNXMAN, timed-out lost connection to system VAX2
%CNXMAN, proposing reconfiguration of the VAXcluster
%CNXMAN, removed from VAXcluster system VAX2
%CNXMAN, completing VAXcluster state transition
Node VAX2 (csid 00010002) has been removed from the VAXcluster

  >>> nine consecutive SHOW CONNECTIONS/NODE=VAX2 samples: NOTHING AT ALL <<<

                  CDTs: MSCP$DISK (rem B01A0009)  -- open
                        VMS$VAXcluster (rem B01A0008) -- open
%CNXMAN, received VAXcluster membership request from system VAX2
%CNXMAN, proposing addition of system VAX2
%CNXMAN, completing VAXcluster state transition       <-- ADMITTED
```

**Two findings, and the second is the one to chase.**

1. **The peer holds NO CDTs at all for a departed node.** Nine samples across the
   dead window return empty. So whatever per-cluster state §4f.2 just proved
   exists, **it is not in the CDT table** — it is in the cluster block / CSB
   layer, which is where §4d.6 and §4d.8 already pointed. The CDT table is a
   *symptom* surface, not the state itself.
2. **A real returning node has `MSCP$DISK` OPEN at the moment its membership
   request is processed.** Side by side at the same instant:

| CDTs the peer holds for the returning node | real VAX2 — **ADMITTED** | OVMX — **DROPPED** (§4e.3) |
|---|---|---|
| `VMS$VAXcluster` | **open** | open |
| disk connection | **`MSCP$DISK` open** | **`VMS$DISK_CL_DRVR` `con_sent`, remote con ID `00000000`** |
| directory | — | `SCS$DIR_LOOKUP` `disc_sent` |

The real node's disk connection is **established**; OVMX's is a connect request
from the peer that we never answered. §4e.4 showed that *initiating* our own disk
run does not fix this — the peer ignores our `SCS$DIRECTORY` connect. **The
unanswered direction is the peer connecting to US**, and that is now the sharpest
remaining difference between an admitted rejoin and a dropped one.

> **⚠ LAB-2 PROVENANCE.** §4f.3 is lab-2 only, on the x86_64 SIMH build. It does
> not contradict any lab-1 result — it agrees with §4c.2c, which independently
> shows a real VAX rejoining — so the "reproduce a contradicting lab-2 result on
> lab-1" rule is not triggered. But the CDT detail has never been seen on lab-1,
> and if it ever becomes load-bearing for a fix, take it there first.
>
> **⚠ `lab2rejoin.sh` has a known flaw.** Its `### T+<n>s` markers are written to
> the `.conn` file as the run proceeds, but the console slice is appended *in one
> block at the end*, so the markers do not interleave with the samples and every
> line appears to carry the last timestamp. The console log is still strictly
> sequential, so ORDER is trustworthy and absolute TIMING is not. Fix it by
> slicing the console incrementally per sample, the way `connpoll.sh` does.

### 4f.4 Where this leaves the investigation

The target has moved from "which byte do we get wrong" to a much narrower place,
and every one of these is now grounded rather than inferred:

- the deciding state is **per-cluster and peer-side** (§4f.2, positive proof);
- it is **not in the CDT table** (§4f.3, a departed real node leaves none);
- it **is not cleared by time** (§3.1, 15 h) and not by which peer we ask (§4d.7);
- the observable difference at the decision point is that a real returning node
  has its **disk connection open** and OVMX has an **unanswered inbound connect**
  (§4f.3 vs §4e.3);
- and OVMX **initiating** that traffic itself does not help (§4e.4).

**The next experiment is named: find out why OVMX never answers the peer's
`VMS$DISK_CL_DRVR` CONNECT-REQUEST on a rejoin.** It is inbound, so it is in our
capture and possibly in `SCSD-T-CMIN`; the question is whether it arrives and we
discard it (a fifth deafness bug, cf. §4d.10's `0x7b`) or never arrives at all.

> **A dead end already eliminated, so nobody repeats it.** The cheap version of
> this — `strings -a <pcap> | grep -c 'MSCP$DISK'` to see whether the peer ever
> names that SYSAP toward us — **does not work**: it returns **0 on the JOINED
> capture too** (`w3A`), where those connections demonstrably happen. SYSAP names
> are not recoverable as plain ASCII from these captures. This needs the lab
> decoder (`tools/cm.py`, `docs/clean-room/tools/af2scan.py`) and is therefore a
> **delegated capture-agent task**, not a grep — dispatch it per §0's doctrine
> rather than reading the bytes in the orchestrator.

---

## 4g. ⭐⭐⭐ THE PEER ASKS FOR OUR DISK SERVER, IS TOLD YES, AND DECLINES TO CONNECT

**Delegated capture analysis (§0 doctrine — the orchestrator read no packet
bytes) against the matched lab-1 pair `d94-w3A` (JOINED) and `d94-w1C`
(REFUSED).** This closes the question §4f.4 named and it does not close it the
way that section guessed.

### 4g.1 The reference signature, and the fact that it is a PRECONDITION

The frame that produces `SCSD-I-MSCPSRV` is an inbound CONNECT-REQUEST:
`msgtype@30 = 0x5b`, `len 124`, `fmt@31 = 0x13`, `op@60 = 0`, `rcid@64 = 0`,
**`bytes[76:92] = "MSCP$DISK       "`** (the target SYSAP — our MSCP server) and
**`bytes[92:108] = "VMS$DISK_CL_DRVR"`** (the peer's local process). In `w3A`
three arrive, one per peer, at +0.23 / +0.93 / +1.03 s, and OVMX answers each
within 5–12 ms.

**It precedes admission in every successful run measured** — not just `w3A`:

| joined run | MSCP$DISK accepts BEFORE `XITDONE` | first accept, relative to admission |
|---|---|---|
| `w1A` | 6 | −14.5 s |
| `w2A` | 3 | −8.2 s |
| `w3A` | 3 | −8.4 s |
| `w4A` | 3 | −7.0 s |
| `w5A` | 6 | −13.5 s |

Three or six — one or two per peer. **Every refused run has zero.** Together with
§4f.3 (a real returning VAX has `MSCP$DISK` *open* at the moment its membership
request is processed) the disk connection is a **precondition of admission**, not
a consequence of it. This is the §4d.9 cause-or-effect question, resolved the
other way for this particular counter.

### 4g.2 ⭐⭐⭐ On a rejoin the frame NEVER ARRIVES — and it is not our deafness

Searched over all 1626 `0x6007` frames / 169 s of `d94-w1C`:

- frames carrying `"VMS$DISK_CL_DRVR"`: **0** (`w3A`: 56)
- connect-requests (`op@60 == 0 && rcid@64 == 0`) of ANY msgtype: only
  `SCS$DIRECTORY`×3 and `VMS$VAXcluster`×3. **Zero `MSCP$DISK`.** (`w3A`: 56)

**The negative is trustworthy, and this is the part that matters.** Each peer's
`send_seq` stream to OVMX in `w1C` runs **1..11 with no gaps**. Where `w3A`'s
peer spends `ss=6` on the `MSCP$DISK` CONNECT-REQUEST, `w1C`'s peer spends `ss=6`
on the `VMS$VAXcluster` lookup. **The step is absent from the peer's own
numbering** — so nothing was lost on the wire, dropped by the capture, or
discarded by us.

**And the peer had already been told the server was there.** In `w1C` the peer
runs the identical directory dialogue minus that one message: `ss=3`
`MSCP$TAPE` lookup, `ss=4` and `ss=5` `MSCP$DISK` lookups — **and OVMX answered
both AFFIRMATIVE** (frames 174/178). A byte-diff of OVMX's affirmative response
against the successful run's differs **only** in Con.IDs, one datagram-ID byte at
`@28`, and the incarnation at `@36`.

> **The peer asked whether we have an MSCP$DISK server, we said yes, and it
> declined to connect anyway.**

### 4g.3 The "fifth deafness bug" is REFUTED — there is no gate to fix

`scsd.c:4617–4629` already accepts all three msgtypes
(`buf[30] ∈ {0x4b, 0x5b, 0x7b}`) and matches
`memcmp(buf+76, "MSCP$DISK       ", 16) == 0`. It fired 39× in `w3A`. **Nothing
reaches it in `w1C`.** §4f.4 expected a `0x7b`-style deaf gate (§4d.10); there
isn't one. Do not go looking for it again.

Downstream and consistent: OVMX's own client-side disk discovery is gated on the
peer's `op=6` (§4e.4), which rides the peer's `ss=14` — and `w1C`'s peers stop at
`ss=11`, so it never arrives either. Both the inbound and outbound halves of the
disk phase are missing for the same upstream reason.

### 4g.4 ⚠ ONE AGENT CLAIM CHECKED AND CORRECTED

The analysis reported inbound frame counts and concluded the OVMX-directed
traffic collapse was "far steeper" than the lab's:

| | JOINED `w3A` | REFUSED `w1C` | ratio |
|---|---|---|---|
| inbound to OVMX | 651 | 76 | **8.6×** |
| peer↔peer (neither endpoint OVMX) | 993 | 105 | **9.5×** |

**Its own numbers say the opposite** — peer↔peer fell slightly *more*. The whole
lab is quieter during a refused run because no cluster state transition happens,
and the OVMX-directed drop is unremarkable against that background. **Do not cite
the traffic collapse as evidence of targeted silence.** (Guardrail 13: verify an
agent's claim against a cheaper oracle — here, arithmetic — before building on
it.)

### 4g.5 Where this leaves it — and the exact next experiment

The decision is made **by the peer, before it would connect to our disk server**,
and is visible as a missing step in the peer's own send sequence. It is not in a
frame we send, not a byte we get wrong, and not something we fail to answer.

**The agent's one INFERRED explanation is testable and it is the next
experiment.** It proposes that the peer's disk class driver still holds a CDT for
this identity from the prior membership, believes a connect is already
outstanding, and therefore never issues a new one — which is exactly the
`VMS$DISK_CL_DRVR` / `con_sent` / `Remote Con. ID 00000000` CDT §4e.3 observed.

**§4f.3 already supplies the control, and the asymmetry is the whole finding:**
after a REAL node dies, the peer holds **zero** CDTs for it — nine consecutive
empty samples across the dead window. If OVMX's identity instead leaves a
`VMS$DISK_CL_DRVR` CDT stranded in `con_sent` across our death, **that is the
per-identity state §4f.2 proved must exist**, in a form we can see and name.

### 4g.6 ⭐⭐ THAT EXPERIMENT WAS RUN IMMEDIATELY — and the inference is DEAD

It needed no lab run at all: lab-1's VAXes were up and three OVMX identities had
already joined and died. `SHOW CONNECTIONS/NODE=` on VAX3 for each:

| subject | state | CDTs held by VAX3 |
|---|---|---|
| `OVMXW5` | joined, then died | **none** |
| `OVMXW3` | joined, then died | **none** |
| `OVMXW1` | joined, died, then refused twice | **none** |
| `VAX1` | live member (positive control) | 4 — `MSCP$DISK`, `SCA$TRANSPORT`, `VMS$DISK_CL_DRVR`, `VMS$VAXcluster`, all `open` |

**The peer holds no stranded CDT for a dead OVMX identity — exactly as it holds
none for a dead real node (§4f.3).** So the agent's INFERRED explanation ("the
peer's disk class driver still believes a connect is outstanding") is refuted,
and the `VMS$DISK_CL_DRVR`/`con_sent` CDT of §4e.3 is **created during the
attempt, not inherited across our death**.

**What survives is sharper for it.** §4f.2 proved positively that the deciding
state is per-cluster and identity-keyed. §4f.3 and §4g.6 now jointly establish
that it is **not in the CDT table for anyone** — real node or OVMX. It is in the
cluster-block / CSB layer, which is where §4d.6 and §4d.8 pointed all along.

### 4g.7 The next experiment, re-aimed at the CSB layer

There is a visible asymmetry already sitting in `SHOW CLUSTER` and nobody has
tested it as the mechanism. After a real VAX2 dies it is **removed** — `%CNXMAN,
removed from VAXcluster system VAX2`, and its CSB goes with it (§4f.3). After an
OVMX daemon dies, lab-1's cluster table still lists **`OVMXW1 | VMX V0.1 |
BRK_NEW`** and **`OVMXW2 | VMX V0.1 | BRK_NON`** — stale CSBs for dead identities
that never clear. §4d.1 saw the same thing on VAX1 (`OVMXR1`, `OVMXR2`, `OVMX`,
all `CSID 00000000`).

> **RUN THIS NEXT — it is a poll, not a code change.** For one identity, capture
> the peer's CSB (`ANALYZE/SYSTEM` → `SHOW CLUSTER`, the per-CSB detail) at four
> points: while joined, immediately after the daemon dies, during the refused
> rejoin, and after. Do the identical four-point capture for a real node across
> `lab2rejoin.sh`'s kill/reboot cycle. **The question is precise: does a dead
> OVMX identity leave a CSB in a state a dead real node does not, and is a
> returning identity matched against it?**
>
> **⚠ Two things must not be assumed here.** §4c.2d already recorded that a
> pristine `BRK_NON` fails on its FIRST attempt, so "`BRK_*` is the quarantine"
> is not a free win — check it. And §4d.1 found VAX3 holding **no** CSB for the
> identity it then aborted, so a stale CSB cannot be the whole story either.
> Both shapes of the refusal (§4d.6) must be classified before comparing runs.

---

## 4h. ⭐⭐⭐ THE CSB, AND WHAT THE REJOIN ATTEMPT DOES TO IT

**§4g.7's probe, run immediately, free — the identities were already on lab-1.**
SDA `SHOW CLUSTER` on VAX3, one dump, six OVMX CSBs and two real ones. The flags
split the identities **exactly** by whether a rejoin was ever attempted:

| identity | history | `Flags` | `CSID` |
|---|---|---|---|
| `OVMXW2` | joined, died | `06040005 long_break,removed,status_rcvd,send_status` | `00010005` |
| `OVMXW3` | joined, died | `06040005 long_break,removed,status_rcvd,send_status` | `00010006` |
| `OVMXW5` | joined, died | `06040005 long_break,removed,status_rcvd,send_status` | `00010008` |
| `OVMXW1` | joined, died, **rejoined ×2 (refused)** | `04000001 long_break,send_status` | **`00000000`** |
| `OVMXW4` | joined, died, **rejoined ×2 (refused)** | `04000001 long_break,send_status` | **`00000000`** |
| `VAX1` | live member | `02060102 member,cluster,selected,status_rcvd` | `00010001` |
| `VAX2` | live member | `02060102 member,cluster,selected,status_rcvd` | live |

All six OVMX CSBs are `State: 09 wait`. **The 2/3 split is exact and has no
exceptions** — `W2`/`W3`/`W5` were fresh joins only, `W1`/`W4` are precisely the
two identities this session rejoined.

### 4h.1 What that means

**The refusal is not a stale quarantine CSB that pre-exists the attempt.** A
died-and-never-returned OVMX identity leaves a CSB that looks *correct*:
`removed`, `status_rcvd`, CSID intact — the ordinary record of a departed node.

**The rejoin attempt itself degrades it.** `removed` and `status_rcvd` are
CLEARED and the CSID is zeroed, leaving `wait` + `long_break,send_status` — and
it never leaves that state. So the peer does react to a returning identity; it
starts something and never finishes it.

**This supplies the "before" that §4d.6 was missing.** §4d.6 noted our CSB after
a refusal carries "zero flags, not even `status_rcvd`, which every long-dead OVMX
CSB on the same node does carry", and could not say when that difference
appeared. It appears **at the rejoin attempt**, and the pre-attempt state is now
recorded above.

### 4h.2 ⭐ `send_status` set, `status_rcvd` cleared — and a message we already know is unanswered

The surviving flag pair names the stall directly: the peer intends to **send**
status and has **not received** ours.

That lines up with §4d.10, which found the peers retransmitting `cat 0x01 op 0x01`
in **member form** (`body[12]=0x21`), `msgtype 0x7b`, ~3 s apart, unanswered — the
retransmit form OVMX was deaf to until `9f98dbf`. **We now hear those frames and
are still refused**, which §4d.10 recorded and could not explain. The CSB flags
give the missing half: the peer is in "send this node its status, await its
status" and our reply does not satisfy it.

> **⚠ Not yet established:** that `cat 0x01 op 0x01` member-form IS the status
> exchange the flags refer to. That is an association between two grounded
> observations, not a decode. Confirm it before building on it.

### 4h.3 The next experiment — and it is now a decode, not a poll

1. **Confirm the pairing.** Take the matched pair `d94-w3A` (joined) / `d94-w1C`
   (refused) and decode what OVMX sends in response to the member-form
   `cat 0x01 op 0x01`, in both runs. In the joined run the peer's CSB reaches
   `member,cluster,selected,status_rcvd`; in the refused run it never sets
   `status_rcvd`. **The difference in our reply is the target.** Delegate this —
   it is byte work (§0 doctrine).
2. **Watch the transition live.** Poll the peer's CSB for one identity at 3–5 s
   cadence *through* a refused rejoin, so the exact moment `removed`/`status_rcvd`
   clear can be placed against our frames. `connpoll.sh` already does the SDA
   parking; point it at `SHOW CLUSTER` instead of `SHOW CONNECTIONS`.
3. **Get the real-node control.** Run the identical CSB poll across
   `lab2rejoin.sh`'s kill/reboot cycle. A real VAX2 goes `removed` → readmitted;
   the question is whether its CSB passes through the same `send_status` state
   and *leaves* it, and what it sends that we do not.

**Do not assume `BRK_*` is a quarantine** (§4c.2d: a pristine `BRK_NON` fails on
its first attempt) and **do not assume a stale CSB is required** (§4d.1: VAX3
held none for the identity it aborted). Both remain true and both constrain any
story built on this section.

---

## 4i. ⭐⭐ A REAL ACK BUG, GROUNDED — and it explains §4d.10 and §4e.4, but is not the gate

Second delegated decode of the same matched pair (`d94-w3A` joined / `d94-w1C`
refused). Three results, in decreasing order of certainty.

### 4i.1 §4h.2's association is DEAD — our status reply is byte-identical

The node-status exchange is the `cat 0x01 op 0x14` + `cat 0x01 op 0x01` pair
(the peer's `op 0x01` body carries the member marker `body[12]=0x21`, member
count, votes, cluster-formed / last-transition quadwords, `"V7.3    "`).

**OVMX answers it in ~4 ms in BOTH runs, and the reply is byte-for-byte
identical: 0 differing bytes out of 132, across 4 frames, two peers.** So the
status reply is *not* the discriminator, and §4h.2's suggestion that the
`send_status`/`status_rcvd` flags point at a reply we get wrong is refuted.

> **Also explicitly NOT grounded:** no wire evidence binds a CSB flag to any
> message. §4h.2 called that an association and it stays one — the flag names
> where the peer is stuck, not which frame would unstick it.

### 4i.2 ⭐ The bug: OVMX never advances `recv_ack` on a SEQUENCED frame

**GROUNDED, with a matched control.** In `w1C` OVMX *did* send `0x48`
credit-returns carrying `acked_seq=11` at t=19.64/22.63/25.63 — **and the peers
retransmitted anyway.** What it never sent in time was `recv_ack ≥ 11` on a
*sequenced* (`0x4b`/`0x5b`) frame: its last such frame was `f194` with `ra=10`,
and the next was `f368` at **t=26.844 with `ra=11`**. The peers' retransmits sit
at 22.63/25.63, 23.29/26.32, 23.41/26.43 — exactly 3.0 s apart — and **each
peer's third retransmit never happens, terminating 0.4–1.1 s after that
`ra=11`.**

**The control eliminates the SYSAP-level alternative.** In `w3A` OVMX's SYSAP
`am` stayed 0 for 7.0 s — the peer's `smsg=2` unacknowledged just as long — and
**no retransmit occurred**, because OVMX's *sequenced* `recv_ack` reached 14 at
t=18.776, piggybacked on the disk-discovery round. So the driver is the SCS-layer
`recv_ack` on a sequenced frame; **the `0x48` credit-return is necessary but not
sufficient.**

**This is the mechanism §4d.9 and §4d.10 were missing.** Those sections
established that a refused rejoin's VC is in congestion collapse, that the
collapse precedes our `op 0x02`, and that the peers retransmit `msgtype 0x7b`
into silence — but not *why*. It is because a successful join advances the
sequenced `recv_ack` as a side effect of the disk-discovery round, and a rejoin,
which never runs that round, has no sequenced frame to carry it.

### 4i.3 ⭐ This retro-explains §4e.4's open question — RETX 6 → 0

§4e.4 recorded that with the disk-discovery ungate ON the peers stopped
retransmitting (`0x7b` 6 → 0, one variable, matched kill-switch control) and
explicitly refused to call it progress: *"we finally answered" and "it gave up
sooner" both fit.*

**§4i.2 decides it — it is progress.** The ungate makes OVMX emit sequenced
`PSC_DIR_CONNECT` frames it otherwise never sends (first at +2.9 s, then
retransmitted six times across the run), and those frames carry the advancing
`recv_ack` whose absence drives the retransmits. Confirmed on the runs:

| run | ungate | first `PSCUNGATE` | `0x7b` retransmits |
|---|---|---|---|
| `w4B` | **ON** | +2.9 s | **0** |
| `w4C` | off | — | **6** |

> **Corroboration, not proof.** The ungate changes more than the ack — it opens a
> whole connection. But an independently derived mechanism predicting an already
> measured matched-control result is strong, and it is the best reading available.

### 4i.4 ⚠ AND IT IS STILL NOT THE GATE

The agent's own caveat, and it is correct: **the refusal survives the fix.** The
peers stop retransmitting at t=26.9 s and OVMX's `op 0x02` at t=27.5 s is *still*
answered by nothing at all. `w4B` had `RETX=0` and was refused. So the ack
starvation explains the PEDRIVER collapse and the retransmit storm — real
defects, worth fixing — and does not explain the admission decision.

Nothing else is missing OVMX→peer, either: enumerated by (msgtype, cat, op),
every pre-decision absence sits on the already-known disk-discovery round, and
counts are identical (3/3) for `op 0x14`, `op 0x01`, `op 0x02` and both
`VMS$VAXcluster` connect frames.

### 4i.5 The fix, specified and deliberately NOT yet written

**Implementation-ready:** in response to a sequenced `0x4b`/`0x5b`/`0x7b` frame
whose `send_seq` exceeds the last `recv_ack` OVMX has placed on a *sequenced*
frame to that peer, OVMX must emit a sequenced pure-ack — reference shape a
72/76-byte `0x4b` with `ra=recv_seq`, consuming one `send_seq` (`w1C` VAX1
`f185`/`f187`). Site: `scsd.c:2596`, the `scs_vc_owes_credit` block.

**Deferred on purpose.** No such builder exists — every OVMX 72/76/80-byte frame
today comes from the directory/connect paths — so this means introducing a new
wire frame shape. Under Rule 8 that must be derived from the reference frames on
the wire, carefully, with byte-exact tests in both directions; it is not a
five-minute edit, and §4i.4 says it will not admit us. **Do it as its own item,
for correctness (guardrail 15), not as an attempt at the rejoin.**

---

## 4j. ⭐⭐⭐ THE REAL-NODE CONTROL — a dead real node's CSB is IDENTICAL to ours, and readmission REPLACES it

**Run `C1`, `tools/csbcycle.sh vaxlab-0 C1`, lab-2 pod `vaxlab-0`.** SIGKILL the
real VAX2, sample SDA `SHOW CLUSTER` on VAX1 every 14 s across kill → removal →
reboot → readmission. The pod also carried two OVMX identities, so **all three
histories appear in the SAME dump, from the same peer, in the same run** — the
in-frame control set §4h could not have.

**Identity provenance, proven from the run logs, not assumed:**

| identity | runs | verdict |
|---|---|---|
| `OVMXL3` | `L3` 02:02 (virgin) → `L3b` 02:07 (same identity) | `JOINED` → **`NOT JOINED`** = refused rejoin |
| `OVMXL4` | `L4` 02:07 (virgin), never returned | `JOINED`, then died |
| `VAX2` | real node, SIGKILLed 02:10:59, rebooted 02:12:24 | readmitted 02:12:52 |

Both OVMX runs logged `identity on the wire: OVMXL3`/`OVMXL4`, so each reached
the wire under the name it claims.

**Refusal shape, established before comparing anything (§4d.6 requires it).**
VAX1's console across the whole pod history gives all three events side by side:

| event | window | peer console |
|---|---|---|
| `OVMXL3` **fresh join** | 02:02:31 | `received VAXcluster membership request from node OVMXL3` → `proposed addition` → transition |
| `OVMXL3` **refused rejoin** | 02:05–02:07 | **NO membership request, NO proposed addition, NO transition** — only `lost connection to node OVMXL3` at 02:07:00 when OVMX exits |
| `OVMXL4` fresh join | 02:08:08 | full machinery: request → `proposed addition` → transition |
| `VAX2` readmission | 02:12:52 | full machinery: request → `proposed addition` → transition |

So **`L3b` is the `s3B` "dropped on the floor" shape**, not the `r1B`
"proposed and aborted" shape: nothing refused it, the add-member request never
produced any machinery. Every comparison in §4j is therefore
*dropped-on-the-floor rejoin* vs *real readmission*.

> **⚠ One thing the console adds that §4d.6 did not have:** the peer logs
> `lost connection to node OVMXL3` at 02:07:00 for the *rejoin* attempt, so a
> connection existed at some level and was torn down when OVMX exited — even
> though no membership machinery ever ran and no CDT was ever allocated. Worth
> reconciling against §4g.2's "declines to connect"; it is not obviously the
> same layer. Not resolved here.
>
> Note also that §4d.6 recorded the `s3B`-shape CSB as `open` with flags
> `00000000` **during** the stall, while §4j sees `09 wait` /
> `04000001 long_break,send_status` **after** OVMX exits. Different times, not a
> contradiction — but do not quote one as the other.

### 4j.1 ⭐⭐⭐ The answer: a dead real node keeps a CSB, and it is OUR CSB exactly

For the 85 s it was dead, VAX2's CSB was **byte-identical in flags to a dead
OVMX identity's**:

| identity | state | CSID | Flags | CDT |
|---|---|---|---|---|
| `VAX2` dead (T+28s…T+99s) | `09 wait` | `00010002` | `06040005 long_break,removed,status_rcvd,send_status` `vcc` | `00000000` |
| `OVMXL4` joined, died | `09 wait` | `00010004` | `06040005 long_break,removed,status_rcvd,send_status` `vcc` | `00000000` |

**Identical.** Same state, same flag word, same absent CDT, CSID retained by both.

**So: DOES a dead real node keep a CSB? YES. Does it pass through `send_status`?
YES — the entire time it is dead. Does it LEAVE it? YES.** Per the decision rule
this run was built on: **holding a CSB is not the problem, and what the peer does
with it is.** Every theory in which OVMX's surviving CSB is itself the asymmetry
is now dead — a real node's is indistinguishable from ours.

### 4j.2 ⚠ PARTLY REFUTED — readmission allocates a new CSB, but so does a REFUSED OVMX rejoin

> **⛔ THE OVMX COLUMN OF THE TABLE BELOW IS WRONG — see §4L.2.** A refused OVMX
> rejoin **also** gets a brand-new CSB at a new address **and** a CDT; watching it
> live shows `879EB040` freed and `879EDFC0` allocated. The claim "OVMX's is
> mutated in place, no new CSB, no CDT" came from sampling `OVMXL3`/`OVMXL4`
> **after their processes had exited** and comparing that corpse against a LIVE
> real-node readmission. The real-node half of this section is unaffected and
> still holds.

At `T+113s` VAX2 returns, and the CSB **address changes**:

| sample | VAX2 CSB address |
|---|---|
| `T-PRE` … `T+99s` (9 consecutive samples) | `87935140` |
| `T+113s` | **`879F4080`** |

The old block is *gone from the dump entirely* — not present in the CSB list nor
as a block. `OVMXL3`/`OVMXL4`/`VAX1` addresses never move across any sample, so
this is not renumbering. The new block is visibly fresh: `CSID 00000000`,
`LNM Seqnum 0000000000000000`, `Ref. time 1-JAN-2001 00:00:00` (uninitialised),
`Ref. count 1`, a **new CDT `8794B0C0`** and a new SB `8794C940`.

**And that is exactly what OVMX never gets.** The three-way contrast, one dump:

| | dead (both) | **real** rejoin `T+113s` | **OVMX** rejoin `OVMXL3` |
|---|---|---|---|
| CSB | — | **NEW address** `879F4080` | same address `879F2780`, degraded in place |
| State | `09 wait` | **`01 open`** | `09 wait` |
| `long_break` | set | **CLEARED** | **set** |
| `status_rcvd` | set | **retained** | **CLEARED** |
| `removed` | set | cleared | cleared |
| CSID | retained | `00000000` | `00000000` |
| CDT | `00000000` | **`8794B0C0` allocated** | `00000000` |
| Flags | `06040005` | `02040000 status_rcvd,vcc` | `04000001 long_break,send_status` |

The discriminator pair is **inverted**: a real return clears `long_break` and
keeps `status_rcvd`; an OVMX return keeps `long_break` and clears `status_rcvd`.

### 4j.3 ⚠ This CORRECTS §4h — CSID zeroing is normal, not damage

§4h read the rejoin attempt's zeroing of the CSID as part of a degradation.
**It is not.** The real node's readmitted CSB also carries `CSID 00000000`.
Zeroing the CSID is the ordinary start of readmission, and it must be struck
from the list of things that look wrong. What survives from §4h is narrower and
still exact: the OVMX rejoin clears `status_rcvd`, retains `long_break`, stays
in `09 wait`, and gets no CDT and no new CSB.

### 4j.4 The ordering, and where the OVMX rejoin actually dies

VAX1's console for the same window:

```
%CNXMAN,  received VAXcluster membership request from system VAX2
%CNXMAN,  proposing addition of system VAX2
%CNXMAN,  completing VAXcluster state transition
OPCOM 02:12:52.94  Node VAX1 (csid 00010001) completed VAXcluster state transition
```

The `T+113s` dump — rendered at ~02:12:52, with `CLUB Nodes 1` and VAX2 not yet
a member — **already shows the new CSB and its CDT**. So the new CSB + CDT are in
place at or before the membership request, and the membership protocol runs
*after* them.

> **⚠ Sub-second ordering is NOT resolved at this cadence.** The 14 s sample and
> the console line fall within the same second. "New CSB and CDT exist no later
> than the membership request" is what is grounded; "they strictly precede it"
> is not. Re-run at 2–3 s cadence if that distinction is ever load-bearing.

**This places the failure earlier than the membership protocol.** OVMX's rejoin
gets no new CSB and no CDT, so it never reaches the stage VAX2 reaches in this
run. That is consistent with §4g.2 — the peer asks about our MSCP disk server, is
told affirmative, and **declines to connect** — and "declines to connect" and "no
CDT allocated" are plausibly the same event seen from two oracles. It is also
consistent with §4i.4: the ack bug is real and downstream of this, which is why
fixing it does not admit us.

### 4j.5 The next experiment

The question is now sharp and singular: **what makes the peer allocate a fresh
CSB+CDT for a returning identity, and why does an OVMX rejoin get its old CSB
mutated instead?** Both nodes were `removed` before returning, so `removed`
alone is not the trigger. The most likely reading — untested — is that the new
CSB is created off a newly *accepted* VC/connection, and OVMX's rejoin never
gets one accepted, so the peer has nothing to hang a new CSB on and touches the
old one instead. Test it by watching the peer's CSB **and** its VC/CDT state
together through both a real reboot and an OVMX rejoin (`scacppoll.sh` +
`csbcycle.sh` cadence, one pod each).

**Do not re-run §4h's poll.** It is answered.

### 4j.6 ⚠ Two tooling defects in `csbcycle.sh`, both real

1. **Console input died at the reboot.** Samples `T+127s` onward are EMPTY — 17
   `SHOW CLUSTER` sends, no echo, no output. The last console byte is the
   transition line at `T+113s`. VAX1 was still wedged afterwards
   (`lab2login.sh` timed out at 120 s), so the pod is spent. The backgrounded
   `kubectl exec … nodedrv.py &` that reboots VAX2 is the prime suspect for
   disrupting VAX1's console input path. **The run survived only because the
   event completed inside the first 113 s.** Fix before reusing.
2. **Markers glue to the previous sample.** `tail -c +$start` output need not end
   in a newline, so the next `########## T+Ns ##########` lands on the same line
   and `grep '^#####'` finds one marker in 26. Split unanchored
   (`re.split(r'#{10} (T[^#]*?) #{10}', raw)`) or `echo` a newline first. This is
   the "markers looked wrong" symptom flagged at handoff — the markers are all
   present and correct; only the anchoring was wrong.

> **⚠ DEFECT 1's DIAGNOSIS IS PROBABLY WRONG — see §4k.7.** `lab2rejoin.sh`
> reboots VAX2 with the *identical* backgrounded `kubectl exec … nodedrv.py &`
> and its console survived. The difference is output volume:
> `SHOW CLUSTER` is ~6.3 KB per sample against `SHOW CONNECTIONS/NODE=`'s few
> hundred bytes, and the death coincides with the OPCOM flood at readmission —
> the documented console-overrun mode (§4e.1, and `connpoll.sh`'s first version).
>
> **✅ BOTH DEFECTS ARE NOW FIXED AND THE FIX IS VERIFIED** (smoke run `SMOKE` on
> `vaxlab-1`). `csbcycle.sh` now samples `SHOW CLUSTER/NODE=<node>` per tracked
> node — **1087 bytes/sample against the full dump's 3078–6269** — takes the full
> `SHOW CLUSTER` only at start and end where no OPCOM flood is running, emits a
> leading newline before every marker (**anchored marker count 6/6, previously 1
> of 26**), and aborts loudly after `MAXEMPTY` consecutive empty samples instead
> of writing 17 worthless ones. Default cadence tightened 14 s → 6 s, which §4k.7
> needs anyway.

---

## 4k. ⭐⭐⭐ THE REFERENCE REJOIN, DECODED — the peer initiates everything, and it stops at one exact step

> **Read §4k.5 first if you read nothing else** — it is the divergence point.
> §4k.1's role reversal is real for a *real* node but is **not** the OVMX
> discriminator (§4k.8 is the null result). §4k.3 and the first two drafts of
> §4k.9 are retracted in place; the surviving conclusions are §4k.4, §4k.5,
> §4k.6 and the two null results.

**Two delegated decodes, run in parallel, against specimens that already
existed.** §4c.2c named this comparison as where effort belonged and it was never
run; §4d–§4j went elsewhere. It is now run.

- **Reference:** `captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` —
  a real VAX3, `kill -9`'d, crash-removed class `0x03`, rebooted under an
  unchanged `SCSNODE`/`SCSSYSTEMID`, readmitted. 19,930 frames.
- **Matched OVMX pair:** `work/d94-r2A.pcap` (`OVMXR2` joins, pure mode) /
  `work/d94-r2B.pcap` (same identity ~2.8 min later, refused).
- Both lab-1, so the comparison is lab-consistent.

### 4k.1 ⭐⭐⭐ The structural finding

**On a rejoin the PEER is the initiator of everything and the returning node is
merely the acceptor. On a first-time join the joiner initiates.** Grounded
frame-by-frame, with a first-join control from a third capture:

| step | first-time join (`vax3-2to3`) | **crash-rejoin** (this specimen) |
|---|---|---|
| `0x41` START opens the VC | **VAX3** (the joiner) | **VAX1 (the peer)**, idx 1176 |
| `SCS$DIRECTORY` connect first by | **VAX3** | **VAX1 (the peer)**, idx 1184 |
| `VMS$VAXcluster` connect by | **VAX3** | **VAX1** idx 1202 **and VAX2** idx 1248 |
| first `cat 0x01 op 0x14`+`op 0x01` config | **joiner** | **peer** idx 1207/1208; node reciprocates |
| node's own directory + `MSCP$DISK` client walk | yes | yes, idx 1264–1295 |
| `op 0x02` membership request | joiner → one peer | joiner → **one** peer (VAX2) |

The returning node's *entire* job before `op 0x02` is: emit one multicast HELLO,
answer `0xb2` with `0xb3`, **answer** the peer's START, **accept** the peer's
directory and `VMS$VAXcluster` connects, answer the directory lookups honestly,
**reciprocate** the config, then run its own directory + `MSCP$DISK` client walk
and send `op 0x02`. Total elapsed, first post-reboot frame → transition
complete: **2.512 s.**

### 4k.2 ⭐⭐ The peer's decision to re-probe is NOT content-driven

VAX3's post-reboot HELLO (idx 1172) is **byte-identical to its last pre-crash
HELLO except bytes 96..103**, which is a VMS absolute-time quadword (all three
nodes advance it at ~1.000 s per wall second and it decodes to the right
calendar date — *inferred*, not oracle-confirmed).

So the HELLO carries **no incarnation counter, no "I am new" flag, no boot
marker.** The peer re-probes because it hears a node it holds no verified
channel to. Nothing in the returning node's first frame announces the return.

### 4k.3 ⚠ RETRACTED — the "incarnation counter is always 1" result was an OFFSET ERROR

> **This section first reported `[22:24]` as `0x0001` on all 12 STARTs of the
> crash-rejoin, and concluded the field does not distinguish incarnations. THAT
> WAS WRONG.** `[22:24]` is **payload-relative**; the payload starts at abs 14,
> so the field is at **abs 36**. Abs 22 — what was actually read — is the §4(a)
> connect flag, constant everywhere. The agent caught and retracted this itself
> when pushed for raw bytes. **See §4k.9 for the corrected result, which reverses
> the conclusion.** Keep this heading so nobody re-derives the retracted version.

What survives from the original section, unaffected by the offset error:

The other START quadword, abs 80..87, is **not** a boot stamp: VAX3's reads
2026-08-01 12:05:21 — 4.5 h *before* this reboot — and sits 0.163 s from VAX2's.
A persistent, disk-resident value. Undecoded.

VAX3's START carries `send_seq = 1` while VAX1's carries a *continuing* `11509`
(VAX2's `8990`) — but §4k.8 shows OVMX already reproduces that exactly, so it is
not a candidate delta either.

### 4k.4 ⚠ §4c.8 IS STALE — byte-verified, and it was load-bearing

§4c.8 states: *"OVMX does none of it, ever — its only outbound CONN-REQ to the
coordinator is the `VMS$VAXcluster` one."* **Both halves are wrong for `r2A`.**

Challenged and re-verified from raw Ethernet bytes (source MAC read at offset
6..11, SYSAP name pair at offset 76..111, not from any labelling logic):

| idx | t | dst | target SYSAP | requestor |
|---|---|---|---|---|
| 79 | +2.1378 | VAX2 | `SCS$DIRECTORY` | `SCS$DIR_LOOKUP` |
| 94 | +2.1390 | VAX2 | `MSCP$DISK` | `VMS$DISK_CL_DRVRV5.0` |
| 160 | +2.3796 | VAX3 | `SCS$DIRECTORY` | `SCS$DIR_LOOKUP` |
| 178 | +2.3803 | VAX3 | `MSCP$DISK` | `VMS$DISK_CL_DRVRV5.0` |
| 282 | +3.9376 | VAX1 | `SCS$DIRECTORY` | `SCS$DIR_LOOKUP` |
| 296 | +3.9387 | VAX1 | `MSCP$DISK` | `VMS$DISK_CL_DRVRV5.0` |

**6 outbound CONN-REQ, all sourced from OVMX's MAC: 3× `SCS$DIRECTORY` + 3×
`MSCP$DISK`, and ZERO `VMS$VAXcluster`** (every `VMS$VAXcluster` connect in
`r2A` is peer-initiated). So OVMX in pure mode *does* run the joiner's directory
+ MSCP client walk — against **all three** peers, where a real node runs it
against **one**.

**And in `r2B` (refused) OVMX sends ZERO outbound CONN-REQ, to any peer, at any
point.** That is the cleanest arithmetic-checkable divergence in the dataset.

> **⚠ Do not read that as "so make OVMX run the walk on a rejoin".** §4e.4 already
> ungated exactly that and it did **not** admit us, and §4f.3 records the peer
> *ignoring* our `SCS$DIRECTORY` connect on a rejoin. The interesting question is
> the inverse: why does the peer accept that same connect on a fresh join and
> ignore it on a rejoin?

### 4k.5 ⭐⭐ THE DIVERGENCE POINT — the peer's directory teardown never comes

Last identical event in both OVMX runs: OVMX's second outbound `VMS$VAXcluster`
DATA frame with `ra=10` (`r2A` frame 277 @ +2.0297, `r2B` frame 174 @ +2.6993).

- **`r2A`:** 0.0003 s later VAX1 sends `DISC-REQ`, tearing down its round-1
  directory connection. Round 2 opens; OVMX runs its client walk; admitted.
- **`r2B`:** no `DISC-REQ` from any peer, ever, for the remaining 158.6 s. Round 2
  never opens. `DISC-REQ`/`DISC-RSP` counts are 3/3 in `r2A` and **0/0** in `r2B`.

The real rejoin does the same thing `r2A` does: VAX1 tears its directory
connection down (`op 6`/`op 7`, idx 1213–1217) immediately after the config
exchange, and only then does VAX3 open its own.

**So the gate is the peer's directory teardown after the config exchange.** In a
refused rejoin the peer completes round 1 — opens the VC, opens the directory,
asks whether we have `MSCP$DISK`, is told yes, opens `VMS$VAXcluster`, exchanges
config — and then simply stops, holding its directory connection open forever.
Capture integrity was checked (`seqchk.py`: zero sequence gaps, zero truncated
frames in either pcap), so the absence is protocol, not capture loss.

This is peer-side, which agrees with §4f.2 (the deciding state is the
CLUSTER's), §4d.6's "dropped on the floor" shape, and §4j (the peer never builds
a new CSB).

### 4k.6 ⚠ THE DISK QUESTION — DIRECTION MATTERS, and a near-miss correction

The wire decode found the peer's connect to VAX3's `MSCP$DISK` arriving **7.47 s
AFTER** the membership request and being **refused 9 times on a 10.0 s ladder for
90 s while VAX3 was already a member**, flipping to `ACCEPT` only once VAX3's own
disk server came up. That looks like it refutes §4f.3/§4g.1's "a real returning
node has `MSCP$DISK` open at the moment its membership request is processed — a
PRECONDITION".

**It does not, and I nearly recorded a false correction.** Checked against
§4f.3's own `R1.conn` — whose *order* is trustworthy even though its absolute
timing is not — the console is unambiguous: `MSCP$DISK` open (line 216) →
`VMS$VAXcluster` open (233) → `received VAXcluster membership request` (250).

**Both are true because they are different directions:**

| direction | when | outcome |
|---|---|---|
| returning node → **peer's** `MSCP$DISK` (node is client, `VMS$DISK_CL_DRVR`) | **BEFORE** `op 0x02`, by 102 ms | accepted; full SET-CTLR-CHAR + GET-UNIT-STATUS walk |
| peer → **returning node's** `MSCP$DISK` (peer is client) | **AFTER** admission, +7.47 s | refused 9× for 90 s, then accepted. **NOT a precondition** |

**Always state the direction when citing the disk connection.** The
before-`op 0x02` one is the node's own client walk (§4c.8's subject, and absent
in `r2B`); the post-admission one is the peer mounting our served disks and is
not a gate on anything.

> **⚠ One flagged inference, deliberately NOT acted on.** The decode argues
> `op 4`/`op 5` is a **REFUSAL** pair rather than the "CONNECT-ACCEPT (alt) /
> confirm" that §4(m) currently records as grounded — on the evidence that no
> `op 10` ever rides that Con.ID pair, each retry mints a fresh Con.ID on a fixed
> 10 s cadence, the ladder stops the instant an `op 2` ACCEPT arrives, and
> `vax3-2to3` carries 24 corroborating `op 4` frames with the same shape.
> **Plausible and important, but it contradicts a grounded spec line. Re-check it
> against SDA `SHOW CONNECTIONS` on a live refusal before editing the spec.**

### 4k.7 What this makes the next experiment

The question is now singular and peer-side: **after the config exchange, what
does the peer need in order to tear down its directory connection and let the
joiner proceed — and what per-identity state stops it doing so for a returning
OVMX identity?**

That lines up exactly with §4j's unanswered half (what makes the peer allocate a
fresh CSB+CDT). They are plausibly the same decision seen from two oracles.

**The experiment:** poll the peer's CSB at 2–3 s cadence *through* the window
between the config exchange and the expected directory teardown, on a refused
OVMX rejoin AND on a real-node readmission, and place the CSB free/realloc
against the `DISC-REQ`. §4j bracketed that transition only to `[HELLO, VMS$VAXcluster
connect]` and the wire cannot narrow it further — this needs SDA, not another pcap.

**Prerequisite:** fix `csbcycle.sh` first. Use `SHOW CLUSTER/NODE=<name>` —
**verified supported on this VMS** (`/NODE=` and `/CSID=` both return semantic
errors, not `%CLI-W-SYNTAX`; `/CSB` is a syntax error). Per-node output is a
fraction of the ~6.3 KB full dump, which is what killed the console in §4j.6.

### 4k.8 ⚠ NULL RESULT — the role reversal is NOT where OVMX diverges

§4k.1's role reversal is real *for a real node* (first-time join = joiner
initiates; rejoin = peer initiates). It is **not** the OVMX discriminator.
Checked on both OVMX runs, all three peer relationships, 18 `0x41` frames each:

**The peer sends the first `0x41` START in every single instance, in both `r2A`
(join) and `r2B` (refused rejoin). Same for the `0xb2`→`0xb3`→`0xb4` probe
order.** There is no case where OVMX initiates.

Two consequences, and they point in opposite directions:

1. **Against the theory:** the join/rejoin initiation-role axis does not separate
   `r2A` from `r2B`. Drop it as a discriminator.
2. **A separate observation worth keeping:** OVMX is peer-initiated even on its
   *first-time* join (`r2A`), where a real first-timer initiates. OVMX is passive
   at VC setup in both cases — and is admitted anyway in `r2A`, so being passive
   there is evidently not fatal.

**Also null: the `send_seq` restart.** §4k.3 floated it as the only remaining
incarnation signal. OVMX already does it correctly:

| | peer's START `send_seq` | node's own START `send_seq` |
|---|---|---|
| real crash-rejoin | **11509 / 8990** (continuing) | **1** (restart) |
| OVMX `r2B` rejoin | **200 / 56 / 387** (continuing) | **1** (restart) |
| OVMX `r2A` join | 1 (fresh, all peers) | 1 |

`r2B` reproduces the real rejoin's signature exactly, and the peer's continuing
value confirms the peer *knows* it already holds state for us. **We already send
the right thing. Do not spend a commit on `send_seq`.**

### 4k.9 ⛔ THE INCARNATION FIELD, SETTLED — OVMX already emits the reference value

> **⚠ THIS SECTION WAS FIRST WRITTEN WRONG AND IS CORRECTED HERE.** The initial
> reading — "OVMX increments an incarnation field that a real node never
> changes" — is **false**, and the cheaper oracle that refuted it was our own
> source, not another capture. `scs_start.c:112` and `scs_start.h:51` are
> explicit and already GROUNDED byte-exact across 6 `vms-af2` specimens spanning
> values `{1,2,3}`: `[22:24]` is **not** ours to choose. It is the incarnation
> the **member attributes to us**, advertised in the member's directed-HELLO at
> payload `[78:80]` (abs 92) and *echoed* by the joiner. OVMX reads it off the
> wire and never hard-codes it. **OVMX sending `2` is correct behaviour.**
> Guardrail: check an agent's *semantic* claim against the source before
> treating a byte value as a divergence.

Same offset table, payload `[22:24]` (abs `[36:38]`), the field §4(i).B calls the
incarnation counter:

| | value in that field |
|---|---|
| real VAX3, **all 12 STARTs** of a crash-rejoin, both directions | **`0x0001`** |
| real VAX3, all 4 STARTs of `vax3-2to3` first-time join | **`0x0001`** |
| peers' own STARTs in `r2A` **and** `r2B` | **1** |
| **OVMX's own START, `r2A` (first join)** | **1** |
| **OVMX's own START, `r2B` (rejoin)** | **2**, all three peers |

**The corrected bytes, at abs 36 / abs 92, with a full-capture census and zero
residuals — and they say OVMX is already correct.**

| capture | direction | `[22:24]` echoed |
|---|---|---|
| `vax3-2to3` first-time join | VAX3→VAX1/VAX2 | **1** |
| **this crash-rejoin** | **VAX3→VAX1, VAX3→VAX2** | **2** |
| `af2-firsttimer` 1st / 2nd / 3rd incarnation | VX3→VAX1 | **1 / 2 / 3** |
| members' own STARTs (never rebooted) | VAX1→VAX3, VAX2→VAX3 | 1 |

And the advertisement that drives it, at payload `[78:80]` = **abs 92**, carried
**only in directed HELLOs `0xb2`/`0xb3`/`0xb4`** (multicast `0xa0` is `0x0000` in
all 309 frames; the `0x41` START's abs 92 is a different field entirely):

| sender→dest | before crash | after reboot |
|---|---|---|
| VAX1→VAX3 | **1** (4 frames) | **2** (all 63 frames, from `+109.7701` on) |
| VAX2→VAX3 | **1** (3 frames) | **2** (all 57 frames) |
| VAX3→peers | 1 | 1 — unchanged, VAX3 lost its state |
| VAX1↔VAX2 (channel never dropped) | 1 | 1 — never moves |

**Member advertises N → joiner echoes N**, chain grounded end to end across three
captures.

### ⛔ AND THAT KILLS THE ORACLE IDEA — the two cases are IDENTICAL here

Peers advertise **2** to a returning real VAX3 and **2** to a returning OVMX
identity; both echo **2**. **The field reads the same on the successful real
rejoin and the refused OVMX one.** It is therefore *not* a readout of whether the
peer freed the old CSB, and the "free wire oracle for the CSB decision" idea
written in the first draft of this section is dead.

Two things do survive, and both are useful:

1. **`[22:24]` is removed from the candidate-delta list for good.** OVMX already
   emits the reference value on the failing path. This independently confirms §3
   item 5 against the reference for the first time.
2. **The increment happens at PEDRIVER channel re-formation, upstream of any
   connection-manager decision** — VAX1's very first post-reboot frame to VAX3
   (idx 1173, `+109.7701`) already carries `2`, before any VC exists and before
   VAX3 has sent anything but one multicast HELLO. *(Inference, but a tight one.)*
   So this counter is channel-layer bookkeeping, not a consequence of the
   admission decision, and it cannot be used to observe that decision.

**Do NOT "fix" this by pinning `[22:24]`.** The field is an echo; sending a value
the member did not advertise is the `vms-691` stall this code already fixed.

> **Method note, worth more than the result.** This section was written wrong
> **twice** — first "OVMX invents a value a real node never changes" (refuted by
> our own source in 2 minutes), then "the advertised value is a CSB oracle"
> (refuted by the agent's own retraction under a request for raw bytes). Both
> survived initial plausibility because a *value* was grounded while its
> *meaning* was not. **Grounding a byte is not grounding a claim.**

---

## 4L. ⭐⭐⭐ THE DISCRIMINATOR IS ONE FLAG WORD — and §4j.2's OVMX column was measured wrong

**A bracketed triple, one lab-2 pod (`vaxlab-2`), one tool, minutes apart, with
the peer's CSB for OUR identity sampled every 5 s THROUGH each attempt.** This is
what §4h.3 item 2 asked for and never got; `tools/csbwatch.sh` (new, §6) does it.

| run | identity | sidecar | verdict |
|---|---|---|---|
| `M1A` | `OVMXM1`, fresh | none — never admitted anywhere | **JOINED** (`XITDONE=1`) |
| `M1B` | **same `OVMXM1`**, ~6 s later | **carried, 41 bytes** | **REFUSED** (`XITDONE=0`), 108 s |
| `M2A` | `OVMXM2`, fresh | none | **JOINED** (`XITDONE=1`) |

All three logged `identity on the wire`. **Controls bracket the negative on both
sides**, as doctrine requires.

### 4L.1 ⭐⭐⭐ The result — everything is identical except the flags

| at T+5 s | `M1A` → **admitted** | `M1B` → **REFUSED** | `M2A` → **admitted** |
|---|---|---|---|
| CSB before the attempt | **none** (`SCSNODE not found`) | `879EB040` `09 wait` `06040005 long_break,removed,status_rcvd,send_status` | **none** |
| CSB during the attempt | **NEW `879EB040`** | **NEW `879EDFC0`** | **NEW `879EC300`** |
| State | `01 open` | `01 open` | `01 open` |
| CSID | `00000000` | `00000000` | `00000000` |
| CDT address | **`87957700`** | **`87957700`** | **`87957700`** |
| **Flags** | **`02040000 status_rcvd,vcc`** | **`00000000` — NOTHING SET** | **`02040000 status_rcvd,vcc`** |
| next sample | `member,selected` @T+10 s | **unchanged for 108 s** | `member,selected` @T+11 s |

**The peer builds the identical structure for a refused rejoin as for a
successful join — same state, same CSID, literally the same CDT address — and
then never sets `vcc` or `status_rcvd`, and never advances.**

That is the `s3B` "dropped on the floor" shape of §4d.6 (`open`, flags
`00000000`), now with the matched control that names exactly which bits are
missing.

### 4L.2 ⛔ §4j.2's OVMX COLUMN IS REFUTED — and the mistake was methodological

§4j.2 claimed a returning real node gets a **new CSB with a fresh CDT** while
**"OVMX's old CSB is mutated in place"** and gets **no CDT**. The first half
stands. **The second half is false.**

- The old CSB `879EB040` was **freed** and a **new one allocated at `879EDFC0`** —
  verified in the full `CSB list`, which shows exactly **one** `OVMXM1` entry at
  each point (`879EB040` before, `879EDFC0` after). No duplicate, no mutation.
- A **CDT was allocated**, at the same address the successful joins got.

**Why §4j got it wrong: it compared a LIVE real-node readmission against DEAD
OVMX identities.** `OVMXL3`/`OVMXL4` were sampled minutes after their OVMX
processes had exited, so their CSBs showed post-mortem decay (`09 wait`,
`long_break`, `cdt=00000000`) — not what the peer does *during* a rejoin. §4j
never held a pre-attempt CSB address for `OVMXL3`, so "mutated in place" was an
inference across two different identities, not an observation.

> **Guardrail 22: a peer-side sample taken after our process exits measures our
> corpse, not our attempt.** Every CSB/CDT claim must state whether OVMX was
> RUNNING at the sample. §4e.3, §4f.3, §4g and §4j all mix the two.

**Consequently §4j.4's inference is dead too** — "declines to connect" (§4g.2)
and "no CDT allocated" are **not** the same event, because a CDT *is* allocated.
And §4k's framing of the failure as "earlier than the membership protocol"
survives, but the reason is not a missing CSB or CDT.

### 4L.3 What is now the single question

**Why does the peer never set `vcc` and `status_rcvd` on the new CSB for a
returning identity, when it sets both within 5 s for a fresh one — given that it
allocated the same CSB and the same CDT in both cases?**

Everything still standing points at the same instant: §4k.5's missing `DISC-REQ`
(the peer never tears down its round-1 directory connection) and these two unset
bits are almost certainly the same stall seen from wire and from SDA. A wire
correlation across these exact three captures is **dispatched**.

### 4L.4 ⭐ RUN — the two bits are set WITH the CSB, not after it

Re-ran the pair at `CAD=1` (~1.2 s effective) on a third identity, `OVMXM3`:

| | `M3A` fresh join → **ADMITTED** | `M3B` same identity → **REFUSED** |
|---|---|---|
| T+0 … T+2/3 | `NO-CSB` (`SCSNODE not found`) | old CSB `879FBB80` `09 wait` `06040005 …` |
| **first sample with a new CSB** | **T+3: `879FBB80`, `01 open`, CDT `879F5E00`, `02040000 status_rcvd,vcc`** | **T+4: `879EC440`, `01 open`, CDT `879FBF00`, `00000000`** |
| next transition | T+9 `member,selected` | **none — flat for the rest of the run** |

**There is no intermediate state.** In the admitted run the CSB does not appear
empty and then gain flags — the very first sample in which it exists already has
`status_rcvd` and `vcc` set. In the refused run it appears empty and stays empty.

**So the question is not "why are the bits never set later" but "why is the CSB
populated at creation in one case and not the other".** At ~1.2 s resolution the
peer has our status by the time it publishes the CSB on a fresh join, and does
not on a rejoin. This also replicates the whole §4L.1 result on a third
independent identity.

> Caveat: 1.2 s is the console round-trip floor, so this bounds the gap rather
> than proving simultaneity. A sub-second separation would not be visible here.

> **⚠ Do not read `status_rcvd` as "our status reply is wrong".** §4i.1 already
> established that OVMX's node-status reply is **byte-for-byte identical** in
> joined and refused runs (0 differing bytes of 132 across 4 frames). The bit is
> unset for some other reason; §4h.2 made exactly this mistake and was refuted.

### 4L.5 ⭐⭐ THE STATUS-DERIVED CSB FIELDS ARE BLANK — and a counter reading that is NOT safe

> **⚠ THIS SECTION'S ORIGINAL HEADLINE — "the peer received ZERO sequenced
> messages from us" — OVERSTATES WHAT WAS MEASURED. See §4L.9c.** The wire shows
> OVMX delivering `msgseq 1,2` to both peers in the refused run, and the mapping
> from SDA's `Last seq num rcvd` to any wire counter is **not established**. The
> flag word and the blank status-derived fields below are solid; the
> zero-messages *interpretation* is not.

Diffing the **whole** CSB block at T+5 s, not just the flag line. **The two
admitted runs are identical to each other on every field**; the refused run
differs on exactly one cluster of fields — and they are all fields the peer fills
in *from our node-status message*:

| field | `M1A` admitted | `M2A` admitted | **`M1B` REFUSED** |
|---|---|---|---|
| Flags | `02040000 status_rcvd,vcc` | same | **`00000000`** |
| **`Last seq num rcvd`** | **`0002`** | **`0002`** | **`0000`** |
| `Next seq. number` | `0002` | `0002` | `0002` *(same)* |
| `Unacked messages` | 2 | 2 | **0** |
| `Quorum/Votes` | `1/0` | `1/0` | **`0/0`** |
| `Quor. Disk Vote` | 1 | 1 | **0** |
| `Lock mgr dir wgt` | 1 | 1 | **0** |
| `SWVers` | `V7.3` | `V7.3` | **`........`** |
| `HWName` | `OVMX Cluster Node` | `OVMX Cluster Node` | **empty** |
| `Cpblty` | `00000A98 ext_status,cwcreprc,ipc_demult_conn` | same | **`00000008 ext_status`** |
| CSID / CDT / SB / PDT / `Ref. count` | — | — | **identical to both controls** |

**`Next seq. number` is `0002` in ALL THREE — the peer sent us two sequenced
messages in every run. `Last seq num rcvd` is `0002` in both admitted runs and
`0000` in the refused one.**

> **⚠ The plain reading — "the peer received ZERO sequenced messages from us" —
> is REFUTED by the wire (§4L.9c). Treat the counter columns above as an
> unexplained oracle disagreement, not as a measurement of delivery.**

**What IS safe** is the cluster of blank fields. `SWVers`, `HWName`, `Cpblty`,
`Quorum/Votes` and `Lock mgr dir wgt` are all populated from the joiner's
node-status message, and in the refused run every one of them is blank or
minimal while `status_rcvd` and `vcc` are unset — with two matched controls
showing the fully-populated form. **The peer has not APPLIED our status to this
CSB.** Whether that is because it never received it, received it and rejected it,
or applied it to a block it then discarded, is *not* settled by this section —
§4L.8 shows the frames are delivered, correctly enveloped and byte-identical, so
"never received" is the least likely of the three.

### 4L.6 ⛔ SUPERSEDED — the hypothesis this created, and why it is NOT simply `vms-950`

> **Its conclusion ("the discriminator is the envelope") is REFUTED by §4L.8** —
> the envelope is clean. Kept for the `vms-950` distinction at the end, which
> still holds, and so the dead branch is not re-walked.

**Grounded facts, all with matched controls:**
1. The peer sends 2 sequenced messages and receives 0 back (§4L.5, this session).
2. On a **rejoin** the peer's `0x41` START carries a **continuing** `send_seq`
   (`200`/`56`/`387` on lab-2 `r2B`; `11509`/`8990` on the real lab-1 specimen),
   while on a **fresh join** every peer's START carries `1` (§4k.8).
3. OVMX's own START always carries `send_seq = 1` (§4k.8) — which matches what a
   real returning node does.

**The inference (NOT yet grounded):** OVMX's sequenced replies on a rejoin carry
sequence/ack numbering the peer does not accept, so the peer discards them and
never advances `Last seq num rcvd`. On a fresh join both sides start at 1 and the
numbering agrees, so the same code works.

**This also contradicts §3 item 6**, which says *"Both sides restart at 1 on every
attempt"*. §4k.8 measured the peer NOT restarting on a rejoin. That item needs
re-checking — it may have been derived only from fresh-join captures.

> **⚠ Distinguish this from `vms-950` before acting.** §4i.2's bug is that **OVMX
> never advances its own `recv_ack`** on a sequenced frame, so *peers retransmit*.
> This is the **opposite direction**: the peer's count of what **it** received from
> us is zero. They may share a root cause in OVMX's sequence handling, or be
> independent. §4i.4 showed the refusal survives with `RETX=0`, so `vms-950` alone
> is not it — but "our sequenced frames are not accepted" was never tested.

### 4L.7 ⭐⭐⭐ WE DO SEND THEM — OVMX's own log settles it, and the peer discards them

The cheapest oracle in the building, and it was sitting in the run logs the whole
time. `grep CMCONFIG` on `scsd-M1A/M1B/M2A.log`:

| run | config burst | outcome |
|---|---|---|
| `M1A` admitted | `answered member config with add-member burst (2 frames)` **×2 peers**, 08:18:25 | + deferred `op 0x02` |
| **`M1B` REFUSED** | **`… burst (2 frames)` ×2 peers, 08:21:01 — IDENTICAL** | + deferred `op 0x02` |
| `M2A` admitted | `… burst (2 frames)` ×2 peers, 08:23:38 | + deferred `op 0x02` |

`cm_send_config_burst()`'s own contract (`scsd.c:1243`) is that each frame *"is a
sequenced SCS message — it advances the VC `send_seq` (SCS layer)"*.

> **So OVMX emits its two sequenced config frames per peer in the REFUSED run
> exactly as it does in both admitted runs — and the peer's `Last seq num rcvd`
> still reads `0000`. The peer is receiving them and NOT COUNTING them.**

This **refutes** the obvious reading of §4L.6 (that OVMX's sequenced-frame
builders live on a path it never runs during a rejoin — §4k.4 shows zero outbound
`CONN-REQ`, which made that tempting). We send. They are discarded.

It also **narrows the target to the envelope**. §4i.1 established the config
reply's *body* is byte-for-byte identical between joined and refused runs. If the
body is identical and the burst is sent in both, then whatever makes the peer
discard it must be in the SCS envelope — `send_seq` `[20:22]`, its mirror
`[30:32]`, `recv_ack` `[18:20]`, the Con.ID pair — or in peer-side state that
makes an otherwise-valid frame unacceptable.

**Two candidate mechanisms, neither yet grounded:**
1. **Sequence numbering.** On a rejoin the peer's VC sequence *continues*
   (§4k.8) while ours restarts at 1. `scs_seq_init()` sets `send_seq = 1` and
   `scs_seq_note_recv()` takes the max of the peer's `send_seq`, so OVMX should
   ack the peer's continuing value — but whether the peer accepts *our* frames
   numbered from 1 against its own continuing window is untested.
2. **Connection identity.** The burst rides `local=0x…0001` with a per-run remote
   Con.ID. If the peer expects the burst on a different connection than the one
   we send it on, it would ignore it without ever counting it.

**The experiment that decides it:** compare the ENVELOPE fields of the config
burst frames between `d94-M1A.pcap` (counted) and `d94-M1B.pcap` (not counted).
The body is known identical; the envelope is where the difference must be.
**Dispatched.**

**And the peer goes silent immediately afterwards.** Census of OVMX's own log
message types, same three runs:

| tag | `M1A` admitted | **`M1B` REFUSED** |
|---|---|---|
| `SCSD-T-CMIN` (inbound CM messages) | **575** | **4** |
| `SCSD-I-CMACK` | 254 | **0** |
| `SCSD-I-CMRESP` | 234 | **0** |
| `SCSD-I-BARRIER` | 12 | **0** |
| `SCSD-I-CMCONFIG` (our burst) | 2 | **2 — same** |

**We send the burst; the peer sends back nothing at all.** Four inbound CM
messages against 575, no acks, no responses, no barrier — the transaction
machinery never starts. That is §4d.6's `s3B` "dropped on the floor" shape
measured from our own side for the first time, and it agrees exactly with the
peer's `Last seq num rcvd 0000`.

> Note `SCSD-I-PSCUNGATE` appears in `M1B` (2) and **not** in `M1A`, alongside
> `DIRCONN` and `CONNRESP`. So OVMX *did* attempt directory/connect work on this
> rejoin — unlike lab-1's `r2B` (§4k.4, zero outbound `CONN-REQ`). **Do not treat
> §4k.4's zero as universal**; it may be lab- or build-specific. Worth pinning
> down before any claim of the form "OVMX never initiates on a rejoin".

> **⚠ Method note.** Between §4L.6 and here I proposed and killed my own
> hypothesis twice inside ten minutes — first "the member-form `op 0x01` trips
> the `!is_member_txn` gate" (refuted by reading the definition: that gate is
> only `{0x03, 0x05}`), then "OVMX never emits a sequenced frame on a rejoin"
> (refuted by its own log). **Both were refuted by our own source and logs, for
> free, before any capture work.** Check the source and the run log before
> dispatching a capture agent. §4L.8 then killed a third.

### 4L.8 ⛔⭐⭐⭐ THE ENVELOPE IS FINE — the peer simply never answers our `op 0x02`

Wire decode of the same three lab-2 captures. **It refutes §4L.6 and §4L.7's
conclusion and replaces it with something much sharper.**

**Refuted: "the discriminator is in the envelope."** OVMX's CM messages in the
refused run are byte-identical in *content* to both joined runs (payload from
abs 72 on; only the known incarnation echo at abs 36 and transport checksums
differ), **and their envelopes are well-formed and gaplessly incrementing exactly
as in the joins** — `send_seq` abs 34, `recv_ack` abs 32, mirror abs 44 (which is
a pure echo of `send_seq` in all runs and carries no information).
**No envelope anomaly exists.** OVMX also sends plenty of sequenced frames:
20 to VAX1 and 21 to VAX2 in the refused run.

**What actually happens, on the CM connection, msg by msg:**

| msgseq | cat/op | `M1A` join | **`M1B` REFUSED** | `M2A` join |
|---|---|---|---|---|
| 1 | `01/14` node-status | +0.60 s | +0.02 s | +0.92 s |
| 2 | `01/01` node-status | +0.60 s | +0.02 s | +0.92 s |
| 3 | `04/00` | +7.06 s | +9.00 s | +7.12 s |
| **4** | **`01/02` membership request** | **+7.06 s** | **+9.00 s** | **+7.12 s** |
| 5 | `81/03` **response from the peer** | **+7.065 s** | **ABSENT** | +7.12 s |

> **In both joins the peer's response follows our `op 0x02` within 2–6
> MILLISECONDS, then 20+ more messages fire immediately. In the refused run
> msgseq 4 is the last thing OVMX ever sends on that connection, and VAX1 sends
> ZERO frames back on ANY connection for the remaining ~100 s** — only periodic
> multicast HELLO beacons.

So the stall is **the peer's non-response to a byte-identical, correctly
sequenced, correctly enveloped membership request.** Nothing is wrong with what
we transmit.

### 4L.9 ⚠ THE TWO ORACLES DISAGREED — and the obvious explanation is refuted

The wire says OVMX delivered msgseq 1–2 on the CM connection at **+0.02 s**.
SDA says that at **T+4/5 s** the peer's CSB for us reads `Last seq num rcvd
0000`. Both are directly observed. They disagree.

**I hypothesised the CSB-replacement explained it — that OVMX's messages were
counted against the old CSB, which was then freed. THE `M3` DATA REFUTES THAT**,
and points somewhere better. Sequence counters across the replacement, `CAD=1`:

| sample | CSB | flags | `Next seq. number` | `Last seq num rcvd` | `Last ack. seq num` |
|---|---|---|---|---|---|
| `M3A` end (admitted, live) | `879FBB80` | `02060002` | `0023` | `0022` | `0023` |
| `M3B` T-PRE … T+3 s | **`879FBB80` (old, residual)** | `06040005` | **`0025`** | **`0022`** | `0023` |
| `M3B` T+4 s … end | **`879EC440` (new)** | `00000000` | **`0002`** | **`0000`** | `0000` |

**The old CSB did not count our messages either.** Its `Last seq num rcvd` sits
frozen at `0022` — exactly where `M3A` left it — while its `Next seq. number`
advances `0023 → 0025`, i.e. **the peer sent two more and received nothing.**
Then it is replaced by a fresh block that sends two more (`0002`) and again
receives nothing. The peer tried twice, on two different CSBs, and counted zero
both times.

### 4L.9a ⭐⭐ A CANDIDATE MECHANISM — a sequence-context mismatch

> **⚠ ONE LEG OF THIS WAS WRONG AND IS RETRACTED.** The first version claimed
> OVMX answers "~30× too fast" on a rejoin (+0.02 s vs +0.60/+0.92 s), taken from
> the capture agent's relative timings. **Those timings do not share an anchor
> with the daemon clock.** Measured against `SCSD started` in our own run logs the
> first `CMCONFIG` lands at **M1A +2.27 s, M1B +1.30 s, M2A +2.31 s, M3A +2.47 s,
> M3B +2.54 s** — the refused runs are **not** systematically earlier, and `M3B`
> is *later* than its own control. **There is no timing anomaly. Do not cite one.**
> The rest of this section does not depend on it.

Put the counters beside the wire timings and a mechanism falls out.

**Grounded:**
- OVMX's `send_seq` **starts at 1** on every run (`scs_seq_init`, §4k.8) — which
  is what a real returning node does.
- The peer's **old** CSB is at `Last seq num rcvd 0022`, so the next sequenced
  message it will accept on that VC is **`0023`**, not `1`.
- The peer replaces the CSB at ~T+4 s, resetting its expectation to `1`
  (`Next seq. number 0002`, `Last seq num rcvd 0000`).
- OVMX's CM traffic straddles that replacement: msgseq 1–2 early, then `op 0x02`
  later, and it does not re-send the earlier pair.

**The inference (NOT established):** on a rejoin the peer still holds a live VC
context from our previous incarnation whose next expected receive sequence is
`0023`, so OVMX's `send_seq = 1` frames are not acceptable to it. The peer then
tears the CSB down and rebuilds it expecting `1`, but OVMX has already spent
msgseq 1–2. Both sides end up one context apart, which is what "peer sent 2,
received 0 — twice, on two different CSBs" looks like.

**It is consistent with every grounded fact** — the frozen `0022`, the doubled
send-with-no-receive, the zero flags, the blank status-derived fields, the
unanswered `op 0x02`, and §4L.10's `DISC-REQ` never running. It would also
explain why a real node succeeds: §4k.1 shows the peer driving the entire rejoin
handshake, which establishes a fresh context before the returning node sends
anything sequenced.

> **Consistency is not evidence.** Every hypothesis this session was consistent
> with the facts known when it was written, and four have died. The byte-level
> test below is what decides it.

**How to test it — one variable, cheap:**
1. **Delay our burst.** Make OVMX wait before answering the member's config on a
   rejoin (an env-gated delay, with the kill-switch guardrail 21 requires), so
   the peer's CSB replacement lands first. If the refusal turns into an
   admission, the race is real.
2. **Or check the wire directly** for whether the peer's pre-replacement frames
   carry `send_seq ≈ 0023` while ours carry `1`. That is a decisive byte-level
   read on `d94-M3B.pcap` and needs no lab time.

> **⚠ Do NOT "fix" this by starting `send_seq` at `0023`.** A real returning node
> restarts at `1` (§4k.3/§4k.8, grounded across three captures). If our `1` is
> being rejected, the bug is that we answer into a context that should have been
> torn down first — not the value.

### 4L.9b ⛔⛔ THE SEQUENCE-CONTEXT HYPOTHESIS IS DEAD — refuted twice, independently

**Refutation 1 — behavioural (`M3C`, a third attempt).** §4L.9a's premise was that
the peer's surviving context expects `0023` while we send `1`. After `M3B` the
peer's CSB was **already** in a fresh context — `Next seq. number 0002`,
`Last seq num rcvd 0000`. So a third attempt sending `send_seq = 1` should have
matched. **It was refused identically:**

| `M3C` | CSB | flags | `nxt` | `rcvd` |
|---|---|---|---|---|
| T-PRE / T+0 | `879EC440` (M3B's, decayed) | `00000001 long_break` | `0002` | `0000` |
| **T+2 s → end** | **NEW `879FD6C0`** | **`00000000`** | `0002` | `0000` |

`XITDONE=0`, identity proven on the wire. **Refused from a fresh context, exactly
as from a `removed` one.**

**Refutation 2 — byte-level (`d94-M3B.pcap`).** Both sides carry `msgseq = 1` from
their very first frame, on **both** peer connections. Peer→OVMX: VAX1 idx 146/147
`msgseq 1,2`; VAX2 idx 194/195 `msgseq 1,2`. OVMX→peer: `msgseq 1,2` likewise.
**A whole-capture scan for any frame in either direction carrying `msgseq > 20`
returns ZERO hits.** The value `0022`–`0025` never appears on the wire at all,
and there is no observable restart-to-1 transition because it starts at 1.

**So the premise was false.** Do not revive it.

### 4L.9c ⚠ AND THIS UNDERMINES §4L.5's HEADLINE — read that section with care

The wire shows OVMX delivering `msgseq 1,2` to both peers in the refused run.
SDA shows the peer's CSB reading `Last seq num rcvd 0000`. **The mapping between
SDA's CSB sequence fields and the wire counters is NOT established** — the decode
could not identify which wire field SDA renders, and its one candidate
(magnitude-matching OVMX→VAX2's msgseq ending at 34 = `0x22`) is explicitly
inference.

**Therefore:**
- **Still solid in §4L.5** — the flag word (`02040000 status_rcvd,vcc` vs
  `00000000`) and the blank `SWVers` / `HWName` / `Cpblty` / votes /
  `Lock mgr dir wgt`. Those are rendered values, not interpretations, and both
  controls agree.
- **NOT solid** — "the peer received ZERO sequenced messages from us." That is an
  *interpretation* of `Last seq num rcvd`, and the wire contradicts the plain
  reading of it. **§4L.5's headline overstates what was measured.**
- The old CSB's `0022`/`0025` is most likely peer-internal residue attached to a
  stale CSB object, since nothing on the wire continues those numbers.

**What to do about it:** establish the SDA-field ↔ wire-field mapping before any
further argument rests on those counters. That is its own small task and it makes
every future CSB reading trustworthy.

### 4L.9d ⭐ A clean result that survives all of this

`M1B`, `M3B` and `M3C` all refuse, starting from **different** prior CSB states —
`06040005 long_break,removed,status_rcvd,send_status` in the first two,
`00000001 long_break` in the third. Every one of them gets a **new CSB with zero
flags** and stalls there.

> **The refusal does not depend on which decayed state the prior CSB is in, and
> repeats indefinitely for the same identity.** That is consistent with §4c.2d's
> pristine-`BRK_NON` result and extends it: not only is the first attempt refused,
> every subsequent one is, from any prior state.

### 4L.9e ⛔ THE SYSTEM BLOCK (SB) PERSISTS PER IDENTITY — and that is NORMAL, not the bug

Worth recording because it looks like the answer for about five minutes.

**The SB address is per-identity and survives every attempt**, while the CSB is
rebuilt each time:

| identity | SB address | across |
|---|---|---|
| `OVMXM1` | `8799A980` | `M1A` join **and** `M1B` refusal |
| `OVMXM2` | `879A3080` | its own, distinct |
| `OVMXM3` | `87999B00` | **all three** attempts (`M3A`/`M3B`/`M3C`) |

Three identities, three distinct stable SBs — not slot recycling. That is exactly
the shape §4d.8 predicted ("the drop is keyed on IDENTITY") and §4f.2 proved
(per-identity cluster-side state surviving our death), so it is tempting.

**The real-node control kills it.** Re-reading `C1` (§4j), VAX2's SB across its
own crash and readmission:

| sample | CSB | **SB** | CDT | flags |
|---|---|---|---|---|
| T-PRE healthy | `87935140` | **`8794C940`** | `8794D780` | `02060002` |
| T+28 … T+99 s dead | `87935140` | **`8794C940`** | `00000000` | `06040005` |
| **T+113 s readmitted** | **`879F4080`** | **`8794C940`** | `8794B0C0` | `02040000` |

**A real VAX's SB persists across a crash and a rejoin too — and it is
readmitted.** SB persistence is ordinary VMS behaviour, not a poison.

### 4L.9f ⭐ WHAT THIS CONSOLIDATES — every structure matches; only the flags differ

Stacking the real-node control against OVMX, at every level SDA exposes:

| | real VAX2 rejoin | OVMX rejoin |
|---|---|---|
| SB | **persists** | **persists** |
| CSB | **freed, new one allocated** | **freed, new one allocated** |
| CDT | zero while dead, **new one on return** | zero while dead, **new one on return** |
| PDT | unchanged | unchanged |
| **CSB flags on return** | **`02040000 status_rcvd,vcc`** | **`00000000`** |
| outcome | **admitted** | refused |

**The peer builds structurally identical state for a returning real node and a
returning OVMX identity, and diverges only in whether it POPULATES that state.**
Every structural theory — stale CSB, missing CDT, residual SB, per-identity
poison — is now excluded by a matched control. What is left is not a missing
object but a validation the peer performs and we fail.

### 4L.9g ⛔ ALSO DEAD — "the peer re-sends its config and our once-only gate ignores it"

Tempting, because `cm_send_config_burst` is gated on `!ps->cm_config_sent` (fires
once per peer) and the SDA counters look like the peer sent config twice (old CSB
`0023 → 0025`, new CSB `0002`).

**Refuted from our own log in under a minute, no code written.** The four inbound
CM messages in `M3B` are **two from each of the two peers** — distinct remote
Con.IDs `0xED83000F` and `0x64BE000E` — and OVMX **answered both**, one
`CMCONFIG` per peer. The gate is per-peer (`ps->cm_config_sent`) and fired
correctly. The peer never re-sends its config after replacing the CSB.

> **And this reinforces `vms-da1`.** The CSB's `Next seq. number` cannot be a
> count of CM messages: SDA polls VAX1 only, yet VAX1's old + new CSBs together
> account for 4 sends while the log shows VAX1 sending 2. Those counters count
> something else — most likely all sequenced SCS traffic on the VC. **Until
> `vms-da1` lands, do not read them as message counts.**

### 4L.9h ⭐ THE QUESTION, POSED SHARPLY

Every structural theory is now excluded by a matched control (§4L.9f), and the
peer's `DISC-REQ` is known not to be a reaction to anything we send (§4L.10).
So the peer's own progression diverges, and the three-cell truth table is:

| peer + | fresh join | **rejoin** |
|---|---|---|
| **OVMX** | `DISC-REQ` runs → **admitted** | **no `DISC-REQ` → refused** |
| **real node** | `DISC-REQ` runs → admitted | **`DISC-REQ` runs → admitted** |

> **What is different about OVMX-on-a-rejoin that is NOT different about
> OVMX-on-a-fresh-join, and NOT different about a real-node-on-a-rejoin?**

That is the whole bug in one sentence, and it excludes a large class of answers:
anything wrong with OVMX generally would break the fresh join too, and anything
inherent to the rejoin path would break the real node too. **The answer must be
something the peer checks only on a rejoin, whose value differs only for us.**

**Two candidates of exactly that shape are already dead — do not re-propose
either:**

1. **The `[22:24]` incarnation echo** (§4k.9). We emit the reference value; the
   field reads identically on a real rejoin and ours.
2. **OVMX's own rejoin-mode behaviour, driven by the prior-admission sidecar.**
   `cm_apply_rejoin_form()` (`scsd.c:1129`) changes our `op 0x02` shape only when
   we hold a prior-admission record for the *same* cluster — the single largest
   OVMX-side "only on a rejoin" difference there is. **`OVMX_REJOIN_FORM=0`
   forces the first-join form and was run as `s1C` in session k: still refused**,
   matching `s1B` with the form on. Deleting the sidecar amounts to the same
   test, so it is not worth a run.

So the answer is most likely **not** something OVMX does differently on a rejoin
— we have now switched off the main one and nothing changed. It is more likely
something the peer checks against state it holds, whose value a real returning
node satisfies and we do not. Finding the next field of that shape is the work.

> **Method reminder before the next hypothesis:** §3 now holds 12 killed
> entries and §4L alone killed seven more. **Read both lists before proposing
> anything**, and prefer a check against our own source or run logs over a
> capture — this session refuted four hypotheses that way in minutes each, and
> wrote no code for any of them.

### 4L.10 Two further grounded results from the same decode

1. **`DISC-REQ` reproduces on lab-2**: 2/2 in both joins, **0/0** in the refused
   run (lab-1 was 3/3 vs 0/0 on a 3-peer cluster). **New and important precision:
   in both joins the peer's `DISC-REQ` arrives immediately after *its own*
   node-status pair — before or concurrent with our reply going out.** So the
   peer's `DISC-REQ` is **not** a reaction to anything we send. It is part of the
   peer's own sequence, and on a rejoin that sequence simply does not run. This
   weakens any theory in which our reply triggers the peer's progress.
2. **⚠ §4k.4's "zero outbound CONN-REQ on a rejoin" does NOT hold on lab-2.**
   Here the refused run sends **14** — `7 × SCS$DIRECTORY` per peer on a ~3 s
   cadence from +2.1 s to +24.1 s, then it gives up for the remaining ~84 s — and
   **0 `MSCP$DISK`, 0 `VMS$VAXcluster`**. OVMX *does* try its own client walk on a
   rejoin here and **is never accepted past step 1**. Against lab-1's `r2B` zero.
   **Do not merge the two numbers** (different SIMH binary). Which behaviour is
   the real one needs settling on one lab.

Capture integrity checked: `seqchk.py` clean on all three, no truncation, full
~148 s spans. The only gap flags are OVMX correctly retransmitting an unacked
`CONN-REQ` with its original sequence number.

## 4M. ⭐⭐⭐ SESSION m (2026-08-02) — §4L.9h ANSWERED: WE MIRROR THE REQUEST MSGTYPE, AND ON A REJOIN THE PEER ASKS WITH `0x5b`

**Found in our own run logs, for free, before any lab time or any capture** —
exactly the oracle order §5 and guardrail 13 prescribe. Four sessions of capture
work walked past it because the log line that carries it is *mislabelled*.

### 4M.1 ⭐⭐⭐ The discriminator, 6/6 across bracketed identity-proven runs

`grep DIRLOOKUP scsd-<tag>.log` over every lab-2 `M`-series run. The field
printed as `(op=0x..)` is **not** our answer — it is `dv.opcode`, the **inbound
request's msgtype** (`scsd.c:4909-4911`). Reading it as our response opcode is
what hid this.

| run | verdict | `MSCP$DISK` lookup-request msgtypes, in order, both peers |
|---|---|---|
| `M1A` | **JOINED** | `4b 4b` · `4b 4b` |
| `M2A` | **JOINED** | `4b 4b` · `4b 4b` |
| `M3A` | **JOINED** | `4b 4b` · `4b 4b` |
| **`M1B`** | **REFUSED** | **`5b`** `4b` · `4b 4b` |
| **`M3B`** | **REFUSED** | **`5b`** `4b` · **`5b`** `4b` |
| **`M3C`** | **REFUSED** | **`5b`** `4b` · **`5b`** `4b` |

**Zero `0x5b` in any of the three joins. A `0x5b` on the first `MSCP$DISK`
lookup in every one of the three refusals.** The three joins are the bracketing
controls §5 requires — `M1A`/`M2A` bracket `M1B` on both sides, and `M3A`
precedes `M3B`/`M3C`. All six logged `identity on the wire`.

**On a rejoin the peer asks its first `MSCP$DISK` directory lookup as msgtype
`0x5b`; on a fresh join it asks as `0x4b`.**

### 4M.2 ⭐⭐⭐ And OVMX MIRRORS IT — while our own source records that the reference never does

`scsd.c:4882`, in the lookup-request branch:

```c
lp.opcode = dv.opcode; /* echo the request opcode (0x5b/0x4b) */
```

**We reflect whatever msgtype the peer asked with.** Set against
`scs_dir.c:244-246`, which grounds the opposite rule from a **336-frame census**
(every `op 5` in the capture library, 4 sender nodes, 15 captures) for the
adjacent confirm frame:

> `[16]` is `0x4b` (SEQAPP), **NOT a mirror of the op-4 being answered**: 86 of
> the observed pairs answer a `0x5b` op-4 with a `0x4b` op-5, and all three
> op-5s a real VAX has ever sent AT OVMX are `0x4b`.

**This codebase already derived "never mirror the request msgtype", wrote it
down, and applied it in `dir_confirm5_tmpl` — while the lookup-response path a
few hundred lines away still mirrors.** No env var covers it (checked, guardrail
16: the 29 `OVMX_*` switches contain nothing for this).

### 4M.3 Why this has §4L.9h's exact shape

> *What is different about OVMX-on-a-rejoin that is NOT different about
> OVMX-on-a-fresh-join, and NOT different about a real-node-on-a-rejoin?*

| | fresh join | rejoin |
|---|---|---|
| peer asks with | `0x4b` | **`0x5b`** |
| **OVMX** answers with (mirror) | `0x4b` — **accidentally correct** | **`0x5b`** |
| **real node** answers with | `0x4b` | **`0x4b`** |

- **Not "wrong with OVMX generally"** — a fresh join never sends us a `0x5b`
  lookup, so the mirroring bug is never exercised and the join succeeds.
- **Not "inherent to the rejoin path"** — a real returning node answers `0x4b`
  and is admitted.

It is a rule we get right by luck on the path we test and wrong on the path we
do not, and the peer only exercises it when it already holds state for us.

### 4M.4 How it joins up with the standing findings

If the peer discards a lookup response whose msgtype it does not accept, its
directory step never completes — and that is **§4k.5's divergence point stated
from our side**: the peer holds its round-1 directory connection open forever and
never sends the `op 6` `DISC-REQ`, so round 2 never opens. Our own log shows the
downstream consequence directly, and names it:

```
M1A: PSCLIENT,  opened OUR SCS$DIRECTORY client connect ... (disk-discovery step 1, post-credit)
     PSCLIENT,  OUR dir bound (remote=0xF294000C) ... -> PSDONE, MSCP disk discovery complete
M1B: PSCUNGATE, no op 6 on our server dir connection after 2000ms -- opened OUR
                SCS$DIRECTORY client connect ... (disk-discovery step 1, UNGATED)
     PSCLIENT,  retransmit disk-discovery step 1 (retx 1 .. retx 6)   <- never bound, ever
```

`PSDONE` is 2 in each join and **0** in `M1B`. That also explains the peer's
`00000000` flag word (§4L.1) without any new mechanism: the peer never reaches
the point where it applies our status.

**Consistency is not evidence** (§4L.9a, and seven hypotheses died here saying
exactly this). What is grounded is the 6/6 correlation and the mirroring in our
source. What is NOT yet grounded is that the peer *discards* a mirrored `0x5b`
lookup response — and whether the peer's choice of `0x5b` on a rejoin is itself
downstream of something earlier. The test below decides it.

### 4M.6 ⛔ RUN — THE MIRRORING FIX IS NOT THE GATE. The observation REPLICATED; the hypothesis is DEAD.

**Bracketed series `N1A`/`N1B`/`N1C`/`N1D`/`N2A` on `vaxlab-2`, fresh identities
`OVMXN1` (1308) and `OVMXN2` (1309), binary built from the §4M commit.**

| run | identity | sidecar | verdict |
|---|---|---|---|
| `N1A` | `OVMXN1`, fresh | none | **JOINED** (`XITDONE=1`) |
| **`N1B`** | **same `OVMXN1`** | carried, 41 B | **REFUSED** |
| **`N1C`** | same, 2nd rejoin | carried | **REFUSED** |
| **`N1D`** | same, 3rd rejoin | carried | **REFUSED** |
| `N2A` | `OVMXN2`, fresh | none | **JOINED** |

Controls join on **both** sides (guardrail 20), all five logged `identity on the
wire`. The experiment is valid and **the answer is no.**

**The fix was live** — guardrail 19, checked before interpreting. Every `N1B`
lookup logs `our resp msgtype=0x4b`, in the new two-field log format that only
exists in the fixed binary.

**But §4M.1's OBSERVATION replicated exactly, on a brand-new identity:**

| run | `MSCP$DISK` **request** msgtypes |
|---|---|
| `N1A` fresh | `4b 4b` · `4b 4b` |
| **`N1B` rejoin** | **`5b 5b`** · **`5b 5b`** |

So "the peer asks with `0x5b` only on a rejoin" is now **7/7 and reproducible on
demand** — a reliable free oracle for *"the peer does not consider its
SCS$DIRECTORY connection to us to be up"* (`scs_dir.h:33`). What is dead is that
**our** mirroring caused it. We answer `0x4b`, exactly as the reference does, and
the peer still asks the next one with `0x5b` and still refuses us.

> **Guardrail 15, for the sixth time on this item: a fix that is right can leave
> the bug unfixed.** The mirroring was wrong against a 336-frame census and is
> **kept** — it is now reference-correct. It is not the rejoin gate. Do not
> relabel it as noise, and do not re-propose it.

**Cause vs effect, settled the right way round:** the `0x5b` is an *effect*. It
reports that the peer's directory connection to us is still establishing; it is
not the peer reacting to what we put in the response.

### 4M.7 ⛔⛔ RETRACTED IN FULL — BOTH CLAIMS REFUTED BY MY OWN KILL-SWITCH RUN

> **Everything in this section as originally written is WRONG. It is kept, with
> the heading, so nobody re-derives it.** It was written from ONE refused run
> (`N1B`) without checking the two refused runs I already had on disk, and
> without running the kill-switch. §4M.9 has the refutation and the guardrail.
>
> - **"The fix advanced the dialogue (inbound CM 4 → 7)" — REFUTED.** `N1E`, the
>   same identity with `OVMX_DIR_MIRROR_MSGTYPE=1` (fix OFF), scores an
>   identical `CMIN`=7 / `CMRESP`=1 / `CLUSTATE`=1. The 4-vs-7 gap is between the
>   morning `M` session and the afternoon `N` session, **not** between fix-off
>   and fix-on.
> - **"On a rejoin the peers ack `cat 0x04 op 0x04`/`op 0x06`; on a join
>   `op 0x00`" — REFUTED.** `N3A` **JOINED** with a leading `cat 0x04 op 0x04`.
>   `N1D` and `N1E` were **REFUSED** with `op 0x00`. The opcode varies freely
>   across both outcomes and discriminates nothing.
>
> **`scsd.c:2804` warned me in the source I had already read:** *"Matched on the
> ROLE SLOT, never the opcode alone — op 0x04 role 0x00 is a different SYSAP's
> opcode 4 and appears in SUCCESSFUL joins (handoff §3.4)."* This is §3 item 4's
> mistake, repeated verbatim, one category over.

### 4M.7-original (RETRACTED) — the peer's ack carries a DIFFERENT OPCODE on a rejoin

Unlike §4e.4's ambiguous "we answered / it gave up sooner", this is
unambiguous: the refused run gets **more**, not less.

| | `M1B` refused (pre-fix) | **`N1B` refused (post-fix)** | `N1A` joined |
|---|---|---|---|
| `SCSD-T-CMIN` inbound CM | 4 | **7** | 576 |
| `SCSD-I-CMRESP` | 0 | **1** | 235 |
| `SCSD-I-CLUSTATE` | 0 | **1** | 2 |

`N1B` now receives the peer's `cat 0x01 op 0x03` and **answers it** — a
transaction step no refused run had ever reached. It still stalls immediately
after.

**And the whole setup phase is IDENTICAL between `N1A` and `N1B`** — `DIRCONN`
2/2, `DIRLOOKUP` 8/8, `CONNREQ` 2/2, `JOINBOUND` 2/2, `JOINCONFIRM` 2/2,
`STARTDONE` 2/2, `STARTRX` 6/6, `INCARN` 2/2, `PADINIT`/`PADACK` 2/2,
`CMCONFIG` 3/3. OVMX's side of the handshake is complete and equal.

**THE NEW DISCRIMINATOR, and it is of §4L.9h's exact shape:**

| after the node-status pair, the peer sends | `N1A` **join** | `N1B` **rejoin** |
|---|---|---|
| peer A | `cat 0x04 op 0x00` | **`cat 0x04 op 0x04`** |
| peer B | `cat 0x04 op 0x00` | **`cat 0x04 op 0x06`** |
| then | `op 0x03` → **`op 0x05`** → 570 more | `op 0x03` → answered → **silence** |

`cat 0x04` is `SCS_MEMBER_CAT_ACK` (`scs_member.h:105`), "member commit/credit
ack". **On a fresh join both peers ack with `op 0x00`. On a rejoin they ack with
`op 0x04` and `op 0x06` — and the two peers disagree with each other.**

`scs_member.h:410` defines `SCS_MEMBER_OP_ABORT 0x04` — *"transition ABORT"* —
which would fit §4d.6's answered-then-aborted shape. **That mapping is for the
`cat 0x01` opcode space and is NOT established for `cat 0x04`. Do not cite it as
an abort until it is grounded.** What IS grounded is that the opcode differs,
only on a rejoin, from both peers, with a matched control.

This is the same bug class as the `0x7b` deafness of §4d.10 if OVMX ignores
these: `cm_req` at `scsd.c:2764` covers only `CAT_CONFIG`/`CAT_MEMBERSHIP`/
`CAT_DLM` — **`CAT_ACK` is not in the set.**

### 4M.8 ⚠ THE CSB IS POPULATED IN A REFUSED REJOIN — but the ATTRIBUTION TO THE FIX IS RETRACTED

> **⛔ THE ORIGINAL HEADLINE — "the peer NOW populates the CSB", crediting the
> §4M fix — IS REFUTED BY `N1E` (§4M.9).** With the fix switched OFF the CSB is
> populated identically. The *measurement* below is real and reproducible across
> four refused runs; the *causal claim* was wrong and is withdrawn. Read the
> "pre-fix" column as **"the morning `M` session"**, not as "without the fix".

**The peer's CSB for our identity, sampled live with `csbwatch.sh` — OVMX
RUNNING at every sample (guardrail 22):**

| field at T+5 s | §4L refused (**`M` session, am**) | **refused (`N` session, pm)** | `N1A` admitted |
|---|---|---|---|
| **Flags** | **`00000000` — NOTHING SET** | **`02040000 status_rcvd,vcc`** | `02060002 member,selected,status_rcvd` |
| `Cpblty` | `00000008 ext_status` | **`00000A98 ext_status,cwcreprc,ipc_demult_conn`** | same |
| `SWVers` | `........` | **`V7.3`** | `V7.3` |
| `HWName` | empty | **`OVMX Cluster Node`** | same |
| `Quorum/Votes` | `0/0` | **`1/0`** | `1/0` |
| `Quor. Disk Vote` | 0 | **1** | 1 |
| `Lock mgr dir wgt` | 0 | **1** | 1 |
| `Last seq num rcvd` | `0000` | **`0003`** | — |
| `Incarnation` | — | **`2-AUG-2026 16:27:28`** (live) | live |
| CDT / SB / PDT | — | all allocated | all allocated |

**Every single field §4L.5 listed as blank in a refused run is now populated,
and the flag word `02040000 status_rcvd,vcc` is EXACTLY the value §4L.1
recorded for the two ADMITTED controls at T+5 s.** The peer has received our
status, applied it, and connected the VC.

> **So §4L.3's single question — "why does the peer never set `vcc` and
> `status_rcvd`" — is NOT UNIVERSAL. In the `N` session the peer sets both on a
> refused rejoin, and refuses anyway.** §4L's framing of the flag word as "the
> whole bug in one flag word" therefore does not hold. **What caused the `M`/`N`
> difference is UNKNOWN** — it is not the fix (§4M.9), and the two sessions are
> 8 hours apart on the same pod.
>
> **This is almost certainly §4d.6's TWO REFUSAL SHAPES, and I walked straight
> into the error the method rules warn about**: *"Establish which of the two
> refusal shapes a run is before comparing it to any other run."* The `M`
> refusals are the `s3B` "dropped on the floor" shape (`open`, `00000000`); the
> `N` refusals are the other shape (`open`, fully populated, never selected).
> **§4L.1 and §4M.8 are measurements of two DIFFERENT failure modes and must not
> be diffed against each other.**

**What is left is exactly two bits.** `02040000` → `02060002`: the peer never
sets **`member`** and **`selected`**. Flat for the full 108 s across `N1B`,
`N1C` and `N1D`, from three different prior CSB states.

**The question, re-posed for the next iteration:**

> The peer has our status, capabilities, votes, incarnation, lock-manager
> directory weight and an open VC. It has allocated the CSB, the CDT and the
> PDT, and it agrees we are who we say we are. **What SELECTS a node for
> membership, and what does the peer check there that a returning identity
> fails and a fresh one passes?**

Its wire correlates, both new and both grounded (§4M.7): the peer acks with
`cat 0x04 op 0x04`/`op 0x06` instead of `op 0x00`, and never sends the
`cat 0x01 op 0x05` lock/resource-rebuild step that immediately follows
`op 0x03` on every join.

### 4M.9 ⛔⭐ THE KILL-SWITCH RUN — it refuted two of my own claims in one run

**`N1E` = `OVMXN1` rejoin with `OVMX_DIR_MIRROR_MSGTYPE=1` (the §4M fix OFF),
same identity, same pod, same session, ONE variable. `N3A` = fresh `OVMXN3`
(1310), closing control — JOINED.** The switch demonstrably worked: every `N1E`
lookup logs `our resp msgtype=0x5b`, the pre-fix echo.

| | `N1B` fix ON | **`N1E` fix OFF** | `M1B` (am) |
|---|---|---|---|
| verdict | REFUSED | REFUSED | REFUSED |
| CSB flags @T+5 s | `02040000 status_rcvd,vcc` | **`02040000` — IDENTICAL** | `00000000` |
| `CMIN` / `CMRESP` / `CLUSTATE` | 7 / 1 / 1 | **7 / 1 / 1 — IDENTICAL** | 4 / 0 / 1 |

**With the fix switched off, everything I had credited to the fix is still
there.** So §4M.7's "dialogue advance" and §4M.8's "the peer now populates the
CSB" are both **withdrawn**. The `M`→`N` difference is a session/shape
difference of unknown cause, not a fix effect.

**What still stands after the kill-switch:** the mirroring bug is real and
grounded (§4M.1/§4M.2, 336-frame census), the fix is reference-correct and
kept, and it is **not the gate** (§4M.6 — three refusals bracketed by joins).

**And the `0x5b` observation, CORRECTED.** §4M.1 claimed "zero `0x5b` in any
join". That is **wrong** — `N3A` **joined** with a leading `0x5b`. The real
pattern, 9/9 over every `N`-session run, is a **transition**, not a presence:

| | `MSCP$DISK` lookup request msgtypes |
|---|---|
| **joins** (`N1A`,`N2A`,`N3A` +`M1A`,`M2A`,`M3A`) | reach `0x4b` by the 2nd lookup at the latest |
| **refusals** (`N1B`,`N1C`,`N1D`,`N1E`) | **`5b 5b 5b 5b` — never reach the data phase** |

Per `scs_dir.h:33` (`0x4b` = "once the SCS$DIRECTORY connection is up"), that
says: **in a refused rejoin the peer's directory connection to us never comes
up, and answering it correctly does not bring it up.** That remains a solid,
cheap, free oracle. It is a symptom, not the cause — `N1E` proves our response
does not drive it.

> ### Guardrail 23 — earned expensively, twice in one session
> **Run the kill-switch BEFORE you write down what your fix achieved, not
> after.** Guardrail 21 said ship the switch and use it in the same session; I
> shipped it, wrote two sections crediting the fix, and only then ran it. Both
> sections were wrong. A fix and a session change landed together and I
> attributed the session change to the fix.
>
> **And: check every run you ALREADY HAVE before naming a discriminator from
> one.** §4M.7's `cat 0x04` claim died to `N1D`/`N1E`/`N3A`, all of which were
> either already on disk or one command away. Cost: zero lab time to check,
> two retracted sections for not checking.

### 4M.10 ⭐⭐⭐ THE CLUSTER OPENS A TRANSITION FOR US AND HANGS IN IT — 6/6, and it survives the kill-switch

**The best-conditioned discriminator this item has produced.** It is a *state*
of the cluster, not a message; it has matched controls on both sides; and unlike
everything in §4M.7/§4M.8 it is **unaffected by the fix** (`N1E`, fix OFF, shows
it), so it is a property of the refusal itself.

SDA `SHOW CLUSTER`'s cluster block (the CLUB), sampled before the attempt and at
the end of the run:

| run | verdict | CLUB flags at T-PRE | **CLUB flags at T-END** |
|---|---|---|---|
| `N1A` | JOINED | `11082001 …quorum` | `11082041 …quorum,`**`lnm_resynch`** |
| `N3A` | JOINED | `11082001 …quorum` | `11082041 …quorum,`**`lnm_resynch`** |
| `N1B` | REFUSED | `11082001 …quorum` | **`31082001 …quorum,transition`** |
| `N1C` | REFUSED | `11082001 …quorum` | **`31082001 …quorum,transition`** |
| `N1D` | REFUSED | `11082001 …quorum` | **`31082001 …quorum,transition`** |
| `N1E` | REFUSED (fix OFF) | `11082001 …quorum` | **`31082001 …quorum,transition`** |

**Every join ends with the transition COMPLETE and the cluster in lock-manager
resynch. Every refusal ends with the cluster STILL IN THE TRANSITION**, 110 s
after it opened, on all four rejoins, from three different prior CSB states.

> **The returning node is NOT being ignored and NOT being rejected. The cluster
> COMMITS to a transition to add it, and then hangs in that transition
> indefinitely.** That is a third shape, distinct from both of §4d.6's: it is not
> `s3B`'s silent drop (a transition is opened) and it is not `r1B`'s
> propose-then-abort (no abort ever arrives — `CM-XITABORT-RECEIVED=0` in every
> run, and OVMX's `cat 0x01 op 0x04 role 0x50` detector is live and quiet).

**And OVMX is never brought into the barrier.** `SCSD-I-BARRIER` is **12** in
every join and **0** in every refusal — the coordinator opens the transition and
never sends us a barrier step. That is consistent with the CSB never reaching
`member,selected` (§4M.8): we are the subject of the transition, not a
participant in it.

**This is where the next iteration goes.** The question is no longer "why does
the peer ignore us" but:

> **The cluster has opened a transition to add this identity. What is that
> transition waiting for, which node owes it, and why does the same transition
> complete in ~10 s for a fresh identity?**

**Oracles, cheapest first — none of them yet run against this framing:**
1. **The peer's OPCOM console during the stall.** §4d.6 records `proposed
   addition` / `aborted` lines existing. `csbwatch.sh` cannot see them — it parks
   VAX1 inside SDA — so this needs `stallpoll.sh`/`probe.sh` or a plain
   `lab2run.sh` with console capture. **A stuck transition is exactly the thing
   VMS complains about on the console.**
2. **SDA `SHOW CLUSTER` FULL during the stall**, not just at T-END — per-node
   state will name which node has not completed its part.
3. `Member State Seq. Num` / `Last trans. number` across the stall.

### 4M.11 ⭐⭐⭐ THE PEER NAMES THE STEP ITSELF — a 90-MILLISECOND WINDOW, and the oracle was free

**§4d.9 said no peer oracle reports a DECISION. That was wrong, and the counter-
example was sitting in a console log on disk the whole time.** `csbwatch.sh`
parks **VAX1** inside SDA — but **VAX2's console is never touched**, and VMS
prints the whole membership dialogue to OPCOM. Zero lab time, zero capture work.

**Refused rejoin (`N1E`, and identically `N1B`/`N1C`/`N1D`):**

```
16:48:03.75 Node VAX1 received VAXcluster membership request from node OVMXN1
16:48:04.34 Node VAX2 received VAXcluster membership request from node OVMXN1
16:48:04.34 Node VAX2 PROPOSED ADDITION of node OVMXN1
            << 115 SECONDS OF NOTHING -- the whole life of our process >>
16:49:59.65 Node VAX2 lost connection to node OVMXN1      <- our process exited
16:50:20.17 Node VAX2 timed-out lost connection
16:50:20.17 Node VAX2 ABORTED VAXcluster state transition
```

**Fresh join (`N3A`), same lab, three minutes later:**

```
16:51:26.75 Node VAX1 received VAXcluster membership request from node OVMXN3
16:51:27.37 Node VAX2 received VAXcluster membership request from node OVMXN3
16:51:27.37 Node VAX2 PROPOSED ADDITION of node OVMXN3
16:51:27.46 Node VAX2 COMPLETED VAXcluster state transition     <- +0.09 s
```

> **THE WINDOW IS NINETY MILLISECONDS WIDE.** Both runs are identical up to and
> including `proposed addition`. A fresh identity completes the state transition
> 90 ms after the proposal. A returning identity never completes it, at all,
> ever — and the eventual `aborted` is triggered by **our process dying**, not by
> any decision the cluster reached.

**⚠ Read the `lost connection` line correctly — guardrail 22 nearly claimed
another victim.** It is NOT the cluster spuriously declaring a live node dead.
It fires 115 s after the proposal, immediately after our 110 s run ends. The
first draft of this section said "the cluster declares lost connection while our
process is still alive"; that was **wrong**, and it was wrong because the
untimestamped `%CNXMAN,` console echoes interleave out of order with the
timestamped `OPCOM` lines. **Use only the timestamped `Node X (csid …)` lines.**

**What this retires and what it sharpens:**
- It confirms §4M.10 from the peer's own words: the transition opens and hangs.
- It kills "the peer never proposes us" — **it proposes us every single time.**
- It gives an exact, named, 90 ms window to instrument, replacing every
  "somewhere in the dialogue" framing in §4L and §4M.

**The question, at its sharpest yet:**

> Between `proposed addition of node X` and `completed VAXcluster state
> transition` a real VMS cluster does something that takes **90 ms** for a fresh
> identity and **never finishes** for a returning one. **What is that step?**

`SCSD-I-BARRIER` is **12** in every join and **0** in every refusal, so the
barrier round is the prime suspect — the coordinator never opens a barrier step
with us. **Next: capture VAX2's console AND a pcap across that 90 ms window on
a matched pair, and identify the first frame the join has that the rejoin does
not.** The console gives the anchor timestamps to align them, which is what
every previous attempt at this comparison lacked.

### 4M.12 ⛔ §4M.2's GROUNDING IS OVERSTATED — "never mirror" is REFUTED for lookup responses

**A delegated byte census over the whole capture library refutes the rule I
extrapolated.** §4M.2 took `scs_dir.c:244`'s 336-frame census — which is about
the **op-5 confirm** frame — and generalised it to the **lookup response**. That
generalisation is wrong.

Real-VAX → real-VAX lookup request/response pairs, 5 captures, OVMX excluded as
responder:

| | count |
|---|---|
| req `0x5b` → resp **`0x5b`** | **10** |
| req `0x5b` → resp `0x4b` | 11 |
| req `0x4b` → resp `0x4b` | 50 |
| req `0x4b` → resp `0x5b` | 1 |

**A real VAX answers a `0x5b` lookup with `0x5b` about half the time.** So
neither "always mirror" (OVMX's old behaviour, 60/72) nor "never mirror" (my
change, 61/72) is the rule, and both are a coin-flip against the corpus.

> **This is the exact failure mode CLAUDE.md Rule 8 and the purity guardrail
> exist to prevent: I presented an OVMX design choice as reference-derived.**
> The census I cited is real and is correctly recorded in `scs_dir.c` — it is
> just about a different frame. **Grounding a rule for message A does not ground
> it for message B.** Sibling of §4k.9's "grounding a byte is not grounding a
> claim".

**But the same data hands over a much better candidate**, visible in two matched
reference pairs where the *same* responder, on the *same* connection, 1.6 ms
apart, answers the same request msgtype two different ways:

| ref frames | SYSAP | req | **resp** | result in the body |
|---|---|---|---|---|
| 1191→1193 | `MSCP$TAPE` | `0x5b` | **`0x5b`** | `NOT PRESENT HERE` |
| 1195→1197 | `MSCP$DISK` | `0x5b` | **`0x4b`** | name echoed (AFFIRMATIVE) |
| 1237→1239 | `MSCP$TAPE` | `0x5b` | **`0x5b`** | `NOT PRESENT HERE` |
| 1241→1243 | `MSCP$DISK` | `0x5b` | **`0x4b`** | name echoed (AFFIRMATIVE) |

**⛔ AND THAT CANDIDATE IS REFUTED TOO — the rule is UNDETERMINED. This is an RE
gap, not a bug.** The 2×2 over every real-VAX lookup response (n=63, OVMX
excluded as responder):

| | AFFIRMATIVE (name echoed) | `NOT PRESENT HERE` |
|---|---|---|
| resp `0x4b` | 35 | **17** |
| resp `0x5b` | **1** | 10 |

**18 of 63 land off-diagonal.** The cleanest kill is reference frame 8994: VAX1
answers a `0x5b` `MSCP$TAPE` request `NOT PRESENT HERE` — the *same result
value* as frames 1193/1239, which carried `0x5b` — and puts **`0x4b`** on the
wire. VAX1 never serves a tape anywhere in the corpus, so the result is not in
doubt.

**Nothing else predicts it either.** Over all 72 real-VAX matched pairs, result,
SYSAP name, responder MAC and request msgtype **all tie at 11 classification
errors**. A brute-force scan of every header byte at abs 14–29 and 31–59 across
**129,084** real-VAX `0x4b`/`0x5b` frames found **no single byte that separates
them**. Two corpus limits worth carrying: result and SYSAP name are perfectly
collinear here (`MSCP$DISK` always affirmative, `MSCP$TAPE` always absent), and
all three VAXes emit both msgtypes, so responder identity carries no signal.

> **RULING (mine, CLAUDE.md Rule 8): the lookup-response msgtype selector is an
> unresolved RE gap. OVMX keeps the fixed `0x4b`, LABELLED AS AN OVMX DESIGN
> CHOICE — not as VMS-authentic.** Rationale: `0x4b` is the overwhelmingly
> dominant form on the wire (18,785 vs 92 in the reference capture), it is the
> best single fixed choice against the corpus (61/72 vs the mirror's 60/72), it
> is behaviourally neutral in the lab, and it is kill-switched. Filed as its own
> item; **not** to be re-litigated inside `vms-2f3`.

**Status of the code meanwhile:** the change is committed, kill-switched
(`OVMX_DIR_MIRROR_MSGTYPE=1`) and **behaviourally neutral in the lab** — three
fresh identities joined with it on and the rejoin is refused either way
(§4M.9). It is not urgent to revert, and it must not be described as grounded
until this is settled.

### 4M.13 ⛔⭐ THE `cat 0x04` DECODE — the deafness lead is DEAD, and the peer treats us correctly

Delegated byte decode of the reference crash-rejoin against `d94-N1A`/`d94-N1B`.

**⛔ DEAD: "OVMX is deaf to `cat 0x04` and that is the gate."** §4M.7 noted
`CAT_ACK` is never dispatched in `scsd.c`. **A real returning node does not
answer these either.** Over 175 `cat 0x04` frames addressed to the rejoining
VAX3, **there is never a same-opcode reply** — the median next transmission is
0.4 ms later and is a response to a *different* message. The decisive case:
VAX2→VAX3 `cat 0x04 op 0x04` (idx 1299), then `cat 0x01 op 0x03` (idx 1303),
then VAX3 replies `cat 0x81 op 0x03` (idx 1304) — the `op 0x04` is absorbed
purely by the ack field advancing 3→4. **`cat 0x04` is acknowledged at the
sequenced-message layer only. OVMX's non-dispatch is correct behaviour.**

**⭐ AND THE PEER IS TREATING US CORRECTLY.** `REF` idx 1299 (real rejoin,
VAX2→VAX3) and `N1B` idx 163 (VAX1→OVMX) are the **same message at the same
dialogue position** — both `cat 0x04 op 0x04`, both `seq=3 ack=3`. In the
meaningful header region they differ only at `body[10]` (`03` vs `be`) and
`body[14]` (`09` vs `0c`); everything past `body[20]` is the sender's stale
uninitialised buffer (it contains readable OPCOM text and resource names — **do
not read signal into those offsets**).

> **So the `op 0x04` the peer sends OVMX on a rejoin is reference-conformant:
> the reference's returning node received exactly this shape and joined anyway.**
> That independently re-kills §4M.7 from the byte side.

**⚠ AND AN AGENT CLAIM CHECKED AND CORRECTED — guardrail 13, again.** The decode
concluded *"op 0x00 = the join form, op 0x04 = the rejoin form … consistent with
the peers correctly classifying N1B as a rejoin."* **That is refuted by runs the
agent did not have:** `N3A` **JOINED** with `op 0x04`; `N1D` and `N1E` were
**REFUSED** with `op 0x00` (§4M.7's retraction). The agent confirmed a real
pattern *within its two-capture window* that does not survive the six-run set.
**A confirmation from a narrower corpus does not overturn a refutation from a
wider one.**

**⭐ ONE GENUINELY NEW ANOMALY, reported as bytes only.** Two 72-byte `mt=0x7b`
frames from VAX1, **byte-identical to each other, present ONLY in `N1B` — zero
in the reference, zero in `N1A`**, at +26.29 s and +29.32 s:

```
abs 30: 7b 13 0b 00 0c 00 01 00 12 00 0b 00 00 00 0c 00
abs 46: 00 00 0b 00 00 00 01 00 00 02 0e 00 04 00 08 00
abs 62: 01 00 | 07 00 c8 f8 | 0e 00 1c 65      <- dst CID / src CID
```

The Con.ID pair is the **SCS$DIRECTORY-side** OVMX↔VAX1 connection, *not* the
`VMS$VAXcluster` CM connection. `0x7b` is the **retransmit** form (§4d.10,
`SCS_DIR_OPCODE_RETX`). **So in a refused rejoin VAX1 retransmits twice on the
directory connection and does so in no join and not in the reference** — which
sits exactly alongside §4M.9's finding that the directory connection never
reaches the data phase. Meaning undetermined; worth the next look.

**And the isolation is total.** After OVMX answers `cat 0x01 op 0x03`, across
**124 s** of further capture: **zero** CM frames from either peer to OVMX, only
HELLO beacons (73) on a ~2.66 s cadence — while the two peers exchange **78** CM
frames with each other. The cluster stays live; only OVMX is cut off.

> **Offset note for future decodes:** the CM body starts at **abs 72**
> (payload-rel 58), so `body[8]`=abs 80 and `body[9]`=abs 81 — which is what
> `tools/cm.py` already uses. An abs-76 figure in a dispatch brief was wrong and
> the agent caught it against the length-prefixed model string. Also: `body[8]`
> is **not a clean category** — it behaves like a bitmask ORed with `0x80` for
> responses, and naive scans produce false `cat 0x04` hits where ordinary data
> lands at abs 80/81.

### 4M.14 ⭐⭐⭐ A THIRD INSTANCE OF THE 0x7b DEAFNESS — on the gate that the source itself says GATES ADMISSION

**Found by following §4M.13's anomaly with a 7-run scan, then reading our own
source.** No lab time, no capture agent.

**The measurement, 7/7 with bracketing controls** — every `mt=0x7b` frame in
every `N`-series capture:

| runs | verdict | `0x7b` frames |
|---|---|---|
| `N1A`,`N2A`,`N3A` | **JOINED** | 9, all **124** bytes, `op=0`, `dstCID=0` (unbound connect-requests at t≈85 s, long after admission — unrelated) |
| `N1B`,`N1C`,`N1D`,`N1E` | **REFUSED** | **2, both 72 bytes, `op=8`, `dstCID = SCS_DIR_OVMX_CONID`**, from VAX1 only, **3.0 s apart** |

Completely disjoint. And scanning for `op=8` in *any* msgtype shows the
originals arrive in **every** run:

| run | `op=8` originals (`0x4b`) | **retransmits (`0x7b`)** |
|---|---|---|
| the three joins | 2 — one per peer | **0 — answered first time** |
| the four refusals | 2 — one per peer | **2, from VAX1** |

> **So VAX1 asks the same thing in both cases. On a join we satisfy it. On a
> rejoin we do not, and it retransmits on the 3.0 s ceiling of §4d.9.**

**And our own source says why.** `scsd.c:2525`, the gate on the op6/op8
credit/ready handshake — whose own comment reads *"VAX1 runs this after a
connection binds and **GATES admission** (incl. accepting the joiner's own client
connects) on it"*:

```c
if (do_connect && n >= 72 &&
    (buf[30] == SCS_MSGTYPE_SEQAPP || buf[30] == SCS_DIR_OPCODE)) {   /* 0x4b || 0x5b */
```

**`0x7b` is not in the set, so every retransmission is discarded before any
handler runs.**

**This is the THIRD instance of one defect.** The `0x5b` comment a hundred lines
below fixed it once; §4d.10 fixed it again for `0x7b` on the connection-manager
path; **this site was missed both times** — and it is the one the source labels
as gating admission. §4d.10's own comment predicted this exact asymmetry:

> *"a fresh identity never provokes a retransmission, so it never meets `0x7b`,
> while a returning one is deaf to every retransmission from the first onward."*

**It has §4L.9h's shape exactly:** joins never retransmit op=8, so the deafness
is **unreachable** on the path we test; rejoins always retransmit, so it is hit
every time; and a real node answers.

**Fixed**, with `OVMX_NO_CREDIT_RETX=1` restoring the deaf behaviour, and a new
`SCSD-I-CREDITRETX` trace plus a `CREDIT-RETX-ANSWERED` summary counter so the
run reports it directly. 39/39 ctest green.

> **⚠ VERDICT PENDING — do not record this as the fix until the bracketed series
> lands.** Guardrail 23: run the kill-switch before writing down what it
> achieved. Three real bugs on this item have already been "obviously it" and
> were not (§3 items 2, 3, 13). Series `P1A`/`N1F`/`P1B`/`P1C`/`P2A` is running;
> `N1F` is `OVMXN1`, which has been refused **four** times.

### 4M.15 ⭐⭐⭐ THE DIVERGENCE, LOCATED TO ONE FRAME AT +10.9 ms — and the barrier is downstream, not the cause

**The decode §4M.11's 90 ms window made possible.** Aligned on the OPCOM
`proposed addition` anchor; the agent measured the console-vs-host residual at
**0 to +4 ms** (the wire goes silent for 460–725 ms before each burst onset, so
the boundary is unambiguous) — far tighter than the ±0.1 s I assumed.

**The join and the refused rejoin are the SAME PROTOCOL, message for message,
for SIXTEEN connection-manager messages** — same direction, same
category/opcode, same seq/ack — and then the rejoin simply ends:

| # | `N3A` **JOIN** | `N1E` **REJOIN** |
|---|---|---|
| 0–8 | `0x24/0x54`, `0x24/0x54`, `0x24/0x44`, `0x01/0x14`, `0x01/0x01`, `0x01/0x02`, `0x24/0x44`, `0x01/0x14`, `0x01/0x01` | **identical** |
| 9 | `0x04/0x41` seq3/ack3 | `0x04/0x00` seq3/ack3 — **residue byte, NOT a discriminator** |
| 10–15 | `0x24/0x44`×2, `0x56/0x41`×2, `0x01/0x03` seq4/ack3, **`0x81/0x03` seq4/ack4 (OVMX answers)** | **identical** |
| **16** | **`VAX2→OVMX cat 0x01 / op 0x05`, seq5/ack4, 204 B, +10.94 ms** | **— nothing, ever —** |

`cat 0x01 op 0x05` is `SCS_MEMBER_OP_LOCKRB`, the lock/resource-rebuild
transaction. **Count over the entire capture: 5 in `N3A`, 5 in `N1A`, ZERO in
`N1E`, ZERO in `N1B`.** Everything downstream — the `0x01/0x06` ⟷ `0x04/0x00`
pump (254/249 frames inside the window) and the barrier — follows from it.

**OVMX's last transmission is a well-formed `0x81/0x03` seq4/ack4 reply,
byte-comparable in both.** We are not sending anything wrong and not going
silent early by choice.

**⚠ AND OVMX IS NOT SHOUTING INTO THE VOID.** In the 5 s after the anchor:
OVMX **6.0 fps**, VAX1 2.4, VAX2 6.0 — against 220/54/161 fps in a join. OVMX
stops first, at **+9.11 ms**; VAX2 continues 6.7 ms longer **but only to VAX1**
and never addresses OVMX again. No retry, no NAK, no disconnect, no error. **The
connection is left open and simply unused.**

> ### ⛔ CORRECTION TO §4M.10 — the barrier is NOT the proximate cause
> §4M.10 named the barrier round as prime suspect because `SCSD-I-BARRIER` is 12
> in every join and 0 in every refusal. **The timing refutes that as a cause.**
> The barrier is `cat 0x01 op 0x0b` → `0x81/0x0b` → `0x01/0x0c`, 12 rounds × 3
> frames, with a literal step counter at **abs 88** counting `01…0c`. It begins
> at **≈+135 ms** — i.e. **~45 ms AFTER** the console logs `completed VAXcluster
> state transition` at +90 ms. **It is a consequence of the transition
> completing, not a step within it.** Its absence in the refusals is real but
> downstream. Corrected in place; do not chase the barrier.

**A second candidate, EARLIER than the named one.** In the join, VAX2 sends an
`SCS-ctl(op6)` (76 B) at **+10.66 ms** — 0.28 ms *before* the `0x01/0x05` — and
it too appears absent in the rejoin. Note OVMX **does** send its `SCS-ctl(op9)`
at +8.77 ms in the rejoin, so the op8→op9 handshake ran on the VAX2 connection.
**Whether the true first divergence is VAX2's `op6` or the `0x01/0x05`, and
whether OVMX's `op9` differs byte-wise between the two, is dispatched.**

> **This also re-scopes §4M.14.** VAX1's retransmitted `op8` (the `0x7b` frames
> OVMX was deaf to) arrives at **+2.26 s and +5.33 s** — over two seconds AFTER
> this divergence at +10.9 ms. So the `0x7b` deafness is a **real bug that
> cannot be the proximate cause of the missing `0x01/0x05`.** It may still
> matter for recovery. **Do not let the bracketed series' verdict be read as
> more than it is.**

### 4M.16 ⭐⭐⭐ THE GATE IS THE PEER'S `op6`, AND NOTHING WE SEND DIFFERS BY ONE BYTE

**The sharpest and best-evidenced statement this item has reached.**

**1. `op6`/`op7` are absent from a refused rejoin entirely — whole capture, both
links, both directions:**

| SCS-ctl op | `N3A` join | `N1A` join | `N1E` rejoin | `N1B` rejoin |
|---|---|---|---|---|
| **`op6`** (76 B) | **4** | **4** | **0** | **0** |
| **`op7`** (72 B) | **4** | **4** | **0** | **0** |
| `op8` (72 B) | 4 | 2 | 6 | 6 |
| `op9` (72 B) | 2 | 2 | **2** | **2** |

`op8` and `op9` occur normally in the refusals. **Only the `op6`/`op7` pair
vanishes.**

**2. ⛔ THE DIVERGENCE IS ~700 ms EARLIER THAN §4M.15 SAID — it predates the
membership proposal.** Corrected in place; §4M.15's "first divergence =
`0x01/0x05` at +10.94 ms" is **too late**. The same pattern runs on the
**OVMX↔VAX1** link *before* the anchor, and the rejoins truncate it identically:

```
N3A (join)   -716.06ms VAX1->OVMX op8      N1E (rejoin)  -702.96ms VAX1->OVMX op8
             -715.95ms OVMX->VAX1 op9                    -702.91ms OVMX->VAX1 op9
             -715.81ms VAX1->OVMX op6  <<<               ---- nothing further ----
             -715.75ms OVMX->VAX1 op7
             -715.71ms OVMX->VAX1 op6      N1B (rejoin)  -609.06ms VAX1->OVMX op8
             -715.59ms VAX1->OVMX op7                    -609.00ms OVMX->VAX1 op9
                                                         ---- nothing further ----
```

Everything before `op8` on that link is present and identically ordered in all
four captures. **Earliest divergent frame in the whole capture: `VAX1 → OVMX`,
SCS-ctl `op6`, 76 bytes, at anchor −715.81 ms (`N3A`) / −592.35 ms (`N1A`).**
And the `0x01/0x05` absence is the same failure surfacing later on the CM
channel — on the VAX2 link the two *race* (`op6` first in `N3A` by 0.28 ms,
`0x01/0x05` first in `N1A` by 1.09 ms), so **neither is reliably "the" first**.

**3. ⭐⭐⭐ NOTHING OVMX TRANSMITS DIFFERS BY A SINGLE NON-PER-RUN BYTE.** Every
offset classified, with a class-discrimination test (joins agree with each other
AND rejoins agree with each other AND the groups differ):

| frame | differing offsets | non-per-run | **class-discriminating** |
|---|---|---|---|
| OVMX's `op9` (72 B) | 11 | 0 | **0** |
| **the PEER's `op8` to us** (72 B) | 11 | 0 | **0** |
| OVMX's CM `0x81/0x03` (204 B) | 90 | 80 | **0** |

The 11 differences in `op9`/`op8` are ConnIDs, PEDRIVER seq/channel and the
NISCA logaddr byte. The `0x81/0x03`'s 80 body differences are **stale-buffer
residue** — readable ASCII (`SYSTEM$VAX1`, `F11B$aSYSDSK1`) — and cut *across*
the classes (a join and a rejoin agree at abs 76). **Zero class signal anywhere.**

**4. `op6` is NOT a reply to our `op9`.** Ordering over all 12 `op8` / 8 `op9` /
8 `op6` / 8 `op7`:

- **`op8` → `op9`**: peer requests, OVMX replies. **8/8**, median +0.12 ms.
- **`op6` → `op7`**: a *symmetric* pair run **twice**, once each direction —
  peer `op6` → OVMX `op7`, then OVMX `op6` → peer `op7`. **4/4 each.**

`op9` already closes the `op8` pair. **`op6` is an independent step the peer
initiates**, firing 0.14–0.26 ms after it receives our `op9`. OVMX's own `op6`
is gated behind the peer's, so the peer withholding it suppresses both
directions — exactly the 0/0 rejoin pattern.

> ### THE STATEMENT
> **The peer completes `op8`→`op9` with us, byte-identically in both cases, and
> then simply does not send `op6`. It does this on EVERY link it opens to OVMX,
> starting ~0.6–0.7 s before it proposes our addition.** `op6` is the
> directory DISCONNECT-REQUEST (`scs_send_disconnect`, `scsd.c:2000`) — so this
> is **§4k.5's "the peer's directory teardown never comes", now proven to the
> byte and located 700 ms earlier than any previous framing.**
>
> **Every candidate on our side is now excluded by a byte-level matched
> control.** The discriminator is state the peer holds about our identity, and
> the decision is instantaneous (sub-millisecond after our `op9`).

**5. Barrier timing restated precisely** (the agent corrected its own first
figure, which was measured from burst onset and conflated two barriers):

| | earliest `0x01/0x0b` **involving OVMX** | console `completed` |
|---|---|---|
| `N3A` | anchor **+137.05 ms** | anchor +90 ms |
| `N1A` | anchor **+134.92 ms** | anchor +90 ms |

**45–47 ms after completion. Confirms §4M.15's correction: the barrier is
downstream and cannot be the cause.** Step index is at abs 88 counting `01…0c`;
the `0x81/0x0b` response carries a constant `0x10` there, not a step.

### 4M.17 ⛔ THE `0x7b` CREDIT FIX IS NOT THE GATE EITHER — `N1F` refused

Full bracketed series, all identity-proven on the wire:

| run | identity | verdict | `CREDIT-RETX-ANSWERED` |
|---|---|---|---|
| `P1A` | `OVMXP1` fresh | **JOINED** | **0** |
| **`N1F`** | **`OVMXN1` — refused 4× already** | **REFUSED** | **2** |
| `P1B` | `OVMXP1` rejoin | REFUSED | **2** |
| `P1C` | `OVMXP1` rejoin ×2 | REFUSED | **2** |
| `P2A` | `OVMXP2` fresh | **JOINED** | **0** |

**The fix engaged exactly as designed** — `0` in both joins, exactly `2` in all
three refusals, matching §4M.14's 7/7 prediction frame for frame. So this is not
a case of an untested change: the new code ran, answered both retransmissions in
every refusal, **and the outcome did not move.**

**This was predicted and is not a surprise:** §4M.15 already showed VAX1's
retransmitted `op8` arrives at **+2.26 s**, over two seconds after the `op6`
divergence at −0.7 s → +10.7 ms. A fix that far downstream could not have
changed the outcome.

**Keep it** (guardrail 15, seventh time on this item): the deafness was real,
it is the third instance of a defect this project has fixed twice elsewhere, it
is on the gate the source labels as gating admission, and a real node answers
those retransmissions. **Added to §3 as killed entry 16.**

### 4M.18 ⭐⭐⭐ THE PEER'S CDT TABLE — the teardown is BLOCKED, not withheld, and the disk connect is queued behind it

**`tools/connwatch.sh` (new, §6): SDA `SHOW CONNECTIONS/NODE=` on VAX1 through a
matched pair.** `Q1A` fresh **JOINED** / `Q1B` same identity **REFUSED** / `Q2A`
fresh **JOINED** — bracketed, identity-proven. This is the oracle §4d.9 named
and nobody had ever pointed at lab-2.

**The CDTs the peer holds for our identity, sampled throughout:**

| CDT → `OVMXQ1` | `Q1A` **join** | `Q1B` **REFUSED** |
|---|---|---|
| `VMS$VAXcluster` | `0002 open`, queue empty, Sent 3→**36** / Rcvd 3→**36** | `0002 open`, **FROZEN at Sent 3 / Rcvd 3** |
| `SCS$DIR_LOOKUP` → our `SCS$DIRECTORY` | **absent — torn down and freed** | **`0005 disc_sent`, Blocked `0004 disc_pend`, Message queue NON-EMPTY, Sent 5 / Rcvd 4, Send Credit 1** |
| `VMS$DISK_CL_DRVR` → our `MSCP$DISK` | absent | **`0007 con_sent`, Blocked `0001 con_pend`, queue NON-EMPTY, Remote Con.ID `00000000`** |
| free CDTs, pre → during | 5 → **4** | 5 → **2** |

> **⭐ THE PEER IS NOT WITHHOLDING `op6`. IT HAS ISSUED THE DISCONNECT AND THE
> DISCONNECT IS BLOCKED.** `disc_sent` + `disc_pend` + a non-empty message
> queue. **That is why no `op6` frame ever reaches the wire** — §4M.16's central
> observation, now explained mechanically rather than described.
>
> And the `MSCP$DISK` connect sits behind it in `con_pend`, which is why **zero
> `MSCP$DISK` CONNECT-REQUESTs appear on the wire in any refusal (22–23 in every
> join)** while §4g called it "the peer declines to connect". It does not
> decline. **It cannot transmit.**
>
> `Rej/Disconn Reason` is **0** on every CDT. **Nothing is being rejected.**

**The full directory dialogue, both runs, from the wire (OVMX's own conid):**

```
Q1A JOIN — 16 frames                Q1B REFUSED — 12 frames
  OVMX op2  ->  PEER op3              OVMX op2  ->  PEER op3
  4x op10 lookup -> 4x answer         4x op10 lookup -> 4x answer
  PEER op8  ->  OVMX op9              PEER op8  ->  OVMX op9
  PEER op6  ->  OVMX op7   teardown       ---- STOPS ----
  OVMX op6  ->  PEER op7
```

**We answer everything the peer sends, in both runs.** The refusal is not a
message we fail to answer at this layer.

### 4M.19 ⭐ A NEW BUG THIS EXPOSED — we answer a retransmit *with* the retransmit form

The tail of `Q1B`, on the VAX1 link:

```
+25.612  PEER->OVMX  mt=4b op=8      ->  OVMX  mt=4b op=9
+28.581  PEER->OVMX  mt=7b op=8      <- VAX1 RETRANSMITS. It did not accept our op9.
+28.582  OVMX->PEER  mt=7b op=9      <- and our reply is marked 0x7b
```

`scs_reflect_credit` builds the reply with `memcpy(r, buf, 72)`, which inherits
**abs 30, the msgtype**. Harmless while requests were `0x4b`/`0x5b` — but §4M.14
made us answer `0x7b`, so our `op9` now goes out **announcing itself as a
retransmission of a frame we never sent.**

**Every `op7` and `op9` in the capture library is `0x4b`**, including a real
node's. **Fixed:** emit `SCS_MSGTYPE_SEQAPP` always; `OVMX_CREDIT_MIRROR_MSGTYPE=1`
restores the inherited form. Same "derive, never inherit" rule as the length
words a few lines below it, which cost three sessions once already.

> **⚠ This is NOT the §4M.12 mirroring question.** That one is about lookup
> **responses**, where the corpus is genuinely split 10-vs-11 and the rule
> remains an open RE gap (`vms-7e7`). Here the corpus is **unanimous**.
>
> **⚠ And do not assume it is the gate.** It fires at +28.6 s, seconds after the
> divergence, and `VAX1` had already failed to accept the `op9` we sent at
> +25.612 in the correct `0x4b` form. **Verdict pending a bracketed run.**

**What this leaves as the live question:** the peer sends `op8`, we answer `op9`
in the reference-correct form, and **VAX1 retransmits `op8` anyway** — so it did
not accept that `op9`. On the VAX2 link the same exchange completes and `op6`
still never comes. Why the peer does not accept an `op9` that §4M.16 proved is
byte-identical to the accepted one is the next question.

### 4M.20 ⭐⭐ WE ANSWER A RETRANSMIT WITH A NEW SEQUENCE NUMBER — a bug §4M.14 CREATED

The sequence numbers on the credit handshake, both links, both runs:

| | `Q1A` **join** | `Q1B` **REFUSED** |
|---|---|---|
| VAX2 link | `op8 ss=12` → `op9 ss=12` → **`op6`** → `op7` → `op6` → `op7` | `op8 ss=13` → `op9 ss=13` → **stop** |
| VAX1 link | `op8 ss=12` → `op9 ss=12` → **`op6`** → `op7` → `op6` → `op7` | `op8 ss=12` → `op9 **ss=12**`<br>`op8 RETX ss=12` → `op9 **ss=13**`<br>`op8 RETX ss=12` → `op9 **ss=14**` |

**The peer replays the identical request (`send_seq=12` every time) and OVMX
answers it with a different sequence number each time — 12, 13, 14.** Two
phantom messages injected into the VC stream.

> **This bug is one I created in §4M.14.** Before it, OVMX ignored `0x7b`
> outright and consumed nothing. Answering them was right; answering them with
> fresh sequence numbers is not. **Guardrail: a fix that makes a previously
> unreachable path reachable must be re-checked on the wire, not just for
> whether it fires.** §4M.14's counter said `CREDIT-RETX-ANSWERED=2` and looked
> like success; the wire showed what those two answers actually carried.

**And the codebase already had the rule written down.** `struct peer_state`'s
own header: *"connect/lookup seqs are **allocate-once/retransmit-reuse** (the
760mscp hole)"*, and `psc_dir_req_seq` implements it for the directory connect.
`scs_reflect_credit()` was the hole.

**Fixed** as `scs_retx_reply_seq()` in `scs_vc.c` — a pure function next to the
VC engine, 11 assertions in `test_scs_vc.c` pinning replay-on-retransmit,
advance-once-per-distinct-request, and the NULL cases.
`OVMX_CREDIT_NO_SEQ_REUSE=1` restores the old behaviour. 39/39 green.

> **⚠ VERDICT PENDING, and temper expectations.** On the VAX1 link the **first**
> `op9` already carried the correct `send_seq=12` — structurally identical to
> the join's accepted one — and the peer retransmitted anyway. So this fix
> cannot explain why the *first* answer was not accepted. It removes a real
> defect and stops us corrupting the stream during recovery.

### 4M.21 ⛔ BOTH FIXES VERIFIED ON THE WIRE, NEITHER IS THE GATE — and my "did not accept our op9" reading is corrected

**R-series, bracketed and identity-proven:** `R1A` fresh **JOINED** · `N1G`
(`OVMXN1`, now refused **five** times) **REFUSED** · `R1B` (`OVMXR1` rejoin)
**REFUSED** · `R2A` fresh **JOINED**.

**Both fixes are provably live**, not merely "fired":

```
N1G / R1B, VAX1 link, with sec 4M.19 + 4M.20 in:
  +24.998  PEER mt=4b op8 ss=12   ->  OVMX mt=4b op9 ss=12
  +28.008  PEER mt=7b op8 ss=12   ->  OVMX mt=4b op9 ss=12   <- 0x4b, not 0x7b; 12, not 13
  +31.028  PEER mt=7b op8 ss=12   ->  OVMX mt=4b op9 ss=12   <- 12 again, not 14
```

Against §4M.20's pre-fix `12 → 13 → 14` and §4M.19's `mt=7b` reply. **Both
defects are gone from the wire and the rejoin is refused exactly as before.**

> ### ⛔ CORRECTION — "VAX1 did not accept our `op9`" (§4M.19's closing line)
> **That reading is now refuted.** OVMX's `op9` is byte-correct, correctly
> typed, correctly sequenced, and **replayed identically** on every
> retransmission — and the peer retransmits anyway. So the retransmission is
> **not** evidence that our reply was rejected. The better reading, consistent
> with §4M.18: **the peer re-drives the `op8` handshake because its own
> disconnect is stuck in `disc_pend`,** and the retransmit is a symptom of that
> block, not a verdict on our answer. Do not chase "our op9 is wrong" again.

**Added to §3 as killed entries 17 and 18.** That is now **nine** real,
grounded, separately-fixed defects on this item, none of which is the cause.

**What is left standing, and it is a short list.** The peer:
- holds our directory CDT in `disc_sent` / `disc_pend` with a **non-empty
  message queue** and **`Send Credit 1`**;
- has an `MSCP$DISK` connect queued behind it in `con_pend`, never transmitted;
- reports `Rej/Disconn Reason 0` — **nothing is being rejected**;
- answers everything we send and is answered correctly in turn.

**The next thread is CREDIT, and it has a number attached.** `SCSD-I-CREDIT` is
**618** in a join and **25** in a refusal, and the stuck CDT sits at **`Send
Credit 1`** with traffic queued behind it. A peer that cannot transmit because
it has no send credit would look exactly like this. **Establish how OVMX grants
credit on the SCS$DIRECTORY connection specifically, and whether the join's
credit flow is what frees the peer to send `op6`.**

### 4M.22 ⭐ THE REFERENCE CONTROL — a returning REAL node DOES get `op6`, and credit is dead

**Two checks that should have been run earlier, both free, both from captures
already on disk.**

**1. The reference control CONFIRMS §4M.16's framing rather than killing it.**
In `vax3-class03-crash-REJOIN-SUCCESS`, the returning real node VAX3 receives
exactly the pattern OVMX gets in a *join*, on **both** links:

```
+110.087  peer -> VAX3   op8    ->   VAX3 -> peer   op9
+110.088  peer -> VAX3   op6    ->   VAX3 -> peer   op7      <- 1 ms after the op9
+110.090  VAX3 -> peer   op6    ->   peer -> VAX3   op7
```

**12 `op6` and 34 `op7` frames involve the returning node.** So the peer *does*
tear down its directory connection to a returning real node, immediately after
its `op9`, and OVMX's 0/0 is a genuine anomaly. **The §4L.9h exclusion test is
satisfied: `op6` runs for OVMX-on-a-fresh-join and for a real-node-on-a-rejoin,
and only OVMX-on-a-rejoin misses it.**

**2. ⛔ CREDIT IS DEAD, before it cost any lab time.** §4M.21 named it as the
next thread on the strength of `SCSD-I-CREDIT` 618-vs-25 and `Send Credit 1`.
Both readings are wrong:

- The `0x48` credit-return flow through the directory window is **essentially
  identical** in `Q1A` and `Q1B` — OVMX acks `1,2,3,…,17` in both, at the same
  offsets. My earlier frame filter required ≥72 bytes and had silently excluded
  every 60-byte `0x48` frame.
- The 618-vs-25 is **downstream message volume**, not a cause — a join goes on
  to exchange hundreds of messages. *This is the same trap as §4M.7 and §4M.8;
  a ratio between a run that proceeds and one that stops is not evidence.*
- Decisively, the peer's own bookkeeping says so: **`Send Credit Q. empty`** on
  the stuck CDT. Nothing is waiting for credit.

**Also checked and NOT a discriminator:** OVMX sets its `op9`'s `recv_ack` to
the request's `send_seq`. A real node usually does the same (`ra=18/ss=18`,
`ra=143/ss=143`), with one case of acking a higher own high-water mark
(`ra=11` against `ss=8`). Consistent, not a delta.

**Where that leaves it.** The peer's directory CDT is in `disc_sent`/`disc_pend`
with a non-empty message queue, no credit shortage, nothing rejected
(`Rej/Disconn Reason 0`), and every frame we send is byte-correct. In a join the
peer emits one more message between our `op9` and its `op6` — on the CM
connection, matching §4M.15's `0x01/0x05` — and in a refusal it emits neither.
**The block is inside the peer's connection-manager, upstream of both.**

> **Method note for the next session.** SDA cannot resolve this: the whole
> directory dialogue is ~5 ms and the console round-trip floor is ~1.2 s
> (§4L.4). The remaining questions are either wire-level — largely exhausted —
> or need **public OpenVMS documentation on SCS connection states and what
> blocks a disconnect** (Rule 8: docs and observation only, never
> disassembly). That documentation route has not been tried and is the cheapest
> unexplored avenue.

### 4M.23 ⭐⭐⭐ THE DISCONNECT PROTOCOL, FROM PUBLIC DOCUMENTATION — it is SYMMETRIC, and both sides must call disconnect

**§4M.22 named public OpenVMS documentation as the cheapest unexplored avenue.
It paid.** Source: **Digital Technical Journal Vol. 1 No. 5, September 1987,
"The System Communication Architecture", p. 25** — a published DEC journal
article, squarely inside CLAUDE.md Rule 8.

> *"When either member of a pair of SYSAPs holding an open connection wishes to
> break that connection, that member performs a **disconnect call** to its SCA
> software. **The SCA software will inform the SYSAP in the other node, which
> must then perform its own disconnect call to synchronize the dismantling of
> the connection.** Each side informs the other of the disconnect call by
> exchanging a **disconnect-request and disconnect-response message pair.**"*

**This grounds `op6`/`op7` for the first time from documentation rather than
inference, and it explains the shape we have measured all along:**

```
peer op6 (disconnect-request)  ->  OVMX op7 (disconnect-response)     side 1
OVMX op6 (disconnect-request)  ->  peer op7 (disconnect-response)     side 2
```

Exactly two request/response pairs, one per side — in every OVMX join and in
the reference real-node rejoin (§4M.22). **The teardown is SYMMETRIC: it is not
complete until BOTH members have performed their own disconnect call.**

> **So `disc_sent` / `disc_pend` reads naturally as: the peer has performed its
> disconnect call and is waiting for the other side to perform its own.**

**⛔ And the "stale CDT from the previous incarnation" idea is dead.** The stuck
CDT's Con.ID pair (`local BAAE000C` / `remote DCAB0007`) **matches `Q1B`'s own
`DIRCONN` line exactly** — `remote=0xBAAE000C local=0xDCAB0007`. It is a
connection established in **this** run, not a leftover.

**THE CANDIDATE THIS CREATES, and it is the best-grounded one on the item:**

OVMX sends its own `op6` **only** from the `cm_op == 6` branch
(`scsd.c`, `scs_send_disconnect`) — i.e. **only in response to the peer's
`op6`.** On a rejoin the peer's `op6` never arrives, so **OVMX never performs
its own disconnect call at all**, and by the documented protocol the teardown
can never complete. OVMX already notices the situation — `PSCUNGATE` fires on
*"no op 6 on our server dir connection after 2000 ms"* — and then opens its
client connect anyway **without ever disconnecting**.

> **Next implementation step:** on that 2000 ms ungate path, have OVMX perform
> its own disconnect call (emit `op6`) on the directory connection, per the
> documented symmetric protocol, instead of only ever reacting to the peer's.
> **Caveat to respect:** `scs_send_disconnect()` today builds its frame by
> copying the *received* 76-byte `op6`, so a proactive send needs a template
> derived under Rule 8, not invented. Ship it behind a kill-switch and bracket
> it like everything else.

### 4M.24 ⛔⭐ SELF-DISCONNECT IS NOT THE GATE EITHER — and the peer NEVER ACKNOWLEDGES our `op9`

**⚠ First, a VOID test of my own making.** The `T`-series tested §4M.23 and said
nothing: `SELFDISC=0` in `T1B`/`T1D` because I hooked the fix into the
`PSCUNGATE` path, **and `PSCUNGATE` never fires in this failure mode**
(`PSCUNGATE=0` in every `T` run, against 2 in the `M`-session runs where I had
seen it). Caught only by checking the counter instead of reading `XITDONE` and
moving on.

**Retriggered on a signal that IS reliable:** the peer's `op8` **retransmit** —
2 per refused run, 3.0 s apart, **7/7, and never in a join**.

**`U`-series, bracketed, fix verified engaged** (`SELFDISC=1` in both refusals,
`0` in both joins): `U1A` fresh **JOINED** · `U1B` rejoin **REFUSED** · `N1I`
(`OVMXN1`, seventh refusal) **REFUSED** · `U2A` fresh **JOINED**.

**Our `op6` reached the wire, well-formed:**

```
+28.059  PEER mt=7b op8 ra=11 ss=12   ->  OVMX mt=4b op9 ra=12 ss=12
+28.059                                   OVMX mt=4b op6 ra=12 ss=13   <- our disconnect call
+31.085  PEER mt=7b op8 ra=11 ss=12       <- retransmits AGAIN; no op7 ever
```

**The peer never answers our `op6` with `op7`.** So performing our own disconnect
call — which the DTJ says the protocol requires — does not unblock it either.
**§3 killed entry 19.**

### ⭐⭐⭐ AND THE REAL FINDING: the peer's `recv_ack` never advances

Every peer `op8`, original and both retransmits, carries **`recv_ack=11`**. Our
`op9` carries `send_seq=12`. In a **join** the peer's very next frame carries
**`recv_ack=12`** — it has accepted our `op9` into its sequenced stream:

| | join (`Q1A`) | **refusal (`U1B`, `N1I`, `Q1B`, `R1B` …)** |
|---|---|---|
| peer `op8` | `ra=11 ss=12` | `ra=11 ss=12` |
| our `op9` | `ra=12 ss=12` | `ra=12 ss=12` — **byte-identical (§4M.16)** |
| **peer's next frame** | **`op6`, `ra=12`** ← accepted | **`op8` retransmit, `ra=11`** ← **never accepted** |

> **The peer does not acknowledge our `op9`, ever, in any refused rejoin — and
> acknowledges it immediately in every join, from a byte-identical frame.**
>
> That is why it retransmits, why its CDT stays `disc_pend`, why `op6` never
> comes, and why our own `op6` is ignored: **from the peer's point of view we
> have never answered its `op8` at all.** Every downstream symptom in §4M.16–
> §4M.23 follows from this one fact.

**This finally separates two things that looked identical.** §4M.16 proved the
`op9` FRAME is byte-correct. This shows the frame is nonetheless **not accepted
into the peer's receive window**. So the discriminator is not the frame's
content but its *acceptability* — sequence-window state, connection binding, or
something the peer keys on that a byte diff of the frame cannot see.

### ⛔ AND THE VC-DUPLICATE HYPOTHESIS IS DEAD — killed free, from disk

The obvious explanation was that `ps->vc.seq` is **shared across every
connection to a peer** (`struct peer_state`: *"All sends ride the shared
per-channel `ps->vc.seq`"*), so `send_seq=12` might already have been spent on
another connection, making our `op9` a VC-level duplicate. **It is not.** Every
OVMX→VAX1 sequenced frame, join vs refusal:

| # | `Q1A` **join** | `U1B` **refusal** |
|---|---|---|
| 1–4 | `op1`, `op2`, `op0`, `op10` | **identical** |
| 5–8 | `op3`, `op10`×3 | **identical** |
| 9–11 | `op10`×3 | **identical** |
| **12** | **`op9`** | **`op9`** |
| 13+ | `op7`, `op6`, `op1`, `op4` → proceeds | `op9` replayed, `op6`, `op9` replayed |

**Twelve frames, same opcodes, same sequence numbers, no duplicate anywhere.**
Our numbering is perfect and identical right up to the `op9`.

> **So the peer receives an identically-numbered, identically-formed `op9` in
> both cases, and acknowledges it in one and not the other. The discriminator is
> not in our frame, not in our sequence numbering, and not in our timing.** It
> is state the peer holds, and every observable we can reach says our side is
> correct.

**⚠ Do NOT re-propose any of these** — each is now killed with a matched
control: "our `op9` is malformed" (§4M.16, §4M.21), "our sequence numbering is
wrong" (here), "we fail to answer something" (§4M.18 — we answer everything),
"the peer is rejecting us" (`Rej/Disconn Reason 0` on every CDT).

**What is genuinely left:** why a VMS connection manager declines to advance
`recv_ack` on a correctly sequenced, correctly formed, correctly addressed
frame — for a returning identity only. That is peer-internal, below every
oracle the lab exposes, and undocumented in both the public SDA manual and the
lab's own `HELP` (§4M.22). **The honest next step is not another OVMX-side fix;
it is either a VMS-internals documentation source we have not found, or an
operator ruling that this is a gate.**

### 4M.25 ⭐⭐⭐ THE BUG REPRODUCES ON A VIRGIN CLUSTER — accumulated state is eliminated

**Operator authorised a different class of experiment. This is the most
important elimination on the item.**

**Every rejoin test across all thirteen sessions has run on a pod already
carrying a dozen dead OVMX identities.** `vaxlab-3` was scaled up fresh and had
never seen an OVMX node. Verified, not assumed: the only `OVMX` string anywhere
in `V1A`'s pre-run `SHOW CONNECTIONS` dump is the `/NODE=OVMXV1` query marker
itself; every CDT belongs to `VAX2` or is a bare `listen`.

| run | identity | verdict |
|---|---|---|
| `V1A` | `OVMXV1` — **the first OVMX identity this cluster has ever seen** | **JOINED** |
| **`V1B`** | same identity | **REFUSED** |
| **`V1C`** | same identity, 2nd rejoin | **REFUSED** |
| `V2A` | `OVMXV2` fresh | **JOINED** |

> **A brand-new VMScluster admits an OVMX node once and then never again.**
> Nothing accumulated, nothing contaminated, no residue from prior identities,
> no CDT exhaustion. **The defect is intrinsic to the second admission of an
> identity, on a cluster with no history whatsoever.**

**What this eliminates outright:** accumulated pod state, cross-identity
contamination, resource exhaustion from prior runs, and any theory that depends
on the peer having seen other OVMX nodes. It also means **every earlier result
on a used pod is trustworthy** — the litter was never the cause.

**And it makes the defect more serious as a product bug, not less:** this is not
a lab artifact that a clean deployment would avoid. It would hit the first
restart of the first OVMX node in any cluster.

> Incidental, recorded without interpretation: the virgin pod reported
> `Number of free CDT's: 1` at T-PRE against `5` on the used pod, and joined
> normally regardless. **Do not read a resource story into it** — that is the
> §4M.7/§4M.22 ratio trap.

### 4M.26 ⚠ THE SUPPRESSION SERIES IS LARGELY VOID — the switches had nothing to act on

Second class tried in the same window on `vaxlab-2`: stop *adding* OVMX
behaviour and instead suppress it, on the theory that our ungated client walk
interferes with the very teardown we are waiting for.

`X1A` fresh **JOINED** · `X1B` (`OVMX_NO_DISKRUN_UNGATE=1`) **REFUSED** · `X1C`
(`OVMX_NO_OWN_VC=1`) **REFUSED** · `X1D` (both) **REFUSED** · `X2A` fresh
**JOINED**.

**But the switches did not measurably change what OVMX did.** `PSCUNGATE=0` and
`PSCLIENT=0` in **every** run *including the joins*, and `CONNREQ=2` in all four
— so `OVMX_NO_DISKRUN_UNGATE` had nothing to suppress (that path does not fire
in this failure mode at all, §4M.24), and `OVMX_NO_OWN_VC` moved no counter I
can see.

> **Recorded as inconclusive, NOT as a kill.** Guardrail 23: verify the switch
> engaged before believing the verdict. A refusal from a switch that did nothing
> proves nothing. If this class is retried, first establish a counter that moves
> when the switch is set.

### 4M.27 ⭐⭐ THE DOCUMENTATION SWEEP — what is published, what is not, and one source DELIBERATELY EXCLUDED

**Operator authorised the documentation hunt. Results, all published sources.**

> ### ⛔ PROVENANCE EXCLUSION — read this first
> The highest-detail source found was the **VAXcluster Disk I/O Internals
> Manual (DEC, March 1988)**, on bitsavers. **It is EXCLUDED from this project
> and must stay excluded.** Every page is footed *"Digital Equipment Corporation
> / Confidential and Proprietary"*, and its symbol index cross-references SCS
> **source listings** (`[SYSLOA.LIS]SYS$SCS.LIS`, `[DRIVER.LIS]SCSXPORT.LIS`).
> That is an internal training manual derived from source — **outside CLAUDE.md
> Rule 8's envelope**, which permits published documentation and our own wire
> only. Nothing from it is used anywhere in this document.
>
> **Excluding it costs nothing:** its flow-control content restates the DTJ
> passage below, which is clean. **Do not go and read it "just to check".** The
> legal protection on this entire RE effort (DMCA 1201(f), EU SW Dir. Art. 6)
> depends on provenance discipline, and a confidential source read once cannot
> be un-read.

**1. ⭐ CREDIT CANNOT CAUSE A RECEIVER-SIDE DISCARD OF A SEQUENCED MESSAGE.**

> *"Datagram credit controls operate at the receiver… Upon receiving a datagram,
> a SYSAP must have available a datagram-receive credit; otherwise, the datagram
> is discarded. **The receiving of messages, however, is guaranteed.
> Message-credit controls are instituted at the sending node.**"*
> — Duffy, "The System Communication Architecture", **DTJ No. 5, Sept 1987,
> p. 26.**

**Independently confirms §4M.22's kill of the credit theory**, and from
documentation rather than a counter: silent credit-based discard is documented
**only for datagrams**. Our `op9` is a sequenced message, so credit is not even
a candidate.

**2. ⭐⭐ A NAMED DISCARD COUNTER EXISTS AND WE HAVE NEVER READ IT.**
`SHOW PORT/VC` publishes per-VC discard buckets — **`Illegal Seq Msg`**,
`Bad Checksum`, `Rcv Short Msg`, `No Xmt Chan`, `TR DFQ Empty`, `TR MFQ Empty`,
`CC MFQ Empty`, `Cache Miss` (*VSI OpenVMS Cluster Systems*, DO-DCLUSY-01A, App.
F, Example F.2, p. 365). The manual gives headers but **no prose definitions** —
which does not matter: **if our unacknowledged `op9` increments a named bucket,
that names the class of failure directly.**

> ### ⛔ NULL RESULT — THOSE COUNTERS DO NOT EXIST ON VAX 7.3
> `tools/portwatch.sh` was built and run (`Y1A` joined / `Y1B` refused), and
> **captured nothing**: every sample returned
> `%SDA-W-NOREAD, unable to access location 0000000A` / `%CLI-W-SYNTAX`.
> **`SHOW PORT/VC` is not a valid VAX 7.3 SDA command.** Probing directly,
> `SHOW PORT` and `SHOW PORTS` both work and yield only Port Descriptor Tables;
> `HELP SHOW PORTS` documents `[/ADDRESS=n][/NODE=name]` and **no `/VC`**. The
> `Illegal Seq Msg` / `Bad Checksum` / `Rcv Short Msg` buckets are Alpha-era
> NISCA counters and **are not reachable on this lab.**
>
> **The `Y1A`/`Y1B` verdicts are therefore VOID as a test of anything** — the
> instrument produced no data. Caught by reading the capture instead of the
> `XITDONE` line; the third time today that check has stopped a bad conclusion.
>
> **What remains at this layer:** SCACP (`MC SCACP` → `SHOW VC`), which is
> PEDRIVER's own view and *does* work here — §4d.9 measured `VC Total Errors`
> **0 on a join vs 21 on a refused rejoin** with it, on lab-1, before nine of
> this session's fixes landed. **Re-measuring that pair now would be cheap and
> is the nearest surviving substitute for the counter the manual promised.**

**3. ⭐ THE PUBLISHED MODEL SAYS A REMOVED NODE IS EXPECTED TO REBOOT.**

> *"**A node that recovers after it has been removed from the cluster is told to
> re-boot by the connection managers.**"* — Kronenberg, Levy, Strecker &
> Merewood, "The VAXcluster Concept", **DTJ No. 5, Sept 1987, p. 17.**

> *"The cluster connection between two computers is broken for longer than
> RECNXINTERVAL seconds. Thereafter, the connection is declared irrevocably
> broken. If the connection is later reestablished, one of the computers shut
> down with a CLUEXIT bugcheck."* — *VSI OpenVMS Cluster Systems*, App. C.7.1,
> p. 314.

Corroborated by Butcher, "OpenVMS Clusters Best Practice", hpUG Nov 2011,
slide 55: *"Excluded member(s) must rejoin as new members (reboot)…"*

> **⚠ FLAGGED AS INFERENCE, NOT DOCUMENTED.** No published source says a peer
> keys admission on `INCARNATION`. What *is* documented is that identity carries
> a **per-boot** component beyond SCSNODE/SCSSYSTEMID (`INCARNATION`: *"Unique
> 16-digit hexadecimal number established when the system is booted"*; `CSID`:
> *"may change when the system reboots"*). **Note §4 already made OVMX's
> incarnation live per boot and §3.11 killed the `[22:24]` echo** — so the
> obvious version of this is spent. Do not re-propose it without a new angle.

**4. Sequence window — PARTIAL.** *"Each transmit message carries a sequence
number; **duplicates are discarded**"* and *"Message acknowledgment — an
increasing value that specifies the last sequenced message segment received…
**all messages prior to this value are also acknowledged**"* (App. F, Tables F.3
and F.10, pp. 359-360, 366). **NOT FOUND:** whether acceptance requires exactly
`last+1`, or what happens to an out-of-window sequenced message.

**5. ⛔ CONNECTION STATES — NOT FOUND, CONFIRMED EXHAUSTIVELY.** Two independent
sweeps across every edition of the VAX/Alpha SDA manuals, HP V8.3/V8.4 and VSI
System Analysis Tools manuals, the VSI wiki and four mirrors: all show the CDT
fields and **none defines any of them**. Published examples show only `listen`
and `open`. **`con_sent`, `disc_sent`, `con_pend`, `disc_pend` are undefined in
every public source**, and *"does a disconnect-pending connection discard
incoming sequenced messages"* is **NOT FOUND anywhere**. §4M.22's conclusion
stands and is now thoroughly evidenced.

**6. Useful null — do not repeat this search.** *VMS Internals and Data
Structures* V5.2 (1462 pp.) was downloaded and full-text searched: **it has no
SCS chapter.** Its only SCS content is a `CDT` pointer in the CDRP layout
(Fig. 24.2, p. 680) and some SYSGEN parameter names. **IDSM is a dead end for
SCS internals.**

**7. The one published source still worth getting.** **Roy G. Davis,
*VAXcluster Principles*, Digital Press, 1993** (ISBN 1555581129) — a *published
book*, squarely in scope, and the most likely public home for the SCS connection
state machine and disconnect-time message handling. On archive.org as
`vaxclusterprinci0000davi`, **borrowable with a logged-in account**. Search it
for "disconnect", "sequenced message", "connection state". **This is the #1
remaining documentation move and it needs an operator with an archive.org
login.** Also unobtained: Kronenberg et al., ACM TOCS 4(2) 1986 (paywalled, 403)
— though the DTJ article by the same authors is likely a superset.

### 4M.28 ⭐⭐⭐ THE SCA CONNECTION STATE MACHINE, FROM THE BOOK — and a hard contradiction with the wire

**Source: Roy G. Davis, *VAXcluster Principles*, Digital Press, 1993, ch. 2
"Systems Communications Architecture", Figures 2-14, 2-15 and 2-16, pp. 2-24 to
2-26.** A published book, borrowed legitimately; passages transcribed under
fair-use quotation exactly as the DTJ material was. **This is the source every
sweep in §4M.27 failed to find, and it answers what no manual would.**

**Figure 2-14, SCA Connection Formation — the states, at last:**

```
NODE_1 (initiator)                     NODE_2 (target)
  CONN STATE = "CLOSED"                  TARGET SYSAP IS "LISTENING"
  SCS SENDS "CONNECT_REQ"  ----------->
  CONN STATE = "CONNECT SENT"            SCS SENDS "CONNECT_RSP"
                           <-----------  SCS PASSES "CONNECT_REQ" TO TARGET SYSAP
  CONN STATE = "CONNECT ACK"             CONN STATE = "CONNECT REC"
                                         TARGET SYSAP INVOKES ACCEPT
                           <-----------  SCS SENDS "ACCEPT_REQ"
  SCS SENDS "ACCEPT_RSP"   ----------->  CONN STATE = "ACCEPT SENT"
  CONN STATE = "OPEN"                    CONN STATE = "OPEN"
```

Figure 2-15 adds the rejection path: target invokes REJECT → `REJECT_REQ` →
`CONN STATE = "REJECT SENT"` → initiator abandons and sends `REJECT_RSP`.

> **These map straight onto SDA's undefined values.** `0001 listen` = LISTENING,
> `0002 open` = OPEN, `0007 con_sent` = **CONNECT SENT**, `0005 disc_sent` =
> **DISC SENT**. §4M.22/§4M.27's "not documented anywhere public" is now
> resolved — it was in a book, not a manual.

**Figure 2-16, Explicitly Breaking an SCA Connection — and it matches our join
frame for frame:**

```
NODE_1                                   NODE_2
  CONN STATE = "OPEN"                      CONN STATE = "OPEN"
  SYSAP INVOKES DISCONNECT
  SCS SENDS "DISCONNECT_REQ" ---------->
  CONN STATE = "DISC SENT"                 SCS SENDS "DISCONNECT_RSP"
                            <----------    SCS NOTIFIES SYSAP
  CONN STATE = "DISC ACK"                  CONN STATE = "DISC RECEIVED"
                                           SYSAP INVOKES DISCONNECT
                            <----------    SCS SENDS "DISCONNECT_REQ"
  SCS SENDS "DISCONNECT_RSP" --------->    CONN STATE = "DISC MATCH"
  CONN STATE = "CLOSED"                    CONN STATE = "CLOSED"
```

**Our join is this figure exactly:** peer `op6` → our `op7` → our `op6` → peer
`op7`. **So `op6` = `DISCONNECT_REQ` and `op7` = `DISCONNECT_RSP`, confirmed
from documentation** rather than inferred from §4k.5's correlation. It also
confirms §4M.23's DTJ reading: the teardown is symmetric, both SYSAPs invoke
disconnect.

### ⭐⭐⭐ THE CONTRADICTION — the peer is in DISC SENT with nothing on the wire

Per Figure 2-16, **`DISC SENT` is entered by "SCS SENDS DISCONNECT_REQ"**. The
peer's CDT for our identity sits in `0005 disc_sent` for the entire run
(§4M.18). **Therefore the peer's SCS believes it has sent a DISCONNECT_REQ.**

**No DISCONNECT_REQ ever reaches the wire.** Re-scanned with the net widened to
**every msgtype** (the earlier scan filtered `0x4b`/`0x5b`/`0x7b`), peer→OVMX
only, whole capture:

| run | verdict | `op6` from peer, ANY msgtype |
|---|---|---|
| `Q1A` | JOINED | **2** |
| `Q1B` | REFUSED | **0** |
| `U1B` | REFUSED | **0** |
| `V1B` | REFUSED (virgin cluster) | **0** |

> **The peer's SCS has entered DISC SENT without the DISCONNECT_REQ reaching the
> wire, and its CDT holds a NON-EMPTY message queue (§4M.18).** The disconnect
> is built and queued and never transmitted. That is a peer-side transmit stall
> on a specific message, now stated against a documented state machine instead
> of inferred from silence.

**⚠ Do not conclude "credit" from this.** The peer's own CDT reports
`Send Credit 1` and **`Send Credit Q. empty`** — nothing is waiting on credit —
and DTJ p.26 documents message credit as enforced at the *sender*, with silent
discard only for datagrams (§4M.27). Credit is dead twice over.

### 4M.29 ⭐⭐ A DOCUMENTED MECHANISM OVMX IMPLEMENTS NOTHING FOR — SCA connect data

Same source, p. 2-25:

> *"Up to **16 bytes of optional data** can be included in the CONNECT_REQ by
> the SYSAP initiating the connection. SCA also permits the target SYSAP to
> optionally provide up to 16 bytes of data to be included in the ACCEPT_REQ.
> In particular, this option is used to limit which versions of VMS can coexist
> with each other in a VAXcluster configuration. **When two Connection Managers
> form a connection with each other, they use this data to effectively identify
> to each other which version of VMS each is associated with.** If the target of
> the CONNECT_REQ does not approve of the source Connection Manager's VMS
> version, it rejects the request. If the source of the CONNECT_REQ does not
> approve of the target Connection Manager's VMS version, **it explicitly breaks
> the connection that the target Connection Manager accepted.**"*

**`grep -i "connect data" src/vmsscs/` returns NOTHING. OVMX has no concept of
this field.** And the peer's CDTs for our identity carry non-zero `Connect Data`
pointers (`87A05404` on the join; `87BE1985` / `87BD5F01` / `87A04204` on the
refusal), so the structure is populated on its side.

**Why this is worth chasing:** it is a documented, connection-manager-level
identification payload exchanged at CONNECT/ACCEPT time, whose documented
failure mode is *"explicitly breaks the connection that the target Connection
Manager accepted"* — which is close to the observed shape. **Unknown whether it
is the gate; it is the first genuinely unexamined mechanism found in sessions.**

**Next:** locate the 16-byte optional-data region in a real node's CONNECT_REQ /
ACCEPT_REQ on our own wire and compare it with OVMX's. Rule 8: the book gives
the mechanism, our own captures give the bytes.

### 4M.30 ⭐⭐⭐⭐ THE MASQUERADE TEST — a documented rule that abandons VC formation silently

**Source: Roy G. Davis, *VAXcluster Principles*, Digital Press 1993, §2.3,
p. 2-21, footnote †.** This is what thirteen sessions of wire work could not
see, because it is a *decision the peer makes internally and never announces*.

> *"† At this time, **special tests are made to ensure that the remote node is
> not masquerading as a node already known to the local system.** For example,
> if the SCS System ID in the formative System Block matches the SCS System ID
> in a System Block already in the Configuration Queue, the SCS Node Names must
> also match. The converse is also true. **If both items match, and if there is
> a Path Block already queued to the System Block in the Configuration Queue,
> then the 64-bit incarnation numbers must also match. Virtual circuit formation
> is abandoned if any of these tests fail.**"*

And the Note immediately preceding it, which explains why a **real** node
survives the same path (same page):

> *"If there are no other Path Blocks queued to the old System Block, the new
> Path Block represents the only open virtual circuit with the remote node. If
> this is the case, **the old System Block is refreshed** based on the contents
> of the formative System Block before the formative System Block is discarded.
> **Typically, this happens when the remote node was once in the cluster,
> departed, and is now rebooting.**"*

### The mechanism, stated

1. The peer keeps a **System Block** per remote node. **§4L.9e measured exactly
   this** — the SB persists per identity across our death, and we recorded it as
   "NORMAL, not the bug". It is normal; it is also the precondition.
2. On our return the peer builds a **formative** SB + Path Block from our START.
3. When the VC would reach OPEN, the formative PB is queued to the **old** SB —
   and the masquerade tests run.
4. SCSSYSTEMID matches. SCSNODE matches. **So if a Path Block is still queued to
   that old SB, our 64-bit incarnation must ALSO match.**
5. **OVMX emits a FRESH incarnation every boot** — correctly, per §4 and the
   published definition (*"established when the system is booted"*). On a rejoin
   that fresh value **cannot** match. → **VIRTUAL CIRCUIT FORMATION ABANDONED.**
6. A real rebooting node passes because **its departure leaves no Path Block**,
   so the SB is *refreshed* with the new incarnation and the test never applies.

### Why this fits EVERY observation, including the ones that killed other theories

| observation | explained |
|---|---|
| Nothing is rejected — `Rej/Disconn Reason 0` everywhere (§4M.18) | VC formation is **abandoned**, not rejected. There is no reject message in this path. |
| The peer never acknowledges our byte-identical `op9` (§4M.24) | The VC carrying it is being abandoned. Our frames are irrelevant. |
| **Nothing we transmit differs by one non-per-run byte** (§4M.16) | Correct — and it never could. The test is on peer-held state vs. one field. |
| CDTs stuck in `con_pend`/`disc_pend`, queued messages never transmitted (§4M.18, §4M.28) | Downstream of an abandoned VC. |
| Reproduces on a **virgin cluster** (§4M.25) | The *first* join creates the SB and PB. History is irrelevant. |
| Identity-keyed; repeats indefinitely (§4L.9d) | The SB is per-identity and persists. |
| A real node rejoins fine (§4M.22) | Its departure clears the Path Block. |
| Ten fixes to what we transmit changed nothing | The gate was never in what we transmit. |

**It also retro-explains §3 item 9** (changing one half of the identity pair
fails *differently*): that is the FIRST masquerade test — ID matches, name must
match — firing instead of the incarnation test.

> **⚠ NOT YET PROVEN. This is a documented rule plus a consistent fit, which is
> exactly the state seven dead hypotheses were in.** §4L.9a: *"Consistency is not
> evidence."* The difference here is that it is falsifiable in one run.

### The test, running now

`OVMX_INCARNATION_TIME=<n>` pins the quadword (§4). **Present the SAME
incarnation on the rejoin that the peer already recorded**; if the masquerade
test is the gate, it passes and the VC forms.

`Z1A` fresh, incarnation pinned · **`Z1B` rejoin, SAME pinned incarnation — THE
TEST** · `Z1C` rejoin, live/differing incarnation — the normal failure as a
one-variable control · `Z2A` fresh closing control.

> **⚠ This is a DIAGNOSTIC, not the fix.** A real node rejoins *with a new
> incarnation*, because its Path Block is gone. If `Z1B` joins, the confirmed
> mechanism points the real fix at **our DEPARTURE** — ensuring the peer's Path
> Block is dequeued when OVMX exits — not at freezing our incarnation, which
> would be VMS-incorrect and would earn the CLUEXIT of §4/App. C.7.1.

**Transcript provenance:** the chapter was photographed from a legitimately
borrowed copy and transcribed to `/home/baron/cluster/transcript/part{1..4}.md`
— **deliberately OUTSIDE the git repo.** Load-bearing passages are quoted here
with citation, as the DTJ material is; the full chapter is not committed.

### 4M.31 ⛔⛔ THE MASQUERADE TEST IS REFUTED — twice, and the second one was free

**Run `Z`, bracketed, one variable, pin verified ON THE WIRE:**

| run | incarnation | verdict |
|---|---|---|
| `Z1A` | pinned `0xbc06e9ee7e3b00` | **JOINED** |
| **`Z1B`** | **SAME pinned value — THE TEST** | **REFUSED** |
| `Z1C` | live (differs) — control | REFUSED |
| `Z2A` | fresh identity | **JOINED** |

The pin is **verified on the wire**, not assumed: OVMX's `0x41` START frames in
`Z1A` and `Z1B` both carry incarnation quadword `0xbc06e9ee7e3b00` at abs 80..87.
`Z1B` genuinely presented the value the peer had recorded, and was refused.

**And the field was the right one.** p. 2-16 lists what an SB carries: *"SCA
requires each node to have a 64-bit **software incarnation number** that changes
each time the node reboots… VMS refers to this as a **software incarnation
time** since VMS implements it as the date and time the system most recently
booted."* That is exactly the quadword §4 grounded four ways, including VAX1
reading our emitted value back as `Incarnation`. **We matched the field the
footnote names, and it did not admit us.**

> ### ⛔ AND THE SECOND REFUTATION WAS ALREADY IN HAND — I should not have needed the run
> The footnote's consequence is *"**virtual circuit formation is abandoned**"*.
> **But the virtual circuit demonstrably FORMS on every refused rejoin.** The
> peer opens `SCS$DIRECTORY` connections to us, runs its 8 directory lookups,
> exchanges config and sends `op8` — **all of which require an OPEN VC**
> (§4M.18's frame-by-frame dialogue; `STARTDONE`=2 in every run). A VC whose
> formation was abandoned carries none of that.
>
> **So the masquerade tests PASS, and §4M.30's mechanism is simply wrong.** This
> was derivable from evidence collected days ago. **Method note: I fitted a
> newly-found documented rule to the symptom set without first checking the rule's
> own stated CONSEQUENCE against what we already knew.** A perfect explanatory
> fit is not a test; §4L.9a said it and it caught me again.

**§3 killed entry 21.** Eleventh hypothesis killed on this item.

### What survives, and it is worth keeping

- **The chapter's connection/disconnect state machines** (§4M.28) — those are
  documentation and stand regardless. `disc_sent` = DISC SENT, `op6`/`op7` =
  DISCONNECT_REQ/RSP, teardown symmetric.
- **The masquerade tests themselves are real** and now explain §3 item 9's
  *different* failure shape when one half of the identity pair is changed.
- **The SCA connect data mechanism** (§4M.29) — 16 bytes at CONNECT/ACCEPT that
  OVMX implements nothing for. **Untouched, and now the best remaining lead.**
- **OVMX performs NO teardown at exit** — `g_stop=1`, print summary, exit. No VC
  close, no disconnect, no departure. Grounded by reading our own source; not
  yet shown to matter, but it is the only asymmetry left against a real node's
  departure.

### The failure is AFTER virtual-circuit formation

That is now certain, and it narrows things usefully. The VC opens; connections
open over it; the directory dialogue completes; `op8`→`op9` completes; and then
the peer stops without rejecting anything. **Every remaining candidate must live
at the SCS *connection* layer or above, not at VC formation.**

### 4M.5 The test, and its kill-switch

1. Ground the reference rule for the **lookup response** specifically (the 336-
   frame census covers the op-5 confirm, not this frame): what msgtype does a
   real VAX put on a lookup response to a `0x5b` request? **Dispatched.**
2. Stop mirroring: emit `0x4b` on the lookup response unconditionally, behind
   `OVMX_DIR_MIRROR_MSGTYPE=1` which restores today's mirror (guardrail 21 —
   ship the switch and use it in the same session).
3. Bracketed triple on `vaxlab-2`: fresh join → **same-identity rejoin** → fresh
   join, identity proven on the wire, `csbwatch.sh`.



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
| `tools/csbcycle.sh` | **SIGKILL a REAL node and sample the peer's SDA `SHOW CLUSTER` across kill → removal → reboot → readmission**, on a pod that also carries dead/refused OVMX identities so all histories land in ONE dump. This is what produced the real-node control (§4j) and killed "our surviving CSB is the asymmetry". Stays parked inside SDA and slices the console INCREMENTALLY per sample. **Both of its original defects are FIXED and verified** (§4j.6): per-node `SHOW CLUSTER/NODE=` sampling instead of the 6.3 KB full dump, leading newline before each marker, and a loud abort on consecutive empty samples. Usage: `csbcycle.sh <pod> <tag> [node ...]`, `CAD`/`DEAD`/`TOTAL`/`MAXEMPTY` env. |
| `tools/csbwatch.sh` | **one OVMX attempt on lab-2 with the peer's CSB for OUR identity sampled THROUGH it.** `lab2run.sh`'s runner (staging, sidecar carry/pull, tcpdump, identity-on-the-wire proof) with `csbcycle.sh`'s narrow `SHOW CLUSTER/NODE=<identity>` sampling replacing the `CLUSTER_NODES` console poll. This is what §4h.3 item 2 asked for and never got. The `CLUSTER_NODES` verdict is lost — the console is inside SDA — and is not missed, because the CSB names the state directly and **`SCSNODE … not found` is itself a datum**: it says the peer holds no CSB for us at all. Usage: `csbwatch.sh <pod> <tag> <store> <duration> <identity> [ENV=V …]`, `CAD`/`MAXEMPTY` env. |

> **⚠ THE LAB TOOLING IS NOT VERSION CONTROLLED.** `/data/training/vax/cluster/`
> is not a git repo, so every script in this table exists only on `workshop`'s ZFS
> volume. This has already cost the project once — `mk_sysgen`'s source is lost
> and had to be reimplemented as `mk_sysgen.py` (§4e.1). `tests/lab/` on main is
> now a natural home for them. **Worth a small item; not done here.**

Run tags session j (part 2): `r1A` `r2A` joined, `r1B` `r2B` refused, `vax3crash` = the real-VAX crash-rejoin specimen. **Last SCSSYSTEMID used: 1241.**
Run tags session k: `s1A` `s2A` `s3A` `s4A` `s5A` joined (fresh, pure); `s1B` refused (rejoin form), `s1C` refused (`OVMX_REJOIN_FORM=0`); `s3B` refused (SDA-polled on VAX3), `s3C` refused (`OVMX_CFG2_PEER=1`, forced to the real coordinator); `s3D` refused / `s6A` joined = the matched SCACP pair (§4d.9); `s3E` refused / `s7A` joined = the high-cadence ORDERING pair (§4d.10); `s3F` refused WITH the 0x7b fix, `s8B` fresh joined with it. **Last SCSSYSTEMID used: 1249.**
Run tags session L (workshop): `w1A` `w2A` `w3A` joined (fresh, pure); `w1B` `w1C` refused (`OVMXW1` rejoin). `w1C`/`w3A` are the matched CDT pair (§4e.3); `w2A` is VOID as an instrumented run — its console overran (§4e.1) — but valid as a join. **Last SCSSYSTEMID used: 1252.**

Run tags session L part 3 (lab-2 `vaxlab-0`): `L3` joined (fresh) / `L3b` refused (`OVMXL3` rejoin) — the matched pair; `L4` joined (fresh, `OVMXL4`, then died, never returned); `C1` = the **real-node CSB cycle** (§4j), which carried `OVMXL3` and `OVMXL4` in-frame as the refused and died controls. `vaxlab-0` is now SPENT — VAX1's console wedged after `C1` (§4j.6). **Last SCSSYSTEMID used: 1304.**

Run tags session L part 4 (lab-2 `vaxlab-2`, the §4L bracketed triples, all via
`tools/csbwatch.sh`): `M1A` joined (`OVMXM1`, 1305, fresh) / **`M1B` refused**
(same identity) / `M2A` joined (`OVMXM2`, 1306, fresh) — the bracketed triple.
Then `M3A` joined (`OVMXM3`, 1307, fresh) / **`M3B` refused** / **`M3C` refused**
(third attempt) at `CAD=1..2`. **Last SCSSYSTEMID used: 1307.**

Run tags session m (lab-2 `vaxlab-2`, the §4M fix under test, all via
`tools/csbwatch.sh`): `N1A` fresh (`OVMXN1`, 1308) = opening control ·
**`N1B`/`N1C`/`N1D` = three consecutive same-identity rejoins**, the test of
`scs_dir_response_msgtype()` (done-criteria 1 and 3) · `N2A` fresh (`OVMXN2`,
1309) = closing control (guardrail 20). Then **`N1E` = the guardrail-21
kill-switch run** (`OVMXN1` rejoin, `OVMX_DIR_MIRROR_MSGTYPE=1`, fix OFF —
REFUSED, and it refuted §4M.7 and §4M.8's attribution) and `N3A` fresh
(`OVMXN3`, 1310) = its closing control, JOINED. Binary:
`build-d94/bin/SCSD.EXE` built from the commit that lands §4M.
**All four `N1*` rejoins were refused; all three fresh identities joined.**

Run tags session m part 2 (the §4M.14 `0x7b` credit-handshake fix): `P1A` fresh
(`OVMXP1`, 1311) opening control · **`N1F` = `OVMXN1` rejoin — the known
reproducer, refused 4× as `N1B`/`N1C`/`N1D`/`N1E`** · `P1B`/`P1C` = `OVMXP1`
consecutive rejoins · `P2A` fresh (`OVMXP2`, 1312) closing control. Kill-switch
`OVMX_NO_CREDIT_RETX=1`. **Result: both fresh identities JOINED, all three
rejoins REFUSED, `CREDIT-RETX-ANSWERED` 0/0 in the joins and 2/2/2 in the
refusals** (§4M.17).

Run tags session m part 3 (`tools/connwatch.sh`, new — SDA `SHOW
CONNECTIONS/NODE=` on VAX1, the link where `op6` first goes missing): `Q1A`
fresh (`OVMXQ1`, 1313) · `Q1B` same-identity rejoin · `Q2A` fresh (`OVMXQ2`,
1314) closing control. **Last SCSSYSTEMID used: 1314.**

**Pod state after this session:** `vaxlab-0` SPENT (console wedged). `vaxlab-1`
DEGRADED — its VAX2 was SIGKILLed by the `SMOKE` tool test and never rebooted.
**`vaxlab-2` is healthy** and carries residual state for `OVMXM1`/`OVMXM2`/
`OVMXM3`, which makes it a ready-made rejoin reproducer. Scale fresh pods with
`kubectl -n ovmx-lab scale sts/vaxlab --replicas=N`.

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
22. **A peer-side sample taken after our process exits measures our corpse, not
    our attempt.** §4j compared a LIVE real-node readmission against `OVMXL3`/
    `OVMXL4` CSBs sampled minutes after those OVMX processes had exited, and
    concluded OVMX gets no new CSB and no CDT. Watching a rejoin *live* (§4L)
    shows it gets both — the old CSB freed, a new one allocated, the same CDT
    address the successful joins get. The post-mortem state (`09 wait`,
    `long_break`, `cdt=00000000`) is decay, not refusal. **State whether OVMX was
    RUNNING at the moment of every CSB/CDT sample**; §4e.3, §4f.3, §4g and §4j all
    mix the two, and any claim of the form "the peer never allocates X for us"
    taken from a dead-OVMX sample must be re-taken live before it is trusted.
25. **Never leave the VAX console inside a utility, and never assume `EXIT`
    exited.** An `SDA> HELP SHOW CONNECTIONS` run ended with its `EXIT` being
    consumed by HELP's `Topic?` prompt rather than by SDA. The console sat at
    `SDA>` and the next lab series aborted with `FATAL -- vax1 not at DCL`.
    **That is guardrail 19 working** — the harness failed loudly instead of
    producing a bad result — but it voided a whole series. Drive an interactive
    utility only from a script that verifies it got back to `$`, and check the
    console tail before trusting any run that follows one. **Specifically: never
    let `HELP` be the last thing you send.** It has its own nested
    `Topic?`/`Subtopic?` prompts that swallow `EXIT`, and it has now wedged the
    console TWICE — once voiding a whole series. Send blank lines to unwind HELP
    first, then `EXIT` to leave the utility, then verify the `$` prompt.
24. **Read the OTHER node's console. `csbwatch`/`stallpoll` park VAX1 inside
    SDA, but VAX2's console is untouched and VMS prints the entire membership
    dialogue to OPCOM** — `received membership request`, `proposed addition`,
    `completed`/`aborted VAXcluster state transition`, with timestamps. §4d.9
    concluded "no peer oracle reports a DECISION"; this one does, it was free,
    it was on disk for four sessions, and it produced the 90 ms window of §4M.11.
    **Corollary: use only the timestamped `Node X (csid …)` OPCOM lines.** The
    bare `%CNXMAN,` echoes interleave out of order and reading them as a
    sequence produced a wrong mechanism that survived one draft.
23. **Run the kill-switch BEFORE you write down what your fix achieved.**
    Guardrail 21 said ship the switch and use it in the same session. Not
    enough: I shipped it, wrote two sections crediting the fix with a CSB
    advance and a dialogue advance, and only then ran `N1E` with the fix off —
    which showed both were there anyway. A code change and a lab-session change
    landed together and I attributed the session change to the code. **Also:
    check every run you already have before naming a discriminator from one.**
    §4M.7's `cat 0x04` claim died to three runs that were already on disk or one
    command away.
17. **Measure the window you are actually naming.** §4b asserted a coordinator
    stall for a whole session. The coordinator's response latency was 0.4 ms and
    had been in our own logs the entire time; the 6.5 s belonged to the next
    window and to us. Timestamp the specific edge before naming who is late.
