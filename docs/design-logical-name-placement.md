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
**rejected on measured cost**: **79.0 µs mean (n=5 trials, range 76.2–81.6 µs)** per executive
round trip against **0.94 µs mean** for the in-process four-table translate it would replace —
**84.5× mean (range 78.5×–88.4×)** — on the only runtime target OVMX has. That 79.0 µs is itself
a measured **lower bound** for a real translate ioctl (§1.1, §4a) — the true cost of option A is
higher still. Option B (kernel-owned + per-process cache with invalidation) is **rejected on failure
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

Reproduce: `podman build -f tests/qemu/Dockerfile -t ovmx-ktest .` then
`podman run --rm ovmx-ktest`, and read the `bench_lnm_cost` section of the serial log.

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

Multiplying the two measurements — using the n=5 trial **range**, not a single point estimate,
on the runtime target as it exists today:

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
output CALIBRATION). `getppid`/`enotty` below are single-sample context; `lnm_hit_s` is the same
n=5 trial protocol as §1.1, native this time:

```
  [getppid]   syscall(SYS_getppid) - syscall floor                    64.0 ns/op
  [enotty]    ioctl(non-vms fd) -> ENOTTY - dispatch floor            70.2 ns/op
  [lnm_hit_p] lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)         29.4 ns/op
  [lnm_hit_s] lnm_translate FILE_DEV, hit LNM$SYSTEM (4 tbl), native  61.2 ns/op  [59.9, 63.0] (n=5)
```

| | QEMU TCG | native host | TCG inflation |
|---|---|---|---|
| syscall floor (`getppid`, single sample) | 40190.5 ns | 64.0 ns | **628×** |
| ioctl dispatch (`ENOTTY`, single sample) | 39121.3 ns | 70.2 ns | **557×** |
| in-process 4-table translate (n=5 mean, both sides) | 935.8 ns | 61.2 ns | **15.3×** |

So TCG inflates **kernel entry ~39× more than it inflates userspace compute** (average of
628×/557× ÷ 15.3× ≈ 38.8). Correcting the measured mean 84.5× by that differential projects an
executive ioctl at roughly **2.2× the in-process translate** on an accelerated (KVM or
bare-metal) runtime — i.e. about **+72 ns per translation** (using the native 61.2 ns baseline,
the relevant one for an accelerated runtime), **+132 ns per mean open** (K = 1.83). That is a
tolerable **~15 % tax**, not a catastrophe. This projection is directional, not a headline figure
this ruling is quoted against — it is not held to the same n≥5 QEMU-target precision bar as
§1.1's `vms_inout`/ratio, which is why it is kept in a caveat section rather than the summary
table's ruled-option cell (§5, and see the labeling fix there).

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
0.94 µs (QEMU, n=5 mean) / 61.2 ns (native, n=5 mean) measured today for `lnm_hit_s` — instead of
the 79.0 µs (QEMU, n=5 mean) measured for option A's ioctl round trip. **This is a projection
about an unbuilt read path, not a measurement of it; do not read "0.94 µs class" in this
paragraph, or the C-corrected cell of the §5 summary table, as something that was measured.**
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
in-process baseline in §1.1 (0.94 µs class, not 79–90 µs class) — this is the point where §2.4's
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

**Reading this table: MEASURED and PROJECTED are not the same claim, and are never merged into
one label again after the previous draft blurred them in this exact table.** MEASURED means a
number `bench_lnm_cost` produced against real `vms.ko`/`/dev/vms` (n=5 trials, §1.1). PROJECTED
means an estimate for a read path that is **not built yet** — it borrows a measured number from
a different, structurally-similar code path as its estimate, per §2.4's reasoning, and must be
checked against a real measurement once `vms-d37` builds it (§3.5).

| | A: ioctl/translate | B: cache + invalidation | C: file `MAP_SHARED` | **C-corrected: `mmap(/dev/vms)`** |
|---|---|---|---|---|
| cost per translation | **79.0 µs mean, range 76.2–81.6 µs (n=5) — MEASURED.** CHKPRIV proxy; a genuine **lower bound**, not a ceiling (§4a) | 0.94 µs steady-state — MEASURED (reuses today's in-process lookup unmodified); **~79 µs — PROJECTED** for the refill after any DEFINE (assumed to cost what A's ioctl costs, since a refill is also an executive round trip) | 0.94 µs class — **PROJECTED.** Reuses today's in-process lookup as an estimate; C as literally proposed is unimplemented | **0.94 µs class — PROJECTED, NOT MEASURED.** The seqlock/arena read this option needs does not exist yet (§2.4). `vms-d37` must re-run this benchmark against the built read path (§3.5) before treating this as confirmed |
| cost per mean open (K=1.83) | **139–149 µs — MEASURED** (range, n=5; floor — §4a) | ~1.72 µs steady — MEASURED; ~139–149 µs — PROJECTED after a DEFINE | ~1.72 µs — PROJECTED | **~1.72 µs — PROJECTED, NOT MEASURED** |
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
