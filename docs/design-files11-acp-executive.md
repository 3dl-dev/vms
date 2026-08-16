# Design record: Files-11 (ODS-2) as a genuine ACP/XQP in the executive

Status: **RATIFIED ARCHITECTURE (operator ruling 2026-08-16) — design only,
not yet built.** This record defines the target and decomposes the build-out.
It **supersedes** the "Architecture A1" of `docs/design-ods2-runtime-flip.md`
(keep RMS/DCL in userspace over a block-device *adapter*) and adopts a faithful
realization of what that record called "A2" and rejected: a real Files-11
**ACP/XQP inside the executive**, reached from userspace by `$QIO` over
`/dev/vms`. The genuine ODS-2 on-disk codec (`src/vmsfs/ods2/`) is **kept** and
becomes the ACP's on-disk engine; the userspace adapter (`ods2_sysdisk.c`) and
the `vmsfs_to_linux_path -> /vms -> POSIX` passthrough are **retired**.

---

## 1. Why the userspace adapter was the wrong shape

OVMX had been building ODS-2 as a userspace library (`src/vmsfs/ods2/`) plus a
userspace *adapter* (`src/vmsfs/ods2_sysdisk.c`) that each VMS component — RMS,
DCL, MOUNT, `$SEARCH`, OPCOM, LOGINOUT — would be individually rewired to call
in place of POSIX `open`/`opendir` on the `/vms` passthrough. That is the same
class of defect the project already forbids for locks, event flags, and
mailboxes (CLAUDE.md **Rule 9 / INV-6**): an executive facility faked in
userspace, where N consumers each *opt in* to a shared library instead of the
system *having* the facility.

**A filesystem is an executive facility.** Real OpenVMS has no POSIX anywhere in
the file path: the Files-11 XQP runs in kernel mode, `$QIO` file operations go
*into* it, and RMS is layered *on* `$QIO`. FIDs, versions, ACLs, and record
attributes are native because it is Files-11 to the platter.

The symptom that proves the adapter is the wrong shape is already documented in
the runtime-flip record: the moment SYS$DISK becomes a genuine ODS-2 disk, *every
independent boot-path reader breaks at once* — IMGACT, SYSUAF, DCL `@file`, the
F$ lexicals, and PID 1 each do their own POSIX `open`/`fopen`/`mmap`/`mount(2)`
on a resolved `/vms/...` path (inventory in §6), because there is no filesystem,
only a passthrough and a pile of consumers. There is no incremental green: the
runtime-flip record found the reroute is "genuinely atomic." That atomicity is
the tell that the abstraction boundary is in the wrong place. Put the filesystem
in the executive and the boundary becomes the one VMS actually uses — `$QIO` —
and every consumer reaches files the one VMS way.

## 2. How VMS actually structures Files-11 (the model to match)

Grounded in public OpenVMS documentation (Rule 8 — no VSI/HPE source or binary
was consulted):

- **ACP → XQP.** ODS-1 used a separate **Ancillary Control Process** (a real
  process, `F11ACP`) that serviced file requests. ODS-2 replaced it with the
  **Extended QIO Processor (XQP)**, `F11BXQP`, which is **not** a separate
  process: it executes **in the context of the requesting process, in kernel
  mode**, and serializes access to on-disk structures through the **distributed
  lock manager** (a per-volume "volume synchronization" / files lock), so many
  processes can each run the XQP concurrently without a single-server
  bottleneck. (VSI OpenVMS *Guide to OpenVMS File Applications*; *OpenVMS
  Internals and Data Structures Manual*; Wikipedia "Files-11" summarizes the
  ACP→XQP transition and the FID = {NUM, SEQ, RVN} triple and reserved files
  INDEXF.SYS/BITMAP.SYS/000000.DIR/etc.)

- **The `$QIO` ACP interface.** File operations are ordinary `$QIO`s on a channel
  assigned (`$ASSIGN`) to the volume's device, using ACP function codes with
  five function-dependent parameters (VSI OpenVMS *I/O User's Reference Manual*,
  DO-DIOURM-01A, Chapter "ACP-QIO Interface";
  https://docs.vmssoftware.com/vsi-openvms-io-user-s-reference-manual/ ,
  https://vmssoftware.com/docs/VSI_IO_REF.pdf ):
  - **P1** = address of the **FIB** descriptor (File Information Block).
  - **P2** = address of the file-name string descriptor (directory ops).
  - **P3** = address of a word to receive the resultant file-name length.
  - **P4** = address of a descriptor for the resultant file-name buffer.
  - **P5** = address of the **attribute (ATR) control-block list**.

  ACP function codes (`$IODEF`), all confirmed against the manual:
  | Code | Modifiers | ACP action |
  |------|-----------|------------|
  | `IO$_ACCESS` | `IO$M_CREATE`, `IO$M_ACCESS` | search a directory for a file (by name or by FID) and/or open (access) it, building the window |
  | `IO$_CREATE` | `IO$M_CREATE`, `IO$M_ACCESS`, `IO$M_DELETE` | create a file header (allocate from INDEXF.SYS), optionally enter it in a directory, optionally access it |
  | `IO$_DEACCESS` | — | deaccess a file, writing back modified attributes |
  | `IO$_MODIFY` | — | modify attributes / extend (allocate) / truncate / write a directory entry |
  | `IO$_DELETE` | `IO$M_DELETE` | remove a directory entry and/or delete the file (deallocate header + blocks) |
  | `IO$_ACPCONTROL` | — | miscellaneous ACP control: wildcard directory context (`$SEARCH`), mount/dismount/lock-volume |
  | `IO$_READVBLK` / `IO$_WRITEVBLK` | — | virtual-block transfer against the accessed file; the ACP window maps VBN → LBN |

- **The FIB** (`$FIBDEF`, documented field-by-field in the I/O manual): `FIB$W_FID`
  = the 3-word file ID {NUM, SEQ, RVN}; `FIB$W_DID` = the directory ID;
  `FIB$L_ACCTL` = access-control flags (`FIB$V_WRITE`, `FIB$V_NOWRITE`,
  `FIB$V_WRITETHRU`, `FIB$V_NOTRUNC`, `FIB$V_DLOCK`, …); `FIB$W_NMCTL` = name
  control (`FIB$V_WILD`, `FIB$V_ALLVER`, `FIB$V_NEWVER`, `FIB$V_SUPERSEDE`);
  `FIB$L_WCC` = wildcard/directory context; `FIB$W_EXCTL`/`FIB$L_EXSZ`/
  `FIB$L_EXVBN` = extend control/size/VBN; `FIB$W_VERLIMIT` = version limit.

- **The ATR list** (`$ATRDEF`): a list of `{size, code, buffer-address}` entries.
  Key codes: `ATR$C_UCHAR` (file-characteristics longword), `ATR$C_RECATTR` (the
  32-byte record-attributes area / FAT — RFM, RAT, RSIZE, HIBLK, EFBLK, FFBYTE,
  MAXREC), `ATR$C_FPRO` (protection), `ATR$C_UIC` (owner), `ATR$C_CREDATE`/
  `_REVDATE`/`_EXPDATE`/`_BAKDATE` (64-bit dates), `ATR$C_READACL`/`_WRITEACL`/
  `_ACLLENGTH` (ACL), `ATR$C_HEADER` (whole header).

- **RMS is layered on the ACP.** `$OPEN` → `IO$_ACCESS`; `$CREATE` →
  `IO$_CREATE`; `$CLOSE` → `IO$_DEACCESS`; `$EXTEND` → `IO$_MODIFY`; `$ERASE` →
  `IO$_DELETE`. Record I/O (`$GET`/`$PUT`) is `IO$_READVBLK`/`IO$_WRITEVBLK`
  virtual-block transfers on the same channel; the ACP window maps VBN→LBN. RMS
  record structure (record format, MRS, bucket/prologue for indexed) lives in the
  file header's `ATR$C_RECATTR` (FAT) and in the file's prologue blocks — **not**
  in any side file. (VSI *Guide to OpenVMS File Applications*.)

This is the shape OVMX must match. The good news (§3) is that OVMX's executive is
already structured the XQP way.

## 3. Ground truth: OVMX already has the executive shape the XQP needs

From the current executive (`src/kernel/`, `src/kernel-core/`, `src/libvmssys/`,
`src/libvms/syssvc/`):

1. **Caller-context, no kthreads.** Every executive facility (`vms_lock.c`,
   `vms_eflag.c`, `vms_ast.c`, …) runs in the caller's `unlocked_ioctl` thread —
   the caller's *process context* — and blocking services sleep in that thread on
   a wait primitive. There are no workqueues or kernel threads. **This is
   precisely the XQP execution model**: run the file system in the requesting
   process's context in kernel mode. An ACP op is just another caller-context
   handler.

2. **`$ASSIGN`/`$QIO`/IOSB and the ACP codes already exist — with no ACP arm.**
   `src/libvms/syssvc/sys_assign.c` assigns channels (16-bit PCB slots), and
   `src/libvms/syssvc/sys_qio.c` dispatches `$QIO` **channel-class-first**
   (`vms$$chan_is_mailbox` → mailbox executive op, `vms$$chan_is_bg` → network
   BG op, else a local-fd path). `iodef.h` already defines `IO$_DELETE=3`,
   `IO$_ACCESS=4`, `IO$_MODIFY=6`, `IO$_ACPCONTROL=7`, `IO$_CREATE=9`; `fibdef.h`
   already defines a FIB (with `fib$w_fid`/`fib$l_acctl` trusted and the rest
   flagged OVMX-clean-room pending operator sign-off, item `vms-531`); `iosbdef.h`
   defines the IOSB. **What is missing is exactly one thing: an ACP arm.** There
   is no `vms$$chan_is_file` class, no FIB marshalling, and no ACP handler in the
   kernel. The ACP does not invent a transport — it fills the hole the existing
   `$ASSIGN`/`$QIO`/FIB scaffolding was cut to hold.

3. **Uniform ioctl facility pattern to clone.** `/dev/vms` is a miscdevice with a
   flat `switch(cmd)` in `vms_dev_ioctl` (`src/kernel/vms_module.c`); each handler
   is `long vms_ioctl_X(struct vms_proc *proc, unsigned long arg)` living in
   `src/kernel-core/` and doing `exec_copyin → take facility lock → work → set
   args.status → exec_copyout → return 0`. Facilities claim an ioctl `nr` band
   (`_IOWR(VMS_IOC_MAGIC 'V', nr, struct)`), init in `vms_init`, and keep
   per-process state in `struct vms_proc` (keyed by tgid, surviving execve).
   `kif_bind()` lazily opens+registers `/dev/vms`; `kif_wait_call` re-enters past
   `-EINTR` for blocking services.

4. **Identity is executive-derived (INV-6).** Registration takes no PID and no
   privilege mask from the caller; the executive derives UIC/username/privileges
   from the task's real Linux credentials. A process cannot declare its own
   identity, so an ACP can gate file protection/ACL checks on `proc->uic`/privs
   and never on anything in the request.

5. **AST completion is already a solved pull-path.** `$QIO` completion ASTs queue
   into `proc->ast[acmode]` and drain via `VMS_IOCTL_DELIVERAST` exactly like lock
   completion ASTs — the asynchronous ACP-QIO reuses this, inventing nothing.

6. **A shared, host-neutral kernel filesystem engine already exists** —
   `src/kernel-core/vmsfs/` — and is the natural substrate to build the ACP on.
   It is substrate-agnostic core code (host-neutral `struct vmsfs_volume`, a
   `vmsfs_bio.h` **block-backend abstraction** — `bget`/`bput`/`bdirty` — plus an
   allocator `vmsfs_alloc.c` (block/FID alloc+free), directory engine
   `vmsfs_dirscan.c` (resolve / add / remove / highest-version), file-header
   `vmsfs_header.c`, retrieval-map `vmsfs_map.c`, name, and version modules)
   compiled against **two** backends today: Linux (`src/kernel/vmsfs/`, backing
   `vmsfs.ko` via `mount_bdev`/`sb_bread`/buffer_head/page-cache) **and NetBSD-vax**
   (`src/kernel-netbsd/vmsfs/`). Two facts matter for placement: (a) the block-I/O
   plumbing and a working allocator/directory/map model are already proven in
   kernel mode against a real `struct block_device`; and (b) because the core lives
   in the *shared* substrate-agnostic layer, anything landed there is inherited by
   **both** the Linux and NetBSD-vax SYSKRNLs at once. Its *on-disk format* is an
   OVMX invention (`"VMFS"`/`"VFH2"`, Unix-epoch timestamps — explicitly **not**
   ODS-2), so it is not itself the ACP; but its backend abstraction is exactly the
   block-I/O seam the userspace ODS-2 codec lacks. The ACP builds on this engine —
   binding the genuine ODS-2 codec to `vmsfs_bio.h` and replacing the bespoke-format
   guts (§4.1, §4.4).

## 4. The OVMX Files-11 ACP architecture

### 4.1 Placement — in `vms.ko`, in caller (ioctl) context. Decision and alternatives.

**Decision: the ACP is executive-resident in `vms.ko`, built on the existing
shared kernel filesystem engine** (`src/kernel-core/vmsfs/`), *not* greenfield and
*not* a VFS module. The ACP is the existing shared core **taught real Files-11
ODS-2** (the bespoke `VMFS`/`VFH2` on-disk guts replaced by the genuine ODS-2 codec
bound to the core's `vmsfs_bio.h` backend, §4.4) plus a **new `$QIO`/FIB/channel
front-end** into `vms.ko` (§4.2) that drives it in the **caller's ioctl/process
context**, serialized by the existing distributed lock manager (`vms_lock.c`) via a
per-volume synchronization lock. This is the faithful XQP model, the lowest-friction
fit to the executive OVMX already has (§3), and — because `src/kernel-core/` is the
substrate-shared layer — it lands the ACP for **both** the Linux and NetBSD-vax
SYSKRNLs at once (multi-substrate, epic `vms-8e8` / SYSKRNL commission).

**Build-on-existing vs greenfield.** The shared core already carries the two
hardest kernel pieces — a proven block-backend abstraction and a working
allocator/directory/retrieval-map model in kernel mode — but in the wrong on-disk
*format*. "Extend it" therefore means teaching it real Files-11, which is most of
the on-disk work regardless of where it lands, so building on the shared core is
strictly cheaper than greenfield: keep `vmsfs_bio.h` and the multi-substrate
wiring; swap the format guts for the real-VAX-validated ODS-2 codec (avoiding a
from-scratch on-disk rewrite the codec already did clean-room). The `$QIO`/channel
front-end and the DLM volume lock are the genuinely new parts.

Alternatives considered and rejected:

| Option | What it is | Verdict |
|--------|-----------|---------|
| **A. Caller-context ACP in `vms.ko`, on the shared `kernel-core/vmsfs` engine** (chosen) | ODS-2 logic runs in the ioctl thread, serialized by the DLM volume lock; on-disk format = the ODS-2 codec bound to the existing shared block-backend | **Faithful** (this *is* the ODS-2 XQP), reuses `struct vms_proc`/AST/lock/exec_* machinery + the proven multi-substrate FS engine, no new scheduling surface, inherited by both SYSKRNLs |
| B. Separate kernel-thread / server-process ACP | An `F11ACP`-style single server thread services a request queue | This is the **ODS-1 model VMS abandoned**; single-server serialization is a bottleneck and adds a scheduling/queue surface the executive does not otherwise have. Rejected. |
| C. User-mode ACP process (mailbox/channel RPC) | File ops marshalled to a userspace daemon | Re-introduces the exact userspace-facility LARP Rule 9/INV-6 forbid; not how ODS-2 works. Rejected. |
| D. Real Linux VFS filesystem (grow `vmsfs.ko` into ODS-2, mount at `/vms`, RMS uses POSIX) | The A1-shaped path | Puts **POSIX back in the file path** — the thing being killed — and Linux VFS/inodes cannot natively carry FIDs, version chains, VMS ACLs, or `$QIO` FIB/ATR semantics: it bends Files-11 to VFS, the mirror image of bending VMS to POSIX. Rejected as the VMS path. (A read-only VFS *presentation* of the volume for Linux host tooling may exist later as a **secondary, non-authoritative** consumer — never the path RMS/DCL use.) |
| E. Companion module `files11.ko` calling into `vms.ko` | Same logic, separate `.ko` | Pure packaging; costs a cross-module ABI for the lock manager, `struct vms_proc`, and AST queues the ACP must share. Keep it one module, one source subtree. Rejected on cost, not correctness. |

The XQP is not a driver or a process in ODS-2; it is executive code run in the
caller's context. Option A is the only one that reproduces that.

### 4.2 The channel + `$QIO` ACP interface

**Reuse `$ASSIGN`/`$QIO`; add the missing ACP arm.**

- **New channel class.** Add `vms$$chan_is_file` / a `PCB_CHAN_FILE` flag and an
  `exec_chan` to disk-device channels, alongside the existing mailbox/BG classes.
  `$ASSIGN "DKA0:"` (or the discovered SYS$SYSDEVICE) returns a channel bound to
  the mounted ODS-2 volume in the executive — not a Linux fd.
- **New ioctl band `0x60–0x6F`** on `/dev/vms`, e.g. `VMS_IOCTL_ACP_ACCESS`,
  `_ACP_CREATE`, `_ACP_DEACCESS`, `_ACP_MODIFY`, `_ACP_DELETE`, `_ACP_CONTROL`,
  `_ACP_READVB`, `_ACP_WRITEVB`, plus `_ACP_MOUNT`/`_ACP_DMOUNT`. Each carries a
  flat, fixed-width, `_Static_assert`-sized `struct vms_acp_*_args` (the executive
  convention). `sys_qio.c` marshals the caller's FIB (P1), name descriptor (P2),
  resultant-name (P3/P4), and ATR list (P5) into these structs and issues the
  ioctl through `kif_call`/`kif_wait_call` (the latter when the op may block on
  the volume lock). Completion fills the IOSB; an `astadr` queues into
  `proc->ast[acmode]`.
- **Function → handler map** is §2's table, each implemented as
  `vms_ioctl_acp_*(struct vms_proc *proc, unsigned long arg)` in
  `src/kernel-core/vmsfs_acp.c`, calling the ported codec. Protection/ACL checks
  gate on `proc->uic`/privs (INV-6).
- **Window model.** On `IO$_ACCESS`/`IO$_CREATE` the ACP builds a *window* — the
  file's VBN→LBN retrieval-pointer map, decoded by the codec's
  `ods2_fh2_map_walk` — and stores it in the channel's per-process ACP sub-state
  in `struct vms_proc` (a new sub-struct, guarded by its own lock, like `ast[4]`).
  `IO$_READVBLK`/`IO$_WRITEVBLK` translate {VBN, byte-offset, length} through the
  window to block I/O on the backing device. A write past EOF triggers an implicit
  extend (BITMAP.SYS allocation + window/HIBLK update), matching RMS `$EXTEND`.

**Clean-room note (Rule 8).** FIB and ATR field layouts come only from the VSI
I/O manual and `$FIBDEF`/`$ATRDEF`. Where the manual does not publish a byte
offset, OVMX defines its own arg-struct layout and labels it an **OVMX design
choice** — the wire is OVMX's own ioctl struct, not a claim of VMS byte-fidelity;
only the *on-disk* ODS-2 structures are byte-authentic (via the codec). This is
the resolution path for `fibdef.h`'s pending sign-off (`vms-531`).

### 4.3 The mounted-volume model (executive-global, replacing the per-process hack)

A `$MOUNT` (or `IO$_ACPCONTROL` mount) hands the executive a backing block device;
the ACP reads and validates the home block + SCB (codec `ods2_home_parse` /
`ods2_scb_parse` — checks `"DECFILE11B "`), and records the volume in an
**executive-global mounted-volume table** (device → `struct block_device` + volume
state + volume-lock resource). Because it lives in the executive, **every** process
that `$ASSIGN`s the device sees the same mounted volume — this deletes the
userspace adapter's per-process `OVMX_SYSDISK_DEV` lazy-registration workaround
(runtime-flip §0), which existed only because a userspace `ods2_bdev_t` fd cannot
be shared across processes. SYS$SYSDEVICE is a discovered logical bound to the boot
unit (device-native naming, epic `vms-47d`).

### 4.4 Hosting the ODS-2 codec executive-side

The codec (`src/vmsfs/ods2/`) was designed for this move; the kernel-safety
assessment (from a full scan) is **low–moderate, cleanly partitioned**:

- `ods2_reader.c` — already kernel-clean (pure parsing, `<string.h>` only, no
  allocation/IO/statics; its header comment already anticipates "the kernel-side
  MSCP server path"). **Drop-in.**
- `ods2_bdev.c` — replace the two POSIX sites (`pread`, `lseek(SEEK_END)`) by
  **binding to the shared core's existing `vmsfs_bio.h` backend** (`bget`/`bput`/
  `bdirty` over `struct vmsfs_volume`), which already resolves to `sb_bread`/bio on
  Linux and to the NetBSD backend on VAX. This both fixes the codec's one real
  structural gap (its block access is hard-coded to a POSIX `fd`, with no
  indirection layer) *and* gives the ACP its multi-substrate block I/O for free. No
  allocation, no statics.
- `ods2_path.c` — one `malloc`/`free` (only in `_read_file_text`) → caller-provided
  buffer or `kmalloc`; `toupper`/`snprintf` → trivial kernel equivalents.
- `ods2_writer.c` — the real work, still mechanical: two `calloc`/`malloc` sites →
  `kmalloc`/`vmalloc` (the ~2 MB write cache wants `vmalloc` or a smaller cap
  driven by the page cache); `pread`/`pwrite`/`lseek` → block-layer I/O; **move the
  file-scope `static uint8_t wcache_scratch[512]` into the `ods2_wvolume_t` handle
  for reentrancy** (multiple processes run the ACP concurrently); one `snprintf`.
  No FP, no recursion, no `errno`, no stdio streams; largest stack frame
  (`dir_insert`, ~2.5 KB) is acceptable on a kernel stack but is the one to watch.

The codec stays a single source of truth compiled **both** ways: userspace (tools:
INITIALIZE, the boot master builder, tests) and kernel-resident (the ACP), the
latter bound to `vmsfs_bio.h` in the shared `src/kernel-core/vmsfs/` engine exactly
as `vms_lock.c` et al. are substrate-agnostic — so the Linux and NetBSD-vax
SYSKRNLs get the same ODS-2 ACP. The bespoke `VMFS`/`VFH2` on-disk modules in the
shared core (`vmsfs_header.c`/`vmsfs_map.c`/`vmsfs_dirscan.c`/`vmsfs_alloc.c` format
specifics) are superseded by the codec's ODS-2 equivalents; the core's backend
seam, allocator/dir/map *shapes*, and multi-substrate wiring are retained.

### 4.5 RMS → `$QIO` → ACP (killing `_linux_fd`)

RMS today keeps `int _linux_fd` in `struct FAB` and does positioned POSIX I/O at
~70 sites across `rms_seq.c`/`rms_idx.c`/`rms_rel.c`/`rms_util.c`. The faithful
replacement re-types the one field and re-points the choke layers:

- **`FAB._linux_fd` → a VMS channel** (the PCB channel concept already exists) plus
  an accessed-file cursor `{chan, FID, window, VBN, byte-offset}` in `_rms_state`.
- **File lifecycle (`rms_core.c`, 9 functions):** `rms_impl_open` → `$ASSIGN`
  volume + `$QIO IO$_ACCESS` (FIB by resolved name, or by FID); `rms_impl_create`
  → `IO$_CREATE` (+`IO$M_ACCESS`), extend via `FIB$W_EXCTL`; `rms_impl_close` →
  `IO$_DEACCESS` + `$DASSGN`; `rms_impl_erase` → `IO$_DELETE`; `rms_impl_extend` →
  `IO$_MODIFY`. `resolve_filename` stops calling `vmsfs_to_linux_path`.
- **Record I/O (the wide surface):** `rms_read_exact`/`rms_write_exact`
  (`rms_util.c`) change from `read`/`write` on a bare fd to `$QIO
  IO$_READVBLK`/`IO$_WRITEVBLK` at a computed VBN + byte-offset on the channel; the
  seq/rel/idx engines keep their offset arithmetic (they already think in file
  offsets, which map cleanly to VBN + offset). This is the runtime-flip record's
  "positioned-I/O sub-project," now done the VMS way (virtual-block QIO) rather
  than the interim per-open working-copy hack.
- **`$SEARCH` (`rms_search.c`):** `opendir`/`readdir` → `IO$_ACPCONTROL` with a
  wildcard `FIB$L_WCC` directory context returning successive matches.
- **Retire the `.rms_meta` sidecar.** It is an OVMX-ism; VMS keeps record
  attributes (RFM/RAT/MRS/…) in the header FAT (`ATR$C_RECATTR`) and indexed
  prologue/bucket structure in the file's own prologue blocks. RMS reads/writes
  these via the ATR list on `IO$_ACCESS`/`IO$_DEACCESS` and via prologue VBNs — no
  side file. (Decision D5 — see §9.)

Because RMS is the layer *most* boot readers should use, the honest end state is
that DCL, IMGACT, SYSUAF, and accounting reach files **through RMS** (hence through
`$QIO`→ACP), not through a second private path.

### 4.6 Boot readers → VMS file access

Each independent POSIX consumer (full inventory in §6) converts to VMS file
access, preferably via RMS:

- **IMGACT** (`src/imgact/imgact.c` freestanding + `src/libvms/syssvc/sys_imgact.c`):
  image header reads and PT_LOAD mapping move from `open`/`lseek`/`read`/`mmap` on a
  `/vms` path to channel `IO$_ACCESS` + `IO$_READVBLK` (or RMS block-mode reads).
  Demand-paging an image from a `$QIO`-accessed file is the VMS "image sections
  mapped through the file's window" model; a first cut may read-then-map.
- **SYSUAF/RIGHTSLIST** (`sysuaf.c`, `sys_uai.c`, `rightslist.c`): the indexed-file
  reads that authenticate a login become RMS `$GET` (indexed) over the ACP — SYSUAF
  is a real indexed file in VMS. First cut may keep sequential RMS access.
- **DCL `@file`** (`dcl_script.c`) and file commands (`dcl_cmd_file.c`): `fopen`/
  `fgets`/`fseek` → RMS sequential `$GET` with position save/restore for GOSUB/CALL.
- **F$ lexicals** (`dcl_lexical.c`): F$SEARCH → RMS `$SEARCH`/`IO$_ACPCONTROL`;
  F$FILE_ATTRIBUTES → attributes from the ATR list / `$DISPLAY`, not `stat`.
  `/FULL` DIRECTORY can finally emit a real File ID.
- **PID 1** (`src/ovmx_init/`): replace `mount("/dev/vda","/vms","vmsfs")` with an
  ACP `$MOUNT` of the boot unit; drop the `/vms` mount-point scaffolding.
- **The two guaranteed per-boot writers** the runtime-flip record surfaced —
  OPERATOR.LOG append (`opcom_kmsg.c`) and LASTLOGIN write+mkdir
  (`ovmx_accounting.c`) — become RMS `$PUT`/`$CREATE` over the ACP (append to an
  existing file = `IO$_WRITEVBLK` past EOF with extend; mkdir = `IO$_CREATE` of a
  `.DIR` + `IO$_MODIFY` directory enter).

### 4.7 Concurrency — the DLM volume lock, not a flock broker

The write half has a real concurrency hazard (three independent processes write
SYS$DISK at boot: OPCOM, LASTLOGIN, RMS `$PUT`). The userspace adapter's answer was
`flock(fd, LOCK_EX)` (runtime-flip §5.1, `vms-49d`). The executive ACP uses the
**VMS-authentic mechanism**: `$ENQ` an EX-mode lock on a per-volume synchronization
resource in the executive's own lock manager (`vms_lock.c`) across the
allocate/read-modify-write/flush span. This is exactly how the ODS-2 XQP
serializes, and — unlike `flock` — it **generalizes to the clustered / MSCP-served
case** where the volume is written from more than one node (the lock resource is
already cluster-aware in `vms_lock.c`). The flock broker is retired with the
adapter.

## 5. The boot path, end to end (no `/vms` POSIX tree)

```
PID 1 (ovmx_init) opens /dev/vms, registers (identity from Linux creds)
  └─ $MOUNT boot unit (/dev/vda)  →  ACP reads+validates home block + SCB
        (DECFILE11B), records volume in the executive-global mounted table;
        SYS$SYSDEVICE = discovered logical for the boot unit
  └─ activate STARTUP/LOGINOUT:
        IMGACT  $ASSIGN SYS$SYSDEVICE + IO$_ACCESS(image FID) + IO$_READVBLK
                → map image sections through the file window   (no mmap on /vms)
  └─ LOGINOUT  RMS $OPEN/$GET SYSUAF (indexed) over the ACP → authenticate
        → Username:                                            (no fopen on /vms)
  └─ per-boot writers: OPERATOR.LOG append, LASTLOGIN write = RMS $PUT/$CREATE
        over the ACP, serialized by the DLM volume lock         (no /vms writes)
```

`vms.ko` is the executive (locks/EF/AST/access-modes **and now the Files-11 ACP**),
reached via `/dev/vms`; SYS$DISK is a genuine ODS-2 block device the ACP owns.
`vmsfs.ko`'s POSIX mount at `/vms` and the whole `vmsfs_to_linux_path` passthrough
are gone.

## 6. Migration from the current userspace adapter

A parallel evaluation is reverting the userspace adapter and its (not-yet-landed)
consumer reroutes. This design assumes that outcome: **the adapter
(`ods2_sysdisk.c`) and the `vmsfs_to_linux_path -> /vms` passthrough go away; the
ODS-2 codec (`src/vmsfs/ods2/`) stays** and becomes the ACP engine.

Facts that make the migration tractable:
- The adapter is **dead code at runtime today** — it has zero live callers; SYS$DISK
  is 100% the `/vms` POSIX passthrough (27 `vmsfs_to_linux_path` call sites). So
  reverting the adapter removes nothing the running system depends on.
- The boot-master builder already emits a genuine ODS-2 volume (`vmsfs_master.c
  --ods2`, `OVMX_MASTER_ODS2=1`); flipping that default is a one-line change that
  the ACP mount then consumes.
- The shared engine `src/kernel-core/vmsfs/` is **kept and extended** into the ACP;
  the Linux VFS module `vmsfs.ko` (which proved the kernel block plumbing) is
  **retired for SYS$DISK** — superseded by the ACP + genuine ODS-2 codec (its
  invented `VMFS`/`VFH2` format is not ODS-2). Keep the VFS surface, if anything,
  only as a Linux-host read-only *viewer* (§4.1 option D, secondary), never the VMS
  path.

**Passthrough consumer inventory** (each converts to VMS file access per §4.5–4.6):
RMS (`rms_core.c`, `rms_search.c`); SYSUAF (`sysuaf.c`, `sys_uai.c`);
RIGHTSLIST (`rightslist.c`); accounting (`ovmx_accounting.c`); DCL
(`dcl_filespec.c`, `dcl_cmd_file.c`, `dcl_lexical.c`, `dcl_main.c`, `dcl_script.c`,
`dcl_cmd_misc.c`); IMGACT (`imgact.c`, `sys_imgact.c`); init (`ovmx_init.c`,
`ovmx_boot_linux.c`, `ovmx_boot_netbsd.c`, `opcom_kmsg.c`); OPCOM
(`sys_operator.c`); SYSGEN params/banner headers; QMAN (`vmsqueue.c`); job control;
provision; SSH host key (`vmssshd.c`). This is a big surface: it is the whole point
— it is big *because* it was N consumers each doing their own POSIX, and it
collapses to "everyone uses RMS/`$QIO`."

## 7. Clean-room posture (Rule 8, HARD)

- **On-disk ODS-2 structures** (home block, SCB, FH2, INDEXF.SYS, FID chains,
  directory records/versions, BITMAP.SYS, retrieval pointers): already
  clean-room in the codec, validated against a real OpenVMS VAX volume
  (`tests/ods2/PROVENANCE-real_vax_ods2.md`), derived from the public Files-11
  ODS-2 spec + lab observation. Unchanged.
- **The `$QIO`/ACP/FIB/ATR interface**: derived only from the VSI *I/O User's
  Reference Manual* (ACP-QIO chapter), *Guide to OpenVMS File Applications* (RMS
  layering), *System Services Reference* (`$ASSIGN`/`$QIO`/`$DASSGN`), and
  `$IODEF`/`$FIBDEF`/`$ATRDEF`. Where a byte offset is not published, the OVMX
  ioctl arg-struct layout is an **OVMX design choice** and is labelled as such;
  it is never presented as VMS-authentic wire. This is the resolution for the FIB
  sign-off item `vms-531`.
- **Never** disassemble/decompile VSI/HPE or paste leaked source.

## 8. Test strategy (Rule 9 / INV-6 — real `/dev/vms`, real boot, real VAX)

1. **Kernel-resident codec proof** — a QEMU kernel test (`tests/qemu/`) loads the
   ACP-in-`vms.ko`, mounts a genuine ODS-2 image (the real-VAX fixture
   `tests/ods2/real_vax_ods2.dsk`) off a block device, and reads files back
   byte-identically **inside the module** — no userspace codec, no POSIX.
2. **ACP-QIO facility tests against real `/dev/vms`** — `$ASSIGN` + each `IO$_*`
   function (access/create/read/write/delete/modify/search) with FIB/ATR, asserting
   real FIDs/versions/attributes and fail-honest `SS$_` codes when `/dev/vms` is
   absent (`SS$_NOSUCHDEV`) — never a POSIX fallback. Each new `vms_kif_acp_*` gets
   a real product caller or an `OVMX-UNWIRED` declaration (caller census), a
   `facility_defects.sh` negctl anchor, and its executive symbols added to
   `mk_vmssys_shr.sh` SYS_VEC + the 6 harness copies.
3. **RMS-over-ACP tests** — sequential/relative/indexed `$GET`/`$PUT`/`$UPDATE`
   with no `_linux_fd`, records surviving byte-exact across `$CLOSE`/reopen; the
   existing RMS suite re-pointed at an ODS-2 backing volume.
4. **QEMU boot-to-login on genuine ODS-2 via the ACP** — the gate: PID 1 mounts via
   the ACP, IMGACT activates through VMS file access, LOGINOUT authenticates from
   SYSUAF via the ACP, reach `Username:`, with **no `/vms` POSIX tree** present.
5. **MSCP-serve the ACP volume to a real OpenVMS VAX** (lab-1 / lab-2) — the
   ultimate fidelity oracle: the same executive-resident Files-11 volume, served
   over MSCP, mounted and read by real OpenVMS. If a real VAX mounts it and reads a
   file, the on-disk + ACP semantics are authentic. (Post-gate fidelity check.)

## 9. Open questions, risks, and operator decisions

**Operator decisions (teed up with a recommendation; default is what I will assume
absent a ruling):**

- **D1 — Scope of the first boot-to-login cut.** The full ACP (all functions,
  indexed RMS, demand-paged image activation) is a multi-session program.
  *Recommendation:* land the ACP incrementally in the executive but only flip the
  live boot atomically once the read+access+readvblk+mount+RMS-sequential subset
  boots to login; defer indexed-RMS-over-ACP niceties and demand-paged image
  sections to fast-follows. **Default: staged, atomic flip at the boot gate.**
- **D2 — FIB/ATR clean-room sign-off (`vms-531`).** The FIB beyond
  `fib$w_fid`/`fib$l_acctl` is flagged pending operator sign-off. §4.2/§7 give the
  posture: public-doc fields where published, OVMX-labelled arg-struct layout
  otherwise. *Recommendation: approve on that basis.* **This is a reserved
  (confidentiality/authenticity) call — escalated, not assumed.**
- **D3 — `vmsfs.ko` / shared-core disposition.** Two distinct things: (i) the
  *shared engine* `src/kernel-core/vmsfs/` is **kept and extended** into the ODS-2
  ACP (§4.1) — not retired; (ii) the *Linux VFS module* `vmsfs.ko` and its
  `mount`-at-`/vms` presentation are **retired for SYS$DISK** (the VMS path is
  `$QIO`, not VFS). *Recommendation:* after the flip, keep the VFS surface only as
  an optional Linux-host **read-only** ODS-2 viewer (a secondary, non-authoritative
  consumer), or delete it. The bespoke `VMFS`/`VFH2` on-disk format is superseded
  either way. **Default: retire the VFS mount from the boot path now; decide
  keep-as-viewer vs delete later.**
- **D4 — Device naming (`vms-9f5`, VDA0: vs DSA0:).** SYS$SYSDEVICE is a discovered
  logical; the ACP mount must bind *some* unit name. This intersects the open
  device-native-naming call. *Recommendation:* inherit whatever `vms-9f5` decides;
  the ACP does not force it.
- **D5 — `.rms_meta` sidecar retirement.** Recommend removing it and carrying record
  attributes in the header FAT (`ATR$C_RECATTR`) + indexed prologue blocks (VMS
  behavior). *Default: retire.* Risk: indexed-file prologue fidelity is real work;
  a first cut may keep a minimal FAT and defer full indexed prologue authenticity.

**Risks:**
- **Atomicity.** As the runtime-flip record found, the live boot cannot flip half:
  the master-default flip, PID-1 ACP mount, RMS re-point, and every boot reader must
  co-land, and test fixtures must switch to an ODS-2 backing volume. The executive
  ACP does **not** remove this atomicity — it relocates the boundary to `$QIO`. Plan
  the flip as one gated group (the tree's `flip` outcome), building every prior
  outcome additively behind it.
- **Kernel write cache size.** The codec's ~2 MB writer cache is fine in userspace
  but wants `vmalloc`/a page-cache-driven cap in the kernel; the `static
  wcache_scratch` **must** move into the handle before concurrent callers exist.
- **Image activation.** Demand-paging an image through a file window is the largest
  single behavioral change to IMGACT; a read-then-map first cut de-risks it but is
  not the end state.
- **Indexed SYSUAF over RMS-over-ACP** is on the critical path to login and is the
  deepest RMS exercise; a sequential-access first cut may be needed to reach the
  boot gate, with indexed fidelity as a fast-follow.

---

*Sources: VSI OpenVMS I/O User's Reference Manual (DO-DIOURM-01A), "ACP-QIO
Interface"; VSI OpenVMS Guide to OpenVMS File Applications; VSI OpenVMS System
Services Reference ($ASSIGN/$QIO/$DASSGN); $IODEF/$FIBDEF/$ATRDEF (SYS$LIBRARY
definition macros); OpenVMS Internals and Data Structures Manual (XQP execution
model, volume synchronization lock); Wikipedia "Files-11" (ACP→XQP summary, FID
and reserved-file list). No VSI/HPE source or binary was disassembled or consulted
(Rule 8).*
