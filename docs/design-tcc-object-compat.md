# tcc arm64 Object Compatibility with LINK.EXE — Reloc-Gap Analysis + Ruling

> Status: **RULING** (item vms-4ba.2, epic vms-4ba "self-host S2: a C compiler as an
> OVMX image"). Route: Systems. This note enumerates every aarch64 relocation stock
> tinycc's arm64 backend emits, diffs that set against LINK.EXE's supported set
> (`src/vmslink/link.c`, authoritative), and rules on how to close the gaps so that
> objects `tcc -c` produces feed cleanly into LINK.EXE. It defines the concrete work
> for **vms-4ba.3** (the first tcc→LINK.EXE round-trip).
>
> **Clean-room posture (CLAUDE.md rule 8 / §4 of design-link-native-toolchain.md):**
> everything below about tcc is grounded in the **public** TinyCC source
> (`arm64-gen.c`, `arm64-link.c`, `tcc.h`, `tccelf.c`, `tccdbg.c`) and in **empirical
> `readelf` output** from stock tcc objects. Nothing here inspects VSI/HPE material.

---

## 1. Method (empirical, reproducible)

Stock tinycc (`github.com/TinyCC/tinycc`, HEAD `85ba3ae`) was built **natively for
aarch64** in an **arm64 musl Alpine container** (host is aarch64; no emulation needed) —
`./configure --cc=gcc --cpu=arm64 && make tcc` — the same class of environment the epic
mandates (arm64 musl, build to `/tmp`, never the repo). Representative C plus tcc's own
largest translation unit (`libtcc.c`, the real bootstrap corpus) were compiled with
`tcc -c` and inspected with `readelf -r`/`readelf -S`.

`tcc -v` → `tcc version 0.9.28rc (AArch64 Linux)`.

Reloc-type numbers below are read directly from the `readelf` `Info` field
(`ELF64_R_TYPE`), e.g. `…0101`=257 ABS64, `…0137`=311 ADR_GOT_PAGE, `…0225`=549
TLSLE_ADD_TPREL_HI12 — so every "tcc emits X" row is observed, not assumed.

### 1.1 ELF-writer conventions (the REL-vs-RELA question)

**tcc emits `SHT_RELA` (Elf64_Rela, explicit addend), never `SHT_REL`, on aarch64.**
Grounded in `tcc.h:394-400`: for `PTR_SIZE == 8` (all 64-bit targets) tcc `#define`s
`ElfW_Rel → ElfW(Rela)`, `SHT_RELX → SHT_RELA`, and `REL_SECTION_FMT → ".rela%s"`.
Confirmed empirically: `readelf -S repr.o` shows **3 RELA** sections and **0 REL**;
`readelf -h` shows `Class ELF64 / Machine AArch64 / Type REL (Relocatable file)`.

This closes the **known gap (a)**: LINK.EXE rejects `SHT_REL`
(`link.c:239-240,284-285`) but **accepts `SHT_RELA`**. Stock tcc on aarch64 already
emits RELA, so **no ELF-writer conversion is required** — the gap does not exist for
this target. (It *would* exist for tcc's 32-bit targets — i386/arm — which emit
`SHT_REL`; those are out of scope, LINK.EXE is aarch64-only.)

Other conventions all match what LINK.EXE expects: ELF64 `ET_REL`, `EM_AARCH64`
(`link.c:176-181`), a standard `Elf64_Sym` symtab, and sections classified by ELF
**flags** not name (`link.c:247-262`) — tcc's `.data.ro`, `.eh_frame`, etc. all bucket
correctly by `SHF_ALLOC/SHF_EXECINSTR/SHF_WRITE`.

---

## 2. What stock tcc's arm64 backend emits

Two sources: **codegen** (`arm64-gen.c`, via `greloca`) and **data/debug**
(`tccelf.c`/`tccdbg.c` static-initializer and unwind relocations).

### 2.1 Codegen relocations (`arm64-gen.c`)

| Site (file:line) | Reloc | When |
|---|---|---|
| `arm64-gen.c:504,506` | `ADR_GOT_PAGE` + `LD64_GOT_LO12_NC` | **every** symbol address (ELF path — `arm64_sym`). tcc routes *all* symbol references, including locally-defined statics and string literals, through the GOT. |
| `arm64-gen.c:726,743,1053` | `CALL26` (and `JUMP26` for tail) | function calls / tail calls |
| `arm64-gen.c:499,501` | `ADR_PREL_PG_HI21` + `ADD_ABS_LO12_NC` | **PE target only** (`#ifdef TCC_TARGET_PE`) — *not emitted on ELF* |
| `arm64-gen.c:517,520` | `TLSLE_ADD_TPREL_HI12` + `TLSLE_ADD_TPREL_LO12` | `__thread` access — tcc uses the **local-exec** TLS model (`arm64_tls_sym`: `mrs tpidr_el0; add #:tprel_hi12:; add #:tprel_lo12:`) |

### 2.2 Data & debug relocations

| Source (file:line) | Reloc | When |
|---|---|---|
| `arm64-link.c:6` (`R_DATA_PTR`) | `ABS64` | pointer-valued static initializers (`const char *msg = "…"`, function pointers) |
| `arm64-link.c:5` (`R_DATA_32`) | `ABS32` | **STABS debug only** — the only emit site is `tccdbg.c:540` (`n_value` when `sizeof != PTR_SIZE`). **Not emitted by `tcc -c` without `-gstabs`.** |
| `tccdbg.c:899` | `PREL32` | `.eh_frame` FDE `initial_location` — tcc emits a CIE/FDE unwind table **unconditionally** |
| `arm64-asm.c:1574,1646,1649` | `CONDBR19` | **inline `asm` only** (hand-written branch mnemonics) — not from C codegen |

> Note: `MOVW_UABS_*`, `GLOB_DAT`, `JUMP_SLOT`, `COPY`, `RELATIVE` appear in
> `arm64-link.c` but are **relocation-*apply*** cases (tcc acting as a linker) or
> dynamic/PLT relocations — they are **not** written into `tcc -c` object output.
> `ADR_PREL_PG_HI21`/`ADD_ABS_LO12_NC`/`LDST*_ABS_LO12_NC` are likewise apply-side for
> the ELF target; on ELF, symbol addressing goes through the GOT (§2.1), so they are
> not emitted into objects either.

### 2.3 Observed reloc histograms (`readelf -r`, stock `tcc -c`)

```
repr.o    (representative C):   ABS64×2  ADR_GOT_PAGE×5  CALL26×7  LD64_GOT_LO12_NC×5  PREL32×6
tls.o     (__thread):          TLSLE_ADD_TPREL_HI12×3  TLSLE_ADD_TPREL_LO12×3  PREL32×2
libtcc.c  (bootstrap corpus):  ABS64×134  ADR_GOT_PAGE×3298  CALL26×5325  LD64_GOT_LO12_NC×3298  PREL32×719
```

Every PREL32 sits in `.rela.eh_frame` (verified: all six in `repr.o` target `.text`
from within the `.eh_frame` section). The switch in `classify()` compiled to a
**compare chain** — tcc emits **no jump table**, so there is **no PREL32/ABS32
switch-table gap**. **ABS32 occurred zero times** across `repr.o`, `tls.o`, and the
entire ~12,700-reloc `libtcc.c` corpus.

---

## 3. Diff against LINK.EXE's supported set

LINK.EXE's supported aarch64 relocs (`link.c:39-86` defs; dispatched in
`patch_pcrel()` `link.c:593-653`, GOT helpers `link.c:871-888`, TLSDESC helpers
`link.c:891-925`, ABS64 `.vms$rel` path `link.c:1143-1150`):

`CALL26, JUMP26, ADR_PREL_PG_HI21, ADR_PREL_LO21, ADD_ABS_LO12_NC,
LDST8/16/32/64/128_ABS_LO12_NC, LD_PREL_LO19, CONDBR19, TSTBR14, PREL32, ABS64,
ADR_GOT_PAGE, LD64_GOT_LO12_NC, TLSDESC_ADR_PAGE21/LD64_LO12/ADD_LO12/CALL`.

| tcc emits | value | LINK.EXE supports? | Where in link.c | Resolution |
|---|---|---|---|---|
| `SHT_RELA` writer (not SHT_REL) | — | **yes** | accepts RELA `link.c:241-244`; rejects REL `link.c:239,285` | none needed — tcc already RELA on aarch64 |
| `R_AARCH64_CALL26` | 283 | **yes** | `patch_pcrel` `link.c:596` | — |
| `R_AARCH64_JUMP26` | 282 | **yes** | `link.c:597` | — |
| `R_AARCH64_ADR_GOT_PAGE` | 311 | **yes** | GOT helper `link.c:876` | — |
| `R_AARCH64_LD64_GOT_LO12_NC` | 312 | **yes** | GOT helper `link.c:880` | — |
| `R_AARCH64_ABS64` | 257 | **yes** | `.vms$rel` `link.c:1149` | — |
| `R_AARCH64_PREL32` (in `.eh_frame`) | 261 | **type yes; dropped in this section** | `patch_pcrel` `link.c:641` handles PREL32, **but** reloc collection only gathers B_TEXT/B_DATA targets (`link.c:273-274,282-283`), so `.rela.eh_frame` (target `.eh_frame`=B_RODATA) is **skipped, not applied** | **benign** — see §4.2 |
| `R_AARCH64_TLSLE_ADD_TPREL_HI12` | 549 | **NO** | — (LINK.EXE has **TLSDESC** 562-569, not TLSLE) | **restrict** (bootstrap) / tcc-codegen change (general) — §4.3 |
| `R_AARCH64_TLSLE_ADD_TPREL_LO12` | 550 | **NO** | — | as above |
| `R_AARCH64_ABS32` (STABS `-g` only) | 258 | **NO** | — | **restrict** — don't pass `-gstabs`; default `tcc -c` emits none — §4.1 |
| `R_AARCH64_CONDBR19` (inline asm only) | 280 | **yes** | `patch_pcrel` `link.c:614` | — (not from C anyway) |

**Bottom line: the entire bootstrap corpus (tcc's own source) emits only the five
types in the first block of the table — every one already supported.** The three
"NO"/"dropped" rows are reached only by opt-in features that the S2 bootstrap does not
use: STABS debug (`-gstabs`), `__thread`, and stack unwinding.

---

## 4. Ruling

**Chosen option: (a) restrict/shape the tcc invocation and corpus to the supported
set. No SHT_REL→RELA conversion is needed (option b is moot on aarch64). No LINK.EXE
change is required to bootstrap (option c is NOT invoked — no escalation).**

Rationale: LINK.EXE was hardened against the *gcc* `-fPIC` reloc vocabulary, and tcc's
aarch64 ELF output is a **strict subset plus two opt-in extras**. tcc is in fact
*friendlier* to LINK.EXE than gcc: it routes **all** symbol addressing through the GOT
(the ADR_GOT_PAGE/LD64_GOT_LO12_NC pair LINK.EXE already synthesizes GOT slots for),
never emits `MOVW_UABS_*`, and — critically — emits **no jump tables**. The only
things outside the supported set are gated behind flags/features the bootstrap avoids.

### 4.1 ABS32 → restrict (compile flags)
Do **not** pass `-gstabs`/STABS-emitting debug flags to tcc. Default `tcc -c`
(and DWARF `-g`) emit zero ABS32 (empirically confirmed across the whole corpus).
No code or LINK.EXE change. If ABS32 ever appears, that is a signal a STABS flag
leaked into the recipe — fix the recipe, not the linker.

### 4.2 PREL32-in-`.eh_frame` → benign, no action to bootstrap
tcc emits an `.eh_frame` unwind table unconditionally, with `PREL32` relocs against
`.text`. LINK.EXE **classifies `.eh_frame` as B_RODATA and copies its bytes**, but its
reloc-collection loop gathers relocations only for B_TEXT/B_DATA targets
(`link.c:270-292`), so the `.rela.eh_frame` PREL32 entries are **silently skipped —
LINK.EXE does not `die`**. The result is an `.eh_frame` whose FDE `initial_location`
fields stay 0. This is **harmless for C execution**: `.eh_frame` is consulted only by a
DWARF stack unwinder (C++ exceptions, `_Unwind_*`, `backtrace`) which a pure-C OVMX
image never invokes. **No action for vms-4ba.3.** (Optional future hardening — strip
`.eh_frame` in the tcc recipe, or teach LINK.EXE to relocate B_RODATA PREL32 — is
tracked as a *non-blocking* note in §5, not gated on here.)

### 4.3 TLSLE (`__thread`) → restrict for bootstrap; tcc-codegen follow-up for general C
The one genuine reloc-type gap. tcc's arm64 backend uses the **local-exec** TLS model
(`arm64_tls_sym`, `arm64-gen.c:514-524`): `mrs tpidr_el0; add #:tprel_hi12:;
add #:tprel_lo12:` → `TLSLE_ADD_TPREL_HI12/LO12`. LINK.EXE implements only the
**general-dynamic TLSDESC** model (`link.c:891-925`, the 562-569 group), which is what
gcc `-mtls-dialect=gnu2` emits for the OVMX libraries.

- **Bootstrap (vms-4ba.3 … the fixpoint):** tcc's own source uses **no `__thread`**
  (grep: the only occurrences are the *parser token* `TOK___thread`, not a TLS
  variable). So the S2 bootstrap never emits TLSLE. **Restrict:** do not compile
  `__thread`-using C through tcc until the follow-up lands.
- **General C-on-OVMX (future):** this is a **tcc-codegen change, in Systems
  file-domain** (`arm64-gen.c`), NOT a LINK.EXE change: make `arm64_tls_sym` emit the
  **TLSDESC** sequence LINK.EXE already supports (`adrp/ldr/add/blr` against a
  `.tlsdesc` GOT-like slot), rather than local-exec. Note that local-exec is *also
  semantically wrong* for an OVMX shareable (an ET_DYN's TP-relative offsets are not
  known at link time), so converting tcc to TLSDESC is the correct fix regardless —
  and it keeps the change inside the tcc file-domain. Tracked as a **separate
  follow-up item (§5), non-blocking for vms-4ba.3.**

### 4.4 Option (c) / escalation — NOT invoked
No part of this ruling requires editing `link.c`/`imgact.c`. The bootstrap round-trip
succeeds with LINK.EXE **as-is**. Therefore **no operator-gated LINK.EXE item is
raised** by vms-4ba.2 (per project rule #7, we escalate only a *required* boundary
crossing; none exists here). The two forward-looking LINK.EXE possibilities (relocate
`.eh_frame`; a native TLSLE path as an alternative to the tcc-side TLSDESC fix) are
recorded in §5 as **optional, non-blocking**, for the operator to schedule if/when
general TLS or unwinding is wanted — not as blockers on S2.

---

## 5. Concrete work this implies for vms-4ba.3 (and follow-ups)

**vms-4ba.3 (the first tcc→LINK.EXE round-trip) — no LINK.EXE dependency:**
1. Build stock tcc for aarch64 musl in the epic's container; compile a `main()` C
   program with `tcc -c` (default flags, **no `-gstabs`**), targeting the proven CFLAGS
   spirit (freestanding/`-fno-builtin` are gcc flags; the tcc analog is: link libc/RTL
   symbols as imports to `DECC$SHR`, so calls stay `CALL26` imports).
2. `LINK.EXE --executable --use DECC$SHR <tcc-objects>` and activate through IMGACT;
   assert a VMS-correct result by exit code — mirroring `run_dcl_native.sh`.
3. **Empirical verification points (new to tcc, absent in gcc output):**
   - tcc GOT-references **locally-defined** symbols (statics, string literals `L.n`)
     by name — confirm LINK.EXE creates same-image GOT slots pointing at the local
     definition and that **per-object local-label names** (`L.1`, `L.2`, …) don't
     collide across TUs in a multi-object link (LINK.EXE's GOT/symbol resolution is
     name-keyed). If a collision surfaces, that is a **new** finding to file — it is a
     *local-symbol-naming* issue, not a reloc-type gap, and would be surfaced to the
     operator per rule #7 at that point.
   - Confirm the dropped `.eh_frame` relocs cause no activation failure (expected:
     none — §4.2).
4. Do **not** compile `__thread` C or pass STABS flags in this bead.

**Follow-up items to file (non-blocking on vms-4ba.3):**
- **tcc TLSDESC codegen** (Systems, `arm64-gen.c`): make `arm64_tls_sym` emit the
  TLSDESC general-dynamic sequence LINK.EXE supports, replacing local-exec — required
  before general `__thread`-using C compiles through the OVMX toolchain. (§4.3)
- **[optional, operator-gated LINK.EXE]** relocate `PREL32` (and other) relocs against
  B_RODATA sections so `.eh_frame` is fully formed — only if DWARF unwinding is ever
  wanted on OVMX. Crosses the link.c boundary; raise as its own gated item **if/when**
  needed. (§4.2) — **not** raised now.
