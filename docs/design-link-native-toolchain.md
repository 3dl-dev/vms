# OVMX VMS-Native Link + Activate Toolchain (LINK.EXE, path B)

> Status: **IMPLEMENTED** (bead vms-b49, pillar vms-ade). Supersedes the
> ELF-symbol-export approach (vms-55f) for how OVMX shareable images export/import
> symbols. §§1-6 are the original design (still current); §7 documents the
> as-built toolchain — whole-archive ingestion, DECC$SHR, weak/strong symbol
> resolution, cross-image import binding, TLS-producer coexistence, and the first
> real OVMX-library migration — with known limitations.
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

## 7. As-built: the LINK.EXE + IMGACT toolchain (OVMX design, clean-room per §4)

The pieces below are **OVMX-original engineering** — how LINK.EXE (`src/vmslink/link.c`)
and IMGACT (`src/imgact/imgact.c`) actually implement §§1-6 in ELF terms. Nothing
here claims VMS-format authenticity beyond what §5 already grounds (symbol vector,
GSMATCH, upward compatibility); the resolution *mechanics* (archive parsing,
GOT/PLT synthesis, TLSDESC rewriting) are ordinary ELF linker/loader engineering,
labelled as such, not derived from or compared against VSI's actual LINK.EXE
internals.

### 7.1 Whole-archive `.a` ingestion (vms-004)
LINK.EXE parses `ar` archives **in-process** — no `ld -r` staging step. `load_archive()`
walks the archive's member table and pulls **every** object member into a growable
`objs[]` array (`src/vmslink/link.c`, `load_archive`/`file_is_archive`). A data-only
member with no symbol table and no relocations (e.g. musl's `stdout.lo`) is accepted
as an empty stub rather than aborting the link. Symbol resolution runs over a global
defined-symbol hash so the whole musl `libc.a` (1345 members) links in seconds, not
minutes. `.rela.data` **ABS64** pointer-initializer relocations (`S+A` written as a
64-bit word — stdio `FILE` structs, locale/pointer tables) are resolved and biased
through a `.vms$rel` image-relative slot at activation, alongside GOT cells.
Verified in `src/vmslink/test/run_test.sh` (multi-object merge, proofs 118/99) and
exercised at archive scale by `run_decc_shr.sh` below.

### 7.2 DECC$SHR — the C-RTL shareable (vms-61f.1, vms-61f.2)
`src/vmslink/mk_decc_shr.sh` whole-archives musl's `libc.a` **and** `libgcc.a`
(soft-float/long-double/complex builtins musl's stdio/printf reference internally)
into a single OVMX shareable, `DECC$SHR.EXE`, exporting the C run-time universals
(`malloc`/`free`/`memcpy`/`strlen`/`printf`/... — see the script for the full list)
as `PROCEDURE` universals in `.vms$sv`. Composition is **strict**: no
`--allow-undefined`, so the image links with **zero deferred externals**.

- **`init_array`/`_DYNAMIC` weak-undef → 0**: musl's `libc.a`/`libgcc.a` carry no
  `.init_array`/`.fini_array` and no dynamic section, so the linker-defined boundary
  symbols (`__init_array_start`/`end`, `__fini_array_start`/`end`, `_DYNAMIC`) are
  weak-undefined with no def anywhere in the archive set. LINK.EXE resolves a
  weak-undefined symbol to address 0 — standard ELF semantics — rather than
  aborting the link (`link.c`, `weak_add`/`resolve_ref`, §7.3 below).
- **Runtime init at activation (vms-61f.2)**: `DECC$SHR.EXE`'s `.vms$sv` also
  exports musl's own bootstrap, `__init_libc`, by name (not consumer-callable —
  looked up by IMGACT, not bound by any consumer's imports). At activation IMGACT
  resolves `__init_libc` from the C-RTL producer's symbol vector and **calls it**
  with the real process `envp`/`argv[0]` (`imgact.c`, `drive_crtl_init` /
  `find_crtl_producer`) before transferring control. `__init_libc` internally runs
  musl's `__init_tls`/`__init_tp` (programs the thread pointer, builds the TCB,
  allocates any main-program TLS) and `__init_ssp` — exactly what musl's own
  `crt`/`__libc_start_main` do, driven from outside rather than replicated inside
  IMGACT. Reading/mirroring musl's MIT-licensed ldso/env sources for this purpose
  is permitted (not a VSI/HPE clean-room concern — musl is not VMS).
- **Tests**: `src/vmslink/test/run_decc_shr.sh` (link-clean assertion + OVMXDUMP
  universal listing; CI job `decc-shr`), `src/imgact/test/run_decc_shr_activation.sh`
  (end-to-end: a consumer calls real musl `malloc`/`snprintf`/`strtod`/`free`
  through IMGACT-driven activation, exit code proves the arithmetic; CI job
  `decc-shr-activate`).

### 7.3 Weak/strong symbol override (vms-36a)
`resolve_ref()` (`link.c`) honors ELF weak-vs-strong override when resolving a
reference **by name**: if the reference's own definition is `STB_WEAK` but a
`STB_GLOBAL` (strong) definition of the same name exists elsewhere in the link,
the strong definition wins — matching standard ELF/`ld` symbol-resolution
semantics, not a VMS-specific rule. This mattered concretely for musl's allocator:
`malloc`/`free` have both a strong `mallocng` implementation and a weak
`__simple_malloc` fallback in `libc.a`; without the override a `free()` call could
bind to the strong `free` while some caller had bound `malloc` to the weak
`__simple_malloc`, producing a buffer with no `mallocng` metadata and a SIGSEGV on
free. `run_decc_shr_activation.sh`'s exit code (14) is only reachable with this fix
in place — it exercises a `malloc`+`snprintf`+`strtod` compute **and** a
`free()`/second `malloc`+`free` pair.

### 7.4 Cross-image import binding for shareables (vms-e65)
Before vms-e65, only a leaf **executable** could import universals from a `--use`
producer (`emit_executable`). vms-e65 extends the same binding to a **shareable**
image being built by `emit_shareable`: a CALL/GOT reference not defined by any
input object of the shareable itself, but exported by one of its own `--use`
producers, becomes a **cross-image import** — LINK.EXE emits a PLT stub (for CALL
sites) and/or an import-GOT cell (for GOT/DATA references), recorded in the
shareable's own `.vms$imp` section (producer soname + symbol-vector index per
entry). At activation IMGACT applies a producer's `.vms$imp` **transitively**: a
consumer that imports from a lib shareable, which itself imports from DECC$SHR,
gets DECC$SHR's bindings pulled in without the consumer naming DECC$SHR directly
(`imgact.c`, `apply_vms_rel` + the transitive producer-load path, see the
"transitive imports" comments near `load_ovmx_producer`). This is the spine
increment that unblocks every real OVMX-library migration (the b65 chain, §7.6):
a library can both **export** universals in its own `.vms$sv` and **import** libc/
pthread universals from DECC$SHR in the same image.
Test: `src/imgact/test/run_shareable_import_activation.sh` (TESTLIB$SHR.EXE
exports `lib_compute`, imports `pthread_mutex_lock`/`unlock`+`malloc`+`memset`+
`free` from DECC$SHR; CI job `shareable-import-activate`).

### 7.5 TLS-producer / C-RTL coexistence (vms-616)
vms-61f.2 made C-RTL TLS ownership and IMGACT-managed TLSDESC ownership **mutually
exclusive** in one process: musl (once `__init_libc` runs) owns the thread pointer
and its own TCB layout, and knows nothing about any other image's TLS module. A
real OVMX library shareable is typically **both** a TLS producer (its own
`.tdata`/`.tbss` + TLSDESC entries) **and** an importer of libc/pthread universals
from DECC$SHR — it must coexist with musl's TP ownership rather than own the TP
itself. `setup_producer_tls_over_crtl()` (`imgact.c`) resolves this: for each
non-C-RTL producer with a TLS module, IMGACT allocates the module its own
per-thread block (anonymous `mmap`, module image copied in, `.tbss` zeroed) and
rewrites the producer's **static** TLSDESC entries relative to musl's
already-programmed TP — `entry[1] += (block − TP)` — so the standard TLSDESC access
sequence (`TP + returned offset`) lands inside the IMGACT-owned block without
replicating musl's private `struct pthread`/DTV internals.
**Scope, stated in the code**: correct for the **main thread only**, which is all
OVMX activation exercises today; a program that spawns pthreads would need the
module registered into each new thread's block by musl itself (a resurrected
`__tls_get_new` / true DTV registration path) — deferred, tracked as vms-244
(§7.7).
Test: `src/imgact/test/run_tls_producer_over_crtl.sh` (TLSLIB$SHR.EXE is a TLS
producer that also imports from DECC$SHR; CI job `tls-producer-over-crtl`).

### 7.6 The b65 library-migration template (vms-b65.1)
`src/vmsprocess` is the **first real OVMX library** migrated onto the VMS-native
toolchain, and the worked template for the rest of the chain (`libvms` → `vmslnm`
→ `vmsfs` → `vmsrms` → `vmsdcl`). Recipe (`src/vmslink/mk_vmsprocess_shr.sh`):

1. Compile the library's translation units `-fPIC -O2 -ffreestanding -fno-builtin
   -fno-stack-protector -mno-outline-atomics` (musl target). `-fno-builtin` keeps
   `memset`/`strncpy`/... as real `CALL26` imports to DECC$SHR instead of inlined
   builtins; `-mno-outline-atomics` avoids `__aarch64_*` outline-atomic helpers
   DECC$SHR does not export (small inline atomics stay lock-free).
2. Enumerate universals (every non-static function defined across the objects) →
   the `--symbol-vector` list, all `PROCEDURE`, cross-checked against the library's
   public header. Order is append-only (§3, upward compatibility) — never reorder
   or delete once a consumer binds an index.
3. Enumerate imports (`nm *.o | awk '$1=="U"'`) and confirm every one is already a
   DECC$SHR universal; append any gap to `mk_decc_shr.sh`'s vector (never insert —
   append-only keeps existing consumers' bound indices valid, a GSMATCH `LEQUAL`-
   compatible change). `getpid` was the one gap found for vmsprocess
   (`vms_get_current_process`).
4. `LINK.EXE --shareable --use DECC$SHR --symbol-vector "<vec>" --gsmatch
   LEQUAL,1,0 -o LIB<NAME>$SHR.EXE <objs>` — **strict**, no `--allow-undefined`:
   every libc/pthread import must bind.
5. Verify: an executable `LINK.EXE --executable --use LIB<NAME>$SHR` a consumer,
   run it for real, assert a VMS-correct result via exit code; DECC$SHR's own
   bindings arrive transitively through the lib's `.vms$imp` (§7.4).

**vmsprocess concretely**: `LIBVMSPROCESS$SHR.EXE` exports the process-control
universals (`vms_pcb_*`, `eflag_*`, `ast_*`, `access_mode_*`, `priv_*`,
`vms_get_current_process`/`vms_pid_from_linux`/`vms_format_uic`/`vms_parse_uic`)
and imports `pthread_mutex_*`/`pthread_cond_*`, `malloc`/`calloc`/`free`,
`memset`/`strncpy`/`snprintf`/`sscanf`, `getpid`, `close`/`raise`/`sigaction`/
`sigemptyset` from DECC$SHR — exercising §7.4 (cross-image import binding) and
§7.5 (TLS coexistence) **together**, since it is also a TLS producer. Its
per-thread state is consolidated into a **single** TLS-defining object
(`vms_pcb.c`'s `current_pcb`): `vms_process.c`'s process-snapshot cache
(`vms_get_current_process`) is deliberately a plain (non-`__thread`) fallback
static keyed off the PCB rather than a second `__thread` variable, because
LINK.EXE supports only one TLS-defining object per image (§7.7) — vmsprocess was
shaped to fit that constraint rather than the constraint being lifted.
Test: `src/imgact/test/run_vmsprocess_native.sh` (CI job `vmsprocess-native`,
`vmsprocess VMS-native Migration (LIBVMSPROCESS$SHR)`).

### 7.7 Known limitations (as-built, tracked)
- **Multi-module TLS unsupported** — `emit_shareable` accepts only **one**
  TLS-defining object per image; a second object in the same image with its own
  `.tdata`/`.tbss` aborts the link (`link.c`: `"multi-module TLS not supported yet
  (one TLS object per image)"`). Multiple `__thread` variables **within** one
  object are fine (they merge into that object's `.tbss`). This is why vmsprocess
  (§7.6) consolidated onto a single TLS object rather than the linker growing a
  general multi-object TLS layout. General support (per-object TLS offsets merged
  into one image TLS block + `.vms$tls` fixups) is tracked as **vms-212**, along
  with the related **ABS64-to-producer cross-image data pointer** gap: §7.1's
  `.rela.data` ABS64 handling resolves pointer initializers **within** an image
  (via `.vms$rel`), but an ABS64 reference to a symbol defined only in a `--use`
  producer is not bound the way CALL/GOT cross-image imports are (§7.4) — not yet
  supported.
- **Producer TLS is single-thread today** — `setup_producer_tls_over_crtl()`
  (§7.5) is correct for the process's main thread, which is all current OVMX
  activation exercises. A program that spawns additional pthreads would need each
  new thread's block populated with the producer's TLS module by musl itself (a
  DTV-registration path), which is not implemented. Tracked as **vms-244**.
