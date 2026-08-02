# HANDOFF — vms-14f, the executive-residency dispatch

**Originally written 2026-08-01. Rewritten 2026-08-02 after an orchestrated round on the three
frontier branches.** Read §1 first. Everything else is reference §1 sends you to.

---

## 0. What changed since the 2026-08-01 revision — read this before trusting anything you remember

Four statements in the previous revision were **measurably wrong**. They are corrected in place
below; they are listed here because agents carried them.

| Previous revision said | Actually |
|---|---|
| reproduce with `podman build …` | **workshop has `docker`, not `podman`.** Every reproduce command needed translating. |
| `vms-a30` is "not wired into the closure" | **It is wired — it blocks `vms-150`.** So `vms-150` does not unblock even when all three branches merge. |
| the lab is at `~/vax/cluster` | **`/data/training/vax`.** The dev seat moved 2026-08-01; `main` now corrects this in CLAUDE.md (`5215509`). |
| main is at `5e69609` (attested `52f8b86`) | **main has moved to `ce11330`** (attesting `5215509`) — lab-2 k3s reference labs + the CLAUDE.md path fix. Re-derive it; do not quote this line back later. |

Also: `origin` carries several rounds per item and **some are decoys**. `work/vms-ecf-f26-r3` and
`work/vms-cb5-env3` were duplicate refs at the *same SHA* as the round before them, while
`work/vms-fbe-r2` was a genuine unrecorded round that rd knew nothing about. **Establish the current
round by SHA, never by branch name.**

---

## 1. Execution pointer — start here

The objective is **`vms-14f`**: *OVMX runs unmodified VMS software: executive-resident system
facilities, no facades.*

**State (re-derived 2026-08-02, after two merges and triage):** closure = **23 open** by DAG walk
from `vms-14f`, no cycles. Main is at **`eff4fe3`**. It was 17 at the start of the round; eight new
defects were triaged in (§3) and three items closed on merge, netting +6.

**`vms-b33` is UNBLOCKED and `active`.** Its last two blockers, `vms-ecf` and `vms-f26`, closed with
PR #47. **The fifth Phase 2 verdict is the next thing to run.**

**Two of the three frontier branches are merged. One remains.**

| Item | State |
|---|---|
| `vms-fbe` | **MERGED** — PR #46, `c243992`. Item closed. |
| `vms-ecf` + `vms-f26` | **MERGED** — PR #47, `57fc164`. Both items closed. |
| `vms-cb5` (+`vms-f39`, `vms-f42d`) | **PR #48 OPEN**, head `work/vms-cb5-env3-r4` @ `1598ef5` — main merged in, plus the `knock_on_fail` fix for the two scenario-G/OPCOM+ reds under `bind-client-no-register`. Verify CI, then merge. |

**The next thing to run is `vms-b33`'s fifth verdict** — Phase 2, *the executive is wired and
provable, not merely present*. It is `active` and unblocked. Four previous runs returned NO-GO and in
all four **every facility attacked survived**; the blocker was always *the gate the verdict rests on*.
Expect the fifth to look the same, and read §5 before running it.

`vms-150` (Phase 3 veracity) does **not** unblock when #48 merges — `vms-a30`, `vms-95f` and `vms-38c`
also block it. See §3.

---

## 2. What this objective actually delivers, and what it does not

**The epic's title overpromises relative to its closure. Say so out loud rather than discovering it
at the end.**

What the closure delivers is the *substrate*: a VMS executive that really is the executive. Process
table, device table, event flags, identity and privileges, terminal characteristics and the lock
manager live in `vms.ko` and are proven **cross-process** — A writes, B reads — because that is the
only test a per-process fake fails. PID 1 refuses to boot without it; the per-call "executive absent"
fallbacks were **deleted rather than corrected**, so there is no degraded mode to drift back into.

What it does **not** deliver:

- **Running unmodified VMS images.** That is the toolchain and image-activation work — `vms-ade`,
  `vms-913` — a different epic.
- **Remote access.** SSH is excluded from Phase 3. `vms-b8d` exists to *decide* whether it is next; §6.

**And one honest subtraction made on 2026-08-02:** the executive-resident count is now **7, not 10**.
`sys$enq` / `sys$enqw` / `sys$deq` were demoted to `OVMX-PARTIAL` + `OVMX-LOCAL` because their answer
is finished in userspace. That is a *measurement*, not a regression — see §4 — but it means the lock
manager, the precedent this entire ruling rests on, is itself not yet fully executive-resident
(`vms-82a`).

---

## 3. The closure, by layer

Seventeen open items are four layers, not seventeen waves:

| Layer | Items | Note |
|---|---|---|
| **Frontier** | `vms-ecf` `vms-f26` · `vms-cb5` `vms-f39` `vms-f42d` · `vms-fbe` | §4 |
| **Verdict gates** | `vms-b33` (Phase 2), `vms-150` (Phase 3 veracity) | both blocked on the frontier |
| **Human gate** | `vms-b8d` | §6 — **you cannot close this, and neither could I** |
| **Sweeps + parents** | `vms-bfd` `vms-50b` `vms-110` `vms-e0f` `vms-b67`, then `vms-042`, then `vms-14f` | five sweeps fan out in parallel once `vms-042` closes |

**`vms-a30` is the seventeenth and it is inside the closure** — it blocks `vms-150`. The previous
revision said otherwise. `MAIL.EXE` picks whose mailbox to open from `getenv(VMS_USERNAME)`; it is a
live member of the same identity class `vms-cb5` is fixing, deliberately left open there.

### The eight items filed 2026-08-02, and where triage put them

Eight defects were found *in passing* while settling the three branches. They were filed unwired, then
triaged into the graph — **that took the closure 17 → 26; three closures on merge brought it to 23.**

| Item | Pri | Wired to | Rationale |
|---|---|---|---|
| **`vms-95f`** | **p1** | **blocks `vms-150`** | NEW parent — "the harness cannot be trusted to report its own results" |
| ├ `vms-b1f` | p1 | child of `vms-95f` | exit status host-sensitive; **makes `run_facility_negctl.sh` dark on workshop** |
| ├ `vms-c9c` | p2 | child of `vms-95f` | negative-control diagnostic names one cause for a two-cause condition |
| ├ `vms-215` | p2 | child of `vms-95f` | three disagreeing inventories (pre-existing) |
| └ `vms-008` | p3 | child of `vms-95f` | ctest `TIMEOUT 120` vs 3m26s actual |
| `vms-38c` | p1 | blocks `vms-150` | the `OVMX-EXECUTIVE` residual — "is the gate real" is a veracity question |
| `vms-82a` | **p1** | blocks `vms-042` | executive returns private lock status numbers, never `ssdef.h` values |
| `vms-e60` | **p1** | blocks `vms-042` | two UICs for one account, both reachable |
| `vms-2f8` | p2 | blocks `vms-042` | `RIGHTSLIST.DAT` ships and nothing reads it |
| `vms-2d37` | p2 | blocks `vms-898` | `sys$sndopr` writes the OPC block as text — an authenticity tell |

**Why the harness parent blocks `vms-150` and NOT `vms-b33`.** `vms-b33` (Phase 2) rests on
*per-suite* evidence — individual `=== SUITE <name> rc= ===` banners and named assertions — which
these defects do not corrupt. It is the near-term unblock and must not be burdened. `vms-150`
(Phase 3 **veracity**) is precisely the claim that our measurements mean what they say: a veracity
verdict quoting an aggregate would quote a number already known to be wrong, and one citing a
per-facility negative control would cite a driver that cannot run on the dev host at all.

**This is a sequencing decision, not a scope decision, and it is reversible.** Un-wiring `vms-95f`,
`vms-38c`, `vms-82a`, `vms-e60` and `vms-2f8` returns the closure to 17 and ships the epic sooner on
softer evidence. That trade is the operator's to make, not an agent's — `rd dep remove` is the undo.

---

## 4. The three branches

All three had their *named* defect genuinely fixed and independently reproduced from a clean
`git archive`. **Build on them; do not restart from main, and do not force-push.**

### `vms-fbe` — `work/vms-fbe-r3` @ `f4a59d7`, draft PR #46, **35/35 green**

Four rounds. `SET PROCESS/NAME` writes the executive's process table through the same
`vms_kif_setprn` path `$CREPRC` uses; `upname` is sized `VMS_PRCNAM_XFER` (64) so an oversized name
reaches the executive intact and *is* refused, rendered in the oracle's two-line
`%SET-E-NOTSET` / `-SYSTEM-F-<ident>` shape (`src/kernel/vms_ioctl.h:653-658`, VAX V7.3 transcript).
`test_syssvc_setname.c` is a genuine A-writes/B-reads proof across four `DCL.EXE` processes.

**Round 4 is test-only — `src/` untouched. The product was already correct; the tests were wrong.**
That is the most transferable thing on this branch:

- The negative-control assertion was a **crash proxy** matching `%DCL-` / `%SYSTEM-` / `$`. With no
  `/dev/vms`, `DCL.EXE` does not crash — it prints
  `%OVMX-E-SETPRNFAIL, SET PROCESS/NAME could not reach the executive (status %X000002A4)`, 139
  bytes, exit 0. The OVMX-branded shape is *correct*: an absent executive is a condition VMS never
  faces, so inventing a `%SYSTEM-` message would itself be the Rule 10 defect.
- **The old assertion was GREEN on a silent userspace fallback printing a bare `$`** — the exact
  Rule 9 defect that gate exists to catch. *A passing test was concealing its own failure mode.*
- `rc=141` under `bind-client-no-register` was **SIGPIPE inside the suite** (it writes a script into a
  pipe whose reader `DCL.EXE` exits first once the bind is deleted). Fixed with the remedy
  `test_syssvc_showterm.c:392-405` already uses for the identical failure under the identical
  control — precedent, not invention.

Measured: negctl `test_syssvc_setname` **rc=77**, every `test_syssvc_*` at 77 · clean 27 SUITE
banners rc=0, `29 suites passed, 0 failed`, **713 PASS / 0 FAIL** · `bind-client-no-register` red set
118 named == 118 observed, no strays · `proctab-duplicate-name` red set preserved exactly.

Known and named, not hidden: hardcoded continuation strings not derived from `run_print_condition()`
/ `sys$getmsg`; `test_syssvc_setname.c:423` prose still describes the superseded single-line shape;
P6 does not pid-check its row; `SET PROCESS/NAME=""` is a silent no-op and `="OVMXFBE!BAD"` is
accepted — both **observed, not oracle-pinned**.

### `vms-ecf` + `vms-f26` — `work/vms-ecf-f26-r4` @ `21519f5`, draft PR #47, **CI RED**

Four rounds, each refused for the same reason at a cheaper door: **the price of a full
`OVMX-EXECUTIVE` exemption kept being purchasable.**

| round | cost of a full exemption | mechanism |
|---|---|---|
| 1 | **one comment line** | check 4 was `grep -qF "$pname" "$proof"` |
| 2 | one phrase appended to an assertion string | edit the text in suite *and* manifest together |
| 3 | one declaration flip | the service was already `PARTIAL`, so both checks were pre-satisfied |
| 4 | **two edits**, disclosed and filed as `vms-38c` | addition, not deletion |

What landed and is confirmed: the universe is derived from **the build** (`nm` over all 127 compiled
product `.c` files; 88 = 88 against source, zero diff either way); the price is derived in four
code-only hops (call graph → `VMS_IOCTL_*` constant → `case` arm in `vms_module.c` → the handler's
file); **no hop can be made to certify silently** — indirect ioctl constant, in-file dispatch thunk
and function-pointer call each go red. Round 2's three renamed assertion texts are reverted:
`git diff origin/main -- tests/qemu/` is **empty**, and `sys$readef` pays honestly via
`eflag-clref-noop` (24/4) and `eflag-waitfr-eintr-normal` (27/1), both measured in QEMU.

**The design that finally held:** the answer path is split and the halves do opposite things. The
executive half **pays**; the userspace remainder — the TU defining the service — **refutes**, because
a defect that changes an observable public-API answer by mutating code in the calling process is
evidence a remainder exists. Check 6 is a **disqualifier**, not a sixth thing to buy.

**Its first result was a demotion, and the demotion is honest.** `sys$enq`/`sys$enqw`/`sys$deq` →
`PARTIAL`+`LOCAL`. Settled by execution: all three `kstat-*` defects hit one `case` arm in
`kstat_to_ss()`; compiling the product TU pristine vs. injected, `diff -rq` over `src/` shows **one
file differs, zero executive code**, while the caller's status changes **3594→2488, 8484→2488,
8508→2488**. `SS$_DEADLOCK` becoming `SS$_NOTQUEUED` is a different *answer*, not a different
spelling. No `/dev/vms` was needed to prove it — which is precisely the point being claimed. Filed as
**`vms-82a`**.

**THE CI BLOCKER, and it is your gate working correctly:**

```
FAIL: BROKEN SYMBOL SCAN: 126 of 127 product source file(s) compiled.
  src/vmsssh/vmssshd.c:37:10: fatal error: libssh/libssh.h: No such file or directory
  ... so a service could hide behind a build error. Do NOT let a compile failure pass silently.
FAIL: BROKEN BASELINE: the pristine sandbox copy is already RED.
ctest: 8 - userspace_service_register, 9 - userspace_service_register_negctl
```

The build-derived universe is new on this branch, so the gate now hard-fails wherever `libssh-dev` is
absent — the GitHub `Build & Test` job, and workshop. **Do not weaken the refusal**; it is the
"a price that cannot be computed must refuse, not certify" control. Two legal answers: install the
dependency (preferred — `vmssshd.c` is a real product source), or declare the exclusion visibly in
the header as the gate's own message instructs. **Silently skipping, allowlisting, tolerating a short
count, or downgrading to a warning are all the forbidden third answer.**

### `vms-cb5` (+ `vms-f39`, `vms-f42d`) — `work/vms-cb5-env3-r4` @ `7ee3ba4`, verification in flight

Round 2's blocker was the defining failure of this dispatch: **it declared a CLASS settled and the
class was alive** — `F$IDENTIFIER(1000,"NUMBER_TO_NAME")` still returned `"BARON"` out of a
`getpwuid()` fallback in the same file the branch edited, 1840 lines below the comment declaring the
class removed.

Round 3 enumerated the class **by execution** — `nm -uD` over every shipped `.EXE` and shared image,
then `objdump` to name the caller — which is the method that transfers:

| image / symbol | host call | disposition |
|---|---|---|
| `DCL.EXE lex_identifier` | `getpwnam`/`getpwuid` | fixed |
| `LIBVMS$SHR.EXE get_current_username` | `getpwuid` | fixed |
| `LIBVMS$SHR.EXE get_uic` | `getuid`/`getgid` | out of scope (`vms-2b8`) |
| `MAIL.EXE main`/`user_exists`/`get_user_homedir` | `getenv(VMS_USERNAME)` + `getpw*` | **live member, `vms-a30`, deliberately open** |
| `AUTHORIZE.EXE check_privilege` | `getenv("USER")` | out of scope |
| `HELP.EXE` | `getenv` | round 3 said it imported none — **wrong row, corrected in round 4** |

**Round 3 was refused because the shipped miss value was an invention, and the lab settled it in
~15 minutes.** Real OpenVMS VAX 7.3 (live node `vax3`, `/data/training/vax`) returns the **null
string** for `NUMBER_TO_NAME` on any miss — plain, general-identifier-range, UIC-format with bit 31,
and zero. The public HP/VSI DCL Dictionary agrees. OVMX shipped `"[0,1000]"`: Rule 10's illegal third
answer. `NAME_TO_NUMBER` returning `"0"` **matches** and is now provable.

Two traps that generalise: **scenario G ASSERTED the invented format**, and that positive was the
liveness anchor for the adjacent negative — so fixing only the code would have left the negative
vacuous (re-anchor on `F$IDENTIFIER(65540,…)` → `"SYSTEM"`, oracle-confirmed). And the justification
"OVMX has no rights database" is **false**: a populated `RIGHTSLIST.DAT` ships into the product
initramfs and nothing reads it.

The OPCOM blank (`from user  on node OVMX`) **is** reachable in the product — `vms_session_qemu.sh:1082`
pins it for SPAWN, cause `vms-afd` — so "report honestly, invent no default" is the right leg of
Rule 10.

**Round 4 acted on all of that.** `F$IDENTIFIER(1000,"NUMBER_TO_NAME")` now returns the null string;
the anchor moved to `F$IDENTIFIER(65540,…)` → `"SYSTEM"` with non-vacuity *executed* (the new control
`dcl-fident-num2name-bracketed-uic` reddens exactly `test_syssvc_ident` and exactly its 2
assertions); the false rights-database clause was **deleted**; `vms-afd` is named as a tripwire; a
named-process OPCOM positive was added; and the doubled space is no longer pinned — the field is
extracted and compared. `NEGATIVE_CONTROL=1` was **run this time**: `test_syssvc_ident` rc=77, every
`test_syssvc_*` at 77. Positive: 26 SUITE banners rc=0, `28 suites passed, 0 failed`, **739 PASS /
0 FAIL**, `test_syssvc_ident` **82/0**. It also declared two `F$IDENTIFIER` controls whose red sets
were over-broad and undeclared (3 and 2 vs 1 and 1) — it reports both omissions as predating round 4.

**The open risk, and it is the same shape as the defect this round fixed:** round 4 also changed
`DEFAULT` from `13107201` to **`8388736`**, derived as `(0200<<16)|0200` — "VMS's octal UIC read as
decimal". That is a *reconstruction*, and a plausible reconstruction is exactly what `"[0,1000]"`
was. **A VMS constant must be pinned at the lab or escalated, never self-certified.** It is with the
adversary now. The forward mapping was added without the reverse, on the stated grounds that only one
direction was measured — check that the missing direction cannot return something invented.

---

## 5. Method — the part that transfers

The most valuable thing in this document. Each was paid for. The first block is from the original
dispatch; the second was earned on 2026-08-02.

**Two legal answers, never three** (Rule 10). Match VMS, or make the condition unreachable. The
illegal third — a reasonable-looking handler for a condition VMS never faces — is the defect behind
nearly every facade found here. **It always looks like diligence.**

**The decisive test for a facility is A-writes / B-reads** (Rule 11). A per-process fake passes every
single-process test perfectly. That is exactly how the process table, the logical name tables and the
unwired `vms_kif` all survived unnoticed.

**Delete emphatic claims; do not correct them.** Six consecutive rounds shipped a *new* false claim
while correcting the previous one. Every round that deleted ended clean. This covers runtime output —
a sentence OVMX prints is a claim.

**Never write a "cannot"/"only"/"never"/"every" you have not tried to break by execution.** Eight
false emphatics across this dispatch, every one the same shape: **a per-site fix carrying a
class-wide claim.**

**A green result only means something if the mutation would otherwise have changed behaviour.**

**An assertion satisfiable by something other than the behaviour under test is vacuous.** Found 20+
times, including a comment that quoted an assertion string and thereby satisfied the manifest
selftest on the assertion's behalf.

**A cardinal must be derived-and-printed or absent.** Moving a hand-maintained count into a variable
does not fix drift — it makes the drift look authoritative.

**Run `nm` on the built artifact.** A facade is invisible in source review and obvious in the symbol
table. This is also how you enumerate a *class* rather than a list of call sites.

**Verify an agent's claim against a cheaper oracle before building on it.** And *when a control
fails, suspect the harness before the theory.*

### Earned 2026-08-02

**Open the draft PR early — CI is the cheaper oracle, and it is a different one.** Both `vms-fbe`
refusals came from gates nobody had run: the `NEGATIVE_CONTROL=1` build and `bind-client-no-register`.
An implementer *and* an independent adversary both passed the branch locally; CI refused it in
minutes. Rounds 1–3 had verified `proctab-duplicate-name` exactly and never ran either of the others.

**A passing test can conceal the exact failure mode it was written to detect.** `vms-fbe`'s
device-absent assertion was a disjunction that went green on a bare `$` — i.e. on a silent userspace
fallback, the Rule 9 defect it existed to catch.

**Distinguish a container's exit status from a suite's.** `vms-b1f` makes the *container* rc
untrustworthy on this host, so agents are told to judge by `FINAL RESULTS` and SUITE banners — but a
**suite's** `rc=141` is real data (SIGPIPE). *My own instruction to ignore exit codes nearly caused a
real signal to be discarded.* State which rc you mean.

**A gate that prints a confident wrong cause is worse than one that prints none.** `vms-c9c`: the
negative control inferred "a fabricated success" from `rc=1` while its own comment says `rc=1` has at
least two causes — and in that run both fabricated-success assertions had *passed*.

**Under-claiming is a wrong measurement too.** When you add a disqualifier, test it for **over-firing**
as hard as for under-firing. The ENQ/DEQ demotion had to be proven honest by execution before it
could be accepted, precisely because it *reduced* what the register claims.

**Go to the lab before declaring something unpinnable.** `F$IDENTIFIER`'s miss value was filed as an
open operator question; it took ~15 minutes on already-booted nodes to settle empirically. Escalate
what only the operator knows — not what you can measure. (See `empirical-not-gate`.)

**Establish which round is current by SHA.** Duplicate branch refs and unrecorded rounds both
occurred here.

---

## 6. What needs a human

- **`vms-b8d`** — the ruling on whether remote access is buildable faithfully yet. Typed `decision`;
  it escalates rather than deciding. **It unblocks the moment `vms-150` closes.** An agent must not
  self-approve it.
- **`vms-c9e`** — a GitHub branch-protection admin action no agent can take.
- **Merging PRs #46 and #47.** Both are draft by choice; the merge decision is the operator's.
- **Any VMS constant or message value** not settleable at the lab. Pin it to the oracle
  (`/data/training/vax`, or public VSI docs) or escalate. **Never self-certify. Green CI is not
  evidence of VMS correctness.**

---

## 7. Harness arithmetic — read before quoting any number

Reconciled in `vms-215`. **The harness publishes three inventories and none of them agree.**

```
"FINAL RESULTS: 28 suites passed"  ← the headline on main, and it is WRONG
26   actual "=== SUITE <name> rc= ===" lines
25   suites printing an "N passed, M failed" tally
695  actual "  PASS:" assertion lines
666  sum of the 25 per-suite tallies
```

`tests/qemu/init.sh` bumps `TOTAL_PASS` at lines 43 and 56 for the **`vms.ko` and `vmsfs.ko`
module-load checks**, then the suite loop bumps the *same counter* at line 108 and line 123 prints it
as "suites passed". So `28 = 26 suites + 2 module loads`. `test_kmod_bind` prints 49 PASS lines but
**no tally**, so 666 silently omits the suite that proves the open→register wiring the Phase 2
verdict turns on.

**These are main's numbers.** On `work/vms-fbe-r3` the same harness reports 27 SUITE banners /
`29 suites passed` / 713 PASS, because that branch adds `test_syssvc_setname`. Say which tree you mean.

**Reproduce with `docker`, not `podman`:** `docker build -f tests/qemu/Dockerfile -t <your-own-tag> .`
then `docker run --rm <your-own-tag>`. **Use your own image tag** — parallel agents race a shared one.
The negative control is the same build with `--build-arg NEGATIVE_CONTROL=1`.

**`vms-b1f` (p1) — two things you must know before running anything locally:**

1. `run_tests.sh` decides its verdict with `echo "$OUTPUT" | grep -q …` under `set -o pipefail`. When
   more than one pipe buffer (~64 KB) of output follows the `FINAL RESULTS` line, `grep -q` exits
   first, `echo` takes SIGPIPE, and the harness prints **"KERNEL MODULE TESTS FAILED" and exits 1 on
   a run with zero failures.** It is **host-sensitive** — CI on main prints `ALL KERNEL MODULE TESTS
   PASSED` and exits 0. Judge local runs by the `FINAL RESULTS` line and the SUITE banners.
2. **It makes `run_facility_negctl.sh` unusable on workshop entirely.** That driver's first step is a
   pristine positive control; it exits 1 through this bug, so the driver refuses and never injects.
   Two separate agents had to drive `inject_and_run.sh` directly and attribute by hand. **The driver
   is behaving correctly** — refusing to certify when its own control fails is the property
   `vms-ecf` added on purpose. Do not loosen it to work around a harness bug.

Separately: `test_executive_integral.sh`, `test_persistent_boot.sh` and `run_facility_negctl.sh` are
driven by `ci.yml` and `distro/Dockerfile.bootable`, **not** by `run_tests.sh`. "The harness" names
two different inventories; say which one you mean.

**Environment:** workshop is x86_64 (the project's primary arch, Rule 5, finally native). `libssh-dev`
is **absent**, so `vmssshd` is not configured — local ctest is 39/41 on main, and the `vms-ecf` gate
honestly refuses there (§4). **Disk runs tight** (it hit 98% during this session). Build one image at
a time, `docker rmi` immediately, and prune *dangling* layers only — the large tagged images belong to
other projects.

---

## 8. Honest accounting

The closure was 17 open on 2026-08-01. After a full round on all three frontier branches — **two
merged (#46, #47), one open (#48)** — it is **23 open**: eight new defects were found in passing and
nine items triaged into the graph (§3), against three items closed on merge.

**The count went the wrong way and that is the honest outcome, not a failure.** Every one of the
three branches passed local verification and was then refused by something nobody had run: two CI
gates, an adversary's lab measurement, and the register's own price. **The work of this round was
finding out that the measurements were softer than they looked.**

That is the same pattern the previous revision recorded and it should still not be read as stalling:
**measuring properly finds more than it closes.** What did move is the quality of the measurement.
`vms-ecf` went from a price payable in a comment to one payable in two edits *and said so in its own
gate text rather than claiming closure*; the executive-resident count dropped 10 → 7 because three
services were demoted on evidence. **A smaller number that is true is the deliverable here.**

Every NO-GO in this dispatch has been narrower than the one before it. `vms-b33` has returned four,
and in all four **every facility attacked survived** — the blocker was always *the gate the verdict
rests on*. The 2026-08-02 round did not change that shape: of the four refusals, three were gates
(negative control, attribution, the register's own price) and one was a value refuted by the lab.
Expect the fifth verdict to look the same.
