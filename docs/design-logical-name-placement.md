# Where do LNM$SYSTEM / LNM$GROUP / LNM$JOB live? — design record + ruling

> **Item:** `vms-ln0` (Phase 3 of the executive retrofit, `vms-6b8`, under the authenticity
> pillar `vms-898`).
> **Status:** RULED — pending operator sign-off (rd gate on `vms-ln0`).
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
**rejected on measured cost**: 90.1 µs per executive round trip against 0.93 µs for the
in-process four-table translate it would replace — **96×** — on the only runtime target OVMX
has. Option B (kernel-owned + per-process cache with invalidation) is **rejected on failure
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
present and answering:

```
  [clock]     clock_gettime pair (noise floor)                       969.0 ns/op
  [getppid]   syscall(SYS_getppid) - syscall floor                 45016.7 ns/op
  [enotty]    ioctl(non-vms fd) -> ENOTTY - dispatch floor         46468.7 ns/op
  [vms_out]   ioctl(/dev/vms, GETMODE) - exec round trip, out only 68502.0 ns/op
  [vms_inout] ioctl(/dev/vms, CHKPRIV) - exec round trip, in+out   90099.5 ns/op
  [lnm_hit_p] lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)        461.7 ns/op
  [lnm_hit_s] lnm_translate FILE_DEV, hit LNM$SYSTEM (4 tbl)         934.4 ns/op
  [lnm_miss]  lnm_translate FILE_DEV, miss all 4 tables              942.3 ns/op

  exec ioctl (out only) / in-process 4-table translate = 73.3x
  exec ioctl (in+out)   / in-process 4-table translate = 96.4x
```

`CHKPRIV` is the figure the ruling uses. It is the closest shape `vms.ko` currently offers to
what a logical-name translate ioctl would do — PCB lookup, `copy_from_user`, `spin_lock`,
consult executive state, `copy_to_user` — and it is still a **lower bound**, because a real
translate ioctl copies a longer name in, walks a hash chain, and copies a longer value out.
Nothing here is rounded up by guess.

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

Multiplying the two measurements, on the runtime target as it exists today:

| | per translation | K = 1.83 (mean open) | K = 3 (system-image open) |
|---|---|---|---|
| Option A (ioctl) | 90.1 µs | **165 µs / open** | **270 µs / open** |
| in-process (today) | 0.93 µs | 1.71 µs / open | 2.80 µs / open |

For scale: the guest's own minimal syscall floor is 45 µs. Option A would make logical-name
translation cost **~4× an entire file open's syscall budget**, and would make it the dominant
term in every `$OPEN`, every image activation, and every DCL command that touches a file.

### 1.4 Honest caveat: QEMU TCG inflates syscalls, and by how much

There is no `/dev/kvm` on the measuring machine and CI runs the QEMU suite inside a container,
so the guest is emulated with TCG. TCG does **not** inflate everything uniformly, and pretending
the 96× is a hardware number would be dishonest. The identical binary was therefore run natively
on the host (`--calibrate`, which explicitly skips the `/dev/vms` cases and labels its output
CALIBRATION):

```
  [getppid]   syscall(SYS_getppid) - syscall floor                    63.4 ns/op
  [enotty]    ioctl(non-vms fd) -> ENOTTY - dispatch floor            67.7 ns/op
  [lnm_hit_p] lnm_translate FILE_DEV, hit LNM$PROCESS (1 tbl)         32.7 ns/op
  [lnm_hit_s] lnm_translate FILE_DEV, hit LNM$SYSTEM (4 tbl)          57.9 ns/op
```

| | QEMU TCG | native host | TCG inflation |
|---|---|---|---|
| syscall floor (`getppid`) | 45016.7 ns | 63.4 ns | **710×** |
| ioctl dispatch (`ENOTTY`) | 46468.7 ns | 67.7 ns | **686×** |
| in-process 4-table translate | 934.4 ns | 57.9 ns | **16.1×** |

So TCG inflates **kernel entry ~43× more than it inflates userspace compute**. Correcting the
measured 96.4× by that differential projects an executive ioctl at roughly **2.3× the
in-process translate** on an accelerated (KVM or bare-metal) runtime — i.e. about **+73 ns per
translation, +134 ns per mean open**. That is a tolerable 5–15 % tax, not a catastrophe.

**Both numbers matter, and they point the same way:**

- On an accelerated runtime, option A is affordable but is a permanent tax on the hottest path
  in the system, bought for **zero correctness benefit** over the ruled option.
- On the runtime OVMX actually has today — unaccelerated QEMU, which is what CI runs and what
  developers boot — option A costs 165–270 µs per open and is disqualifying.

A design that is only tolerable on hardware nobody in the project currently runs is not a
design; it is a bet. The ruled option is fast on both.

---

## 2. The options, and why C-corrected wins

### 2.1 Option A — kernel-owned, ioctl per translation

Rejected. Simplest and most consistent with the rest of the executive, and correctness is not
in question — but §1.3 prices it at 165–270 µs per file open on the runtime target. Its only
advantage over the ruled option is that it needs no `->mmap` in `vms.ko`, which is perhaps
80 lines of kernel code.

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

And it keeps what made C attractive: **translation performs no syscall**, so the hot path costs
what it costs today (0.93 µs in QEMU, 58 ns native) instead of 90.1 µs.

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
the 90 µs already measured. The compact, offset-addressed record is therefore **common cost**,
not a differentiator, and it should be built first because A, B and C all need it.

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

Re-run `tests/qemu/bench_lnm_cost` and `bench_lnm_peropen` after the change. The expected
result is that per-translation cost is **unchanged** from the in-process baseline in §1.1
(0.93 µs class, not 90 µs class). If it lands in the 90 µs class, the read path is going
through an ioctl and the ruling has been implemented as option A by accident.

---

## 4. Purity — values that need operator sign-off before they are written

Per the standing purity constraint, these are **not** self-certified here. `vms-d37` must pin
each to public OpenVMS documentation or the reference lab, and raise them for sign-off:

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

| | A: ioctl/translate | B: cache + invalidation | C: file `MAP_SHARED` | **C-corrected: `mmap(/dev/vms)`** |
|---|---|---|---|---|
| cost per translation (QEMU, measured) | 90.1 µs | 0.93 µs steady, 90.1 µs after any DEFINE | 0.93 µs class | **0.93 µs class** |
| cost per mean open (K=1.83) | 165 µs | ~1.7 µs steady | ~1.7 µs | **~1.7 µs** |
| works with no `/dev/vms` | no (correct) | no (correct) | **yes (wrong)** | **no (correct)** |
| silent-wrong-answer failure mode | none | **missed invalidation** | none | none |
| write protection | executive | executive | **none** | **MMU, read-only** |
| cross-process locking needed | none | none | robust mutex / futex | **none** |
| needs compact shared record | yes | yes | yes | yes (common cost) |
| new kernel surface | ioctls | ioctls + notify | none | ioctls + `->mmap` |

**Ruled: C-corrected.** LNM$PROCESS stays per-process.
