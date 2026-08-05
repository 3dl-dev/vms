# HANDOFF — vms-14f, the executive-residency dispatch

**Revised 2026-08-05 (round 10), after the round that finished the last wrong-answer defect and
found the data file underneath it was an invention.** Read §0-R10 first, then §1.

Every number below was re-derived on 2026-08-05. **Re-derive them again.**

---

## 0-R10. What changed in round 10

Round 10 worked exactly one item, `vms-2f8`, because it turned out to be two.

| The 2026-08-04/round-9 revision said | Actually |
|---|---|
| closure = **30 open** | **28 open.** `vms-2f8` closed; `vms-e60`/`vms-82a` had already closed. |
| `vms-2f8` is "F$IDENTIFIER's data source … not a wrong-answer defect" | **Half right, and the wrong half was the dangerous one.** The SOURCE question was real and is now answered. But the file it was going to be pointed at — the shipped `RIGHTSLIST.DAT` — **was an invention**, and wiring the reader to it unchanged would have shipped six wrong answers *while looking like it had started reading a real facility*. |
| local ctest **64 tests**, 1 expected failure | **66 tests, 2 failures.** `test_libvms_protection` (`vms-2a1`, the documented apparmor one) and **`rd_citations_fresh`, which is RED ON MAIN and was before this round** — filed as `vms-344`, left exactly as found. |
| disk **2.7 GB free, 99%** | **4.5 GB free, 98%.** Slightly better. Still do not prune `vat-env-*`. |
| no PR open | **PR #79 merged**, `e68fc4d`, 35/35 CI green by SHA `4fe0deb`. |

### What `vms-2f8` actually was

`F$IDENTIFIER` now READS the rights database (`src/libvms/rtl/rightslist.c`): general identifiers
from `SYS$SYSTEM:RIGHTSLIST.DAT`, UIC identifiers **derived** from SYSUAF rather than duplicated,
because two files free to disagree about one account's UIC is the defect `vms-e60` closed.

**The shipped file carried `INTERACTIVE:1 BATCH:2 NETWORK:3 LOCAL:4 REMOTE:5`, all attributed
`RESOURCE`. Asked of the oracle: none of 1–5 is an identifier on real VMS — every one answers the
null string.** The real environmental identifiers are `%X80000001`–`%X80000006` assigned
*alphabetically*; `DIALUP` was missing entirely; the Attributes column is empty. Corrected in the
same commit that made anything read it. Transcript: `docs/oracle/vax73-rights-database.md` (new).

The `8388736 -> "DEFAULT"` reverse mapping round 9 deliberately declined to add on the strength of
symmetry has now been **asked** and answers `DEFAULT`. Not inferred — read out of the database.

### The three things round 10 found that outlive it

1. **`vms-79f` — the VMS-native shareable recipes keep a hand-maintained TU list parallel to
   `CMakeLists.txt`, and nothing checks they agree.** Adding `rtl/rightslist.c` to CMake and not to
   `src/vmslink/mk_libvms_shr.sh` left **every local signal green** — clean build, 64/66 ctest,
   every host test of the new code passing — and failed only in CI's arm64 VMS-native DCL link job,
   5m19s in, on an undefined symbol. `mk_vmsrms_shr.sh` has the same shape. The comment claiming
   the lists were equal was itself the thing that was wrong.
2. **`vms-c71` — `SHOW SYMBOL` renders integers as 64-bit**: negatives sign-extend in Hex/Octal, and
   the Octal field is 12 digits where VMS prints 11 (wrong for *positive* values too, today). The
   six general identifiers are the first negative values `F$IDENTIFIER` can return, which is why
   this surfaced now.
3. **`vms-344` — `rd_citations_fresh` is red on `main`.** `sys_lock.c`'s three `OVMX-EXECUTIVE`
   declarations cite `vms-82a`, which round 9 closed. **Regenerating the ledger does NOT fix it** —
   that fixes only the freshness half; the tree-wide rescan fails independently. And the underlying
   tension is general: an `OVMX-EXECUTIVE` declaration is a claim a facility IS executive-resident,
   yet the gate requires it to cite an item, and every item eventually closes. **Every successful
   executive-residency fix arms this same red the moment its item closes.** `vms-82a` is the first
   to fire, not the last.

### Method, round 10

**THE MEASUREMENT THAT LOOKED LIKE A FINDING, THREE TIMES.** Establishing whether new assertions
would enlarge a control's red set meant applying each `dcl-fident-*` mutation and probing. The first
three attempts all reported "no effect" — a clean, believable result. All three were **void**: first
the subcommand was wrong (`inject`, not `apply`), then the src-root was wrong (`.`, not `src`, since
`targets` is a path relative to it). The landed-check that should have caught it diffed against git
`HEAD`, which was already dirty with the round's own edits, so it reported "landed" every time.
*Prove which binary ran — the md5 of the artifact, not the exit status of the tool.*

**A FIRST GUESS AT WHICH ASSERTION CATCHES WHICH MUTATION WAS WRONG, IN THE COMFORTING DIRECTION.**
The new test's header claimed the `1..5` miss checks would catch a reader wired to the uncorrected
file. Measured: they do not — the old file's bare-decimal values do not parse at all, so the
*positive* checks fail instead. A second mutation (old numbering in the new notation) is what the
misses actually catch. Both populations are in the test because neither mutation is caught by both.

**AN ASSERTION YOU CANNOT MEASURE WHERE IT RUNS DOES NOT GO THERE.** The discriminating `4 -> ""`
check would have enlarged two declared red sets, and its behaviour under one control is
host-sensitive (`getpwuid(4)` is `SYNC` on this host; the guest stages a different passwd). It was
kept out of the QEMU suite and lives in the host test where its negative control is real and
complete. Declaring a red-set entry measurable only on the host, for a gate that runs in QEMU, is
the reasoning the manifest's own rulings forbid.

---

## 0. What changed in round 9

| The 2026-08-04 revision said | Actually |
|---|---|
| "**Do this first: merge #74**" | **Done — merged by the operator** at 20:59:44Z as `a923eff`, 42 seconds into this round's first command. Pre-merge check still stands: the diff was confined to `tests/qemu/*` + `ci.yml`, 1832 insertions / 3 deletions, no census reversion. |
| closure = **34 open** | **30 open.** `vms-b2e` and `vms-a30` fixed and closed; `vms-4c2` filed and wired. |
| four real VMS defects remain | **Two.** `vms-b2e` and `vms-a30` are fixed (PR #76, squash `9b72ba4`). `vms-82a` and `vms-e60` remain, plus `vms-2f8`. |
| `vms-d894` is the scope call you inherit | **Raised as an `rd` gate (`scope`) with a recommendation, not decided.** It was NOT closed by assertion. §3. |
| local ctest **63 tests** | **64 tests**, 1 expected failure (`vms-2a1`). #74 added one. |
| disk **3.4 GB free, 98%** | **2.7 GB free, 99%.** Tighter than briefed. Still do not prune `vat-env-*`. |
| PR #75 is open | **Merged** (`75f49dc`). PR #71, recorded here as "closed as refuted", was **fixed and merged** (`5dde28c`). |

### Then round 9 continued, and finished the defects

| | |
|---|---|
| "**two** real VMS defects remain" | **Zero.** `vms-e60` and `vms-82a` are fixed in the same PR as this revision. `vms-2f8` (F$IDENTIFIER's data source) and `vms-4c2` (AUTHORIZE's positive control) remain, but neither is a wrong-answer defect. |
| `vms-e60` "the site set is nine sites in four files" | Confirmed and fixed, **plus two decimal literals** the enumeration missed: `DEFAULT_UIC_GROUP` and `next_uic_member()`'s floor. |
| `vms-82a` "real executive work, not cheap" | Correct, and the expensive part was not the kernel change. It was that `kstat_to_ss()` carries **three defect-manifest controls**; deleting it naively would have meant deleting three defects, their record rows and lowering the floor — **the exact action the open `vms-d894` gate is about**. The controls were **repointed at the executive** instead: 42 defects, floor 42, every record row still valid, gate not prejudged. |

---

## 1. Execution pointer — start here

The objective is **`vms-14f`**: *OVMX runs unmodified VMS software: executive-resident system
facilities, no facades.*

**State (re-derived 2026-08-05, end of round 10):** closure = **28 open** by DAG walk from
`vms-14f`; **16 of them unblocked**. Main is at **`e68fc4d`**. No PR is open.

> **Round 10 note on §1's "go here next" list below: both of its entries are DONE.** `vms-e60` and
> `vms-82a` closed in round 9; `vms-2f8` closed in round 10. **There is no wrong-answer VMS defect
> left unblocked in this closure.** What remains unblocked is `vms-4c2` (AUTHORIZE's positive
> control — needs a human first, §6) and fifteen machinery items. The ratio the last two revisions
> kept flagging is now the whole picture, and §6's "whether to keep paying for the machinery" is
> the live question rather than a background one.

**`vms-b33` is still blocked by exactly one item: `vms-d894`**, and `vms-d894` is now sitting on a
human as an `rd` **scope gate**, not on you. Do not close it by asserting its criterion is met — it
is measurably not met (§3). Everything else in the closure is workable without it.

**Do this first: `rd gates`.** If `vms-d894` has been answered, act on the answer. If it has not,
**do not wait** — go to the defects below.

### The honest shape of what remains

| Category | Count | Note |
|---|---|---|
| Gate / harness machinery | **19** | Whether the measurement proves what it claims |
| Phase containers & rulings | 6 | `vms-14f`, `vms-b33`, `vms-042`, `vms-150`, `vms-cb5`, `vms-b8d` |
| End-of-epic sweeps | 5 | Cannot start until the phases close |
| **Real VMS defects** | **2** (+2) | `vms-82a`, `vms-e60`; plus `vms-2f8` and `vms-4c2` |

**Nineteen of thirty remaining items are still machinery that measures the work rather than the
work.** That was the last revision's headline and it survived a round in which four items closed,
because the round deliberately spent itself on the defects instead. Anyone picking this up is still
inheriting a verification program with a VMS program inside it.

### Go here next, in this order

**1. `vms-e60` — the question is already answered; the work is mechanical.** SYSUAF.DAT's UIC fields
are **OCTAL**, derived (not picked) from an oracle value that is already pinned *and already
asserted in-tree* at `tests/qemu/test_syssvc_ident.c:1039`: `F$IDENTIFIER("DEFAULT")` = `8388736` =
`%X00800080` = `[200,200]` octal. The file literally contains `200|200`. Octal reproduces the
oracle; decimal (`13107400`) matches nothing. SYSTEM's `1|4` reads the same in both bases — that is
the coincidence that hid this.

**The site set is NINE sites in FOUR files, not the three the item names**, and they must move in one
commit or the fix recreates the same defect with different endpoints. The two the item misses are
`src/libvms/syssvc/sys_uai.c:137,141` and — the worst — `src/ovmx_init/ovmx_init.c:1061,1062`, where
**PID 1 parses SYSTEM's UIC and stamps it into the executive** via `vms_kif_setident()`. Full
enumeration, including the write path (`vms_authorize.c:203`, `%u`→`%o`), the two display sites and
the two `/UIC=[g,m]` input sites, is in the item's notes. The fixing commit must carry a
**discriminating** test — assert DEFAULT resolves to `8388736`; SYSTEM passes under both bases and
proves nothing.

**2. `vms-82a` — real executive work, not cheap.** `vms.ko` uses a private numbering (100/108/116)
and never produces an `ssdef.h` value; `kstat_to_ss()` manufactures the VMS-visible status in the
calling process, so `SS$_DEADLOCK` vs `SS$_NOTQUEUED` — a *different answer*, not a different
spelling — is decided in userspace. This is the facility the whole executive ruling turns on. Note
the binding side condition in the item: the `OVMX-PARTIAL`/`OVMX-LOCAL` declaration block at the top
of `src/libvms/syssvc/sys_lock.c` must be rewritten **in the same commit**, and
`tracking/rd-citations.tsv` regenerated with `tools/gen_rd_citations.py` **from the repo root**
(see §5 — running it from a worktree produces churn).

---

## 2. What merged in round 9

| Item(s) | PR | What it bought |
|---|---|---|
| `vms-d894`/`vms-659` | #74 | The negctl driver emits what it OBSERVED; the static gates read it. **Merged by the operator**, not by this round. |
| — | #75 | The previous revision of this document. |
| `vms-004e` follow-up | #71 | The FAIL headline names the population that found the id. Recorded in the last revision as "closed as refuted"; it was fixed and merged. **Not mine.** |
| `vms-b2e`, `vms-a30` | **#76** | AUTHORIZE and MAIL take identity from the executive, not the environment. |

**Closed with verified reasons:** `vms-b2e`, `vms-a30`.
**Filed:** `vms-4c2` (wired as a blocker of `vms-cb5`).
**Raised to a human:** `vms-d894` (`rd gate`, type `scope`).

### What #76 actually proved, and what it did not

`AUTHORIZE.EXE` decided who may manage SYSUAF from `getenv("USER")`. Measured before: `env
USER=baron` refused; `env USER=SYSTEM` opened a full SYSUAF-management session **and printed
SYSTEM's password hash**. Measured after: all three refuse.

**That replay is weaker than it looks and the item says so.** The dev host has no `/dev/vms`, so
every refusal is equally explained by the executive read failing rather than by the SYSPRV bit being
clear. A `check_privilege()` hardwired to `return 0` passes everything in the repo today. The
positive control — a SYSPRV holder is still **admitted** — is unproven at every layer and is
`vms-4c2`, which also blocks `vms-cb5`.

**The durable half is the census.** `test_env_identity_census.sh` was green throughout the exploit's
life because it scanned five `VMS_`-prefixed names and plain `USER` was outside them. Its "6 sites"
was true of its own scope and said nothing about AUTHORIZE. `USER`/`LOGNAME` are now in the
universe, and with MAIL's reader deleted the write-only claim iterates `$VARS` — every identity
variable in `src/` and `tools/` is now write-only, a total claim where it was partial.

---

## 3. `vms-d894` — the one blocker, and why it does not close

**STATUS (round 9): raised as an `rd` scope gate with a recommendation. NOT decided, NOT closed.**
Check `rd gates` before reading further — if it has been answered, this section is history. The
recommendation given was **(A) close as sufficient**, on the ground that 19 of 30 remaining items
are already machinery and option (B) spends a round hardening a measurement against an adversary who
must already write a self-contradicting three-file, 69-line commit. Absent an answer the round did
neither and worked the defects instead.

Everything below is the measurement the gate was raised on. It is measured, not argued.

**What PR #74 builds.** `run_facility_negctl.sh` — the only thing in this program that executes
anything — now emits `tests/qemu/facility_negctl_observed.tsv`: one `RUN` row per defect, one `RED`
row per observed failing assertion. The static gates read it instead of guessing.

**It is live, not theoretical.** The first real record is committed: CI run `30940272725`, job passed
in 15m44s, tree-commit `6ffde90`, positive-control pass, **42 RUN rows and 272 observed failing
assertions**. `FACILITY_NEGCTL_REQUIRE_RECORD` is now `1`.

**What the gate said before and after, same tree:**

```
before:  OBSERVED: of those 25, 0 are PROVEN ABLE TO GO RED and 25 are NAMED ONLY
         NOT MEASURED: no execution record

after:   OBSERVED: of those 25, 25 are PROVEN ABLE TO GO RED and 0 NAMED ONLY.
         PASS: 42 of this manifest's 42 defect(s) are OBSERVED-EXECUTED
```

**"PROVEN" is now spendable only by a QEMU run that happened.** That was `vms-659`'s better option
and it is done.

**Deletion test 1 — delete a defect, leave the record:**

```
FAIL: REFUSING to certify ...: it records defect(s) the manifest [no longer has]   rc=1
```

and the PROVEN population drops back to **0** rather than holding a stale claim. A contradicted
record certifies nothing.

**Deletion test 2 — delete the defect AND its record rows AND lower the floor:**

```
OBSERVED: of those 25, 25 are PROVEN ABLE TO GO RED and 0 NAMED ONLY.
PASS: 41 of this manifest's 41 defect(s) are OBSERVED-EXECUTED
```

**Every record check goes quiet and certifies a smaller manifest as fully proven.** Both files are in
the same commit, the deleter owns both, and a regenerating CI run agrees with them.

**So the item's actual criterion — a floor sourced from something the deleter does not own — is
unmet.** What closes it is comparing against the **base branch's** record, which cannot be faked
without rewriting history. Needs `fetch-depth: 0` and a base-record extraction. Deliberately not
bundled: an untestable CI step that can red the only job which executes anything is the worse trade.

Price of deleting a defect is now a **measured 69 changed lines across 3 files** — measured by the
test, which searches for the cheapest deletable defect rather than asserting a number. It was ~21
lines / 3 files.

**The decision:** close `vms-d894` on the grounds that 69 lines and a visible three-file diff is
enough, or build the base-branch floor. That is a scope call. **Do not close it by asserting the
criterion is met — it is measurably not.**

---

## 4. Method — what this round paid for

The 2026-08-03 and 2026-08-04 blocks still hold. **New in round 9:**

**A DISPROOF WITH A RED BASELINE PROVES NOTHING, AND IT LOOKS LIKE IT PASSED.** Proving the new
census controls were non-vacuous meant deleting the mechanism and showing exactly the right controls
fail. The first attempt removed `USER`/`LOGNAME` from `VARS` but **left their `DECLARED` entries**,
so the unmutated tree went red ("a declared site no longer exists") — and every mutation case
"went red" too, including the two that were supposed to. It read as a clean disproof. The second
attempt removed the declarations as well, held the baseline green, and then **exactly H and I
failed** with A–G and J unchanged. *Before believing a disproof, confirm its baseline is green.*

**AN ITEM'S STATED SITE SET CAN BE AN UNDERCOUNT, AND THE MISSING ONES ARE THE DANGEROUS ONES.**
`vms-e60` names three `strtoul(...,10)` sites; there are nine in four files. The two it misses are
`sys_uai.c` and `ovmx_init.c` — the latter being PID 1 stamping SYSTEM's UIC into the executive.
A partial fix there would recreate the exact defect the item describes with different endpoints.
Enumerate from the tree, not from the item.

**TEST A WIDENED GATE FOR OVER-FIRING IN THE SAME COMMIT.** Adding a short name like `USER` to a
substring-matched census risks matching `USERNAME`/`VMS_USERNAME`; that is how a gate acquires an
exemption and stops being read. Case J pins it.

**MERGING IS NOT THE END OF THE RE-DERIVATION.** Main moved twice more (#75, #71) *while #76's CI
ran*. The file-overlap check before merging was empty, so it was safe — but the check is the reason
that is known, not an assumption. Re-read *which files* differ every time.

Still true from before:

**THREE DEFECTS THIS ROUND WERE MISDIAGNOSED IN THE SAME WAY: A KILL READS AS A FAILURE.**
`vms-b1f` was filed "host-sensitive" (it is trailing-output-size). `vms-008` was carried as a known
failing test (ctest was killing a passing one). `vms-86a` I watched manufacture a red on my own PR.
A `timeout-minutes` kill, a ctest TIMEOUT and an infra cancellation **all** report
`conclusion: cancelled` or `Timeout` and render in `gh pr checks` as a plain `fail`. **Check
`.conclusion` and whether the assertion step actually ran before attributing any red to the diff.**

**A DISPROOF CAN BE WRONG IN THE DIRECTION THAT COMFORTS YOU.** `ctest --timeout 120` does **not**
override a per-test `TIMEOUT` property. My first `vms-008` disproof therefore passed, which would
have "proved" the timeout was fine. The real disproof needs a tree configured with the old property.
If a disproof tells you the thing you wanted to hear, check the disproof.

**MEASURE ONE ORACLE AT A TIME.** Three PRs' CI running concurrently took the per-facility negctl
job from 27m09s to >60m and it was killed by its own timeout — a red I manufactured by parallelism.
Series: **27m09s idle / 50m39s moderate / >60m killed**. Serialize the long jobs.

**DO NOT GARBAGE-COLLECT A TREE A BACKGROUND JOB IS STANDING IN.** I deleted a worktree under a
running negctl and got 16 "failures", every one carrying `getcwd() failed`. That run was void, not a
result, and it looked exactly like a finding.

**THE OBVIOUS HONEST FIX CAN COLLIDE WITH AN EXISTING DISCRIMINATOR.** PR #71 reworded a FAIL
headline to be truthful and CI refuted it: `F_CITE_TREE_UNRESOLVED="FAIL: the tree cites "` already
existed, so the reword made three distinct failure modes share a prefix. Closed rather than patched.
`vms-c13c` carries the constraint any replacement must satisfy.

**A BRANCH CUT EARLIER IN A ROUND CAN SILENTLY REVERT WHAT LANDED SINCE.** PR #74's branch predated
#69 and #66; `git diff main` showed 448 lines of census work as *removals*. Always read *which files*
differ before merging, not just whether it merges.

---

## 5. Environment

- **Disk is the binding constraint: 2.7 GB free, 99% used** (tighter than the last revision said). Build one image at a time. The ~48 GB of
  `vat-env-*` images are **not ours** — do not prune them.
- Local ctest is **64 tests, 1 expected failure**: `vms-2a1` (`test_libvms_protection`, 6 of 8
  subtests need unprivileged user namespaces, restricted by apparmor on 24.04). That is a genuine
  host restriction, not a budget.
- `docker`, not podman. `export PATH="$HOME/.local/bin:$PATH"` for `rd`.
- **`rd` run outside the repo returns a PARTIAL set, not an error.** 289 items from
  `~/projects/vms`, **159** from another directory, stable across runs. This is `vms-10c`'s real
  mechanism and it silently corrupted a regenerated ledger — genuinely-active items got written
  `closed`, with their true status still in column 3. `closed active` is self-contradictory and is
  the tell.
  **Round 10: it no longer corrupts, it REFUSES** — `gen_rd_citations.py` rejects a `closed` verdict
  the item's own status contradicts. The behaviour of `rd` itself is unchanged, so the advice below
  about running from the repo root still stands; what changed is that ignoring it now produces a
  loud refusal instead of a wrong ledger. **`--project` does NOT fix this and was measured: from
  `/tmp`, `rd list --json` returns 0 items and `rd list --project vms --json` also returns 0** — the
  flag filters within a board rd already resolved from cwd, it does not select one. Neither does
  `--rd-home` (identity/config, not board). So of `vms-10c`'s two proposed directions, "pin the
  project explicitly" is **not available with rd's current flags**; the refusal is the fix.
- **`run_facility_negctl.sh` still runs in CI only, and round 10 measured WHY it still does.** Its
  software blocker is gone — `vms-b1f` (the `pipefail`/SIGPIPE exit status that made the driver
  refuse to start here) is **closed**, and the driver takes a defect list, so a single-defect run is
  possible. What blocks it now is **DISK**: a fresh `ovmx-ktest-negctl-base` image has to be built
  and the host had **3.2 GB free (98%)**, on a machine also running k3s and the live SIMH VAX lab.
  Filling `/` there would take out another thread's experiment. Not attempted. **This matters
  because three separate p1 items (`vms-2b2`, `vms-d33`, `vms-38c`) independently conclude that only
  execution can attribute, and all three name this driver as the instrument.** They are not blocked
  by analysis; they are blocked by 3 GB.
- **Run `tools/gen_rd_citations.py` from the repo root, never a worktree.** Doing it from a worktree
  produced 23 lines of status churn that had nothing to do with the change — the same `vms-10c`
  cwd-dependence described above. It also correctly adds nothing for plain prose citations: it only
  records `OVMX-<TOKEN>:` declaration markers.

---

## 6. What needs a human

- **`vms-d894` — now a live `rd` gate (`scope`), awaiting an answer.** Close on
  69-lines-and-visible, or build the base-branch floor. §3. This is the only thing blocking
  `vms-b33`, and it is the only reason Phase 2 is not closeable.
- **`vms-b8d`**: is remote access next.
- **Whether `vms-1e1` (B) still holds.** It was ruled on the side condition that the citation check
  is machine-checked. `vms-004e` and `vms-35f` were attacks on exactly that check and are now fixed
  and merged, which arguably restores it — but re-affirming is a scope call. **Round 9 did not
  touch this; it is unchanged and still open.**
- **Whether to keep paying for the machinery.** Now **19 of 30**. The ratio did not improve because
  round 9 spent itself on defects rather than gates — which is the shape the last revision asked
  for, and it means the machinery count is flat, not falling. §1.
- **`vms-4c2` (new).** Proving AUTHORIZE still *admits* a SYSPRV holder needs `AUTHORIZE.EXE` in the
  QEMU test image, which today plumbs `DCL.EXE` only. That is a small but real widening of the one
  CI job that executes anything, on a host with 2.7 GB free. Worth confirming someone wants it
  before it is built.
