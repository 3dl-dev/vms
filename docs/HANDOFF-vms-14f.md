# HANDOFF — vms-14f, the executive-residency dispatch

**Written 2026-08-01, at the end of a long `/swarm-dispatch vms-14f` run.**
Read §1 first. Everything else is reference you can reach for when §1 sends you there.

---

## 1. Execution pointer — start here

The objective is **`vms-14f`**: *OVMX runs unmodified VMS software: executive-resident system
facilities, no facades.*

**State:** closure = 44 items, **27 terminal, 17 open**. Main is at `5e69609` (attested `52f8b86`).

**The frontier is three pushed branches, none merged, each with a named blocker.** Start by running
`rd show` on each of `vms-ecf`, `vms-cb5`, `vms-fbe` — the blockers are written there as settling
commands you can run, not as prose. §4 summarises them.

**Do not start new work until those three merge.** They are the only things standing between the
current state and `vms-b33`'s fifth verdict, which is the item the rest of the tree hangs off.

---

## 2. What this objective actually delivers, and what it does not

**The epic's title overpromises relative to its closure. Say so out loud rather than discovering it
at the end.**

What the closure delivers is the *substrate*: a VMS executive that really is the executive. Process
table, device table, event flags, identity and privileges, terminal characteristics and the lock
manager all live in `vms.ko` and are proven **cross-process** — A writes, B reads — because that is
the only test a per-process fake fails. PID 1 refuses to boot without it; the per-call
"executive absent" fallbacks were **deleted rather than corrected**, so there is no degraded mode to
drift back into.

What it does **not** deliver:

- **Running unmodified VMS images.** That is the toolchain and image-activation work — `vms-ade`,
  `vms-913` — a different epic entirely.
- **Remote access.** SSH is explicitly excluded from Phase 3. `vms-b8d` exists to *decide* whether
  it is next; see §6.

---

## 3. The closure, by layer

Seventeen open items are not seventeen waves. They are four layers:

| Layer | Items | Note |
|---|---|---|
| **Frontier — three pushed branches** | `vms-ecf` `vms-f26` · `vms-cb5` `vms-f39` `vms-f42d` · `vms-fbe` | §4 |
| **Verdict gates** | `vms-b33` (Phase 2), `vms-150` (Phase 3 veracity) | Both blocked on the frontier |
| **Human gate** | `vms-b8d` | §6 — **you cannot close this one, and neither could I** |
| **Sweeps + parents** | `vms-bfd` `vms-50b` `vms-110` `vms-e0f` `vms-b67`, then `vms-042`, then `vms-14f` | Five sweeps fan out in parallel once `vms-042` closes |

Also open, filed by this dispatch but **not wired into the closure** (deliberately — they are real
defects that are not blockers): `vms-a30` (MAIL reads `VMS_USERNAME`), `vms-ee4` (census alias
escape), `vms-215` (harness arithmetic), `vms-012` (SET UIC writes nothing reads), `vms-484`
(SYSUAF advertises accounts that fail closed).

---

## 4. The three open branches

All three exist on the remote. All three had their *named* defect genuinely fixed and independently
reproduced from a clean `git archive`. All three were held back by something the adversary found
afterwards. **Build on them; do not restart from main, and do not force-push.**

### `work/vms-ecf-f26` @ `5acb129`

**Landed and confirmed — preserve it:** the service register's universe is now derived from the
**build**, not source text. The gate compiles all 127 product `.c` files and reads exported `sys$*`
symbols with `nm`. On main the symbol set and the source set are **equal, 88 = 88, zero diff either
way**, so nothing was papered over; the asm-alias rename now goes red. Controls 34 → 39, coverage
21 → 24. The false header sentence is deleted, not corrected.

Also worth keeping: the round caught its own mistake — it assumed unknown `-Wno-*` flags are ignored,
measured otherwise (gcc rejects one at `cc1`, **all 127 files failed to compile**), and the gate
**refused to certify rather than passing on an empty symbol set.** "A price that cannot be computed
must refuse" is now a control.

**Blocker:** the `OVMX-EXECUTIVE` price is still purchasable — one line dearer. Appending a phrase
that names a service to an existing assertion string, in both the suite and the manifest, makes the
gate print it as a paid claim. **That is the same instrument the round used to make `sys$readef`
pay.** If editing an assertion's *words* is how a service becomes covered, coverage is a naming
convention and not a measurement. The fix direction is in the item: derive the price from **which
code a proven-reddening defect actually mutates** — data `facility_defects.sh` already carries.

### `work/vms-cb5-env2` @ `82dfaa7`

**Landed and confirmed — preserve it:** both false emphatics deleted, with the replacement comment
now saying the *opposite*, sourced, and reconciled against the in-tree UAT assertion that had
contradicted it. The `"SYSTEM"` fallback is gone at all five remaining DCL sites with **no default
invented**. Scenario G in `test_syssvc_ident.c` (41 → 64) is a real cross-process proof: a session
stamps an identity through the executive, drops its Linux credentials, forks a subprocess that
registers on its own, and execs DCL with a **poisoned `VMS_USERNAME=SYSTEM` environment** — every
assertion made by a third process that is neither.

**Blocker:** the round declared a *class* settled and the class is alive.

```
X = F$IDENTIFIER(1000,"NUMBER_TO_NAME")   →   X = "BARON"
LOGOUT  →  %%OPCOM, ... request 1 from user baron on node OVMX
```

The first is the host Linux login name out of a `getpwuid()` fallback at `dcl_lexical.c:2471` — **in
the same file the branch edited, 1840 lines below the comment declaring the class removed.** The
second is `sys_operator.c:85-93` and falsifies a comment the round itself added.

**Enumerate the class by execution against the built binary before touching anything.** The class is
`get_current_username()`, the `F$IDENTIFIER` conversions and `sys_operator.c` — not a list of call
sites.

### `work/vms-fbe` @ `80691cf`

**Landed and confirmed — preserve it:** `SET PROCESS/NAME` now writes the executive's process table
through the same `vms_kif_setprn` path `$CREPRC` uses; `ctx->process_name` updates on success only.
`tests/qemu/test_syssvc_setname.c` is a genuine A-writes/B-reads proof across four separate
`DCL.EXE` processes, and a clean revert reddens it. The `facility_defects.sh` wiring was verified
against the real kernel DUPLNAM mutation with **exact red-set equality** — preserve that.

**Blocker:** a 16-character name. The executive correctly refuses it with `SS$_IVLOGNAM`; **DCL
silently succeeds, the row carries a truncated name, and nothing is printed.** The oracle is already
in this repo — `src/kernel/vms_ioctl.h:653-658` carries the VAX V7.3 transcript showing
`%SET-E-NOTSET` / `-SYSTEM-F-IVLOGNAM` and the old name unchanged. Root cause: `upname` is sized
`sizeof(ctx->process_name)` (16) instead of `VMS_PRCNAM_XFER` (64), so the name is truncated **before
it reaches the executive** — the exact defect that constant exists to prevent.

---

## 5. Method — the part that transfers

This is the most valuable thing in this document. These are not preferences; each was paid for.

**Two legal answers, never three** (Rule 10). Match VMS, or make the condition unreachable. The
illegal third answer — a reasonable-looking handler for a condition VMS never faces — is the defect
behind nearly every facade found here. It always looks like diligence.

**The decisive test for a facility is A-writes / B-reads** (Rule 11). A per-process fake passes every
single-process test perfectly. That is exactly how the process table, the logical name tables and the
unwired `vms_kif` all survived unnoticed.

**Delete emphatic claims; do not correct them.** Six consecutive rounds shipped a *new* false claim
while correcting the previous one. Every round that deleted ended clean. This covers runtime output
too — a sentence OVMX prints is a claim.

**Never write a "cannot" / "only" / "never" / "every" you have not tried to break by execution.**
Eight false emphatics were found across this dispatch. Every one had the same shape: **a per-site fix
carrying a class-wide claim.**

**A green result only means something if the mutation would otherwise have changed behaviour.** One
gate deleted a `$DEQ` write-back, got 26/26 green, and *refused to report it* — because the deleted
code was plausibly redundant and the mutation probably had no observable effect. Reporting "nothing
went red" would have been a false coverage claim dressed as a finding.

**An assertion satisfiable by something other than the behaviour under test is vacuous.** Found 20+
times, including a comment that quoted an assertion string and thereby satisfied the manifest
selftest *on the assertion's behalf*.

**A cardinal must be derived-and-printed or absent.** Moving a hand-maintained count from prose into
a variable does not fix drift — it makes the drift look authoritative.

**Run `nm` on the built artifact.** Two facades in one wave (`vms-fbe`, and the asm-alias universe
escape) were found by asking which symbols the binary actually imports or exports. **A facade is
invisible in source review and obvious in the symbol table.**

**Verify an agent's claim against a cheaper oracle before building on it.** Also: *when a control
fails, suspect the harness before the theory.*

---

## 6. What needs a human

- **`vms-b8d`** — the ruling on whether remote access is buildable faithfully yet. Typed `decision`;
  it escalates rather than deciding. **It unblocks the moment `vms-150` closes.** An agent must not
  self-approve it.
- **`vms-c9e`** — a GitHub branch-protection admin action no agent can take.
- **Any VMS constant or message value.** Pin it to the oracle (`~/vax/cluster`, or public VSI docs)
  or escalate. **Never self-certify. Green CI is not evidence of VMS correctness.**

---

## 7. Harness arithmetic — read before quoting any number

Reconciled in `vms-215`. **The harness publishes three inventories and none of them agree.**

```
"FINAL RESULTS: 28 suites passed"  ← the headline, and it is WRONG
26   actual "=== SUITE <name> rc= ===" lines
25   suites printing an "N passed, M failed" tally
695  actual "  PASS:" assertion lines
666  sum of the 25 per-suite tallies
```

`tests/qemu/init.sh` increments the suite counter at lines 43 and 56 for the **`vms.ko` and
`vmsfs.ko` module-load checks**, before the suite loop begins. So `28 = 26 suites + 2 module loads`.
And `test_kmod_bind` prints 49 PASS lines but **no tally**, so the 666 total silently omits the suite
that proves the automatic open→register wiring the Phase 2 verdict turns on.

Reproduce the baseline with `podman build -f tests/qemu/Dockerfile -t <your-own-tag> .` then
`podman run --rm <your-own-tag>`. **Use your own image tag** — parallel agents race a shared one.

Separately: `test_executive_integral.sh`, `test_persistent_boot.sh` and `run_facility_negctl.sh` are
driven by `ci.yml` and `distro/Dockerfile.bootable`, **not** by `run_tests.sh`. "The harness" names
two different inventories; say which one you mean.

---

## 8. Honest accounting

The closure grew from 38 items to 44 during this dispatch while 27 closed. **The count did not move
much, because measuring properly found more than it closed.** That is the correct outcome for an
authenticity objective and it should not be read as stalling — but it does mean the remaining work is
bounded by how much is still unmeasured, not by how much is left on the original list.

Every NO-GO in this dispatch was narrower than the one before it. `vms-b33` has now returned four,
and in all four **every facility attacked survived** — the blocker was always *the gate the verdict
rests on*. That is the pattern to expect on the fifth.
