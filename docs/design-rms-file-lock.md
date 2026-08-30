# Design — RMS file-level share arbitration behind the DLM (vms-50e, vms-407-A)

> The DLM engine's first real client. RMS's FAB access/share intent
> (`fab$b_fac` / `fab$b_shr`) today reaches NO arbitrator — two accessors of one
> file are serialised by nothing RMS does (`rms_core.c` says so in its own header).
> This wires it to the REAL lock manager: at `sys$open`, RMS takes a file-access
> `$ENQ`; a conflict returns `RMS$_SHR`; `sys$close` `$DEQ`s it. No userspace lock
> table, no `flock` — the executive lock manager (`vms.ko`, via `vms_kif_enq`) is
> the only arbitrator (INV-6).

## Clean-room provenance (Rule 8)
The access/share COMPATIBILITY semantics ("two accessors are compatible iff each
one's access is permitted by the other's sharing") are public — Guide to OpenVMS
File Applications, and the `$ENQ`/$DEQ lock-management interface. The **mapping to
a specific 6-mode lock** below is OVMX's construction over the public
lock-compatibility matrix; it reproduces the documented compatibility for the
common cases and is labelled an OVMX design choice where VMS's internal RMS
lock scheme is not publicly byte-specified. No VSI binary disassembled.

## The lock

**Resource name** — the file's identity, so two opens of the SAME file contend on
the SAME resource and different files never do. Built from the FID the ACP access
returns (`vms_acp_access_args.fid_num/nmx/seq/rvn`, already captured into the
internal file handle in `rms_core.c`): `"RMS$" + hex(fid_num|nmx<<16) +
"." + hex(fid_seq) + "." + hex(fid_rvn)` — ≤ 32 chars (the DLM resnam width).
(The RVN keeps it correct on a bound volume set.)

**Lock mode** — from `fab$b_fac` (access) and `fab$b_shr` (share), against the
executive's 6-mode matrix (NL,CR,CW,PR,PW,EX):

| accessor intent | mode |
|---|---|
| no sharing at all (`shr` == `FAB$M_NIL` or `shr` == 0 for a write open) | **EX** |
| read only (`fac` = GET, no write bits), does NOT tolerate writers (no SHRPUT/SHRUPD) | **PR** |
| read, tolerates concurrent writers (SHRPUT or SHRUPD set) | **CR** |
| write (PUT/UPD/DEL/TRN), allows concurrent writers (SHRPUT or SHRUPD) | **CW** |
| write, does NOT allow concurrent writers | **PW** |

Why this is faithful against the matrix:
- EX conflicts with everything → exclusive access blocks all other accessors.
- PR/PR compatible → two strict readers coexist; PR vs EX/PW/CW conflict → a
  strict reader excludes any writer.
- CW/CW compatible → shared writers coexist at the FILE level (individual record
  conflicts are arbitrated by RECORD locking, vms-0dd/part B).
- CR compatible with all but EX → a tolerant reader coexists with a writer that
  permits it (CR/CW = CR/PW = 1).
- PW/PR = 0 → a writer that does not share write excludes a strict reader; PW/CR =
  1 → but admits a tolerant one. Exactly the documented sharing behaviour.

## The seam (`src/vmsrms/rms_core.c`)

Store the file-access lock id in the internal file handle (the `_rms_file` /
FAB internal region), one lkid per open instance.

- **`sys$open` / `sys$create`**, immediately AFTER the file's `IO$_ACCESS`
  succeeds (where `vms_kif_acp_access` already runs and the FID is in hand):
  1. `mode = rms_fileshare_mode(fab$b_fac, fab$b_shr)`.
  2. `st = vms_kif_enq(efn, mode, LCK_M_NOQUEUE, resnam, &lkid, ...)` — NOQUEUE so
     a conflict returns immediately rather than blocking (`RMS$_SHR` is an error,
     not a wait, on this slice; a timed wait is a later refinement).
  3. On grant: stash `lkid` in the handle; the open proceeds.
  4. On a conflict (`$ENQ` returns not-granted / `SS$_NOTQUEUED`): `IO$_DEACCESS`
     the file just accessed and return **`RMS$_SHR`** — the accessor is turned
     away by a REAL executive lock, never a userspace flag.
  5. If `/dev/vms` is absent, `vms_kif_enq` returns its honest `SS$_` error;
     surface it (do NOT fall back to a local allow — INV-6 / Rule 9). No `flock`.
- **`sys$close`**: `vms_kif_deq(lkid, ...)` releasing the file-access lock (before
  or after `IO$_DEACCESS`), then clear the stashed lkid.

`sys$erase`'s missing interlock and `sys$create`'s share intent ride the same
helper; the minimal slice proves `sys$open`/`sys$close`.

## Proof (real `/dev/vms`, INV-6)

Two RMS opens of the SAME file (same FID → same resource):
- **Conflicting share** — e.g. open #1 `fac=PUT, shr=0` (EX) then open #2 `fac=GET`
  (any): #2 returns **`RMS$_SHR`** from a real `$ENQ` conflict; `$GETLKI` on #1's
  lkid shows a real granted EX lock on the FID resource.
- **Compatible share** — open #1 `fac=GET, shr=SHRGET` (PR) and open #2 the same
  (PR): BOTH open; two real granted PR locks coexist on the resource.
- Close #1 → its lock is `$DEQ`'d (GETLKI no longer finds it); a previously-denied
  conflicting open now succeeds.
- With `/dev/vms` absent the open fails honestly (the `vms_kif_enq` error), never
  a silent local allow.

Two processes opening one file on one node exercises the single-node lock manager
(the same executive lock manager the cluster DLM extends); because the resource is
FID-named it masters and arbitrates cross-node unchanged when the file lives on a
shared volume — the cluster case is the same lock, no RMS change.

## Reconciliation
No new SCS opcode, ioctl, or wire send — RMS calls the EXISTING `vms_kif_enq`/`deq`.
So no census / NetBSD-mirror / `render_compat` needed. The one requirement is
ground-source testing: a real-`/dev/vms` test that proves the conflict + the
compatible coexistence + release, wired into the RMS/QEMU test path.

## Out of scope (part B, vms-0dd — blocked by this)
Record-level locking: `RAB$M_RLK/NLK/ULK` + `rab$l_rop` → a per-record `$ENQ`
named by RFA/VBN, taken UNDER the file-access lock as parent (`parid`), returning
`RMS$_RLK` on a held record — threaded through `$get`/`$put`/`$update`/`$delete`
in `rms_seq.c`/`rms_rel.c`/`rms_idx.c`.
