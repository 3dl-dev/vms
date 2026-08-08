# PARTS — the OVMX 0.2 "runs a real VMS app" demo

Beads: **vms-e97** (author PARTS) + **vms-f20** (native build chain). Parent
`vms-2579`, from `docs/release-plan-0.2-to-0.5.md` §4 (0.2 — "It runs a real VMS
app"). De-risked by **vms-530** (`docs/derisk-vms-530-imgact-qemu.md`, PR #168):
native `LINK.EXE → IMGACT` activation works non-root under QEMU.

## What PARTS is

A small VMS C business-records application over an **RMS indexed (ISAM) file**
with a single primary key — the part number. It exercises exactly the Tier-1/2
RMS system services, nothing more:

| Service      | Use in PARTS                                    |
|--------------|-------------------------------------------------|
| `sys$create` | create the indexed file with a primary-key `XABKEY` |
| `sys$connect`| open a record stream (RAB → FAB)                |
| `sys$put`    | load part records                               |
| `sys$get`    | **keyed random** lookup by part number (`RAB$C_KEY`) |
| `sys$disconnect` / `sys$close` | tear down                     |

It is **single-user**: RMS record locking is unwired in OVMX (`vms-407`), so the
file is opened `FAB$M_NIL` (no sharing) and no shared-record lock is taken.

### Record and key layout (`src/apps/parts/parts.h`)

56-byte packed fixed-length record; the primary key is the first 8 bytes:

```
offset  0  char     part_number[8]   "PNnnnnnn"  <- primary key (XAB$C_STG)
offset  8  char     description[40]  blank-filled
offset 48  uint32   quantity
offset 52  uint32   price_cents
```

The `XABKEY` is `xab$b_ref=0` (primary), `xab$b_dtp=XAB$C_STG`, `xab$w_pos0=0`,
`xab$b_siz0=8`, `xab$b_nseg=1`. Part numbers are fixed 8-char strings so the
stored key bytes and a lookup key buffer compare byte-for-byte.

### Source layout

- `parts.h` — record/key definitions, db-layer prototypes.
- `parts_db.c` — the RMS access layer (create/open/put/keyed-get/close).
- `parts.c` — `main`: the self-demo plus `LOAD`/`LOOKUP` sub-commands.

Two translation units, deliberately: it matches the multi-object shape that
activates cleanly (the vms-530 de-risk's *single*-object scratch image was the
one that misbehaved), and it is good structure (access layer vs. presentation).

## Native build chain (vms-f20)

`src/apps/parts/mk_parts.sh`, modelled on `src/vmslink/mk_dcl.sh`:

```
cc parts.c parts_db.c          (musl, -fPIC, same flags as the DCL graph)
LINK.EXE --executable --use DECC$SHR --use LIBVMSRMS$SHR -o PARTS.EXE
```

The result is an **ET_DYN executable**, `PT_INTERP=IMGACT.EXE`, **no
DT_NEEDED/DT_HASH**, carrying `.vms$sv`/`.vms$imp`/`.vms$rel`. PARTS imports 21
externals across two producers directly (libc → `DECC$SHR`, the RMS services →
`LIBVMSRMS$SHR`); `LIBVMSRMS$SHR` pulls the rest of the producer graph
transitively, and IMGACT loads all of it at activation. This is the *same*
toolchain that builds `DCL.EXE`/`LOGINOUT.EXE`; PARTS is just the first
application consumer.

`mk_parts.sh` is **not** wired into the CMake `link_native_graph` target (which
carries a strict 9-artifact ground-source gate) nor into `distro/Dockerfile.bootable`
— shipping PARTS in the boot image is a later item (`vms-cde`). It builds
standalone, exactly like the graph recipes.

## Proof under QEMU (the real DoD)

`tests/qemu/test_parts_rms_qemu.sh` (host-run: docker + qemu + cpio):

1. builds `PARTS.EXE` via the native toolchain and asserts its VMS-native shape;
2. repacks the **already-built** fat initramfs to add `PARTS.EXE` to
   `SYS$SYSTEM:` — the run-time repack pattern of `tests/qemu/inject_and_run.sh`,
   so distro mastering is untouched;
3. boots QEMU with the real `vms.ko` executive, logs in as `SYSTEM`, runs
   `$ RUN SYS$SYSTEM:PARTS`, and asserts the indexed file is created, records are
   loaded, and keyed random lookups print the **correct** records (first, middle,
   last found; a never-loaded key reported NOTFOUND; no wrong-record).

A host-only functional gate, `parts_rms_indexed_functional` (ctest, via
`src/apps/parts/test_parts_functional.sh`), exercises the same RMS logic on every
build against the ordinary `vmsrms` library.

## Host-development build

The `PARTS` CMake target (guarded by `BUILD_TESTS`, so the distro image build is
unaffected) links the dev `vmsrms` and runs the functional gate. `$ PARTS LOAD n`
and `$ PARTS LOOKUP pnnnnnnn` sub-commands support manual use; `PARTS_FILE` and
`PARTS_COUNT` override the file spec and record count.

## Two defects surfaced (worked around in-lane, filed separately)

1. **`vms-5c6d` — `sys$close` loses the tail of an indexed load.** `sys$close`
   (`src/vmsrms/rms_core.c`) `free()`s the in-memory B-tree without saving it,
   while `rms_idx_put` only persists every 100 inserts — so a close-then-reopen
   loses records added since the last periodic save (and leaks the tree nodes).
   PARTS sidesteps this by doing load-then-query on **one open stream** (a normal
   RMS pattern), so all lookups hit the in-memory index.

2. **`vms-e5c` — no user-writable directory at boot.** A logged-in session runs
   as its UIC-mapped uid (SYSTEM = uid 4), but `SYS$SCRATCH`, `SYS$LOGIN`,
   `DKA0:[USERS]` are all `root:root 0755`, so `sys$create` gets `EACCES(13)`
   (`RMS$_CRE`). PARTS tries those VMS locations first and, until the boot lane
   (Seat 2 / `ovmx_init`) provisions a writable `SYS$SCRATCH`, falls back to a
   scratch file so the demo still runs end-to-end. The clean "zero Unix leaks"
   demo wants `vms-e5c` fixed so the file lives under a VMS filespec.

## The `cmd_run` fix (vms-17f9)

The de-risk found that DCL's `RUN` (`src/vmsdcl/dcl_cmd_process.c` `cmd_run`)
**silently swallowed** a crashed (signaled) child — falling through to
`SS$_NORMAL` — and reported a nonzero exit with no message. That is exactly what
masked the de-risk's "no output" scratch image. `cmd_run` now surfaces both a
nonzero `WIFEXITED` status and a `WIFSIGNALED` termination via `dcl_error`, so a
failed `RUN` is diagnosable instead of looking identical to success.
