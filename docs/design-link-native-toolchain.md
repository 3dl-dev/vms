# OVMX VMS-Native Link + Activate Toolchain (LINK.EXE, path B)

> Status: DESIGN (bead vms-b49, pillar vms-ade). Supersedes the ELF-symbol-export
> approach (vms-55f) for how OVMX shareable images export/import symbols.
>
> **Operator ruling (2026-07-26): "no unix/linux, all VMS." Modern VMS, path B.**
> Wean OVMX off the Unix linker/loader (`ld`, `ld.so`/ld-musl, ELF dynamic-linking
> resolution). Build the VMS tools: **LINK.EXE** (the VMS linker) and **IMGACT.EXE**
> (SYS$IMGACT, the image activator), joined by the VMS **symbol vector**.

## 1. The ruling and why

Two facts set the architecture:

1. **Modern OpenVMS (I64 / x86-64) already uses ELF** relocatable objects and
   ELF-based images with VMS-specific structure. The classic `MHD/GSD/TIR/EOM`
   VMS Object Language is the **VAX/Alpha** lineage only. On our runtime arch
   (aarch64 / x86-64) the ELF `.o` that gcc emits **is** the modern-VMS object.

2. **Relocation is physical.** aarch64 (and x86-64) relocations pack bits into
   fixed instruction fields (`ADRP`/`ADD`/`CALL26`, etc.). The classic VMS TIR
   relocation model is a whole-aligned-value stack machine built for VAX
   displacement/absolute fixups. RISC instruction-field fixups **cannot** be
   laundered through TIR — the mismatch is at the silicon, not cosmetic.

Therefore: **do not build a C compiler, and do not translate ELF→classic-OBJ.**
Keep gcc emitting ELF `.o`. **LINK.EXE consumes ELF directly.** All the VMS-ness
lives in the **image** LINK.EXE produces and in how **IMGACT** activates it.

This is authentic to what x86/I64 VMS actually does — not a Linux shortcut.

## 2. Pipeline

```
   gcc  (C -> ELF .o)          [Unix compiler stays — for now; VMS-obj
        |                       compiler is the far-future language tier]
        v
   LINK.EXE   (the VMS linker — BUILD THIS)
        |  reads ELF .o (+ options: /SHAREABLE, SYMBOL_VECTOR=(...), GSMATCH=)
        |  emits an OVMX image (ELF container) carrying:
        |    - SYMBOL VECTOR   (universal symbols at fixed offsets)
        |    - GSMATCH         (version-match rule + major/minor)
        |    - VMS image ident (.vms.ident)
        v
   IMGACT.EXE  (SYS$IMGACT — the image activator)
        |  maps image, resolves referenced shareable images,
        |  binds imports through their SYMBOL VECTORs (NOT DT_HASH),
        |  enforces GSMATCH, transfers control
        v
   running VMS image
```

`ld` and `ld.so` drop out: LINK.EXE replaces the linker, IMGACT replaces the
dynamic loader, and the symbol vector replaces ELF hash-table symbol binding.

## 3. The symbol vector (the load-bearing VMS construct)

A VMS shareable image exports **universal symbols** through a **symbol vector**:
an ordered table of entries at **fixed offsets**. Its defining property (public,
grounded — see §5): a universal symbol's link-time value **is its offset from the
base of the symbol vector**, not an address in the image. Consumers bind to that
numeric **vector position** and remember only the offset at run time — never the
name. The vector's own location is recorded in the image header, so it need not be
anchored anywhere specific in memory (unlike the VAX transfer-vector model, §5.4).

Because offsets are the contract, upward compatibility is a matter of **never
disturbing existing offsets**: preserve entry order, never delete an entry (retire
it in place), append new symbols only at the end. A GSMATCH-compatible newer image
keeps every prior slot where it was, so consumers linked against the old version
keep binding correctly. This is VMS's upward-compatible shareable-image ABI, and
the reason VMS shareable images version far more cleanly than ELF `.so`s. It is the
piece that most distinguishes a VMS image from an ELF `.so`, and the piece OVMX
LINK.EXE + IMGACT must implement faithfully.

## 4. Clean-room posture (extends CLAUDE.md rule 8 to the toolchain)

All object/link/image/symbol-vector/GSMATCH details are derived ONLY from
**public VMS documentation** (VSI OpenVMS Linker Utility Manual; the "Migrating
to OpenVMS I64" / x86 porting guides; public symbol-vector and GSMATCH docs) and
**observed tool behavior**. NEVER from VSI/HPE source or binaries. This is the
same clean-room invariant that protects the cluster RE (rule 8) — extended here
to cover the link/image toolchain. (Proposed CLAUDE.md rule-8 wording update is
part of closing this bead.)

## 5. Grounded format (public VSI/HPE docs only)

Sourced from public documentation, clean-room. Primary source: **VSI OpenVMS
Linker Utility Manual, Ch. 4 "Creating Shareable Images (x86-64 and I64)," §4.2**
(https://docs.vmssoftware.com/vsi-openvms-linker-utility-manual/) and the archived
HP Alpha V8.3 equivalent
(https://www.digiater.nl/openvms/doc/alpha-v8.3/83final/4548/4548pro_021.html,
.../4548pro_028.html). Where the public docs are silent on byte-level layout, that
is stated explicitly and OVMX makes a **labelled design choice** (§5.6) rather than
claiming VMS-authenticity — the clean-room boundary.

### 5.1 `SYMBOL_VECTOR=` — declaring universal symbols  ✅ grounded
Syntax: `SYMBOL_VECTOR=(name=KEYWORD, ...)`. Each listed symbol gets a symbol-vector
entry **and** a global-symbol-table (GST) entry. Keywords:
`PROCEDURE` (entry-point address), `DATA` (address of relocatable/constant data),
`PSECT` (global data as an overlaid program section), `PRIVATE_PROCEDURE` /
`PRIVATE_DATA` (make a vector entry but exclude from the GST — used to *retire* a
symbol without disturbing offsets). Aliases (`alias/internal=KEYWORD`) allowed for
DATA/PROCEDURE only. **No `SPARE` keyword found** — retirement is via `PRIVATE_*`.
A universal symbol's link-time value is *"its location in the symbol vector,
expressed as an offset from the base of the symbol vector."*

### 5.2 Symbol-vector entry shape  ✅ grounded (VSI X86 Porting Considerations)
**x86-64: a vector entry is a *pair of quadwords* (16 bytes). I64: a single
quadword (8 bytes).** On x86-64 a function symbol's value is *always a code
address* (no GP, no short-data segment; the `SHORT` attribute is ignored).
Source: https://wiki.vmssoftware.com/X86_Porting_Considerations

### 5.3 Upward-compatibility rules  ✅ grounded
(1) preserve order/placement of existing entries; (2) never delete an entry —
retire with `PRIVATE_PROCEDURE`/`PRIVATE_DATA`; (3) add new symbols only at the
**end**. At activation the loader validates the referenced **index (I64) / offset
(Alpha)** is within the current vector's bounds, else fails — `LOADER-E-BADSVINDX`
(I64) / `IMGACT-F-SYMVECMIS` (Alpha). *(OVMX picks the authentic message set at
implementation; see §5.6.)*

### 5.4 `GSMATCH=` — version match  ✅ grounded
`GSMATCH=match-control,major-id,minor-id` stored in the image. At activation the
activator compares the executable's link-time GSMATCH vs the shareable image found:
**major mismatch ⇒ refuse** (unless `ALWAYS`); if majors match, compare minors by
match-control: **`LEQUAL`** ⇒ accept image minor ≥ linked minor (same/newer OK);
**`EQUAL`** ⇒ exact; **`ALWAYS`** ⇒ any version. **Default when omitted: `EQUAL`
with major/minor derived from image creation time** (so every relink is distinct).
No `NEVER` keyword found. (Matches the imgact.c/913.4 GSMATCH scaffolding.)

### 5.5 Image identification  ✅ grounded (partial)
`IDENTIFICATION=` option, ≤15 chars. **On I64 the ident string is stored in an ELF
NOTE section** (Alpha/VAX: image header). A binary-ident form exists (24 bits minor
+ 8 bits major, `EIDC$V_BINIDENT`, match `EIDC$C_LEQ`/`EIDC$C_EQUAL`) in the older
Alpha image-file-format appendix — *not confirmed to persist verbatim on x86-64
ELF*. New I64/x86 linker qualifiers `/EXPORT_SYMBOL_VECTOR`,
`/PUBLISH_GLOBAL_SYMBOLS` are named in the porting guide.

### 5.6 NOT PUBLICLY SPECIFIED → OVMX labelled design choices
The public docs do **not** publish these, so OVMX defines its own representation,
documented AS an OVMX choice (not a VMS fact):
- ⚠️ **Exact ELF section name / on-disk byte layout carrying the symbol vector on
  x86-64/I64.** Public docs say only "the symbol table is an ELF section" + the
  entry *shape* (§5.2). OVMX will define a named section (proposal: `.vms$sv`) and
  document its layout as OVMX-original. *(Decide in vms-9dd/LINK.EXE.)*
- ⚠️ Whether the EIHI binary-ident record (§5.5) applies on x86 ELF — OVMX chooses
  its ident carrier (likely the ELF NOTE, per §5.5) explicitly.
- ⚠️ `NEVER`/`SPARE` keywords — treated as nonexistent unless later grounded.

### 5.7 Activation resolution algorithm (OVMX, built on grounded pieces)
map image → locate each referenced shareable image → check **GSMATCH** (§5.4) →
bind each import to its producer's **symbol-vector position** (§5.1–5.3), bounds-
checking the index → apply ELF relocations (913.2 mechanics) → transfer control.
This replaces DT_HASH/DT_NEEDED name-hash resolution with VMS vector-position
binding.

### 5.8 ELF is the modern-VMS container  ✅ grounded
I64 object + image files conform to 64-bit ELF (System V ABI draft 2001-04-24);
x86-64 likewise. Confirms path B: keep ELF, put VMS semantics in the image.
Source: VSI "Porting Applications … Alpha to I64"
(https://docs.vmssoftware.com/vsi-openvms-porting-applications-from-vsi-openvms-alpha-to-vsi-openvms-industry-standard-64-for-integrity-servers/).

## 6. Relationship to existing work

- The **913.2 ELF loader** (freestanding IMGACT) proved the clean-room ELF
  mapping + relocation mechanics. Those mechanics stay valid; bead vms-8d5 swaps
  the **symbol-resolution model** from DT_HASH to symbol vectors.
- The **OVMX_IMGACT cmake mode** (vms-913.3) and DT_HASH images are a **bootstrap
  crutch** — kept working, not deepened, superseded image-by-image by vms-b65.
- **vms-55f / vms-034** (ELF-symbol-export / DCL-e2e over DT_HASH) are superseded
  by the symbol-vector path and re-sequenced behind vms-b65.
