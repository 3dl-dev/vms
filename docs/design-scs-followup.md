# SCS follow-up: what vms-187 built, what it did not, and what comes next

**Status:** proposal, 2026-08-05. Written at the close of `vms-187` (the SCA layer epic) and
after the research pass that resolved `vms-c35`, reframed `vms-abd` and `vms-da1`, refuted
`vms-7e7`'s fourth rule, and put the MSCP numerics on a documented footing.

This is a **decomposition source**, not a plan of record. The plan of record is the rd tree
built from it.

---

## 1. Where the code actually is

`vms-187` replaced "OVMX replays byte-exact captured wire shapes" with an implementation of the
SCA architecture: configuration queue (SB/PB/PDT), CDT/CDL/CONID, VC formation FSM, connection
FSM, credit and Credit Wait, departure/rejoin teardown, DFREEQ, SDIR, the five services,
symmetric disconnect, process poller. It then merged with the cluster-manager layer and **joins a
real VMScluster** — lab-2 `vaxlab-4`, `cm190 rx=575`, `XITDONE=1`, bracketed against a control.

The reachability ledger (built from `nm --defined-only` plus a call-graph BFS from `main`,
following address-taken function pointers) says: **all 19 `vmsscs` libraries are linked; 371 of
433 text symbols are reachable from `main`.** Every in-tree comment claiming a library is "not
linked into scsd_exe" is false and is being corrected.

So the architecture is not shelfware. But two things are genuinely not wired, and they are the
same shape:

| Gap | Evidence |
|---|---|
| **No message or datagram is routed through the CDL** | `scs_cdl_deliver_message`, `scs_cdl_deliver_datagram`, `scs_cdl_resolve` — all dead, honestly declared |
| **Flow control has no live entry point** | credit accounting is linked and its teardown path runs, but nothing debits or piggybacks on a real send |
| `scs_reject` has no production caller | matches its header table; REJECT is architected but never issued |

**Read that as one fact:** the connection layer is live for *lifecycle* — form, open, tear down,
notify — and dead for *data*. Credit and DFREEQ are therefore accounting for traffic that never
flows through them. That is the honest headline, and it is the largest single piece of remaining
work.

---

## 2. The objective is still unanswered — and is now testable for the first time

`vms-2f3` asks: can a returning OVMX identity rejoin a cluster it was removed from?

`vms-70e2` never got to ask. Its **positive control failed** — the closure branch could not
complete a *first* join, because the cluster-manager layer lived on an unmerged branch. `vms-578`
fixed that and proved a first join. **Nobody has re-run the rejoin triple since.**

This is the cheapest high-value item on the list and it should be first. The tree that now joins
also contains the p.2-21 REFRESH path that `vms-17f` drove end-to-end on real wire — a path that
was *structurally unreachable* when the epic began. Whether that is sufficient for readmission is
an open, testable question, not a hypothesis to argue about.

**Constraint carried forward:** 21 hypotheses are dead in `HANDOFF-vms-2f3.md` §3/§4M, three of
them whole classes killed with matched controls, and the bug reproduces on a virgin cluster. A
negative result reported cleanly is worth more than a positive one that cannot be decoded.

---

## 3. What the research pass changed

- **`vms-c35` resolved.** p.2-12: the round-1 106-byte frame is a STACK; the 46-byte frame is the
  ACK. Spec §4(g) and `scs_vc.c` now agree.
- **`vms-abd` reframed.** ch.2 documents *no* precondition on DISCONNECT_REQ, so the
  missing-precondition theory is dead. It is a peer **state mismatch**; ch.7's CSB states are the
  ladder. DISCONNECT_RSP does exist in the corpus, so OVMX should expect one.
- **`vms-7e7`'s fourth rule refuted.** p.4-68 requires datagrams to carry credit 0; both `0x4b13`
  and `0x5b13` carry mostly-nonzero credit over 930k real-VAX frames, so that pair is not the
  message/datagram split. **Anything that classified datagrams on that basis must be re-derived.**
- **`vms-da1` reframed.** The four SDA CSB sequence fields are NISCA/VC state in the PPD layer
  (p.2-55), not SCS — which is why no SCS field ever matched.
- **MSCP numerics are documented**, not inferred: AA-L619A-TK confirms the capture-derived
  constants. Decoding MSCP drops from "reverse-engineer" to "transcribe from a public spec".
- **Types 8/9 weakened as the special-credit candidate**: constant credit 1 across 855 frames,
  and a Pending Receive Credit must vary. Note the corroborating half — types 5/7 carrying credit
  0 in 5,257/5,257 — is *consistent but not discriminating*; credit-0 is necessary for a
  non-credit-extending class, not sufficient.

---

## 4. Method debts this epic incurred

These are not optional cleanup; each one produced a wrong recorded fact at least once.

1. **Lab hygiene.** Six lab-2 captures were deposited into the lab-1 grounding library, silently
   mixing two labs whose simh binaries differ (aarch64 vs x86_64, subset type vocabulary). The
   corpus needs a **manifest the tools read**, not a glob.
2. **Census discipline.** `vms-c11`: every census filtered on SCA lengths `{62,66,110}` and was
   blind to the 58-byte class, which is how the spec came to claim message types 5 and 7 do not
   exist when 958 are in the library. The mirror error also happened — over-generalising `[46:48]`
   across a class that does not share the envelope. Tools should refuse a class-restricted census
   without an explicit opt-in.
3. **Evidence provenance.** A census counted OVMX's own transmitted frames as evidence about a
   real VAX. The fix (split by source with a rule that *reds* on an unknown source) exists in
   `tools/scs_connect_data_measure.py` and should be the house pattern.
4. **Gates must gate.** Figures gates string-match prose against a checked-in table; four of them
   stayed green while the tools they cite failed. A gate that cannot fail is worse than none.

---

## 5. The executive question, stated once

None of this touches `/dev/vms`. The whole SCS layer is a userspace daemon. Under CLAUDE.md
Rule 9 the kernel/QEMU path is the only runtime and an executive facility is not done until a
test exercises it against a real `/dev/vms`. SCS is not currently claimed to be an executive
facility — but if cluster membership is ever to be visible to VMS software running *inside* OVMX
(`SHOW CLUSTER`, `$GETSYI`, the lock manager's cluster-wide behaviour), that boundary has to be
crossed deliberately rather than by accident. It is out of scope below; it is named so it is not
forgotten.

---

## 6. Proposed shape of the follow-up

Five tracks, in dependency order. Track A is small and should go first because it answers the
question the whole programme exists for.

- **A — Ask the rejoin question.** Re-run the bracketed triple on the tree that now joins.
  Decode or report negative. Nothing else depends on it, and everything else is more valuable
  once it is answered.
- **B — Make the data path live.** Route messages and datagrams through the CDL; give flow
  control a real entry point so credit and DFREEQ account for actual traffic; decide whether
  REJECT should ever be issued. This is where the architecture stops being lifecycle-only.
- **C — Finish the wire decode.** Type 10 (2,889 frames) with the MSCP spec now in hand; types
  8/9 with the constant-1 lead; the `vms-abd` state-mismatch ladder against ch.7 CSB states.
- **D — Server posture.** Whether OVMX ever serves a disk. Answering it un-defers block data
  transfer and makes MSCP$DISK registration honest; answering it "no, permanently" lets several
  parked items close.
- **E — Method debts.** Capture manifest, census guards, gate integrity. Cheap, and each one has
  already cost a wrong fact.

Roughly twenty parked findings exist from the epic and should be triaged into these tracks rather
than carried as a separate list.
