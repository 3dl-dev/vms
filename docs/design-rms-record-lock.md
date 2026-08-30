# Design — RMS record-level locking behind the DLM (vms-0dd, vms-407-B)

> Completes RMS-behind-DLM: after vms-50e wired FILE-level share arbitration, a
> RAB's record-locking intent (`rab$l_rop`) now takes a per-record `$ENQ` on the
> real executive lock manager — as a CHILD of the file-access lock — so two RABs
> contending for one record are arbitrated by the real DLM, not by nothing. No
> userspace record table (INV-6).

## Two halves, one outcome
1. **Executive parent_id linkage** (DLM engine, this item): `vms_kif_enq` already
   carries `parid`; the executive now HONORS it — `vms_lock_entry.parent_id` is set
   from `vms_enq_args.parid` at creation and reported by `$GETLKI`. Purely additive:
   every existing `$ENQ` passes `parid=0` (a root lock), unchanged. The
   parent-child AUTO-RELEASE cascade is deferred (vms-489) — RMS releases records
   explicitly, so the done-condition doesn't need it.
2. **RMS record-lock wiring** (RMS internals): the seam below.

## The record lock

**Resource name** — per (file, record), so two streams on the SAME record contend
on the SAME resource: from the file FID + the record's RFA (`rab$w_rfa`, set by the
org handler when it locates the record). E.g. `"RMSR" + hex(fid_num|nmx<<16) + "."
+ hex(rfa page) + "." + hex(rfa offset)`, ≤ 32 chars. Distinct from the file lock's
`"RMS$"+hex(fid)` resource.

**Parent** — `parid = fab->_rms_file->access_lkid` (the file-access lock from
vms-50e). The record lock is a CHILD of the file lock, so `$GETLKI` on the record
lkid reports `parent_id` = the file lock — "the record held UNDER its file lock".

**Behavior from `rab$l_rop`** — CRITICAL: `RAB$M_NLK`/`RAB$M_RLK` are NOT lock
modes, they are read MODIFIERS (Guide to OpenVMS File Applications; the RMS status
codes below are the oracle). Getting this wrong is a plausible-but-wrong mapping —
the exact class the fidelity program exists to kill. The DOCUMENTED semantics:

| rop on a `$get` | what it means | DLM realization |
|---|---|---|
| neither (default) | LOCK the record so it can be `$update`/`$delete`d | take a real **EX** `$ENQ` on the record (parid = file lock). Another stream's default `$get` of that record → `RMS$_RLK`. |
| `RAB$M_NLK` (0x0080) | "no lock" — retrieve the record WITHOUT locking it | take **NO** `$ENQ`. A non-locking read; the stream holds nothing, so it never blocks and is never blocked. Returns `SS$_NORMAL`. |
| `RAB$M_RLK` (0x0100) | "read locked record" — read the record EVEN IF another stream has it LOCKED (read-through), and don't lock it yourself | take no lock; PROBE the record's real lock state (a `LCK_M_NOQUEUE` EX `$ENQ`: if it is refused `SS$_NOTQUEUED`, the record is genuinely locked by another → read the data + return **`RMS$_OK_RLK`**; if it grants, `$DEQ` it immediately — RLK holds nothing — and return `SS$_NORMAL`). The OK_RLK-vs-NORMAL distinction is decided by REAL DLM state, never a userspace guess (INV-6). |
| `$put` / `$update` / `$delete` | lock the target record exclusively | **EX** `$ENQ`; `$update`/`$delete` operate on the record the stream already holds locked from its prior `$get`. |

Status codes are the oracle: `RMS$_RLK` (98986, "record locked" — the conflict a
default `$get` hits on a record another stream locked); `RMS$_OK_RLK` (98337,
"record successfully read, record locked" — an RLK read-through of a locked
record); `RMS$_OK_WAT` (98401, honored only if a wait is requested — the minimal
slice uses NOQUEUE). OVMX's `rab.h` defines only `NLK` and `RLK` (no `ULK`/`WAT`
bits), so those two + the default are this rung's surface.

So the core conflict is DEFAULT-vs-DEFAULT (both take EX, second → `RMS$_RLK`); RLK
and NLK are the read-modifier VARIANTS that must ALSO be proven, not just the
default conflict.

## The seam — `src/vmsrms/rms_record.c` (dispatch level)

One record lock per stream (the "current record" lock), so the logic is central in
`rms_impl_get`/`rms_impl_put`/`rms_impl_update`/`rms_impl_delete`, not duplicated
across `rms_seq/rel/idx.c`. The RAB tracks its current record lkid (a field on the
RAB internal stream state, mirroring `FAB._rms_file->access_lkid`).

- **`rms_impl_get`**: after the org handler (`rms_seq/rel/idx_get`) locates the
  record and sets `rab$w_rfa`, RELEASE the stream's previous record lock (if any),
  then — unless `RAB$M_NLK` — `vms_kif_enq(mode_from_rop(rop), LCK_M_NOQUEUE,
  record_resnam, parid=access_lkid, ...)`. Grant → stash the lkid on the stream and
  return normally (or `RMS$_OK_RLK` when the read succeeded on a record the caller
  read-locked). A real conflict (`SS$_NOTQUEUED`) → **`RMS$_RLK`**. `/dev/vms`
  absent → the honest `SS$_` (the ACP-present path only; the vms-5f0 POSIX body has
  no lock), never a userspace record table.
- **`rms_impl_update` / `rms_impl_delete`**: operate on the record the stream
  currently holds locked (its stashed lkid) — the prior `$get` took the EX lock; if
  the stream holds no lock on the target, that is `RMS$_CUR`/`RMS$_RLK` per VMS. Do
  NOT silently write an unlocked record.
- **`rms_impl_put`**: take an EX lock on the new record's RFA (a fresh record
  nobody else can name yet, but honor it uniformly), release on the next op.
- **Release**: the next `$get`/`$find` releases the prior record lock; an explicit
  `$free`/`$release`, `RAB$M_NLK`, and `sys$disconnect`/`sys$close` release the
  held record lock. `$DEQ` via `vms_kif_deq`.

`RMS$_OK_WAT` (record locked, waiting) is honored when a wait is requested; the
minimal slice uses NOQUEUE (immediate `RMS$_RLK`), a timed record wait is a
refinement.

## Proof (real `/dev/vms`, PASS-not-SKIP on a kernel-exec shard)

Two RABs on one file (same FID), a real ODS-2 fixture:
- RAB1 `$get` a record (holds it); RAB2 `$get` the SAME record → **`RMS$_RLK`** from
  a real `$ENQ` conflict; `$GETLKI` on RAB1's record lkid shows a real granted lock
  whose `parent_id` == the file-access lock (the record held UNDER its file lock).
- `$update`/`$delete` on the held record succeed for the holder; a non-holder is
  refused.
- `RAB$M_NLK` reads without a lock (no `$ENQ`); an explicit release / `RAB$M_ULK` /
  next `$get` releases (getlki → `SS$_IVLOCKID`), and the previously-denied `$get`
  then succeeds.
- No `/dev/vms` → honest SKIP (77).

## Reconciliation
NO new SCS op / ioctl / wire send — reuses `vms_kif_enq`/`deq` (parid was already in
the ABI). So NO census / NetBSD-mirror-for-ops / `render_compat`. The executive
struct field `parent_id` IS mirrored in both `vms_internal.h` (Linux + NetBSD) —
the #928 twin trap. Regression net: the H0→H11 DLM harness + vms-50e file-lock
(both parentless, `parid=0`) must stay green — the linkage must not change the
no-parent path.

## Deferred (Rule 5, filed)
- **vms-489** — parent-child auto-release cascade (file-lock release → child record
  locks). RMS releases explicitly today, so not needed for this rung.
