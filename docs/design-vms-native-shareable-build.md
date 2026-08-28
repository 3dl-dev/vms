# Design: VMS-Native Shareable-Image Build (`.OLB` + `SYMBOL_VECTOR`, no hand TU-lists)

> Status: **DESIGN — operator approval required before any migration.** Docs-only.
> Rule 8 (clean-room): every Part A claim is grounded in the public VSI OpenVMS
> Linker Utility Manual / LIBRARIAN Utility Manual, or explicitly labelled as an
> OVMX design choice. No VSI/HPE source or binary was consulted.
> Companion: `docs/design-link-native-toolchain.md` (the LINK.EXE/IMGACT toolchain,
> §5 symbol-vector grounding) and `docs/design-self-host-mmk-spine.md` (LIBRARIAN,
> §3 `.OLB` container).

## 0. Problem statement (why this design exists)

The OVMX VMS-native shareable-image build (`LINK.EXE --shareable`, path B) is driven
by six hand-maintained recipes — `src/vmslink/mk_vmssys_shr.sh`,
`mk_vmsprocess_shr.sh`, `mk_vmslnm_shr.sh`, `mk_vmsfs_shr.sh`, `mk_libvms_shr.sh`,
`mk_vmsrms_shr.sh`. Each **hand-lists the translation units** of its library in an
inline shell list, a *second* copy of the source list that already lives in the
library's `CMakeLists.txt`. Nothing proves the two agree.

They have already drifted. `mk_libvms_shr.sh` documents the last incident in its own
banner (vms-2f8: `rtl/rightslist.c` added to `src/libvms/CMakeLists.txt` and not to
the recipe; every host test passed; the VMS-native DCL link job went red five minutes
later on an undefined `rightslist_name_to_value`). A live instance exists **today**:
`src/libvms/CMakeLists.txt` lists `prv_agreement.c` in `add_library(vms …)`; the
`mk_libvms_shr.sh` `LIST=` does not. (That one is benign — `prv_agreement.c` is a
compile-time `_Static_assert` guard that defines no referenced universal — but it is
the exact divergence class that bites the moment the missing TU defines a symbol a
consumer imports.) The drift is **invisible on a full dev build and on host ctest**;
it only reddens in the native-link CI job.

The fix must be **how VMS actually builds a shareable image**, not a bolted-on codegen
step that diffs two lists. This document establishes the VMS model (Part A), locates
the OVMX gap precisely (Part B), and proposes the VMS-faithful build (Part C) with the
one LINK.EXE capability gap that blocks it.

---

## Part A — how REAL VMS builds a shareable image

Primary source: **VSI OpenVMS Linker Utility Manual**
(https://docs.vmssoftware.com/vsi-openvms-linker-utility-manual/, PDF
DO–DLKRRM–01A), with the archived HP OpenVMS Linker Utility Manual (V8.4,
`4548pro_*`) as the corroborating older edition, and the **VSI/HP LIBRARIAN Utility
Manual** for the object-library half.

### A.1 Object libraries (`.OLB`) and how LINK searches them

An **object module library** (`.OLB`) is a container of object modules plus a
**library symbol table (name table)** mapping each globally-defined symbol to the
module that defines it. LIBRARIAN builds and maintains it (`LIBRARY/CREATE/OBJECT`,
`/INSERT`, `/DELETE`, `/LIST`), and maintains the symbol table as modules are inserted
(LIBRARIAN Utility Manual). The developer does **not** enumerate modules to the linker
— the linker discovers them.

The linker resolves references in two distinct ways (Linker Utility Manual §1.2.3,
"How the Linker Processes Input Files", and Table 1.3 / Table 1.4 qualifiers):

- **Default library search.** When LINK has an unresolved (strong) reference and a
  file is presented with **`/LIBRARY`**, the linker *"searches the library's name
  table for the definitions of symbols referenced in the other input files it has
  processed previously"* (§1.2.3.2). When a name is found, *"the linker includes the
  associated library element in the link operation and processes it as it would any
  other object module."* **Only the modules needed to satisfy outstanding references
  are pulled** — the rest of the library is never linked in. Search **order matters**:
  a library is searched against references accumulated from files processed *before*
  it. The default system libraries are searched last (via `LNK$LIBRARY*` logicals;
  suppressed with **`/NOSYSLIB`**).

- **Selective processing is a library-only capability.** *"The linker can process
  object modules selectively in an object module library (.OLB) file only. …It cannot
  selectively process object modules within an object file"* — an object file (`.OBJ`)
  is always ingested **whole** (every module in it). This is the crux: selectivity is
  a property of the **library**, not of the object.

- **`/INCLUDE` — force-load named modules.** Appending **`/INCLUDE=(module,…)`** to a
  library spec pulls those named modules *unconditionally*, whether or not anything
  references them, and — importantly — *"the linker does not process the name table of
  a library file specified using the `/INCLUDE` qualifier"* (§1.2.3.2). `/INCLUDE` is
  the deliberate "I want this module in, referenced or not" hook.

- **`/SELECTIVE_SEARCH`** (Table 1.3) narrows what a processed input *contributes* to
  the global symbol table to only those symbols actually referenced by
  previously-processed inputs — a GST-hygiene qualifier, distinct from library search.

Net: in real VMS the developer's per-build act is *not* "list the modules." It is
"here is the `.OLB` (or the `LNK$LIBRARY` default) — resolve what you need." **Which
modules end up in the image is the linker's job**, derived from the reference graph.

### A.2 `SYMBOL_VECTOR=` — declaring the universal interface

A shareable image exports its callable/importable interface through a **symbol
vector**, declared in a linker options file (Linker Utility Manual Ch. 4, "Creating
Shareable Images (x86-64 and I64)," §4.2; and `docs/design-link-native-toolchain.md`
§5.1, already grounded for OVMX):

```
SYMBOL_VECTOR = (name = PROCEDURE, -
                 name = DATA, -
                 name = PSECT, ...)
```

Each listed symbol gets **both** a symbol-vector entry **and** a global-symbol-table
entry; keywords are `PROCEDURE` (entry-point address), `DATA` (data address), `PSECT`
(overlaid data section), and the retirement keywords `PRIVATE_PROCEDURE` /
`PRIVATE_DATA` (a vector entry kept out of the GST — used to retire a symbol without
disturbing later offsets). A universal symbol's link-time value is *"its location in
the symbol vector, expressed as an offset from the base of the symbol vector"* (§5.1).

**Position/order is the ABI.** A consumer image binds an imported universal by its
**index/offset** into the producer's vector. The upward-compatibility rules (Linker
Utility Manual, and §5.3 as grounded) are therefore: **(1) preserve the order and
placement of existing entries; (2) never delete an entry — retire it in place with
`PRIVATE_*`; (3) add new symbols only at the end.** Break any of these and every later
index shifts, invalidating already-linked consumers.

### A.3 `GSMATCH=` — versioned binary compatibility

`GSMATCH = match-control, major-id, minor-id` (Table 1.4; §5.4) stamps the shareable
with a version and a match rule. At activation the loader compares the consumer's
link-time GSMATCH against the shareable it finds: **major mismatch ⇒ refuse**; if
majors match, **`LEQUAL`** accepts an image minor ≥ the linked minor (same-or-newer),
**`EQUAL`** demands exact, **`ALWAYS`** any. `LEQUAL` is what makes "append a new
universal, bump the minor, old consumers keep working" sound — **and it is sound only
because the append-only rule of A.2 keeps every existing index fixed.** GSMATCH and the
symbol-vector ordering are one contract.

### A.4 The authentic pipeline

```
   .C ──DCC──▶ .OBJ ──LIBRARIAN /CREATE──▶ FOO.OLB   (library + symbol table)
                                              │
   FOO.OPT:  ┌─────────────────────────────┐ │
             │ SYMBOL_VECTOR=(a=PROCEDURE,  │ │
             │                b=PROCEDURE,   │ │   selective search of FOO.OLB:
             │                t=DATA, ...)   │ │   LINK pulls only the modules
             │ GSMATCH=LEQUAL,1,0           │ │   needed to define the vector
             └─────────────────────────────┘ │   (+ their transitive refs)
                          │                   ▼
                          └────▶ LINK /SHAREABLE FOO.OPT/OPTION, FOO.OLB/LIBRARY
                                              │
                                              ▼
                                        FOO_SHR.EXE  (symbol vector @ fixed offsets)
```

The developer's **only per-symbol act is the deliberate `SYMBOL_VECTOR` line**.
"Which modules go in" is derived by LINK from the reference graph rooted at the symbol
vector, searched selectively out of the `.OLB`. Confirmed against Linker Utility Manual
§1.2.3 (library search / selective processing), Ch. 4 §4.2 (`SYMBOL_VECTOR`, `GSMATCH`),
and the LIBRARIAN Utility Manual (`.OLB` creation + symbol table).

---

## Part B — how OVMX does it now, and where the gap is

### B.1 The recipes hand-list objects and link them directly

Every OVMX-source shareable recipe enumerates its TUs inline and links the resulting
objects directly — **no `.OLB`, no selective search**:

| Recipe | Source enumeration | file:line |
|---|---|---|
| `mk_vmssys_shr.sh` | `for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif kif_transport_linux` | `src/vmslink/mk_vmssys_shr.sh:66` |
| `mk_vmsprocess_shr.sh` | `for u in vms_pcb vms_process ast access_modes` | `mk_vmsprocess_shr.sh:69` |
| `mk_vmslnm_shr.sh` | `for u in lnm_table lnm_translate lnm_client lnm_defaults` | `mk_vmslnm_shr.sh:75` |
| `mk_vmsfs_shr.sh` | `for u in vmsfs_translate vmsfs_version vmsfs_case vmsfs_protect vmsfs_device` | `mk_vmsfs_shr.sh:74` |
| `mk_libvms_shr.sh` | `LIST="descrip status syssvc/… rtl/…"` (~55 TUs) | `mk_libvms_shr.sh:106` |
| `mk_vmsrms_shr.sh` | `LIST="rms_core rms_seq rms_rel rms_idx rms_record rms_parse rms_search rms_util"` | `mk_vmsrms_shr.sh:90` |

Each `LIST` is a **second source of truth** for the corresponding
`add_library(...)` in `src/libvms/CMakeLists.txt` et al. `mk_libvms_shr.sh:96`
states it outright: *"THIS LIST IS A SECOND SOURCE OF TRUTH FOR
src/libvms/CMakeLists.txt, AND NOTHING CHECKS THAT THE TWO AGREE."* `mk_vmssys_shr.sh:8`
names *"the same drift risk."* The making-them-agree work is filed as vms-79f. The
recipes compile with musl freestanding `-fPIC` flags — **different objects** than the
dev build's — so they legitimately recompile; but the **module list** they iterate is
a hand-kept duplicate.

**Live drift instance (this repo, origin/main):** `src/libvms/CMakeLists.txt:66`
lists `prv_agreement.c`; `mk_libvms_shr.sh`'s `LIST` does not (`grep -c prv_agreement
= 0`). Benign today only because that TU exports no consumed universal.

### B.2 LINK.EXE and LIBRARIAN already support the VMS-faithful path — the recipes just don't use it

The capability the recipes bypass **already exists in the toolchain**:

- **LIBRARIAN.EXE** builds `.OLB` object libraries: `LIBRARIAN /CREATE lib.olb file.obj
  …`, `/INSERT`, `/DELETE`, `/LIST`, `/EXTRACT` (`src/vmslink/librarian.c:26-30`). The
  `.OLB` is an OVMX-labelled `ar` container of ELF `.OBJ` members
  (`src/vmslink/include/ovmx_olb.h`; VSI does not publish the LBR byte layout, so this
  is an OVMX design choice per Rule 8).

- **LINK.EXE** does **selective library search** with fixpoint iteration: `.OLB` inputs
  are pre-parsed into a candidate pool (`load_olb_pool`, `link.c:607`) and
  `resolve_olbs` (`link.c:718`) pulls only members that define a currently-unresolved
  **strong** reference, iterating until no more are pulled — *"a pulled member may
  reference symbols another member defines"* (`link.c:566-571`). A `.OLB` is searched
  selectively; a `.a` archive is still ingested whole (`file_is_olb`, `link.c:593`).
  This is exercised end-to-end by the CI job **"LIBRARIAN.EXE .OLB + LINK.EXE Selective
  Pull + Activate (self-host spine #3, x86_64)"** (`.github/workflows/ci.yml:4922`).

- **`--symbol-vector` + `--gsmatch`** already declare the universal interface and the
  version match rule at link time, and `--use <producer>.EXE` binds cross-image imports
  to the six-shareable graph (`link.c:2360`, and `emit_shareable`).

So OVMX has both halves — it simply **does not wire the shareable recipes through the
`.OLB` + selective-search path**. The recipes predate LIBRARIAN.EXE/selective-search
(the vms-b65 lib-migration chain, vms-b6a) and were never retrofitted.

### B.3 The `.vec` / `.frozen` manifest IS already the VMS `SYMBOL_VECTOR` — keep it

`src/vmslink/libvms_shr.vec` and `libvmssys_shr.vec` are **exactly** the VMS symbol
vector as an options file: an **ordered, append-only, frozen** manifest of
`NAME=CLASS` universals where **a line's position is its index**
(`mk_libvms_shr.sh:132-160`; `symvec_emit.sh` is the single parser;
`tests/integration/test_symvec_freeze.sh` proves the committed `*.vec.frozen` is still
an exact ordered prefix of the live manifest — reorder/drop ⇒ RED, append ⇒ GREEN).
This is the deliberate ABI contract of A.2/A.3 and it is **VMS-right**. It stays.

### B.4 The one thing that is *not* VMS-faithful (the gap, precisely)

Two mechanisms in `mk_libvms_shr.sh` / `mk_vmsrms_shr.sh` invert the VMS model:

1. **The module list is hand-kept** (B.1) instead of derived by the linker from the
   reference graph. VMS: the `.OLB` holds every module; LINK pulls what the vector
   roots. OVMX: the human lists modules and links them whole.

2. **The exported vector is auto-grown by `nm`.** `mk_libvms_shr.sh` generates the
   `--symbol-vector` by running `nm` over *all* compiled objects, taking every global
   `T`/`D`/`B`/`R` symbol as a universal, then merging with the frozen manifest and
   **appending any newly-discovered universal** (`mk_libvms_shr.sh:118-165`).
   `mk_vmsrms_shr.sh:96` exports *every* `nm` `T` symbol. In VMS the exported set is
   **exactly what `SYMBOL_VECTOR` declares** — export is a deliberate act, not "whatever
   `nm` found." The auto-append means a new non-static symbol silently becomes ABI.

The fix reunifies both: **the `.OLB` (built from the one CMake source list) is the pool;
the frozen `.vec` manifest is the root set and the exported interface; LINK derives the
modules.**

---

## Part C — the design

### C.1 Target pipeline (per OVMX-source library)

```
 CMakeLists add_library(vms …)  ──(one source list)──▶  compile -fPIC musl freestanding
        │                                                        │  N × .OBJ
        │  file(GENERATE) libvms.srclist                         ▼
        └──────────────▶ mk_libvms_shr.sh reads srclist ── LIBRARIAN /CREATE libvms.OLB
                                                                 │
     src/vmslink/libvms_shr.vec  ──symvec_emit.sh──▶ --symbol-vector (roots + interface)
                                                                 │
     LINK.EXE --shareable --symbol-vector … --gsmatch LEQUAL,1,0 \
              --use DECC$SHR --use LIBVMSPROCESS$SHR … libvms.OLB   (SELECTIVE)
                                                                 ▼
                                                         LIBVMS$SHR.EXE
```

Every recipe becomes: **(a)** read the CMake-generated source list; **(b)** compile
each source to `.OBJ` with the existing musl freestanding `-fPIC` flags; **(c)**
`LIBRARIAN /CREATE lib.OLB *.OBJ`; **(d)** `LINK.EXE --shareable --symbol-vector
$(symvec_emit.sh lib.vec) --gsmatch … --use <producers> lib.OLB`. LINK pulls the
members the symbol vector (and their transitive refs) require, out of the `.OLB`.

### C.2 Single source of truth for the module list

CMake already owns the authoritative list in each `add_library(...)`. Materialize it
once so the container recipe consumes it instead of re-typing it:

- In each library's `CMakeLists.txt`, emit a source manifest from the target's
  `SOURCES` property, e.g. `file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/vmslink/libvms.srclist
  CONTENT "$<JOIN:$<TARGET_PROPERTY:vms,SOURCES>,\n>")` (paths made relative to the
  library source dir). `build_link_native.sh` already runs under `cmake --build`
  (`src/vmslink/CMakeLists.txt:150-176`, `link_native_graph`), so the generated
  `.srclist` is available to every `mk_*_shr.sh` with zero new plumbing.
- `mk_*_shr.sh` reads `<lib>.srclist` in place of its inline `LIST=`/`for c in …`.
- A tiny gate (fold into the existing native-link CI or `test_symvec_freeze.sh`'s
  tree-wide checks) asserts **no `mk_*_shr.sh` contains an inline TU list** — the
  mechanical guarantee that the single source can't be re-forked.

This is the VMS-faithful shape: the `.OLB` is built from *the same source list the dev
build compiles*, materialized by the build system, never hand-copied.

### C.3 What this DELETES vs KEEPS

**DELETE**
- The inline `LIST=` / `for c in …` TU enumerations in all six `mk_*_shr.sh`
  (`mk_vmssys_shr.sh:66`, `mk_vmsprocess_shr.sh:69`, `mk_vmslnm_shr.sh:75`,
  `mk_vmsfs_shr.sh:74`, `mk_libvms_shr.sh:106`, `mk_vmsrms_shr.sh:90`) — the drift bug.
- The **`nm`-based auto-discovery + auto-append** of universals in `mk_libvms_shr.sh`
  (`:118-165`) and the whole-`nm` export in `mk_vmsrms_shr.sh` (`:96`). Export becomes
  a deliberate `.vec` line only.
- vms-79f's premise ("make the two lists provably agree") — obviated: there is one
  list, not two to reconcile.

**KEEP (VMS-authentic, unchanged)**
- The frozen, ordered, append-only `.vec` manifests + `.frozen` goldens + `symvec_emit.sh`
  + `test_symvec_freeze.sh`. This *is* `SYMBOL_VECTOR` (A.2) and its upward-compat rule
  (A.3). Adding an export = one freeze-gated line. This is the legitimate ABI edit.
- `--gsmatch LEQUAL,1,0` and the `--use` producer graph (cross-image binding).
- LIBRARIAN.EXE and LINK.EXE selective search — now *used* by the product build, not
  only by the self-host proof job.

### C.4 Feasibility — does LINK.EXE selective search handle the real graph?

**The dependency graph within one library:** yes. `resolve_olbs` iterates to a fixpoint
across members of a pool (`link.c:718-758`), so intra-library transitive pulls (member
A pulls member B that A references) already work and are CI-proven.

**The six-shareable `--use` graph:** yes, and it is *not* a selective-search problem.
Each shareable searches **only its own `.OLB`**; references that leave the library
(libc → DECC$SHR, `vms_pcb_*` → LIBVMSPROCESS$SHR, `vms_kif_*` → LIBVMSSYS$SHR,
`vmsfs_*` → LIBVMSFS$SHR, etc.) are **cross-image imports** bound by `--use` in
`emit_shareable`, *after* `resolve_olbs` runs (main sequences `resolve_olbs` →
`emit_shareable`). A strong undefined ref that neither the `.OLB` nor any `--use`
producer defines is a hard error (strict link, no `--allow-undefined`) — the same
integrity the recipes have now. So the graph shape is already handled.

#### C.4.1 CAPABILITY GAP — the one blocker rung

**`resolve_olbs` does not seed the undefined-root set from the `SYMBOL_VECTOR`.** Today
it builds the unresolved set `U` **only from already-loaded root objects**
(`link.c:731-740`) and the symbol vector (`uv/nuniv`) is parsed but **not passed to
`resolve_olbs`** (`link.c:2404`). Consequence: a shareable linked from **`.OLB` alone**
(no root `.OBJ` on the command line) has an empty `U`, pulls **zero** members, and
fails. The current recipes never hit this because they pass every object explicitly —
which is exactly the hand-list we are deleting.

VMS behavior (A.2): the `SYMBOL_VECTOR` universals are references that must resolve;
they root the library search that pulls the defining modules. **OVMX must make the
symbol-vector universal names seed `resolve_olbs`'s initial `U`** (force-pull the
`.OLB` members that define them, then iterate transitively). This is a small, localized
change — thread `uv/nuniv` into `resolve_olbs` and pre-load `U` with the universal
names before the fixpoint loop — but it is a **prerequisite**: the migration cannot land
until LINK.EXE roots selective search at the symbol vector.

- **Rung 1 (blocking): `LINK.EXE --shareable` seeds selective `.OLB` search from
  `--symbol-vector`.** Add the universal names to the initial unresolved set so members
  defining them are pulled from the `.OLB`. Guard with a positive test: link a
  shareable from an `.OLB` + a `--symbol-vector` naming a symbol in a
  not-otherwise-referenced member; assert that member is pulled and the universal
  resolves. (Extends the vms-ca9 selective-pull suite.) This is the systems-engineer
  rung that must merge first.

- **Rung 1b (verify, likely already satisfied):** confirm a symbol-vector-rooted pull
  followed by `--use` import binding leaves no spurious undefined — i.e. every remaining
  strong ref is either pulled from the `.OLB` or a `--use` universal. Covered by making
  one real recipe (smallest first: `mk_vmslnm_shr.sh`, 4 TUs, no TLS) the pilot.

No LIBRARIAN gap: `/CREATE` from a set of `.OBJ` is already all this needs. No `.OPT`
options-file parser is required — `--symbol-vector`/`--gsmatch`/`--use` on argv are the
functional equivalent of the options file (LINK.EXE uses the direct-argv convention;
`docs/design-link-native-toolchain.md` §7).

#### C.4.2 Behavior-change risk to flag

Dropping `nm` auto-append (C.3) means **only manifest-declared universals are exported.**
Any symbol currently auto-exported (present in a producer image today via `nm` but
*not* in the `.vec` manifest) and actually imported by some consumer would become an
unresolved import after the flip. Mitigation rung:

- **Rung 0 (reconcile before flip): prove the manifest is complete.** For each producer,
  diff the set of universals the *current* recipe would export (`nm` + append) against
  the `.vec` manifest; any manifest-absent-but-consumed universal is appended to the
  `.vec` (a deliberate, freeze-gated ABI line) *before* the recipe switches to
  manifest-only export. If the delta is empty (the frozen state the freeze gate implies
  for libvms/vmssys), this rung is a no-op assertion. Do this per producer as its recipe
  migrates.

### C.5 Migration scope and sequencing

**Recipes that change (6):** `mk_vmssys_shr.sh`, `mk_vmsprocess_shr.sh`,
`mk_vmslnm_shr.sh`, `mk_vmsfs_shr.sh`, `mk_libvms_shr.sh`, `mk_vmsrms_shr.sh`.
`mk_decc_shr.sh` is unchanged (musl C-RTL, a whole-archive `.a` — not an OVMX-source
`.OLB`; A.1 says a `.a` stays whole). `mk_dcl.sh`/`mk_loginout.sh` and the executable
links are out of scope for this pass (they consume producers via `--use`; they can adopt
the same `.OLB` shape later if desired).

**Suggested order (smallest blast radius first):**
1. Rung 1 (LINK.EXE symbol-vector-seeded selective search) — blocking, systems-engineer.
2. Rung C.2 (CMake `file(GENERATE)` srclist + no-inline-list gate).
3. Pilot: `mk_vmslnm_shr.sh` (4 TUs, no TLS, small `.vec`) end-to-end through the new
   path; verify the native-link CI job and IMGACT activation stay green by SHA.
4. Roll the remaining five, largest/riskiest last: `mk_libvms_shr.sh` (TLS producer,
   ~55 TUs, the auto-append deletion) and `mk_vmsrms_shr.sh` (whole-`nm` export deletion),
   each preceded by its Rung 0 reconcile.

**Risk to the in-flight converged-program native-link, and when to land:** this pass
touches the *shareable-producer* recipes and the LINK.EXE selective-search path — the
same machinery the in-flight converged-program native-link work exercises. Landing it
mid-reap risks destabilizing that reap with a linker-behavior change (Rung 1 alters how
`--shareable` pulls objects). **Recommendation: land this AFTER the converged-program
native-link reaps green.** Rung 1 (LINK.EXE) may be *authored* in parallel behind its own
positive test since it is purely additive (seeding an empty root set changes nothing for
the current explicit-object recipes), but the *recipe migration* (C.2–C.5) waits for the
reap so a single stream owns native-link stability at a time.

### C.6 Out of scope — kernel-module list drift (separate issue)

`src/kernel/vmsfs/Makefile:48` (`vmsfs-y := vmsfs_super.o …`) is a Kbuild object list
that must mirror the vmsfs source set. It has the *same shape* of bug (a hand-kept second
list that can drift) but it is a **Linux-substrate Kbuild** concern, **not** VMS
LINK.EXE/LIBRARIAN — there is no `.OLB`, no symbol vector, no GSMATCH. It needs its own
fix (derive `vmsfs-y` from one source + an equality gate) and should be filed as a
separate item. It is mentioned here only to disclaim it from this VMS-native design.

---

## Appendix — grounding ledger

| Claim | Source | Status |
|---|---|---|
| `/LIBRARY` searches the library name table for prior unresolved refs; pulls only needed modules | Linker Utility Manual §1.2.3.2 | ✅ grounded |
| Selective module processing is `.OLB`-only; object files ingested whole | Linker Utility Manual §1.2.3 | ✅ grounded |
| `/INCLUDE` force-loads named modules, bypassing the name table | Linker Utility Manual §1.2.3.2 | ✅ grounded |
| `/NOSYSLIB`, `LNK$LIBRARY` default search order | Linker Utility Manual §1.4.1 / §1.2.3.2 | ✅ grounded |
| `SYMBOL_VECTOR=(name=KEYWORD,…)`, keywords, value = vector offset | Linker Utility Manual Ch. 4 §4.2; design-link-native-toolchain.md §5.1 | ✅ grounded |
| Upward-compat: preserve order, retire in place, append at end | Linker Utility Manual; §5.3 | ✅ grounded |
| `GSMATCH=match,major,minor`; LEQUAL/EQUAL/ALWAYS semantics | Linker Utility Manual Ch. 4; §5.4 | ✅ grounded |
| LIBRARIAN builds/maintains `.OLB` + library symbol table | LIBRARIAN Utility Manual | ✅ grounded |
| `.OLB` on-disk byte layout (OVMX = `ar` container) | — VSI does not publish | ⚠️ OVMX design choice (Rule 8; ovmx_olb.h) |
| argv options in lieu of an `.OPT` file parser | — | ⚠️ OVMX design choice (design-link-native-toolchain.md §7) |
