# OVMX Self-Host MMK Spine — Anchor Design + RTL-Gap Verdict

> Status: **ANCHOR** (bead vms-52c, parent epic vms-59a "self-host MMK spine").
> This is the first child of the spine and the document every other spine child
> reads first. It produces the go/no-go facts that gate the spine's
> implementation work. All RTL-coverage claims below are grounded in a real
> symbol/file on `origin/main` (paths cited); anything unverified is marked
> `verify:`.

> **LADDER RECONCILE (operator 2026-08-22, Rule 1 / `vms-ports-build-ladder`):**
> this spine is a faithful ladder rung. MMK/LIBRARIAN are *the* VMS build tools; a
> real, unmodified VMS software port also needs them, so every rung here is
> VMS-compat surface others reuse. Build the vendored MadGoat MMK **the faithful
> way** — OVMX bends to it via tagged `OVMX_RMS_IO` seams over the VMS-compat
> surface, never a fork hacked down to an ad-hoc OVMX surface. tcc in this chain is
> the temporary bootstrap (`vms-678`), retired once the OpenVMS GCC port builds on
> OVMX (`vms-da0`).

## 0. Scope and the resolved decisions (do not relitigate)

The spine ports **MMK** (MadGoat Make, vendored at `tests/corpus/tier3-mmk/`) and
**LIBRARIAN** to run as OVMX-native images, built by the OVMX-native toolchain
(TCC.EXE → LINK.EXE → IMGACT.EXE). These decisions are **settled** by the epic
and are inputs to this doc, not open questions:

1. **Use the vendored MadGoat MMK.** Not GNU make, not a from-scratch
   implementation of the MMS spec. The corpus at `tests/corpus/tier3-mmk/` (57
   files: `mmk.c`, `build_target.c`, `parse_descrip.c`, `parse_objects.c`,
   `fileio.c`, `misc.c`, `symbols.c`, `mem.c`, … plus `parse_tables.mar` and
   `mmk_cld.cld`) is the source of truth.
2. **Platform order: aarch64 → x86_64.** Alpha rides the `vms-054` Alpha-port
   epic, **not** this spine. (The corpus ships `mmk.alpha_opt` / `.x86_64_opt` /
   `.ia64_opt` / `.vax_opt`; we build the aarch64 and x86_64 targets only.)
3. **The RTL is a shared substrate.** The absent entry points identified in §1
   become **children under `vms-801`** (source-compat / RTL coverage), not under
   this spine — the spine consumes them.

The spine's own children are:
- **vms-52c** (this doc) — anchor + RTL-gap verdict.
- **vms-486** — MACRO-32 `parse_tables.mar` resolution (see §2).
- **vms-<LIBRARIAN>** — LIBRARIAN + `.OLB` format (see §3).

---

## 1. RTL-gap verdict — the central GO/NO-GO

### 1.1 How MMK actually reaches the RTL

MMK's command-line path is **not** the "minimal argc/argv" path the source-compat
roadmap (`docs/roadmap-source-compat.md` Phase 10) hoped for. `mmk.c` does:

```
tests/corpus/tier3-mmk/mmk.c:341   status = lib$get_foreign(&cmdstr);
tests/corpus/tier3-mmk/mmk.c:343   status = cli$dcl_parse(&cmdstr, MMK_CLD, lib$get_input, ...);
tests/corpus/tier3-mmk/mmk.c:841   status = cli$present(&argnamd);        /* + cli$get_value */
```

So MMK **hard-requires the CLI$ callable interface driven by a compiled command
table** (`MMK_CLD`, produced from `mmk_cld.cld`). And its description-file and
object-list parsers are TPARSE-driven:

```
tests/corpus/tier3-mmk/parse_descrip.c:218  unsigned int lib$table_parse();
tests/corpus/tier3-mmk/parse_descrip.c:219  #define lib$tparse lib$table_parse
tests/corpus/tier3-mmk/parse_descrip.c:289  status = lib$tparse(&tpablk, &parse_state, &parse_key);
tests/corpus/tier3-mmk/parse_objects.c:179  status = lib$tparse(&tpablk, &po_state, &po_key);
```

where `parse_state`/`parse_key`/`po_state`/`po_key` are the TPARSE state/key
tables defined in `parse_tables.mar` (§2).

### 1.2 Full RTL surface MMK calls

Extracted from the corpus C/H sources
(`grep -ohiE '(lib|sys|cli|str|ots)[$][a-z_0-9]+' tests/corpus/tier3-mmk/*.c *.h`).
The high-frequency callers, mapped to OVMX `origin/main` coverage:

| Entry point | MMK use | OVMX status | Where (origin/main) |
|---|---|---|---|
| `lib$signal` / `lib$stop` / `lib$establish` / `lib$sig_to_ret` | condition handling (72×) | **present** | `src/libvms/rtl/lib_signal.c` |
| `lib$sys_fao` / `sys$fao` | formatted output (17×) | **present** | `src/libvms/rtl/lib_datetime.c`, `src/libvms/syssvc/sys_fao.c` |
| `lib$put_output` / `lib$get_input` | terminal I/O | **present** | `src/libvms/rtl/lib_output.c` |
| `lib$get_vm` / `lib$free_vm` / `lib$create_vm_zone` / `lib$reset_vm_zone` | memory zones (36×) | **present** | `src/libvms/rtl/lib_vm.c` |
| `lib$insert_tree` / `lib$lookup_tree` / `lib$traverse_tree` | balanced trees | **present** | `src/libvms/rtl/lib_tree.c` |
| `lib$get_symbol` / `lib$set_symbol` / `lib$set_logical` | DCL symbols/logicals | **present** | `src/libvms/rtl/lib_symbol.c`, `rtl/lib_logical.c` |
| `lib$find_file` | wildcard file iteration | **present** | `src/libvms/rtl/lib_misc.c:292` |
| `lib$find_image_symbol` | dynamic symbol lookup | **verify:** `lib$find_image_symbol` — confirm a real impl vs header-only |
| `lib$cvt_dtb` / `ots$cvt_tu_l` | numeric conversion | **present** | `src/libvms/rtl/ots_routines.c` (ots), `verify:` `lib$cvt_dtb` |
| `lib$getdvi` / `lib$getjpi`-class | device/job info | **verify:** `lib$getdvi` — confirm impl |
| `str$append` / `str$copy_dx` / `str$free1_dx` / `str$match_wild` / `str$concat` / `str$position` / `str$prefix` / `str$case_blind_compare` | dynamic strings | **present** | `src/libvms/rtl/str_routines.c` |
| `sys$parse` | RMS filespec parse | **present** | `src/vmsrms/rms_parse.c` |
| `sys$search` | RMS wildcard search | **present** | `src/vmsrms/rms_search.c` |
| `sys$open`/`close`/`connect`/`disconnect`/`get`/`put`/`find`/`rewind` | RMS record I/O | **present** (RMS) | `src/vmsrms/` |
| `sys$assign`/`sys$dassgn`/`sys$qiow`/`sys$trnlnm`/`sys$gettim`/`sys$getsyi`/`sys$create`/`sys$hiber`/`sys$wake`/`sys$dclexh` | system services | **present** | `src/libvms/syssvc/`, `src/libvms/include/starlet.h` |
| **`lib$tparse` / `lib$table_parse`** | **description-file parser** | **PARTIAL — stub only** | `src/libvms/rtl/lib_tparse.c` is **14 lines, "Stub implementation"**; `lib$table_parse` symbol **absent** |
| **`lib$get_foreign`** | fetch invocation command tail | **PARTIAL — wrong semantics** | `src/libvms/rtl/lib_output.c:139` exists but **reads a line from stdin**, it does not return the foreign-command tail |
| **`cli$dcl_parse`** | parse cmd against CLD table | **ABSENT** | referenced only in a comment (`src/vmsdcl/dcl_cmd_process.c:1573`); no callable routine |
| **`cli$present` / `cli$get_value`** | retrieve parsed qual/params | **ABSENT** | only status constants exist (`src/libvms/include/libclidef.h`, `libclidef.h` `CLI$_PRESENT` etc.); no callable routine |
| **`sys$filescan`** | lightweight filespec field parse | **ABSENT** | only structure headers exist (`src/libvms/include/fscndef.h`, `iledef.h`); no `sys_filescan.c` |
| **`sys$setddir`** | set default directory | **ABSENT** | no symbol anywhere in `src/` |

### 1.3 Verdict: **CONDITIONAL GO — NO-GO until the five gaps below are closed**

The RTL **foundation is strong and mostly complete**: VM zones, balanced trees,
dynamic strings, `$FAO`, symbols/logicals, condition handling, `lib$find_file`,
and — critically — the full **RMS `sys$parse`/`sys$search`** engine are all real,
implemented code on `origin/main`. There is no architectural blocker.

But MMK **cannot link-and-run today**. Five entry points it hard-calls are
absent or non-functional. Each becomes a **prerequisite child under `vms-801`**;
the spine's implementation children (vms-486, LIBRARIAN) are **blocked on them**:

| # | Prereq (→ vms-801 child) | Gap | Recommended scope |
|---|---|---|---|
| **P1** | **CLI$ callable interface + compiled-CLD support** — `cli$dcl_parse`, `cli$present`, `cli$get_value` | **ABSENT**. New `src/libvms/rtl/lib_cli.c` (roadmap already names it). MMK passes `MMK_CLD` (compiled from `mmk_cld.cld`) to `cli$dcl_parse`, so a **CLD command-table representation** and a way to produce it (SET COMMAND / CLD compile, or a hand-authored table) are part of this item. **This is the largest gap** and the roadmap's "minimal argc/argv, not full CLD" note is insufficient — MMK needs the CLD path. |
| **P2** | **Real `lib$table_parse` / `lib$tparse` engine** | **PARTIAL (14-line stub)**, and `lib$table_parse` symbol missing. Implement the full TPARSE state-machine engine in `src/libvms/rtl/lib_tparse.c` and export **both** names (MMK aliases `lib$tparse`→`lib$table_parse`). Must define the **C table format** that vms-486 (§2) targets. Blocks vms-486. |
| **P3** | **`sys$filescan`** | **ABSENT**. New `src/libvms/syssvc/sys_filescan.c`; structures already exist in `fscndef.h`/`iledef.h`. Distinct from `sys$parse` (no RMS I/O — pure field extraction). |
| **P4** | **`sys$setddir`** | **ABSENT**. Set/read process default directory; small. |
| **P5** | **`lib$get_foreign` semantics fix** | **PARTIAL**. Present at `lib_output.c:139` but reads stdin; must return the **invocation command tail** for foreign-command activation. `verify:` exact required semantics against the VSI RTL manual before implementing. |

`verify:` items to resolve while filing P1–P5: `lib$find_image_symbol`,
`lib$getdvi`, `lib$cvt_dtb` — confirm real implementations vs header-only
declarations (they appear in MMK but were not confirmed as `.c` definitions in
this pass).

**Bottom line:** GO on the architecture and the bulk of the RTL; **NO-GO on a
native MMK+LIBRARIAN build until P1–P5 are closed under vms-801.** P1 and P2 are
the long poles.

---

## 2. Assembler decision — `parse_tables.mar` (spine-child vms-486)

### 2.1 Is it actually needed? — **Yes, it is load-bearing.**

`tests/corpus/tier3-mmk/parse_tables.mar` (704 lines MACRO-32, no C twin) is
titled *"PARSE_TABLES - TPARSE tables for MMK description file parser"* and
*"contains the TPARSE parse tables and stub action routines used with the
PARSE_DESCRIP and PARSE_OBJECTS routines."* Its exported data symbols
(`parse_state`, `parse_key`, `po_state`, `po_key`) are **exactly** the tables
MMK's C parsers pass to `lib$tparse`:

```
parse_descrip.c:289  lib$tparse(&tpablk, &parse_state, &parse_key);
parse_objects.c:179  lib$tparse(&tpablk, &po_state,   &po_key);
```

Without these tables MMK cannot parse a description file — its entire reason for
existing. So "confirm MMK builds without it" is **rejected**: it does not.

### 2.2 The constraint

The MACRO-32 assembler is **out of scope** by the compatibility contract, and
TCC.EXE's integrated assembler is **GAS-syntax only** — it cannot assemble
MACRO-32. So `parse_tables.mar` cannot be assembled by the OVMX toolchain.

### 2.3 Decision: **hand-port `parse_tables.mar` to a C translation unit.**

`parse_tables.mar` is not procedural code — it is a **declarative state/key table**
built from the STARLET TPARSE table-definition macros (`$INIT_STATE` / `$STATE` /
`$TRAN` and friends) plus a handful of tiny stub action routines. This ports
cleanly to a C source (`parse_tables.c`) that emits the **same in-memory
state/key table layout** the real `lib$table_parse` engine (P2) consumes. This is
a **clean-room port** (Rule 8): the port is derived from the vendored MadGoat
MACRO-32 source we already have the right to use and the public VSI TPARSE table
format — no VSI/HPE binaries or leaked source.

**Concrete path for vms-486:**
1. **Blocked by P2** (the real `lib$table_parse` engine, which fixes the C table
   format the port targets).
2. Author `tests/corpus/tier3-mmk/parse_tables.c` (or a spine-local port dir):
   translate the `$STATE`/`$TRAN` transitions and the stub action-routine
   addresses in `parse_tables.mar` into C table initializers matching P2's format.
   The transition function codes must keep matching the C-side enums the parsers
   already declare (*"Must match counterparts in PARSE_TABLE.MAR"* —
   `parse_descrip.c:140`, `parse_objects.c:116`).
3. Add a **round-trip test**: feed a known description file through
   `PARSE_DESCRIP` and assert the parsed target/dependency structure — this is the
   acceptance gate proving the C port is equivalent to the MACRO-32 original.

This keeps the MACRO-32 assembler firmly out of scope while unblocking MMK's core.

---

## 3. `.OLB` format decision — LIBRARIAN (spine-child, Rule 8)

### 3.1 What LINK.EXE consumes today

`src/vmslink/link.c` **already ingests `ar` archives in-process** — whole-archive,
VMS-native, no `ld -r` (bead vms-004):

```
link.c:2112  static int file_is_archive(const char *path)   /* peeks the !<arch>\n magic, not the extension */
link.c:518   /* Parse every object member of an `ar` archive into the growable objs array. */
link.c:519   static void load_archive(...)
link.c:556   %LINK-I-ARCHIVE, %s: %d object member(s) pulled (whole-archive)
```

Because detection is by **magic bytes, not extension**, a file named `FOO.OLB`
that is an `ar` container is consumed by LINK.EXE **as-is**.

### 3.2 Decision: **OVMX-labeled `.OLB` = the `ar` container (Rule 8), NOT the VMS-authentic LBR binary format.**

Rationale:
1. **Rule 8 permits it.** The VMS LBR `.OLB` on-disk byte layout is **not
   published** at byte level in the public docs. Rule 8 says where the public docs
   do not publish a layout, OVMX defines its **own** representation and **labels it
   an OVMX design choice** — never presented as VMS-authentic. An `ar`-container
   `.OLB` is exactly such a labeled OVMX representation.
2. **Zero toolchain-core risk.** LINK.EXE already reads `ar` whole-archive
   (§3.1), so LIBRARIAN emitting an `ar` container needs **no edit to `link.c`**.
   The consume path already works.
3. **Semantics already match.** LINK.EXE's whole-archive ingestion + global
   defined-symbol hash (`link.c:849`) already provides library-style symbol
   resolution for a self-contained build.

### 3.3 The operator gate (flag — do NOT proceed without it)

The **alternative** — implementing the **VMS-authentic LBR binary format** — would
require a **brand-new LBR reader inside `link.c`** (toolchain core). Per the
established pattern for toolchain-core edits (`docs/design-tcc-object-compat.md`
marks such changes "optional, operator-gated LINK.EXE" items), **any `.OLB`
consumption change that edits `link.c` is a separate OPERATOR-GATED sibling of
`vms-ca9`** and must not be undertaken as part of the normal spine flow.

**The chosen `ar`-container path deliberately avoids this gate** (no `link.c`
edit). The operator-gated LBR item should be filed as **deferred**: pursue it only
if a future requirement forces byte-level interop with a *real* VMS LIBRARIAN or
LINK (e.g. consuming a third-party `.OLB` shipped by VSI), which the self-host
spine does not need.

**Concrete path for the LIBRARIAN spine-child:**
1. LIBRARIAN.EXE emits the **`ar` whole-archive container**, file type `.OLB`,
   header-commented as an OVMX-labeled design choice (Rule 8).
2. Reuse the RTL surface from §1 (RMS `sys$parse`/`sys$search`, `str$*`, `lib`
   VM/tree) — LIBRARIAN adds **no new RTL prereqs** beyond P1–P5; its novelty is
   the `.OLB` writer (new code, not RTL) and its own CLD (same CLI$ path as MMK,
   so it shares P1).
3. Acceptance: `LIBRARIAN/CREATE FOO.OLB *.OBJ` then `LINK FOO.OLB` produces a
   runnable image — end-to-end through the OVMX-native toolchain.
4. **Refinement (non-blocking):** current `link.c` is whole-archive (pulls every
   member); real `.OLB` search pulls only members that resolve undefined symbols.
   Selective, symbol-index-driven extraction from `.OLB` is a possible follow-up,
   not a spine blocker (an `ar` symbol index `/` member can carry the map when
   wanted).

---

## 4. Summary for downstream spine children

- **RTL verdict: CONDITIONAL GO.** Architecture and most of the RTL are ready.
  **NO-GO on a native MMK+LIBRARIAN build until P1–P5 (§1.3) are closed under
  `vms-801`.** Long poles: **P1 (CLI$ + CLD)** and **P2 (real TPARSE engine)**.
- **Assembler: hand-port `parse_tables.mar` → C** (vms-486, blocked by P2). It is
  load-bearing; MMK does not build without it. MACRO-32 assembler stays out of
  scope.
- **`.OLB`: OVMX-labeled `ar` container** (Rule 8), consumed by LINK.EXE unchanged.
  A VMS-authentic LBR reader is a **separate operator-gated `link.c` sibling of
  `vms-ca9`**, filed **deferred**.
- **Settled inputs:** vendored MadGoat MMK; aarch64 → x86_64; Alpha rides
  `vms-054`.

### Prerequisite items to file under `vms-801`
- **P1** — CLI$ callable interface (`cli$dcl_parse`, `cli$present`, `cli$get_value`)
  + compiled-CLD table support (`lib_cli.c`). *Largest.*
- **P2** — real `lib$table_parse`/`lib$tparse` TPARSE engine (replace the stub;
  export both names; define the C table format). *Blocks vms-486.*
- **P3** — `sys$filescan` (`sys_filescan.c`).
- **P4** — `sys$setddir`.
- **P5** — `lib$get_foreign` semantics fix (return the foreign-command tail, not
  stdin).
- **`verify:`** — confirm real implementations of `lib$find_image_symbol`,
  `lib$getdvi`, `lib$cvt_dtb` (header-vs-`.c` unconfirmed in this pass).
