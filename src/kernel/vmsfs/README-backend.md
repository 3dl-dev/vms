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
  vmsfs_bio.h                     the block/inode seam CONTRACT: struct
                                  vmsfs_volume + the vmsfs_bio ops + the
                                  vmsfs_fh_* codec + the allocator/dir-scanner
                                  prototypes                        (rd vms-d69)
  vmsfs_alloc.c                   storage/FID/cluster allocator + home-block
                                  write-back + block-map growth     (rd vms-d69)
  vmsfs_dirscan.c                 directory-block scanner: resolve / highest-
                                  version / add / remove / empty / readdir decode
                                  + display-name format             (rd vms-d69)
  vmsfs_header.c                  file-header decode / encode / partial writes
                                  (flush + rename)                  (rd vms-d69)

src/kernel/vmsfs/               LINUX BACKEND — all the <linux/…> lives here
  vmsfs_backend_linux.h           realizes vmsfs_backend.h AND the vmsfs_bio ops
                                  (sb_bread / bitops / le*_to_cpu / ktime) on Linux
  vmsfs.h                         Linux glue types (vmsfs_sb_info embeds a
                                  vmsfs_volume, vmsfs_inode_info) + op-table externs
  vmsfs_super.c                   file_system_type, mount/kill_sb, super_ops,
                                  option parsing, home-block + bitmap load
  vmsfs_inode.c                   inode slab cache, dentry ops (case-blind
                                  hash/compare/revalidate), OVERLAY-mode iops
  vmsfs_dir.c / vmsfs_file.c      OVERLAY-mode dir/file ops (backing-dir passthru)
  vmsfs_blkdev.c                  BLOCK-DEVICE mode, now the THIN VFS + vmsfs_bio
                                  backend: iget/flush (POD<->inode copy), lookup,
                                  readdir emission, create/mkdir/unlink/rmdir/
                                  rename, permission, address_space_ops — every
                                  ODS-2 decision delegated to the core
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

## 3. Seam #2 — the block/inode seam — BUILT (rd vms-d69)

> **Status: this seam is now built.** The tables and code sketches below are the
> as-designed contract; the extraction landed it in `vmsfs_bio.h` (contract),
> `vmsfs_alloc.c` / `vmsfs_dirscan.c` / `vmsfs_header.c` (the moved algorithms),
> and the vmsfs_bio realization in `vmsfs_backend_linux.h`. `vmsfs_blkdev.c` is
> now the thin VFS + vmsfs_bio backend. The design matched the build with two
> refinements, both noted inline: `vmsfs_fh_decode` returns an
> `enum vmsfs_fh_status` (so the backend reproduces the original's three distinct
> diagnostics/errnos — EIO magic, EIO checksum, ENOENT not-in-use) rather than a
> bare `int`; and the flush/rename header write-backs are explicit partial-update
> writers (`vmsfs_fh_write_meta` / `vmsfs_fh_write_rename`) rather than being
> forced through `vmsfs_fh_encode`, so they preserve every untouched on-disk field
> without a decode-on-write that could change the error behaviour.

The ODS-2 logic that was still in `vmsfs_blkdev.c` before this landing is not
stuck on Linux by accident: it is the logic that touches the **volume** (512-byte
blocks by LBN, the storage bitmap, volume geometry) and the **in-core file
object** (`struct inode` / `vmsfs_inode_info`: cached retrieval map, size, block
count, FID). The seam-#1 vocabulary shim does not cover those, so this code could
not move until this second seam existed. This mirrors the executive core, which
left its block-coupled facility (`vms_devtab`) in `src/kernel/` until its seam
was built — not before.

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
   (`vmsfs_bget` / `vmsfs_bdata` / `vmsfs_bdirty[_sync]` / `vmsfs_bput` /
   `vmsfs_bit_*` / `vmsfs_le*_to_cpu` / `vmsfs_cpu_to_le*` / `vmsfs_now_seconds`)
   plus the thin `vmsfs_fh_info` ↔ vnode/vattr copy. *Available now:* the NetBSD
   backend provides `vmsfs_backend_netbsd.h` realizing those ops over its own
   buffer cache (`bread`/`brelse`/`bwrite`), bitops and byte-order, embeds a
   `struct vmsfs_volume` in its mount (`vol.host` = the mount/vnode), and calls
   the shared `vmsfs_alloc.c` / `vmsfs_dirscan.c` / `vmsfs_header.c` for every
   ODS-2 decision — `vmsfs_fh_decode` / `vmsfs_fh_encode`, `vmsfs_dir_resolve`,
   `vmsfs_alloc_block`, etc.
4. `VFS_MOUNT`/`VFS_ROOT` ↔ `vmsfs_super.c`'s mount + MFD-root logic;
   `VOP_LOOKUP`/`VOP_READDIR`/`VOP_READ` ↔ the block-device lookup/readdir/read
   in `vmsfs_blkdev.c`. These are the **backend's** to write; the ODS-2 decisions
   inside them come from the core. For readdir specifically, follow the Linux
   backend's split: the backend owns the map-walk + emission and calls
   `vmsfs_dir_entry_decode` + `vmsfs_dir_format_name` per entry.

---

## 5. Invariants (do not regress)

* `src/kernel-core/vmsfs/**` has **0** `<linux/…>` includes. (CI/grep-checkable.)
* No ODS-2 format constant or algorithm is duplicated in a backend — it lives in
  the core and the backend calls it. A second copy is drift (vms-8e5).
* Clean-room (CLAUDE.md Rule 8): the core and both backends are OVMX's own code
  over public, documented host-kernel APIs. No VSI/HPE/Linux/NetBSD source is
  copied.
