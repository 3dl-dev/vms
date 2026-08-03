# Continuation prompt — `vms-2f3`, run to completion

> Paste everything below the line into a fresh Opus session on `workshop`.
> Keep this file updated as the objective moves; it is the boot record for the
> autonomous loop, not a historical document.

---

Work `vms-2f3` — **OVMX cannot REJOIN a VMScluster it was just removed from,
under the same SCSNODE/SCSSYSTEMID** — continuously to completion. The item is
claimed and active. Do not stop to report progress and do not ask whether to
continue; this is a standing completion mandate. Stop only on the reserved list
at the bottom.

## Objective and definition of done

**Done = a returning OVMX identity is readmitted to a VMScluster it was removed
from, reproducibly, with the mechanism understood and a regression test that
fails without the fix.** Concretely, all of:

1. A bracketed triple on a lab-2 pod — fresh join → **same-identity rejoin** →
   fresh join — where the middle run **JOINS** (`XITDONE=1`, peer CSB reaches
   `member,selected`), with the identity proven on the wire.
2. The same result on **lab-1**, so it is not a lab-2 artifact.
3. Three consecutive rejoins of one identity all admitted (§4L.9d shows the
   refusal currently repeats indefinitely; the fix must too).
4. The mechanism written up in `docs/HANDOFF-vms-2f3.md` as a decode, not a
   correlation — which frame or field, and why the peer accepted it.
5. A test under `tests/` that exercises the rejoin path and fails on the parent
   commit. Ground-source rule: no work is complete while a test in a layer you
   touched is skipped, failing, or absent.
6. CI green by SHA on the pushed branch.

If the root cause turns out to be a VMS behaviour OVMX cannot match without an
operator ruling, that is a **gate**, not a completion — see the reserved list.

## ⚠ SESSION m CLOSED 2026-08-03 — READ THIS BEFORE THE BOOT SEQUENCE

**The strategy changed. `vms-2f3` is no longer a bug hunt; it is now the
acceptance test for a build.**

Session m falsified **eleven** hypotheses and landed **ten** real, separately
fixed defects, none of them the cause (§3 now carries 21 killed entries). It
then obtained **Roy G. Davis, *VAXcluster Principles*, Digital Press 1993,
ch.2** — the source every documentation sweep had failed to find — and measured
OVMX against it. The finding that reframes everything:

> **OVMX reproduces the WIRE SHAPES of selected SCA exchanges by replaying
> byte-exact captured templates. It does not implement SCA.** Grep over
> `src/vmsscs/`: `conn_state`=0, `path_block`=0, `system_block`=0,
> `connect_data`=0, `reason_code`=0, `rspid`=0, `credit_wait`=0,
> `ACCEPT_REQ`=0. State lives in a MAC-keyed `struct peer_state`.
>
> **That is why eleven hypotheses died.** The failure is a connection-state-
> machine failure and OVMX has no connection state machine for it to be legibly
> wrong in. Every fix so far corrected a frame; none could correct a state.

**THE PLAN EXISTS: `vms-187`** — "OVMX SCS layer implements the SCA
architecture", 45 items under 5 deliverable epics, dispatch-ready.
`rd dep tree vms-187`. Dispatch with:
`/swarm-dispatch --strategy worktree --workers 4 vms-187`.
Its end-to-end item (`vms-70e2`) **is** this document's definition of done, and
is explicitly permitted to fail honestly and raise a gate rather than licence a
twelfth guess.

**⚠ THE CHAPTER TRANSCRIPT IS NOT IN GIT AND EXISTS ONLY ON THIS HOST:**
`/home/baron/cluster/transcript/part{1..4}.md` (pp. 2-1..2-57, all figures,
uncertain characters marked `[?]`). It is a copyrighted book and **must not be
committed**; load-bearing passages are quoted with page citations in
`docs/HANDOFF-vms-2f3.md` §4M.23/§4M.28/§4M.30 and in the `vms-187` item
descriptions. **If this host is lost the transcript is lost** — same exposure
class as `vms-f7a`. The book was borrowed from archive.org
(`vaxclusterprinci0000davi`) and can be borrowed again.

> **⛔ PROVENANCE — do not undo this.** The *VAXcluster Disk I/O Internals
> Manual* (bitsavers) is stamped DEC Confidential and cross-references SCS
> **source listings**. It is EXCLUDED under Rule 8 and nothing from it is in
> this project. Do not read it "just to check" — the DMCA 1201(f) protection on
> the whole RE effort depends on provenance, and a confidential source read once
> cannot be un-read. See §4M.27.

## Boot sequence, in order

1. `cd ~/projects/vms`, branch `worktree-760-active-directory`. Re-derive the
   SHA; do not trust any SHA written down anywhere.
2. **Read `docs/HANDOFF-vms-2f3.md` §4M first** — it is the newest and it
   supersedes §4L's framing. Then §4L (for its observations, not its
   conclusion), then §4k. Then **§3's killed list (18 entries) and §4L's seven
   more.** Then §7 guardrails (24 entries) — they are the most transferable part
   of the whole document.
3. Read `docs/HANDOFF-vms-760.md` §0 for the orchestrator doctrine. It still
   applies verbatim.
4. `rd show vms-2f3`. Also open: `vms-da1` (SDA↔wire counter mapping — blocking
   the interpretation of several CSB fields), `vms-950` (real ack bug, NOT the
   gate), `vms-f7a` (lab tooling not version controlled).

## Where the investigation actually stands

> **⚠ §4L's framing is SUPERSEDED. Start at §4M, not §4L.** §4L reduced the bug
> to "one flag word" (`00000000` vs `02040000 status_rcvd,vcc`). §4M fixed the
> cause of that flag word and **the rejoin is still refused**. §4L's
> observations stand; its framing does not. Do not re-open §4L.3.

**§4M landed a real bug and moved the failure a long way down the dialogue.**
OVMX mirrored the request's msgtype onto its SCS$DIRECTORY lookup responses; a
real VAX always answers `0x4b` (336-frame census, `scs_dir.c:244`). Mirroring is
correct by luck on a fresh join and wrong on a rejoin, where the peer asks with
`0x5b`. Fixed, kept, tested, **and not the gate.**

**Where the refusal now sits — and READ §4M.9 BEFORE TRUSTING ANY OF IT.** In
the `N` session (2026-08-02 pm) the peer's CSB for a **refused** rejoin is
populated exactly as an admitted one — `02040000 status_rcvd,vcc`, `Cpblty
00000A98`, `SWVers V7.3`, `HWName`, `Quorum/Votes 1/0`, `Lock mgr dir wgt 1`,
live incarnation, CDT/SB/PDT all allocated. **Two bits are missing and nothing
else: `member` and `selected`**, flat for 108 s across four refused rejoins.

**⚠ That is NOT the fix's doing, and it is NOT the same failure as §4L's.** The
kill-switch run `N1E` shows the identical populated CSB with the fix OFF. The
morning `M` session refused with `00000000`; the afternoon `N` session refuses
fully populated. **These are §4d.6's two refusal shapes and they must not be
diffed against each other.** What makes a run take one shape or the other is
**unknown**. ⚠ It is NO LONGER the most valuable open question — §4M.16–§4M.22
superseded it by locating the gate itself. Kept only so the two shapes are not
diffed against each other by accident.

## ⭐⭐⭐ THE FRONTIER AS OF SESSION m END — READ THIS FIRST

**The gate is located, proven to the byte, and confirmed against a real-node
control. It is peer-internal.**

| | fresh join | **rejoin** | real node rejoining |
|---|---|---|---|
| peer's `op8` → our `op9` | runs | **runs, byte-identically** | runs |
| peer's `op6` (directory DISCONNECT-REQ) | **4** | **0 — never** | **12** |
| peer's CDT for us | torn down, freed | **`disc_sent`/`disc_pend`, queue non-empty** | torn down |
| peer's `MSCP$DISK` connect | 22–23 on the wire | **0 — `con_sent`/`con_pend`, never transmitted** | present |

**The peer has ISSUED the disconnect and it is BLOCKED** (§4M.18). It is not
withholding `op6` and it is not rejecting us — `Rej/Disconn Reason` is **0** on
every CDT. The `MSCP$DISK` connect is queued behind it, which is why §4g's "the
peer declines to connect" is wrong: **it cannot transmit.**

**NOTHING OVMX SENDS DIFFERS BY ONE NON-PER-RUN BYTE** (§4M.16) — a class
discrimination test over our `op9`, our `0x81/0x03` and the peer's `op8` to us
returns **zero** discriminating offsets. Every candidate on our side is excluded
by a byte-level matched control.

**⛔ Already dead, do not re-propose:** credit (§4M.22 — the `0x48` flow is
identical, and the peer's own CDT says `Send Credit Q. empty`), the barrier
(§4M.15 — it starts 45 ms *after* completion), the `cat 0x04` opcode (§3.14),
`cat 0x04` deafness (§3.15), and the four `0x7b`/msgtype/sequence defects
(§3.13, 16, 17, 18) — all real, all fixed, none the cause.

**NINE separately-fixed real defects on this item, none of them the gate.**
Expect the tenth not to be either; fix them anyway (guardrail 15) and **run the
kill-switch before writing down what a fix achieved** (guardrail 23).

### ⭐⭐⭐ THE SINGLE SHARPEST FACT — read this before proposing anything (§4M.24)

**The peer never acknowledges our `op9`, and acknowledges a byte-identical one
in every join.**

| | join | **refusal** |
|---|---|---|
| peer `op8` | `recv_ack=11 send_seq=12` | identical |
| our `op9` | `recv_ack=12 send_seq=12` | **byte-identical (§4M.16)** |
| **peer's next frame** | **`op6`, `recv_ack=12`** ← accepted | **`op8` retransmit, `recv_ack=11`** ← never accepted |

**From the peer's point of view we have never answered its `op8` at all**, and
every downstream symptom — the retransmits, the `disc_pend` CDT, the missing
`op6`, our own `op6` being ignored — follows from that one fact.

**⛔ ALL OF THESE ARE KILLED WITH MATCHED CONTROLS. Do not re-propose:**
- "our `op9` is malformed" — §4M.16, §4M.21 (zero class-discriminating bytes)
- "our sequence numbering is wrong" — §4M.24 (12 frames identical, no duplicate)
- "we fail to answer something" — §4M.18 (we answer everything it sends)
- "the peer is rejecting us" — `Rej/Disconn Reason 0` on every CDT
- credit · the barrier · `cat 0x04` · `OVMX_DISKLESS` · self-disconnect · the
  four `0x7b`/msgtype/sequence defects — §3 entries 13, 16–20

**TEN real defects found and fixed on this item. None was the gate.** Fix the
eleventh too (guardrail 15) but do not expect it, and **run the kill-switch
before writing down what it achieved** (guardrail 23).

**What is genuinely left:** why a VMS connection manager declines to advance
`recv_ack` on a correctly sequenced, correctly formed, correctly addressed frame
— for a returning identity only. That is peer-internal, below every oracle the
lab exposes (SDA cannot resolve a ~5 ms dialogue against a ~1.2 s console
floor), and **undocumented in both the public SDA manual and the lab's own
`HELP`** (§4M.22).

> **THE HONEST NEXT STEP IS NOT ANOTHER OVMX-SIDE FIX.** It is (a) a VMS
> internals documentation source not yet found — the DTJ SCA article paid off
> (§4M.23), so other published DEC material may too; or (b) an operator ruling
> that this is a **gate**. Raised as such in rd.

---

**§4M.11 — a 90-MILLISECOND WINDOW NAMED BY THE PEER ITSELF.**
VAX2's console is never touched by `csbwatch` (which parks VAX1 in SDA), and
VMS prints the whole membership dialogue to OPCOM. Both outcomes are identical
up to and including `proposed addition of node X`:

| | fresh join (`N3A`) | **returning identity** (`N1B`–`N1E`) |
|---|---|---|
| `received membership request` | yes | yes |
| `proposed addition of node X` | yes | **yes, every time** |
| `completed VAXcluster state transition` | **+0.09 s** | **NEVER** |
| what ends it | — | our process exits 115 s later → `lost connection` → `aborted` |

> **Between `proposed addition` and `completed state transition` a real VMS
> cluster does something that takes 90 ms for a fresh identity and never
> finishes for a returning one. WHAT IS THAT STEP?**

> **⛔ The barrier was named here as prime suspect and is REFUTED (§4M.15).** It
> starts at +137 ms, **45 ms after** the console logs `completed`. It is
> downstream of the transition, not a step within it. Its 0/0 count in refusals
> is a consequence. **Do not chase the barrier.**
>
> **⛔ And this window is no longer the frontier** — §4M.16 found the divergence
> ~700 ms EARLIER, before the membership proposal, on the directory connection.
> Read the frontier section above; this section is kept for the OPCOM oracle and
> the anchor technique, which are still the right tools.

**The anchor technique, which is reusable:** align a pcap on the
`proposed addition` OPCOM timestamp — the measured console-vs-host residual is
0 to +4 ms, and the wire goes silent for 460–725 ms before each burst onset, so
the boundary is unambiguous. **Use only the timestamped `Node X (csid …)` OPCOM
lines — the bare `%CNXMAN,` echoes interleave out of order (guardrail 24).**

**The one surviving wire correlate (§4M.9), a symptom not a cause:**

| | `MSCP$DISK` lookup request msgtypes |
|---|---|
| **joins** (6/6) | reach `0x4b` by the 2nd lookup |
| **refusals** (4/4) | **`5b 5b 5b 5b` — never reach the data phase** |

`0x4b` means "the SCS$DIRECTORY connection is up" (`scs_dir.h:33`), so in a
refused rejoin the peer's directory connection to us never comes up — **and
answering it correctly does not bring it up** (`N1E`). A join CAN carry a
leading `0x5b` (`N3A`), so it is the transition that matters, never the presence.

**⛔ Do NOT re-propose the `cat 0x04` ack opcode** (§3 item 14): `N3A` JOINED
with `op 0x04`, `N1D`/`N1E` were REFUSED with `op 0x00`.

**The §4L.9h exclusion test still applies and is still the best filter:**
anything wrong with OVMX generally would break the fresh join too; anything
inherent to the rejoin path would break the real node too.

**Already dead, of exactly that shape — do not re-propose:** the `[22:24]`
incarnation echo (we emit the reference value); OVMX's own rejoin-mode behaviour
(`OVMX_REJOIN_FORM=0` refused as `s1C`); and now the directory-response msgtype
mirror (§3 item 13). Also dead: initiation role, `send_seq` restart, the SCS
envelope, the sequence-context race, SB persistence, the `cm_config_sent` resend
gate, and everything in §3.

## The loop — run this until done

Repeat, without pausing for approval between iterations:

1. **Name ONE hypothesis** in a single falsifiable sentence, and state the
   observation that would kill it.
2. **Check the killed lists first.** §3 and §4L. If it is there, pick another.
3. **Try the cheapest oracle that could refute it, in this order:**
   our own source → our own run logs (`scsd-<tag>.log`) → SDA on the peer →
   a capture. This session refuted four hypotheses in minutes each at the first
   two levels and wrote no code for any of them. A capture agent is the *last*
   resort, not the first.
4. **If it survives, test it with a matched control.** Controls must be
   identity-proven (`identity on the wire:` in the run log) and must **bracket**
   the negative — a fresh-identity control immediately before *and* after.
   Change one variable per run. Ship every wire change with an env kill-switch
   and use it in the same session.
5. **Record the outcome in `docs/HANDOFF-vms-2f3.md` immediately** — kills as
   well as confirmations, in a numbered subsection, and add the kill to §3 or
   the §4L list. Commit and push each finding separately. A killed hypothesis
   recorded is worth as much as a confirmation; six of this session's findings
   were kills.
6. **When you correct yourself, correct in place and say so.** Retitle the
   section, mark the retraction, keep the heading so nobody re-derives it.
7. Go to 1.

When a hypothesis is confirmed: implement behind an env flag, prove it with the
bracketed triple, then make it the default, add the test, push, and verify CI
green by SHA.

## Method rules — non-negotiable

- **Run as an ORCHESTRATOR. Get the ORACLE to answer before you get an AGENT to
  explain.** You read no packet bytes yourself; delegate byte work.
- **Verify an agent's claim against a cheaper oracle before building on it.**
  Three agent claims were corrected this session — one by arithmetic on its own
  numbers, one by our own source, one by the agent's own retraction under a
  demand for raw bytes.
- **Grounding a byte is not grounding a claim.** §4k.9 was written wrong twice
  because a *value* was grounded while its *meaning* was not.
- **Guardrail 22: a peer-side sample taken after our process exits measures our
  corpse, not our attempt.** State whether OVMX was RUNNING at every CSB/CDT
  sample. §4e.3, §4f.3, §4g and §4j all mix the two.
- **Establish which of the two refusal shapes (§4d.6) a run is** before comparing
  it to any other run.
- **Never mix a lab-1 and a lab-2 run inside one comparison** — different SIMH
  binary. Reproduce a contradicting lab-2 result on lab-1 before trusting it.
- **Consistency is not evidence.** Every hypothesis this session was consistent
  with the facts known when written, and seven died.
- **Do not read the CSB sequence counters** (`Next seq. number`,
  `Last seq num rcvd`, `Unacked messages`) as message counts until `vms-da1`
  lands — the SDA↔wire mapping is unestablished and the plain reading is
  contradicted by the wire.

## Environment

- Dev seat is **`workshop`** (x86_64). Repo `~/projects/vms`.
- **The lab is at `/data/training/vax`, NOT `~/vax`.** A symlink makes the 29
  hardcoded paths in 23 lab scripts resolve — do not edit those scripts.
- Over ssh, `export PATH="$HOME/.local/bin:$PATH"` or `rd` is not found.
- Lab volume is ZFS, 40G quota; `rsync --sparse` for disk images.
- `tools/mk_sysgen` is an aarch64 binary with no source — **use
  `tools/mk_sysgen.py`**.
- **Last SCSSYSTEMID used: 1316.** Take the next one and update §6.
- **Pods:** `vaxlab-0` SPENT (console wedged), `vaxlab-1` DEGRADED (its VAX2 was
  SIGKILLed and never rebooted), **`vaxlab-2` healthy** and carrying residual
  state for `OVMXM1/M2/M3` — a ready-made rejoin reproducer. Fresh pods:
  `kubectl -n ovmx-lab scale sts/vaxlab --replicas=N`. One pod = one isolated
  2-node VMScluster, so every pod is a disposable reproducer AND a virgin cluster.
- **Key tools** (`/data/training/vax/cluster/tools/`, documented in §6):
  `csbwatch.sh <pod> <tag> <store> <dur> <identity> [ENV=V…]` — one attempt with
  the peer's CSB for our identity sampled *during* it; this is the instrument
  that cracked §4L. `csbcycle.sh` — real-node kill/reboot CSB cycle.
  `lab2run.sh` — plain lab-2 run. `scacptrace.sh` — high-cadence SCACP + capture.
- **`rd update` takes `--context`, not `--description`.** `rd note` is not a
  subcommand; use `rd progress <id> --notes "…"`.
- **`vms-2f3` can no longer sync** — its event exceeds the relay size limit and
  the write succeeds locally while `rd sync` reports need=0, so it looks clean
  while other machines see a stale item. **`docs/HANDOFF-vms-2f3.md` in git is
  the authoritative record.** Keep rd notes short and point at the doc.
- The repo hook forbids the orchestrator from working inside a git worktree while
  the background-job guard demands one. Already resolved — `.claude/settings.local.json`
  sets `"worktree": {"bgIsolation": "none"}`. Work in the project root.
- **Lab tooling is NOT version controlled** (`vms-f7a`). It exists only on this
  host's ZFS volume, and this has already cost the project `mk_sysgen`'s source.

## Binding constraint — CLAUDE.md Rule 8, clean-room RE

Wire formats and the link/image toolchain come **only** from (a) observing
behaviour on our own lab wire or documented tool output (SDA/SYSGEN/SYSMAN/LINK)
and (b) public OpenVMS documentation. **Never disassemble, decompile or copy
VSI/HPE source or binaries; never paste leaked VMS source.** This is what makes
the interop RE legally protected. You have standing operator authority to operate
the lab without asking.

## Reserved — stop and raise a gate for these only

- A VMS constant value that needs operator sign-off (never self-certify).
- Weakening, skipping or deleting a test.
- Anything irreversible or externally visible.
- Product scope: descoping or deferring the objective.
- A finding that requires changing a documented spec line in
  `docs/cluster-protocol-spec.md` that is currently marked GROUNDED — record the
  evidence, raise `rd gate`, and keep working on everything else.

Everything else is yours. Decide it, state the decision and its reason in one
line, and proceed.
