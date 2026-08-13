<!-- SPDX-License-Identifier: GPL-2.0 -->
# vmsfs: the Linux VFS backend and the ODS-2 core seam

*(rd vms-00c, epic vms-8e8 / vms-8e5 "one shared core, Linux + NetBSD VFS
backends, no feature drift". This is the ODS-2 filesystem analog of the
executive's `docs/design-netbsd-executive-core.md`.)*

OVMX's ODS-2 filesystem (`vmsfs.ko`) is being factored the same way the VMS
executive was: the **substrate-neutral ODS-2 logic lives once** in
`src/kernel-core/vmsfs/`, and each host kernel provides a **thin backend** that
binds that logic to its own VFS. This directory (`src/kernel/vmsfs/`) is the
**Linux backend**. A NetBSD `vnode`/`VFS_*` backend (rd vms-308) is written
against this same core, so it must implement the same seam — never re-derive the
ODS-2 format or algorithms, or the two backends drift (the exact failure
vms-8e5 exists to prevent).

This document is the map a second backend is written from: what already lives in
the shared core, what the Linux backend supplies, and — for the ODS-2 logic that
is **not yet** in the core — the seam that has to exist before it can move.

---

## 1. The two layers today

```
src/kernel-core/vmsfs/          SUBSTRATE-NEUTRAL — 0 <linux/…> includes
  vmsfs_backend.h                 the vocabulary-shim contract (selects a
                                  per-substrate realization at build time)
  vmsfs_core.h                    shared constants + core algorithm prototypes
  vmsfs_ondisk.h*                 on-disk format (home block / file header /
                                  dir entry / retrieval ptr) + checksum
  vmsfs_version.c                 VMS "NAME.TYPE;VERSION" parse / build / match
  vmsfs_map.c                     retrieval-map VBN<->LBN math
  vmsfs_name.c                    filename FORMAT: split name/type, uppercase,
                                  case-blind name match            (rd vms-00c)

src/kernel/vmsfs/               LINUX BACKEND — all the <linux/…> lives here
  vmsfs_backend_linux.h           realizes vmsfs_backend.h on Linux
  vmsfs.h                         Linux glue types (vmsfs_sb_info,
                                  vmsfs_inode_info) + op-table externs
  vmsfs_super.c                   file_system_type, mount/kill_sb, super_ops,
                                  option parsing, home-block + bitmap load
  vmsfs_inode.c                   inode slab cache, dentry ops (case-blind
                                  hash/compare/revalidate), OVERLAY-mode iops
  vmsfs_dir.c / vmsfs_file.c      OVERLAY-mode dir/file ops (backing-dir passthru)
  vmsfs_blkdev.c                  BLOCK-DEVICE mode: iget/flush, lookup, readdir,
                                  create/mkdir/unlink/rmdir/rename, permission,
                                  address_space_ops, AND the not-yet-extracted
                                  ODS-2 storage allocator + directory scanner
```
\* `vmsfs_ondisk.h` physically sits beside the Linux glue and is reached by the
core via `-I$(src)`; it is already substrate-neutral (its only host branch,
`<linux/types.h>` under `__KERNEL__`, is simply not taken on a non-Linux build,
which falls to the `<stdint.h>` branch). Its physical relocation into
`kernel-core/` is a separate zero-algorithm move (it has ~20 userspace includers
to avoid churning) and is not required for a second backend — a NetBSD build
just adds it to its own `-I` path.

`vmsfs.ko` supports **two mount modes**. Only the **block-device** mode
(`mount -t vmsfs /dev/vdX …`) is the real OVMX runtime and the target of the
NetBSD port: it reads/writes the ODS-2 on-disk format directly. **Overlay** mode
(`-o backing=/path`) is a Linux-only convenience that projects VMS versioning
onto a host directory; it is pure Linux VFS glue (`vmsfs_dir.c`,
`vmsfs_file.c`, the overlay iops in `vmsfs_inode.c`) and a NetBSD backend need
not replicate it.

---

## 2. Seam #1 — the vocabulary shim (`vmsfs_backend.h`) — EXISTS

The first seam is the one V1 (rd vms-544) built and V2 (rd vms-00c) widened by
three names. It is a **build-time-selected header of ordinary freestanding
names**, not a set of ops: a core `.c` file `#include "vmsfs_backend.h"` and
calls `strchr`, `memcpy`, `snprintf`, … by their normal names; the selected
backend header guarantees those names are declared and resolve to the host's own
primitive. On Linux every name is EXACTLY the primitive the filesystem already
used, so promoting an algorithm onto the shim is behaviour-preserving.

A NetBSD backend supplies `src/kernel-netbsd/vmsfs/vmsfs_backend_netbsd.h`
(selected by `-DOVMX_KBACKEND_NETBSD`) providing:

| vocabulary                                   | Linux source            | NetBSD source |
|----------------------------------------------|-------------------------|---------------|
| `uint8/16/32/64_t`, `size_t`, `bool`         | `<linux/types.h>`       | `<sys/types.h>` / `<sys/stdint.h>` |
| `strchr strrchr strlen strncasecmp memcpy memcmp strscpy` | `<linux/string.h>` | libkern (`strscpy`→`strlcpy`; core uses only its truncating-copy effect, never the return value) |
| `isdigit toupper`                            | `<linux/ctype.h>`       | libkern `<sys/systm.h>` |
| `snprintf`                                   | `<linux/kernel.h>`      | libkern |
| `VMSFS_EINVAL/ENAMETOOLONG/EIO/ENOSPC`       | == host `errno` values  | == host `errno` values |

Everything under seam #1 — **version resolution, retrieval-map math, filename
format** — is already in the core and needs no further work for a second backend.

---

## 3. Seam #2 — the block/inode seam — DESIGNED HERE, NOT YET BUILT

The ODS-2 logic still in `vmsfs_blkdev.c` is not stuck on Linux by accident: it
is the logic that touches the **volume** (512-byte blocks by LBN, the storage
bitmap, volume geometry) and the **in-core file object** (`struct inode` /
`vmsfs_inode_info`: cached retrieval map, size, block count, FID). The
vocabulary shim does not cover those, so this code cannot move until a second
seam exists. This mirrors the executive core, which left its block-coupled
facility (`vms_devtab`) in `src/kernel/` until its seam was built — not before.

The still-Linux ODS-2 logic, and what each piece is coupled to:

| function(s) in `vmsfs_blkdev.c`                     | ODS-2 job                                   | host coupling |
|-----------------------------------------------------|---------------------------------------------|---------------|
| `vmsfs_alloc_block` `vmsfs_free_block` `vmsfs_write_bitmap_block` | storage-bitmap cluster allocator | `sbi` geometry + `unsigned long *bitmap` + Linux bitops + `sb_bread`/`mark_buffer_dirty` |
| `vmsfs_alloc_fid` `vmsfs_free_fid`                  | file-header (FID) allocator                 | `sbi->index_lbn/max_files` + `sb_bread` |
| `vmsfs_update_home_block`                           | superblock free-count write-back            | `sb_bread` + endian + wall-clock |
| `vmsfs_blkdev_iget`                                 | read+validate+decode a file header → inode  | `sb_bread` + endian + **`struct inode` populate** + `iget_locked` |
| `vmsfs_blkdev_flush_inode`                          | encode inode → file header, write back      | `sb_bread` + endian + **`struct inode` read** |
| `vmsfs_blkdev_resolve` `_highest_version` `_iterate` `dir_add_entry` `dir_remove_entry` | directory-block scanner (lookup/readdir/link/unlink) | map walk + `sb_bread` per block + (some) `struct inode` mutate + `dir_emit` |
| `vmsfs_ensure_blocks` `vmsfs_get_block`             | grow a file's block map on write            | allocator + `struct inode` + `map_bh` |

### 3a. The block half (`vmsfs_bio` — the `sb_bread`/`brelse` equivalent)

A substrate-neutral volume descriptor plus a buffer handle whose lifecycle is
get → data → dirty → put. A core allocator calls, e.g.:

```c
struct vmsfs_bh *bh = vmsfs_bget(vol, lbn);   /* Linux: sb_bread((sb*)vol->host, lbn) */
if (!bh) return -VMSFS_EIO;
void *p = vmsfs_bdata(bh);                     /* Linux: bh->b_data                    */
/* … mutate the 512-byte block … */
vmsfs_bdirty_sync(bh);                          /* Linux: mark_buffer_dirty + sync_dirty_buffer */
vmsfs_bput(bh);                                 /* Linux: brelse                        */
```

The descriptor `struct vmsfs_volume { void *host; unsigned long *bitmap;
uint32_t bitmap_lbn, bitmap_blocks, index_lbn, max_files, data_lbn,
total_blocks, free_blocks; }` is embedded in the Linux `vmsfs_sb_info` (and in
the NetBSD mount) so the allocator reaches geometry with no host struct. The
bitmap ops (`vmsfs_bit_find_zero/set/clear/test`) wrap Linux
`find_next_zero_bit`/`set_bit`/… and NetBSD's equivalents. Endian
(`vmsfs_le32_to_cpu`…) and a wall-clock (`vmsfs_now_seconds`) round it out.
Once this seam exists the storage allocator, FID allocator and home-block
write-back move **whole** into the core — they touch no `struct inode`.

### 3b. The inode half (a parse/populate split — `vmsfs_fh_info`)

`vmsfs_blkdev_iget` and `_flush_inode` interleave **ODS-2 header decode/encode**
(portable) with **`struct inode` populate/read** (host). The extraction is a
split, not a move: the core owns a POD `struct vmsfs_fh_info` and two pure
functions —

* `int vmsfs_fh_decode(const void *block512, struct vmsfs_fh_info *out)` —
  validate magic/checksum/INUSE, decode every field (endian + `fh_map[]`) into
  the POD;
* `void vmsfs_fh_encode(const struct vmsfs_fh_info *in, void *block512)` —
  the inverse, then re-checksum —

and each backend keeps the thin `POD <-> host inode` copy (`iget_locked`,
`set_nlink`, `inode_set_ctime`, `KUIDT_INIT` on Linux; the `vnode`/`vattr`
equivalents on NetBSD). The directory scanner factors the same way: the core
owns "scan one 512-byte directory block for {free slot | fid+version | best
version | name match}"; the backend owns the map-walk + `vmsfs_bget` loop and
the `dir_emit`/`vnode` emission.

> **Why 3a/3b are a separate landing, not this one.** Unlike the seam-#1 moves
> (which are textually identical and provably behaviour-preserving by
> disassembly), 3a/3b are a **behaviour-preserving restructure**: moving these
> `static` functions across a translation-unit boundary loses the interprocedural
> specialization (`.isra`/`.constprop`) GCC applies inside `vmsfs_blkdev.c`, and
> 3b actively splits functions. So they are verified **behaviourally**, and their
> only behavioural oracle is the QEMU `test_kmod_vmsfs_blkdev` suite (the Kernel
> Executive gate) — they belong in their own focused, green-by-SHA landing rather
> than riding on a mechanical move.

---

## 4. What a NetBSD vnode backend implements (checklist for rd vms-308)

1. `vmsfs_backend_netbsd.h` — seam #1 (section 2). *Available now.*
2. Call the shared core for all name/version/map logic — never re-derive it.
   *Available now:* `vmsfs_parse_version`, `vmsfs_build_versioned_name`,
   `vmsfs_vbn_to_lbn`, `vmsfs_next_vbn`, `vmsfs_extend_map`,
   `vmsfs_split_name_type`, `vmsfs_strupper`, `vmsfs_name_match`.
3. Seam #2 realization (section 3) — the `vmsfs_bio` buffer/geometry/bitmap ops
   and the `vmsfs_fh_info` copy. *Blocked on the seam #2 landing; until then a
   NetBSD read path reuses `vmsfs_ondisk.h` + the core name/map logic and
   carries its own header decode, to be replaced by `vmsfs_fh_decode` when it
   lands.*
4. `VFS_MOUNT`/`VFS_ROOT` ↔ `vmsfs_super.c`'s mount + MFD-root logic;
   `VOP_LOOKUP`/`VOP_READDIR`/`VOP_READ` ↔ the block-device lookup/readdir/read
   in `vmsfs_blkdev.c`. These are the **backend's** to write; the ODS-2 decisions
   inside them come from the core.

---

## 5. Invariants (do not regress)

* `src/kernel-core/vmsfs/**` has **0** `<linux/…>` includes. (CI/grep-checkable.)
* No ODS-2 format constant or algorithm is duplicated in a backend — it lives in
  the core and the backend calls it. A second copy is drift (vms-8e5).
* Clean-room (CLAUDE.md Rule 8): the core and both backends are OVMX's own code
  over public, documented host-kernel APIs. No VSI/HPE/Linux/NetBSD source is
  copied.
