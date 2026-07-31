# Handoff — T1: OVMX survives cluster life as a MEMBER

**Written 2026-07-31, end of session g. Read `docs/HANDOFF-vms-760.md` §0 FIRST
for the orchestrator doctrine and the lab procedure — it still applies verbatim.**

---

## 0. Status in one paragraph

`vms-760` and `vms-4f2` are **CLOSED**: OVMX joins a real 3-node VMScluster,
`SHOW CLUSTER` on a real VAX lists it as **MEMBER**, reproducible 3/3, holds 9+
minutes, leaves cleanly, zero bugchecks. The live work is **T1 (`vms-32b`)** —
making OVMX safe and correct while the cluster changes shape *around* it. T1.1
(`vms-e81`) is claimed and in progress.

## 1. ⚠ FIRST THING TO DO — verify or revert

Run `by8` (the last of the session) reported **`PHASE1 transitions=0 restarts=0`**
where every earlier run reported `transitions=1` at the same point.
`restarts=0` is good (a storm I introduced and fixed is gone). **`transitions=0`
is not** — it may mean commit `68f6e9e` (always emit the op-1 CONNECT-ECHO)
perturbed OVMX's *own* join path, which previously reached MEMBER reliably.

**Do this before anything else:**

1. Re-run a plain join against a pristine 3-node lab (`reset3.sh`, then
   `waitnodes.sh 3`, then `try.sh <tag> OVMXV1 1203 90 OVMX_JOIN_SEQ=1`).
2. If OVMX still reaches MEMBER → `68f6e9e` is fine, `by8`'s phase-1 was a
   timing artefact (its check fires at 110 s), carry on to §2.
3. If OVMX no longer reaches MEMBER → **`68f6e9e` regressed the milestone.**
   The echo consumes a `send_seq`, so the accept path's sequence accounting
   changed. Either scope the echo to peers where we are the *acceptor of a
   member-initiated VC* (which is the grounded case) or fix the sequence
   arithmetic. **Do not leave the tree in a state where MEMBER is broken.**

## 2. The T1.1 state (`vms-e81`)

**What OVMX now does right** — all verified live, all committed:

| fix | commit | what it stopped |
|---|---|---|
| barrier re-arms every transition | `aff7916` | going silent forever after our own join |
| `op 0x12` carries the **epoch**, not a member count | `16f105e` | a fabricated field |
| `body[55]=0` scoped to `op 0x09` only | `16f105e` | overwriting a byte in someone else's message |
| never run the joiner sequence at a newcomer | `b15cc51` | shouting at a node mid-join |
| a member **initiates** to a newcomer, on a timer | `e070a48`, `bb60b15` | a deadlock where both sides waited |
| always emit the op-1 CONNECT-ECHO | `68f6e9e` | **the actual blocker — see below** |
| re-STARTs handled, storm-guarded | `cde8e2b`, `600aac7` | silently dropping a peer's recovery |

**The root cause, found last:** when VAX3 opened a `VMS$VAXcluster` connect **to**
OVMX we answered op 0 → op 2, skipping the mandatory **op-1 CONNECT-ECHO**. VAX3
therefore withheld its op-3 CONFIRM, the VC stayed half-open, and **VAX3 never
ran the add-member dialogue with anyone — it never sent `op 0x02` at all** and sat
at NEW for 280 s. Every other symptom (60 unanswered directory retransmits, 55
re-STARTs, OVMX being dropped from the member set) cascaded from that.

The fix was **already written and commented in our own source**, reachable only
with `OVMX_PURE_SERVER` set. It cost a day of chasing ordering, patience,
incarnation and membership propagation.

**Success oracle for the next bystander run** (control idx 3707→3708):
VAX3 emits an **op-3 CONFIRM**, then a **204-byte CM frame**, then `op 0x02`
appears for VAX3 in `cm.py`. Then look for OVMX's **second `XITDONE`** — that is
T1.1's done condition.

**Lab procedure for the bystander test** (no existing script produces this
ordering):
```
bash ~/vax/cluster/tools/reset2.sh          # VAX1+VAX2 up, VAX3 down but intact
bash ~/vax/cluster/tools/waitnodes.sh 2 30  # MUST pass; || exit
# start SCSD, wait for OVMX to be MEMBER
bash ~/vax/cluster/tools/boot3.sh 300       # VAX3 boots into the LIVE cluster
bash ~/vax/cluster/tools/waitnodes.sh 4 12  # 4 = VAX1+VAX2+VAX3+OVMX
```

## 3. Queue after T1.1

- **`vms-e4b` (P0)** — key the CM allowlist on **role slot**, not opcode.
  `body[16:18]` is `<generation><role>`; the transition-open opcode varies by
  generation (`0x09` gen-2, `0x08` gen-3, **`0x0d` gen-4, unhandled**). A cluster
  that has run enough transitions will send us `op 0x0d` and we will go silent
  and strand it. Reproduce deliberately with join/exit cycles.
- **`vms-1ce` / `vms-584`** — differential decoding + lab expansion. Highest-value
  and ideal for long unattended runs: **4–5 nodes** (does the 12-step barrier
  scale with membership? every specimen so far is 2–4 nodes) and **join/exit
  cycling** (drives the epoch past the node count). Media for VMS 5.5 / 7.1 and a
  second CPU model are already on disk — see the item.
- `vms-ae5` node leaves · `vms-b8a` explicit leave · `vms-c7d` VC breakage ·
  `vms-405` cluster group+password · `vms-7d4` undecoded surface.
- `vms-70c` — derive the replayed constants (honesty debt from `vms-4f2`).

## 4. Guardrails that were earned, not assumed

1. **Never answer a `(cat, op)` pair you have not grounded.** Three separate
   incidents bugchecked real VAXes (`INCONSTATE`, `INVEXCEPTN`, `LOCKMGRERR`).
   Silence is a legitimate grounded answer — but it must be *established*
   (cat `0x02` `op 0x01` is grounded silence; a transition-open is not).
2. **Being faster than the reference is a compatibility bug.** OVMX answered the
   barrier GO in ~20 µs, beat the coordinator's own fan-out, and was tagged
   "not counted". Suspect this at every multi-target broadcast point.
3. **A repair that fires during normal operation is worse than the fault.** My
   re-START detection matched the ordinary round-1 of a handshake and looped
   2883 times; the bug it replaced only made *one peer* unreachable.
4. **One reset, one believable run.** An unverified precondition must **abort** —
   `waitnodes.sh` exists because an empty probe silently voided three runs.
5. **Copy console logs out before the next reset.** `reset2.sh`/`reset3.sh`
   truncate `/tmp/clean-vax1-test/vax1.log`; that destroyed evidence twice.
6. **Generalising from one leg of one capture is the recurring error.** The member
   count, the `body[55]` rule, the DLM lock-state theory and the
   member-waits-to-be-connected rule were all confounded samples read as rules.
   Construct the disagreeing configuration instead.
