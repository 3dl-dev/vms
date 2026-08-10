# The OVMX kit container format

**Status:** implemented (vms-0b6). Companion to
`docs/design-vms-faithful-install.md` §3.3 (the PCSI row) — this document is
that row's detail.

**Scope:** this document defines the kit **container** and the **factory
tool** that produces/inspects one. It does **not** define product
installation, the product database, or `PRODUCT INSTALL` — that is
`vms-df9`'s job. This format is the consumer contract vms-df9 builds against.

---

## 1. What OpenVMS does (the behavior this reproduces)

OpenVMS ships and installs software — including the operating system itself
— as PCSI product kits. Measured on real media, both VAX and Alpha
(`docs/design-vms-faithful-install.md` §2), installing the OS proceeds as:

```
The following product has been selected:
    DEC AXPVMS OPENVMS V8.4                Platform (product suite)
Configuring DEC AXPVMS VMS V8.4: OpenVMS Operating System
```

`PRODUCT INSTALL VMS /SOURCE=...` reads a kit file, copies its payload to
target VMS filespecs with the right protection and ownership, and records
the installation so `PRODUCT SHOW PRODUCT` can list it afterward. That is
the full extent of what is **publicly documented and observable**: a kit is
self-identifying (product name / producer / version) and carries a file
payload with enough per-file metadata to place it correctly. **VSI has never
published the PCSI kit file's byte layout.**

## 2. Rule 8 labeling — behavior-derived vs. OVMX-invented

| Element | Status | Source |
|---|---|---|
| A kit is one file, self-identifying, carrying target-system files | Behavior-derived | PCSI observed behavior; Alpha oracle capture (`design-vms-faithful-install.md` §2) |
| Per-file protection is the 16-bit VMS SOGW mask | Behavior-derived | `src/libvms/include/ovmx_fileprot.h` — pinned to the VSI `$CRMPSC` wiki page + its worked default-mask example (fetched 2026-08-09); this format reuses that header's constants rather than re-deriving the bit layout |
| `SYS$COMMON:[DIR]NAME.TYPE` VMS filespec syntax | Behavior-derived | Standard, documented OpenVMS convention |
| Product naming *shape*: "vendor + arch-code + VMS" | Behavior-derived shape, OVMX-invented tokens | Alpha oracle showed `DEC AXPVMS VMS V8.4` (vendor `DEC`, arch code `AXPVMS`, product `VMS`, version `V8.4`); OVMX renders the same shape honestly as `OVMX X86VMS VMS V0.1` — **never** `DEC` or `VSI` |
| Container magic, header field layout/order/sizes, flat index+payload arrangement, XOR content checksum | **OVMX-invented** | No public PCSI byte layout exists to match. Labeled in `src/libvms/include/ovmx_kit_format.h`. |
| Default per-file protection/UIC policy for packed files (SYSTEM `[1,4]`, `S:RWED,O:RWED,G:RE,W:RE`) | **OVMX build-tool choice** | Matches the existing `tools/vmsfs_master.c` mastering convention for the same class of system files (arrived at independently, not shared code) |

Nothing in this format is presented as PCSI-byte-authentic. Where OVMX had
no public layout to match, it made its own choice and says so, in the header
comment and here.

## 3. Container layout

Sequential, single file (constraint: it must eventually travel a VMS
filesystem as an ordinary file) — no compression, no random-access
structure beyond a flat index small enough to read in one shot before any
payload byte:

```
+--------------------------------------------+  offset 0
| struct ovmx_kit_header      (128 bytes)     |
+--------------------------------------------+  offset kh_index_offset (128)
| struct ovmx_kit_entry[kh_file_count]        |  (160 bytes each)
+--------------------------------------------+  offset kh_payload_offset
| file payloads, back-to-back, in index order |
+--------------------------------------------+
```

All multi-byte fields are native little-endian — OVMX targets only
little-endian architectures (x86-64, aarch64) today, so this is a
build-tool convenience, not a portability format.

Full field-by-field layout: `src/libvms/include/ovmx_kit_format.h` (the
single shared description — anyone consuming a kit, including the future
`vms-df9` installer, includes this header instead of re-deriving the
layout, exactly the pattern `vmsfs_ondisk.h` already established for
`vmsfs.ko` / `INITIALIZE.EXE` / `vmsfs_master`).

### 3.1 Header (`struct ovmx_kit_header`, 128 bytes)

| Field | Type | Meaning |
|---|---|---|
| `kh_magic` | `char[8]` | `"OVMXKIT1"` (raw bytes, no NUL) |
| `kh_format_version` | `uint32` | Container format version (currently 1) |
| `kh_product_name` | `char[40]` | e.g. `"OVMX X86VMS VMS"` |
| `kh_producer` | `char[16]` | e.g. `"OVMX"` |
| `kh_product_version` | `char[16]` | e.g. `"V0.1"` |
| `kh_build_time` | `uint64` | Seconds since epoch |
| `kh_file_count` | `uint32` | Number of index entries |
| `kh_index_offset` | `uint64` | Always `sizeof(header)` = 128 |
| `kh_payload_offset` | `uint64` | `kh_index_offset + kh_file_count * sizeof(entry)` |
| `kh_checksum` | `uint32` | XOR-fold checksum of the header with this field zeroed |

### 3.2 Index entry (`struct ovmx_kit_entry`, 160 bytes each)

| Field | Type | Meaning |
|---|---|---|
| `ke_filespec` | `char[128]` | Target VMS filespec, e.g. `"SYS$COMMON:[SYSEXE]DCL.EXE"` |
| `ke_protection` | `uint16` | VMS SOGW protection mask (`ovmx_fileprot.h` encoding) |
| `ke_uic_group` / `ke_uic_member` | `uint16` each | Owner UIC |
| `ke_size` | `uint64` | Payload length in bytes |
| `ke_offset` | `uint64` | Absolute byte offset of the payload in the kit file |
| `ke_checksum` | `uint32` | XOR-fold checksum of the file content |

### 3.3 Version single-sourcing (constraint)

`kh_product_version` is **always** `OVMX_PRODUCT_VERSION` from
`src/libvms/include/ovmx_identity.h` (INV-1, the identity single source of
truth) — the packer never carries a second hardcoded version literal.
`kh_producer` defaults to `OVMX_PRODUCT_NAME` from the same header.
`kh_product_name` is assembled at pack time as
`OVMX_PRODUCT_NAME + " " + <caller-supplied arch/product suffix>` (e.g.
`"OVMX" + " X86VMS VMS"`) — the vendor token is single-sourced; the
`"X86VMS VMS"` suffix is a literal the build script passes (an
architecture/product label, not a version), so bumping `OVMX_PRODUCT_VERSION`
changes every kit's version with no scattered edits.

## 4. The packer tool

`tools/ovmx_kit_pack.c` (built as plain `ovmx_kit_pack`, no `.EXE`, no VMS
name) is factory **build** tooling — never shipped on the media, never run
at boot, same class as `tools/vmsfs_master.c`. It has three modes:

```
ovmx_kit_pack pack    <kit-file> <staging-dir> <product-name> [producer]
ovmx_kit_pack list    <kit-file>
ovmx_kit_pack extract <kit-file> <output-dir>
```

- **`pack`** walks `<staging-dir>` recursively. Its **direct children** are
  the top-level VMS directories (`SYSEXE`, `SYSLIB`, `SYSMGR`, `SYSHLP`,
  ...); each file's target filespec is
  `SYS$COMMON:[<DIR[.SUBDIR]>]<NAME>.<TYPE>` (root-level files map to
  `[000000]`), mirroring the staged tree 1:1 — an OVMX build-tool policy
  choice, not a PCSI fact. Every packed file gets the default protection
  and owner UIC from §2. Output is deterministic (entries sorted by
  filespec) so kit builds are reproducible.
- **`list`** prints the product identification and the full manifest
  (filespec, size, UIC, protection rendered as `(S:RWED,O:RWED,G:RE,W:RE)`)
  — the inspection PCSI's `PRODUCT SHOW PRODUCT` needs before `vms-df9`
  exists to consume kits for real.
- **`extract`** is the round-trip reader: reverses each entry's filespec
  back into a relative host path and writes the payload out, verifying the
  per-file checksum on the way. This is the anti-LARP proof a kit actually
  carries file *bytes*, not a manifest of promises — `tests/integration/
  test_ovmx_kit_pack_roundtrip.sh` packs a synthetic tree (nested
  subdirectory, an empty directory, a 0-byte file, a binary file, a
  no-type file) and byte-compares every payload file against the source.

  Note: the kit format carries files, not empty directories — an empty
  staged directory has nothing to extract, which is expected and is why the
  round-trip test compares file-by-file rather than a whole-tree `diff -r`.

## 5. Build integration

`distro/Dockerfile.bootable`'s build stage packs the real OS payload: after
the fat-initramfs staging tree's `/vms/SYS0/SYSCOMMON/{SYSEXE,SYSLIB,SYSMGR,
SYSHLP}` is fully populated (DCL.EXE, LOGINOUT.EXE, the shareables,
STARTUP.COM, etc.), the build runs

```
ovmx_kit_pack pack /boot/ovmx-os.kit <staged SYSCOMMON tree> "X86VMS VMS"
ovmx_kit_pack list /boot/ovmx-os.kit
```

and the build fails if the manifest is missing `DCL.EXE`, `LOGINOUT.EXE`, or
`STARTUP.COM` — a ground-source gate baked into the build, matching the
pattern already used for the VMS-native LINK.EXE artifact checks earlier in
the same file.

## 6. Non-goals (this item stops here)

- No `PRODUCT.EXE` / `PRODUCT INSTALL` implementation.
- No product database (installed-kit registry).
- No consumption of a kit file at boot or install time.

All of the above are `vms-df9` (blocked by this item), which reads
`ovmx_kit_format.h` rather than re-deriving the layout.
