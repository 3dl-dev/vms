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

## Boot sequence, in order

1. `cd ~/projects/vms`, branch `worktree-760-active-directory`. Re-derive the
   SHA; do not trust any SHA written down anywhere.
2. **Read `docs/HANDOFF-vms-2f3.md` §4L first** — it is the newest and the
   sharpest. Then §4k, then §4j. Then **§3's killed list (12 entries) and §4L's
   seven more.** Then §7 guardrails (22 entries) — they are the most transferable
   part of the whole document.
3. Read `docs/HANDOFF-vms-760.md` §0 for the orchestrator doctrine. It still
   applies verbatim.
4. `rd show vms-2f3`. Also open: `vms-da1` (SDA↔wire counter mapping — blocking
   the interpretation of several CSB fields), `vms-950` (real ack bug, NOT the
   gate), `vms-f7a` (lab tooling not version controlled).

## Where the investigation actually stands

**The bug is reduced to one flag word.** During a rejoin the peer allocates the
same new CSB and the same CDT it allocates for a successful join, and then never
sets `vcc`/`status_rcvd` and never advances:

| at T+5 s | admitted | **refused** |
|---|---|---|
| CSB flags | `02040000 status_rcvd,vcc` | **`00000000`** |

Against a matched real-node control the peer builds **structurally identical**
state for a returning real VAX and a returning OVMX identity — SB persists, CSB
freed and reallocated, CDT allocated, PDT unchanged. **Only population differs.**

**The question, posed sharply (§4L.9h):**

| peer + | fresh join | rejoin |
|---|---|---|
| **OVMX** | `DISC-REQ` runs → admitted | **no `DISC-REQ` → refused** |
| **real node** | runs → admitted | runs → admitted |

> What differs for OVMX-on-a-rejoin that does **not** differ for
> OVMX-on-a-fresh-join and **not** for a real-node-on-a-rejoin?

That excludes a large class of answers: anything wrong with OVMX generally would
break the fresh join too; anything inherent to the rejoin path would break the
real node too.

**Already dead, of exactly that shape — do not re-propose:** the `[22:24]`
incarnation echo (we emit the reference value), and OVMX's own rejoin-mode
behaviour (`OVMX_REJOIN_FORM=0` forces the first-join form and was refused as
`s1C`). Also dead: initiation role, `send_seq` restart, the SCS envelope, the
sequence-context race, SB persistence, the `cm_config_sent` resend gate, and
everything in §3.

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
- **Last SCSSYSTEMID used: 1307.** Take the next one and update §6.
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
