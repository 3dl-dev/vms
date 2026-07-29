# Where do LNM$SYSTEM / LNM$GROUP / LNM$JOB live? — design record + ruling

> **Item:** `vms-ln0` (Phase 3 of the executive retrofit, `vms-6b8`, under the authenticity
> pillar `vms-898`).
> **Status:** RULED — pending operator sign-off. This record does not itself raise an `rd`
> gate: a prior draft claimed to, but that gate published to a mangled board ID and never
> reached the operator. The design gate for `vms-ln0` is raised from the project root by the
> orchestrator, carrying this ruling and the un-self-certified values in §4.
> **Blocks:** `vms-d37` (`DEFINE/SYSTEM` propagates across processes). A cold agent reading
> `vms-d37` plus this file should know exactly what to build.
> **Plan context:** `docs/design-executive-retrofit.md` §5, §6.

---

## 0. Ruling in one paragraph

**The executive owns LNM$SYSTEM / LNM$GROUP / LNM$JOB. Userspace reads them through a
read-only shared mapping obtained by `mmap()` on `/dev/vms`; every mutation goes through an
ioctl.** This is option **C, corrected**: the mapping comes from the *executive*, not from a
file. That single correction turns all three of option C's stated costs — cross-process
locking, crash consistency, and no protection — into non-issues, while keeping option C's
defining property of zero syscalls on the hot path. Option A (ioctl per translation) is
**rejected on measured cost**: across **11 independent boots** of the identical binary now on
record (n=5 trials/boot, 55 trials total — see §1.1 for the full boot-by-boot table, which now
includes 3 boots supplied by the round-3 veracity adversary and 2 fresh boots run for this
rework), the executive round trip's per-boot mean has so far ranged **~72–83 µs**, and its
per-boot ratio mean has so far ranged **~81×–97×** against the in-process four-table translate
it would replace, on the only runtime target OVMX has. **This is an observed range over 11
samples, not a proven bound.** Round 3 caught exactly this overclaim once already — three fresh
boots landed outside the previously-stated 6-boot range — and the range will keep widening as
more boots are run; §1.1 states the widest honest figure (the union of within-boot trial
brackets, not just of boot means) for that reason. No single boot's tight n=5 bracket (e.g.
"79.0 µs, 84.5×") bounds this reproduction variance either — that bracket measures within-boot
jitter only (§1.1). Every figure here is itself a measured **lower bound** for a real translate
ioctl (§1.1, §4a) — the true cost of option A is higher still. None of this threatens the
ruling: the margin over the ruled option is roughly two orders of magnitude and holds across
all 11 boots (§1.1). Option B (kernel-owned + per-process cache with invalidation) is **rejected on failure
mode**: a missed invalidation is a silently wrong translation that reports success, which is
the same class of defect as the per-process fake this epic exists to kill. LNM$PROCESS stays
per-process and is not touched.

---

## 1. What was measured, and where

Two measurement programs were written and are checked in. Both run **inside the QEMU guest
against a real `vms.ko` and a real `/dev/vms`** as part of `tests/qemu/`, so the numbers are
reproducible by anyone who runs the kernel-module suite:

| Program | Measures |
|---|---|
| `tests/qemu/bench_lnm_cost.c` | cost of ONE translation: executive ioctl round trip vs. the in-process hash lookup it would replace |
| `tests/qemu/bench_lnm_peropen.c` | how many translations ONE file open performs, and how many of them would have to reach the executive |

Neither program stubs anything on the measured path:

- `bench_lnm_cost` links the **real** `src/vmslnm/{lnm_table,lnm_translate,lnm_client,lnm_defaults}.c`
  and populates the tables with the **real** `lnm_setup_defaults()`. The ioctl cases are real
  ioctls on a real `/dev/vms` served by a real `vms.ko`.
- `bench_lnm_peropen` links the **real** `src/vmsfs/` translation pipeline and interposes on
  `lnm_translate` with `-Wl,--wrap`; the wrapper counts and then forwards to
  `__real_lnm_translate`. The call counts are the shipping code path's, not a reading of the
  source.

**No silent fallback.** `bench_lnm_cost` refuses to invent a number when the executive is
absent. Negative control, run on a host with no `/dev/vms`:

```
$ ./bench_lnm_cost ; echo "EXIT=$?"
=== vms-ln0 logical-name placement cost measurement ===
MODE: RUNTIME TARGET (/dev/vms required)

  FAIL: /dev/vms absent or unopenable (No such file or directory)
  This is SS$_NOSUCHDEV. The executive is the only OVMX
  runtime; there is no per-process fallback to measure.
FAIL: bench_lnm_cost
EXIT=1
```

It also refuses to time an unregistered channel. The first run against a real `/dev/vms`
failed honestly with `VMS_IOCTL_GETMODE rejected (No such process)` — `vms.ko` requires
`VMS_IOCTL_REGISTER` before any other ioctl. Registration is a per-process one-off and is
performed outside every timed loop, so it is not charged to the per-translation cost.

### 1.1 Cost of one translation — runtime target

QEMU guest, `aarch64`, Linux `6.8.0-136-generic`, `vms.ko` + `vmsfs.ko` loaded, `/dev/vms`
present and answering.

**A single 300 ms-accumulation sample understates run-to-run variance.** An earlier draft of
this record quoted a single run (90.1 µs, 96.4×) as if it were exact. An independent re-run of
the identical binary gave 78.1 µs / 83.7× — a 13% spread on the number the whole ruling is
quoted against. `bench_lnm_cost` was changed to sample the two figures the ruling turns on —
the `CHKPRIV` round trip and the in-process 4-table translate it is compared against — as
**5 independent trials** (fresh warmup, fresh 300 ms window, each trial), and report
min/mean/max with `n` stated instead of a single point estimate. This run, reproduced today:

```
  trial 1: vms_inout=  81575.7 ns/op   lnm_hit_s=  945.4 ns/op   ratio=86.3x
  trial 2: vms_inout=  76212.9 ns/op   lnm_hit_s=  971.0 ns/op   ratio=78.5x
  trial 3: vms_inout=  79718.1 ns/op   lnm_hit_s=  901.5 ns/op   ratio=88.4x
  trial 4: vms_inout=  80425.1 ns/op   lnm_hit_s=  919.3 ns/op   ratio=87.5x
  trial 5: vms_inout=  77154.4 ns/op   lnm_hit_s=  941.8 ns/op   ratio=81.9x

  vms_inout (CHKPRIV round trip)                     : mean 79.0 us  [76.2, 81.6] us   (n=5)
  lnm_hit_s (in-process 4-table translate)           : mean 0.94 us                    (n=5)
  exec ioctl (in+out) / in-process 4-table translate : mean 84.5x    [78.5x, 88.4x]     (n=5)
```

Context figures below are single-sample (they are not the headline the ruling is quoted
against — only `vms_inout` and `lnm_hit_s` above are):

```
  [clock]     clock_gettime pair (noise floor)                       1101.3 ns/op
  [getppid]   syscall(SYS_getppid) - syscall floor                  40190.5 ns/op
  [enotty]    ioctl(non-vms fd) -> ENOTTY - dispatch floor          39121.3 ns/op
  [vms_out]   ioctl(/dev/vms, GETMODE) - exec round trip, out only  59775.7 ns/op
  [lnm_hit_p] lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)         387.4 ns/op
  [lnm_miss]  lnm_translate FILE_DEV, miss all 4 tables               930.4 ns/op
```

`CHKPRIV` is the figure the ruling uses. It is the closest shape `vms.ko` currently offers to
what a logical-name translate ioctl would do — PCB lookup, `copy_from_user`, `spin_lock`,
consult executive state, `copy_to_user` — and it is still a **lower bound** (see §4a): a real
translate ioctl copies a longer name in, walks a hash chain, and copies a longer value out, so
its true cost is unmeasured and **higher** than the 79.0 µs mean reported here. Nothing here is
rounded up by guess; the direction of the remaining error is always toward strengthening the
rejection of option A.

**The n=5 bracket above does NOT bound reproduction variance — it bounds within-boot jitter.**
All five trials in that box share one process, one QEMU boot, one warm TCG translation cache,
and one host thermal state; only the 300 ms accumulation window is fresh per trial. A prior
adversarial pass falsified treating it as a reproduction bound by following the `Reproduce:` line
below twice and getting figures outside the box's own range in both directions. Six independent
boots of the identical binary were on record going into round 3. Round 3's veracity adversary
ran 3 more independent boots and reported them falsifying the round-2 range; this rework ran 2
more of its own on top of that. **All 11 are transcribed below, not just the ones that agree
with each other** — the point of this table is to show the range as it actually is, not to
curate it down to a tight-looking bracket:

| boot | source | `vms_inout` mean | ratio mean [range] | `lnm_hit_s` mean |
|---|---|---|---|---|
| 1 | this record (box above) | 79.0 µs | 84.5× [78.5×, 88.4×] | 0.94 µs |
| 2 | this rework, `podman run` #1 | 74.4 µs | 87.9× [85.1×, 89.1×] | (not recorded) |
| 3 | this rework, `podman run` #2 | 74.6 µs | 92.9× [90.1×, 97.1×] | (not recorded) |
| 4 | this rework, `podman run` #3 | 73.7 µs | 92.6× [87.9×, 100.3×] | (not recorded) |
| 5 | veracity adversary, round 2 | 75.3 µs | 89.3× [84.8×, 93.4×] | (not recorded) |
| 6 | veracity adversary, round 2 | 83.4 µs | 96.7× [89.2×, 104.5×] | (not recorded) |
| 7 | veracity adversary, round 3 | 82.0 µs | 80.9× [75.7×, 86.4×] | 1.013 µs |
| 8 | veracity adversary, round 3 | 71.9 µs | 96.3× (range not reported) | 0.747 µs |
| 9 | veracity adversary, round 3 | 72.6 µs | 91.5× (range not reported) | 0.794 µs |
| 10 | this rework, round 3, `podman run` #1 | 78.6 µs | 90.4× [85.2×, 92.9×] | 0.87 µs |
| 11 | this rework, round 3, `podman run` #2 | 77.6 µs | 83.1× [78.7×, 87.3×] | 0.93 µs |

Rows 2-6 are transcribed from earlier rounds' output and were not re-captured for this rework;
rows 7-9 are the round-3 adversary's own rebuild (their report is the source, not this rework's
own runs); rows 10-11 are fresh, captured by this rework via `podman build`/`podman run` of the
unmodified binary at the commit this rework started from.

**Union across all 11 boots (the widest honest figure — the union of every boot's OWN n=5
trial-level [min, max] where available, not just the range of the 11 boot means):** ratio
**75.7×–104.5×**, vms_inout mean-of-boot-means **76.7 µs**, ratio mean-of-boot-means **89.6×**.
This is **wider** than the 6-boot union it replaces (78.5×–104.5×) — the 78.5× floor did not
survive 5 more boots, exactly as the round-3 adversary demonstrated by falsifying it on the
first reproduction. **This is not a bound. It is the widest range observed so far, over 11
samples, and it should be expected to widen again the next time someone runs this suite** —
each new boot has moved at least one edge of the range in every round to date. The claim this
record actually needs does not depend on the exact edges: the margin between this range and the
ruled option's projected cost (§2.4) is roughly **two orders of magnitude**, and no boot on
record — including the 5 added since the last round — has come remotely close to closing that
gap. **§0 and §5 quote this range as an observed range, explicitly not as a bound, and not
boot 1's tight n=5 bracket.**

**The in-process `lnm_hit_s` baseline (the QEMU-side number §2.4's projection for the ruled
option borrows) has the same problem, and the prior round did not give it the same honest
treatment it gave `vms_inout`/ratio — corrected here.** Of the 11 boots, 6 have a recorded
`lnm_hit_s` mean (column 5 above): 0.94, 1.013, 0.747, 0.794, 0.87, 0.93 µs. Min 0.747 µs, max
1.013 µs, mean 0.882 µs — a **(max−min)/min spread of ~36%**, with 2 of those 6 samples (rows
10-11) captured independently by this rework rather than copied from the adversary's report.
**Wherever this record or §5 states "0.94 µs" for the in-process baseline, that is boot 1's
single sample, not a settled constant** — the honest figure is **0.75–1.01 µs across 6 boots,
mean ~0.88 µs**. This does not change the ruling — the margin is still ~2 orders of magnitude
against option A (§2.4) — but a number quoted to 2 significant figures as "measured" should not
carry an unacknowledged 36% boot-to-boot spread three lines under a reading note that promises
MEASURED means exactly that.

Reproduce: `podman build -f tests/qemu/Dockerfile -t ovmx-ktest .` then
`podman run --rm ovmx-ktest`, and read the `bench_lnm_cost` section of the serial log. Run it
more than once — no two boots in the table above are the same execution — to see the spread
directly. It will very likely produce a 12th boot outside some edge of the range above; that is
expected, not a failure of this method.

### 1.2 Translations per file open — K

```
  SYS$SYSTEM:LOGINOUT.EXE      3 translations, 3 reach the executive
  SYS$LIBRARY:DECC$SHR.EXE     3 translations, 3 reach the executive
  SYS$LOGIN:LOGIN.COM          2 translations, 1 reaches the executive
  DKA0:[USERS.SYSTEM]FOO.DAT   1 translation,  1 reaches the executive
  SYS$HELP:HELPLIB.HLB         3 translations, 3 reach the executive
  [USERS.SYSTEM]BAR.TXT        0 translations, 0 reach the executive

  mean translations per open        : 2.00
  mean reaching the executive (K)   : 1.83
```

The reason K is high is structural, not incidental: `vmsfs_resolve_device_r()`
(`src/vmsfs/vmsfs_translate.c:340`) **recurses**. `SYS$SYSTEM` resolves to
`SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]`, which forces a translate of `SYS$SYSDEVICE`, which
yields `DKA0:`, which forces a third translate. Opening a system image — the single most
common open in the system, done by every image activation — costs **three** executive round
trips, not one.

Note also that a **miss** costs a round trip too. `DKA0` is not a logical name; it is resolved
by the device table. Under `vms-dv1` the device table also moves into the executive, so that
lookup does not become free — it becomes a *different* executive round trip. The ruling must
not assume misses are cheap.

### 1.3 Option A, priced

Multiplying the two measurements — using boot 1's n=5 trial **range**, not a single point
estimate, on the runtime target as it exists today. This table prices **one specific boot**;
it is a worked example of the method, not the reproduction-variance range — see §1.1's 11-boot
table for that, and §5 for the headline that quotes it instead of this single boot:

| | per translation (n=5) | K = 1.83 (mean open) | K = 3 (system-image open) |
|---|---|---|---|
| Option A (ioctl, measured floor — §4a) | 76.2–81.6 µs (mean 79.0 µs) | **139–149 µs / open** | **229–245 µs / open** |
| in-process (today, measured) | 0.94 µs (mean) | ~1.72 µs / open | ~2.82 µs / open |

For scale: the guest's own minimal syscall floor (`[getppid]`, single sample, §1.1) is in the
~39–45 µs class across runs of this suite. Option A would cost roughly **3–6× an entire file
open's syscall budget** depending on how many translations reach the executive (K = 1.83 to
K = 3), and would make it the dominant term in every `$OPEN`, every image activation, and every
DCL command that touches a file. Recall these are floor figures (§4a): the real translate ioctl
option A would need is not yet built, and its measured proxy is a lower bound, not a ceiling.

### 1.4 Honest caveat: QEMU TCG inflates syscalls, and by how much

There is no `/dev/kvm` on the measuring machine and CI runs the QEMU suite inside a container,
so the guest is emulated with TCG. TCG does **not** inflate everything uniformly, and pretending
the mean 84.5× is a hardware number would be dishonest. The identical binary was therefore run
natively on the host (`--calibrate`, which explicitly skips the `/dev/vms` cases and labels its
output CALIBRATION).

**This is the program's actual, complete `--calibrate` output for one native run, pasted
verbatim — not reconstructed, and not trimmed.** A previous draft hand-composed a
`[lnm_hit_s] ... native ... (n=5)` line as if it were one of the individual `[label]` RESULTS
lines; the program cannot emit that line in that form. A later draft fixed that but silently
dropped the `[vms_out]` line the program prints unconditionally in calibration mode (it has
`valid=0` there, so `print_result()` emits its "(no sample)" branch) — that line is restored
below; it is the honest marker showing the `/dev/vms` case was skipped, not an omission. In
calibration mode `n_trials` stays 0 (the `/dev/vms` half of every trial is skipped by design),
so `lnm_hit_s` never gets its own `[lnm_hit_s]` line in the RESULTS block — it appears exactly
once, inside the `DERIVED (n=0 trials; ...)` block, phrased as
`lnm_hit_s (in-process 4-table translate), native: ...`. Reproduce natively (no `/dev/vms`
needed): `gcc -static -O2 -Wall -Wextra -o bench_lnm_cost tests/qemu/bench_lnm_cost.c
src/vmslnm/lnm_{table,translate,client,defaults}.c -Isrc/kernel -Isrc/vmslnm/include
-Isrc/libvms/include -lpthread && ./bench_lnm_cost --calibrate`:

```
RESULTS (lower is better):

REPEATED TRIALS (n=5), CHKPRIV round trip vs in-process 4-table translate:
  [clock]    clock_gettime pair (noise floor)                          492.7 ns/op   (n=609600)
  [getppid]  syscall(SYS_getppid) - syscall floor                       60.7 ns/op   (n=4938800)
  [enotty]   ioctl(non-vms fd) -> ENOTTY - dispatch floor               66.8 ns/op   (n=4493200)
  [vms_out]  ioctl(/dev/vms, GETMODE) - exec round trip, out only   (no sample)
  [lnm_hit_p] lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)            23.1 ns/op   (n=12972000)
  [lnm_miss] lnm_translate FILE_DEV, miss all 4 tables                  57.3 ns/op   (n=5239800)

DERIVED (n=0 trials; MEAN with [MIN, MAX] range -- not a single-sample point estimate):
  (calibration mode: /dev/vms cases skipped, no ioctl trials to aggregate)
  lnm_hit_s (in-process 4-table translate), native: mean 54.6 ns/op [52.8, 56.7] (n=5)
```

`getppid`/`enotty` above are single-sample context, as on the QEMU side; `lnm_hit_s` is the same
n=5 trial protocol as §1.1, native this time. **This transcript is one native run.** Two more
independent native invocations of the identical binary, run back to back for this rework, gave
`lnm_hit_s` native means of 54.5 and 57.0 ns/op — a ~4.6% spread ((max−min)/min across all
three runs), smaller than the QEMU-side spread (§1.1) but the same class of run-to-run noise,
and the run transcribed above is near the low end of it, not a cherry-picked minimum.

| | QEMU TCG | native host | TCG inflation |
|---|---|---|---|
| syscall floor (`getppid`, single sample) | 40190.5 ns | 60.7 ns | **662×** |
| ioctl dispatch (`ENOTTY`, single sample) | 39121.3 ns | 66.8 ns | **586×** |
| in-process 4-table translate (n=5 mean, both sides) | 935.8 ns | 54.6 ns | **17.1×** |

So TCG inflates **kernel entry ~36× more than it inflates userspace compute** (average of
662×/586× ÷ 17.1× ≈ 36.4). Correcting the measured mean 84.5× (boot 1, §1.1 — the same caveat
about single-boot precision applies here too, but this whole paragraph is already labelled
directional, not a headline figure) by that differential projects an executive ioctl at roughly
**2.3× the in-process translate** on an accelerated (KVM or bare-metal) runtime — i.e. about
**+72 ns per translation** (using the native 54.6 ns baseline,
the relevant one for an accelerated runtime), **+132 ns per mean open** (K = 1.83). That is a
tolerable **tax in the low tens of percent**, not a catastrophe. This projection is directional,
not a headline figure this ruling is quoted against — it is not held to the same n≥5
QEMU-target precision bar as §1.1's `vms_inout`/ratio, which is why it is kept in a caveat
section rather than the summary table's ruled-option cell (§5, and see the labeling fix there).

**Both numbers matter, and they point the same way:**

- On an accelerated runtime, option A is affordable but is a permanent tax on the hottest path
  in the system, bought for **zero correctness benefit** over the ruled option.
- On the runtime OVMX actually has today — unaccelerated QEMU, which is what CI runs and what
  developers boot — option A costs 139–245 µs per open (§1.3) and is disqualifying.

A design that is only tolerable on hardware nobody in the project currently runs is not a
design; it is a bet. The ruled option is fast on both.

---

## 2. The options, and why C-corrected wins

### 2.1 Option A — kernel-owned, ioctl per translation

Rejected. Simplest and most consistent with the rest of the executive, and correctness is not
in question — but §1.3 prices it at 139–245 µs per file open on the runtime target (n=5 trial
range; mean 79.0 µs/translation), and that figure is itself a measured **lower bound** (§4a) —
the real translate ioctl this option needs is not yet built, so its true cost can only be
higher. Its only advantage over the ruled option is that it needs no `->mmap` in `vms.ko`, which
is perhaps 80 lines of kernel code.

### 2.2 Option B — kernel-owned + per-process cache with invalidation

Rejected, and not primarily on performance. The whole difficulty of B is the invalidation
protocol, and **the failure mode of a missed invalidation is a translation that returns the
wrong value and reports `SS$_NORMAL`.** That is the same class of defect as the per-process
`DEFINE/SYSTEM` fake that this epic exists to eliminate: silent, successful, wrong. Trading a
loud architectural lie for a quiet cache-coherence lie is not progress.

Beyond that: any invalidation channel that does not poll is itself a piece of shared state the
executive publishes to every process — at which point the shared mapping already exists and C
is strictly simpler than B, because C shares the *answer* instead of sharing a *hint that the
answer changed*.

(For the record, the hybrid — a one-page mapping holding only a generation counter, plus an
ioctl to refill a per-process cache when it changes — was considered and is a real design. It
is rejected because it requires **both** mechanisms of A and C, plus a cache, plus a coherence
argument, to arrive at a slower version of C.)

### 2.3 Option C as literally proposed — file-backed `MAP_SHARED`

Rejected **as written**, and the reason is the one this epic is about.

The in-tree precedent is `src/imgact/known_images.c`, which `mmap(MAP_SHARED)`s a plain file.
A plain file works perfectly well **with `vms.ko` absent**. Adopting that pattern for logical
names would give `DEFINE/SYSTEM` cross-process propagation that succeeds on a runtime with no
executive — reproducing exactly the drift described in `docs/design-executive-retrofit.md` §2,
where the architecture bent to fit a harness that had no `/dev/vms`. It would also satisfy
`vms-d37`'s done-condition while leaving the executive uninvolved, which is the failure mode
`vms-vx2` exists to catch.

It is also unprotected: any process can scribble on the system logical name table. On real
VMS, system space is protected by processor access mode.

### 2.4 Option C corrected — the mapping comes from `/dev/vms` — **RULED**

Change one thing about option C: **the shared pages are obtained by `mmap()` on the `/dev/vms`
file descriptor**, not from a file. `vms.ko` owns the memory, gains a `->mmap` in
`vms_fops` (`src/kernel/vms_module.c:269` — it has `.unlocked_ioctl`, `.open`, `.release` and
no `.mmap` today), and maps the arena **read-only**, clearing `VM_MAYWRITE` so `mprotect()`
cannot re-enable writes.

That single correction resolves every cost the item attributed to option C:

| Stated cost of option C | Under C-corrected |
|---|---|
| cross-process locking (robust mutexes / futex) | **gone.** The kernel is the only writer. Readers take no lock at all; they use a seqlock generation counter in the arena header. |
| crash consistency when a process dies holding a lock | **gone.** There is no userspace lock to die holding. A dead process just unmaps. |
| no protection — any process can corrupt it | **gone.** The MMU enforces read-only. This is the direct analogue of VMS protecting system space by processor access mode, not a workaround for the absence of one. |
| offsets, not pointers | **survives** — see §3.2. But this cost is common to every option, including A. |

And it keeps what made C attractive: **translation performs no syscall**. The C-corrected read
path — a seqlock-guarded arena read via `mmap()` — does not exist yet (that is `vms-d37`'s job,
§3), so its cost is **projected, not measured**: the projection is that a lock-free read of an
offset-addressed arena is the same shape of work as today's in-process hash lookup (pointer/offset
chasing over a small fixed structure, no syscall), so it should cost in the same class as the
**0.75–1.01 µs observed across 6 QEMU boots (mean ~0.88 µs, §1.1) / 54.6 ns observed on native
host (§1.4)** measured today for `lnm_hit_s` — instead of the ~72–83 µs per-boot range (§1.1)
measured for option A's ioctl round trip. **This is a projection about an unbuilt read path, not
a measurement of it; do not read "sub-microsecond class" in this paragraph, or the C-corrected
cell of the §5 summary table, as something that was measured — and do not read the 0.88 µs
mean as a settled constant either; it is itself an observed range, not a bound (§1.1).**
§3.5 states the follow-up: `vms-d37` must re-run `bench_lnm_cost` after building the arena and
confirm the projection against a real measurement, precisely because a projection is not
evidence until it is checked.

It also satisfies the standing constraints without a special case:

- **One runtime target.** The mapping is only obtainable from `/dev/vms`. There is no
  file-backed variant to fall into.
- **No silent fallback.** No `/dev/vms` → `open()` fails → **`SS$_NOSUCHDEV`**, exactly as
  `src/libvms/syssvc/sys_lock.c` already does. There is no per-process fake to reach for,
  because the read path has no non-executive implementation at all.
- **Not done until proven against a real `/dev/vms`.** The read path is *physically incapable*
  of working without one, which is the strongest form of that guarantee.

---

## 3. What `vms-d37` must build

### 3.1 Split of duties

```
  translate (hot, ~every file open)   -> read the mapped arena. NO syscall.
  define / deassign  (rare)           -> ioctl. Executive validates privilege, mutates,
                                         bumps the generation counter.
```

`LNM$PROCESS` is untouched: it stays a private in-process table and is searched first, exactly
as `search_file_dev()` does today (`src/vmslnm/lnm_translate.c:41`). §1.2 shows this is not a
minor case — `SYS$LOGIN:LOGIN.COM` gets one of its two translations answered without ever
consulting the executive.

**Do not rebuild the working semantics.** All four tables, the LNM$FILE_DEV search order,
`/TABLE=`, `/PROCESS`, `/SYSTEM`, `DEASSIGN/SYSTEM` and table attribution in `SHOW LOGICAL`
work today. The only change is *where the SYSTEM/GROUP/JOB storage lives* and therefore what
`lnm_find_table()` / `lnm_table_lookup()` read for those three.

### 3.2 The shared record format is the first deliverable — and it is common cost

`sizeof(lnm_entry_t)` today is **33,560 bytes** (`char name[256]` + 128 ×
`lnm_translation_t` at 260 bytes each + a `struct lnm_entry *next`). A thousand system
logicals would be **32 MB** of arena.

This is not an argument against C. It is an argument that **every** option needs a compact
record: option A would have to `copy_to_user` 33 KB per translation, which is far worse than
the ~79 µs already measured for a 32-byte round trip (§1.1). The compact, offset-addressed
record is therefore **common cost**, not a differentiator, and it should be built first because
A, B and C all need it.

Requirements for the shared record:

- Offsets, never pointers — the arena is at a different virtual address in every process.
- Variable-length name and equivalence storage; the 128-equivalence maximum is a *limit*, not
  an allocation.
- Fixed-size arena sized at module load. Real VMS sizes its logical name hash tables from
  SYSGEN parameters; adopt that shape rather than growing the mapping under readers' feet.
  When the arena is full, **fail honestly** with a status code (see §4).
- Per-table containers keyed appropriately: LNM$SYSTEM is singular, LNM$GROUP is per UIC
  group, LNM$JOB is per job tree. The layout must carry that key from day one even if only
  LNM$SYSTEM is populated first, because retrofitting a key into a shared format is a flag-day.
- Reader protocol: seqlock. Header holds a `u64` generation; the executive makes it odd before
  mutating and even after; readers sample, read, re-sample, and retry on mismatch. Writes are
  rare, so retries are effectively never. No syscall, no lock, no torn read.

> **Clean-room note (CLAUDE.md Rule 8).** Public OpenVMS documentation describes logical name
> *behaviour* — the table hierarchy, the search order, the attributes, the privileges — and
> that behaviour is what OVMX reproduces. It does **not** publish the byte-level layout of the
> executive's logical name database. The arena layout defined by `vms-d37` is therefore an
> **OVMX design choice** and must be labelled as such in the header that defines it, exactly as
> `docs/design-link-native-toolchain.md` does for the image formats. It is never to be
> presented as VMS-authentic.

### 3.3 Kernel side

- Add `.mmap` to `vms_fops`. Map the arena `PROT_READ`; clear `VM_MAYWRITE`; reject
  `MAP_PRIVATE` write intent and any offset/length outside the arena.
- New ioctls in the `src/kernel/vms_ioctl.h` `'V'` space for define / deassign / (optionally)
  a translate used only by tooling. Numbering: the header currently allocates `0x01`–`0x04`
  access mode, `0x10`–`0x12` AST, `0x20`–`0x27` event flags, `0x30`–`0x33` locks, `0x40`
  register. Take a fresh block; do not reuse.
- Mutation ioctls must check privilege through the executive's existing privilege machinery,
  not in userspace. See §4 for which privileges — that value needs oracle sign-off.
- `vms.ko` already keys everything off a registered PCB (`vms_proc_find_or_err()`, `-ESRCH`
  otherwise). GROUP and JOB scoping should derive from the PCB, not from anything the caller
  passes, or the scoping is advisory.

### 3.4 Userspace side

- `lnm_get_manager()` (`src/vmslnm/lnm_client.c:203`) gains an executive attach: open
  `/dev/vms`, `VMS_IOCTL_REGISTER`, `mmap` the arena. If any step fails, **return the failure**
  — `SS$_NOSUCHDEV` — do not construct in-process SYSTEM/GROUP/JOB tables.
- `search_file_dev()` keeps its order: process table (private, unchanged) → job → group →
  system (all three from the arena).
- `src/vmslnm/lnm_daemon.c` and its `/tmp/ovmx/lnm.sock` become dead. Nothing outside the
  daemon connects to that socket; `src/ovmx_init/ovmx_init.c` only `stat()`s it to confirm
  boot. Its one real job — reading `SYS$MANAGER:SYLOGICALS.CONF` — moves to whichever
  privileged process seeds the executive at boot. Deleting the daemon belongs to `vms-fk1`
  (Phase 4, retire the fakes), not to `vms-d37`; flag it there rather than leaving a live
  unused precedent that a later agent will mistake for the intended design.

### 3.5 How `vms-d37` proves itself

The done-condition is unchanged and must be demonstrated **in the QEMU guest with `vms.ko`
loaded**, two processes:

```
  proc 1:  DEFINE/SYSTEM CROSSPROC HELLO
  proc 2:  SHOW LOGICAL CROSSPROC          ->  "CROSSPROC" = "HELLO"   (currently %DCL-W-NOLOG)
```

plus a negative control in the same suite: with `/dev/vms` absent, `DEFINE/SYSTEM` **fails**
with `SS$_NOSUCHDEV` and does not report success.
`tests/integration/test_runtime_target.sh` is the gate that keeps a fallback from creeping back.

Re-run `tests/qemu/bench_lnm_cost` and `bench_lnm_peropen` after the change, with n=5 trials as
§1.1 does. The expected result is that per-translation cost is **unchanged in class** from the
in-process baseline in §1.1 (sub-microsecond class, not the tens-of-microsecond class option A
occupies) — this is the point where §2.4's
projection gets checked against an actual measurement of the built arena read. If it lands in
the tens-of-µs class, the read path is going through an ioctl and the ruling has been
implemented as option A by accident.

---

## 4. Purity — values that need operator sign-off before they are written

### 4a. Option A's residual: the measured 79.0 µs is a floor, not a ceiling

Not a sign-off item (nothing here is invented or self-certified) — a completeness caveat that
applies everywhere §1.1–§1.3 cite Option A's cost. `bench_lnm_cost`'s `CHKPRIV` ioctl was chosen
as the closest existing shape to a logical-name translate ioctl (PCB lookup, `copy_from_user`,
`spin_lock`, compare, `copy_to_user` — verified against `src/kernel/vms_access.c:175-196`), and
it is genuinely representative of that shape. But a real translate ioctl copies a **longer** name
in, walks a **hash chain** the way `lnm_translate()` does today, and copies a **longer** value
out (up to `LNM_MAX_VALUE`, not the fixed 32 B `CHKPRIV` moves). None of that extra work is
included in the 79.0 µs mean. So **every Option-A figure in this record — 79.0 µs, the 78.5×–
88.4× ratio, the 139–245 µs/open range — is a measured lower bound.** The true cost of Option A,
if it were built, is higher. This strengthens the rejection of Option A; it does not weaken it.

Per the standing purity constraint, the following ARE self-certification risks and are **not**
self-certified here. `vms-d37` must pin each to public OpenVMS documentation or the reference
lab, and raise them for sign-off:

1. **Privilege required to define in LNM$SYSTEM and LNM$GROUP.** Believed `SYSNAM` and
   `GRPNAM` respectively; pin to the documented privilege list before the executive enforces it.
2. **Status returned when the logical name table / quota is exhausted.** `SS$_EXLNMQUOTA` is
   the expected name but **does not exist in `src/libvms/include/ssdef.h` today**, so it would
   have to be added — and `ssdef.h:86` already carries an in-file warning that multi-source
   drift on these values is live (`SS$_NOSUCHDEV` 2312 vs 2680). Pin the numeric value; do not
   invent one.
3. **SYSGEN parameter names governing table sizing** (`LNMSHASHTBL` / `LNMPHASHTBL` class).
   If OVMX exposes them, the names and semantics come from the documented parameter set.
4. **`SS$_NOSUCHDEV` = 2680** as used on the no-executive path — inherited from the existing
   `sys_lock.c` behaviour, and already flagged in-file as contested. Not introduced by this
   ruling, but `vms-d37` should not deepen the dependency without sign-off (tracked by
   `vms-c90`).

---

## 5. Summary table

**Reading this table: MEASURED, DERIVED and PROJECTED are three different claims, and are never
merged into one label.** MEASURED means a number `bench_lnm_cost` produced directly against real
`vms.ko`/`/dev/vms` (n=5 trials/boot, §1.1) — but MEASURED here is always an **observed range
over multiple boots, not a single point estimate and not a proven bound**; round 3 exists
because an earlier draft quoted a MEASURED figure to more precision than 11 boots of data
support (§1.1). DERIVED means a number obtained by combining two
*separately* measured quantities — neither program prints it as a single figure — for example
`bench_lnm_cost`'s per-translation cost multiplied by `bench_lnm_peropen`'s K (§1.2); the
previous draft labelled one such row MEASURED and that was wrong, corrected below. PROJECTED
means an estimate for a read path that is **not built yet** — it borrows a measured number from
a different, structurally-similar code path as its estimate, per §2.4's reasoning, and must be
checked against a real measurement once `vms-d37` builds it (§3.5).

| | A: ioctl/translate | B: cache + invalidation | C: file `MAP_SHARED` | **C-corrected: `mmap(/dev/vms)`** |
|---|---|---|---|---|
| cost per translation | **~72–83 µs per-boot mean, ~81×–97× per-boot mean vs in-process, union of within-boot trial brackets 75.7×–104.5× (11 boots, n=5 trials/boot, §1.1) — MEASURED, observed range, NOT a bound.** No single boot's tight n=5 bracket bounds this, and the range has widened every round it has been checked (§1.1). CHKPRIV proxy; a genuine **lower bound**, not a ceiling (§4a) | 0.75–1.01 µs across 6 boots (mean ~0.88 µs, §1.1) — MEASURED, observed range, NOT a settled constant (reuses today's in-process lookup unmodified); **~72–83 µs — PROJECTED** for the refill after any DEFINE (assumed to cost what A's ioctl costs, since a refill is also an executive round trip) | 0.75–1.01 µs class — **PROJECTED.** Reuses today's in-process lookup as an estimate; C as literally proposed is unimplemented | **0.75–1.01 µs class — PROJECTED, NOT MEASURED.** The seqlock/arena read this option needs does not exist yet (§2.4). `vms-d37` must re-run this benchmark against the built read path (§3.5) before treating this as confirmed |
| cost per mean open (K=1.83) | **~132–152 µs class — DERIVED** (Row 1's observed per-boot range × K=1.83, where K comes from the *separate* `bench_lnm_peropen` program, §1.2 — not a single number either program printed; floor — §4a) | ~1.6 µs steady — MEASURED (range, not point); ~132–152 µs — PROJECTED after a DEFINE (same DERIVED class as option A's row) | ~1.6 µs — PROJECTED | **~1.6 µs — PROJECTED, NOT MEASURED** |
| works with no `/dev/vms` | no (correct) | no (correct) | **yes (wrong)** | **no (correct)** |
| silent-wrong-answer failure mode | none | **missed invalidation** | none | none |
| write protection | executive | executive | **none** | **MMU, read-only** |
| cross-process locking needed | none | none | robust mutex / futex | **none** |
| needs compact shared record | yes | yes | yes | yes (common cost) |
| new kernel surface | ioctls | ioctls + notify | none | ioctls + `->mmap` |

**Ruled: C-corrected.** LNM$PROCESS stays per-process. The ruling does not depend on the
PROJECTED cells turning out exactly right — it depends on C-corrected having **no syscall on the
hot path at all** (§2.4), which is true by construction regardless of the projected number's
precision. The projected cells exist so `vms-d37`'s post-build re-measurement (§3.5) has
something concrete to confirm or contradict.
