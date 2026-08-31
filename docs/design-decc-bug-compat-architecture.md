# Design: DEC C / VSI C behavior-compatibility — the source architecture, not the whack-a-mole

**Status:** design / roadmap fork for the operator. Not implementation.
**Lane:** GCC production compiler (`vms-da0`, under `vms-df7`).
**Companion:** `docs/design-gcc-port-surface-gaps-register.md` (the surface-gap
register), `docs/design-gcc-vms-oracle-lane.md` (the base-picks go/no-go).
**Provenance (Rule 8):** reasons ONLY from public GCC source/behavior, the public
VSI/DEC C User's Guides and Language Reference, OpenVMS porting guides, and observed
cross-`cc1` output on `ovmx-cross-alpha-vms`. No VSI/HPE proprietary source is read,
disassembled, or copied.

---

## 0. Why this doc exists — the tarpit, stated plainly

OVMX's north star is **compiling real OpenVMS C source unchanged** (`vms-fd1`:
"builds unchanged, zero port-source hacks"). The instinct, when a real program
miscompiles or misbehaves under our compiler, is to chase the individual defect:
reproduce, isolate, patch the backend or work around the source, move on. That
instinct is a **tarpit**. Every VMS program is a fresh draw from an unbounded bag of
DEC-C-specific behaviors, and chasing them one at a time is unbounded work with no
convergence guarantee — and worse, it produces *false* bugs that burn real time.

**The canonical example is `vms-4b5`, and it is worth internalizing before reading
the rest of this doc.** A toolchain-stress test punned a 64-bit `double` through a
`union { double d; unsigned long u; }` and reported that our cross `cc1` "silently
miscompiles a branch controlled by the punned member at -O0 — drops the compare and
the branch, no diagnostic." It was filed as a compiler-correctness bug and sat as a
standing threat to the whole GCC-port bar.

It was not a bug. On `alpha-dec-vms`, **`long` is 32 bits** — `sizeof(long)==4` even
under `-mpointer-size=64`. That is the OpenVMS C data model (see §2.1), and GCC's
`alpha-dec-vms` backend implements it *correctly and faithfully*. The union punned a
64-bit `double` through a 32-bit member, capturing only the low 32 bits; branching
that value against the 64-bit constant `0x3FF0000000000000` is provably false, and
GCC's front-end folds a provably-false comparison even at `-O0`. Widen the member to
`unsigned long long` and the branch codegens perfectly (`ldq` + `cmpeq` + branch).

The lesson is inverted from the bug report: **our compiler did the VMS-faithful
thing, and *we* misread VMS-faithful behavior as a bug because we assumed LP64.** The
danger in the GCC-port lane is not primarily "GCC miscompiles VMS source." It is
"OVMX and its tests assume GCC-native / LP64 / Linux semantics, and every place VMS
differs surfaces as a phantom bug or a real divergence discovered late." The way out
of the tarpit is not a faster bug-swatter. It is a **source architecture that makes
DEC C semantics explicit, declarative, and differentially tested** — so divergences
are enumerated up front against public docs, not rediscovered one program at a time.

---

## 1. Scope: "bug-compatible" vs "behavior-compatible" — pick the right target

The operator's framing is "bug-compatible with DEC C." That phrase spans three very
different commitments, and conflating them is itself a tarpit:

| Level | Commitment | Cost | Do we need it? |
|-------|-----------|------|----------------|
| **L1 Behavior-compat** | Match DEC C's *documented, observable* semantics: data model, struct/bitfield layout, default float format, pragmas, predefined macros, language extensions, calling standard, char/enum sign. | Bounded — the set is enumerable from public docs. | **YES — this is the actual requirement.** VMS source depends on these *by construction* (its headers are saturated with them). |
| **L2 Result-compat** | For a given program, produce the same *observable result* DEC C would, even where the standard leaves it undefined/unspecified (eval order, uninitialized reads, aliasing outcomes). | Higher — requires a differential oracle and case-by-case judgment. | **PARTIALLY** — only where real VMS source actually relies on it. Gate empirically, don't pursue speculatively. |
| **L3 Bug-compat** | Reproduce DEC C's *undocumented defects* bug-for-bug (a specific optimizer mis-transform, a specific layout quirk contradicting its own docs). | Unbounded, and requires observing the DEC C bug — which we can only do behaviorally on the oracle, never by reading its source. | **ALMOST NEVER.** Reserve for a proven, specific dependency in real shipped VMS source. Most such cases are better fixed in the source than reproduced in the compiler. |

**Recommendation up front (expanded in §5): target L1 as the standing contract,
treat L2 as an empirically-gated exception list, and treat L3 as a per-incident
operator decision — never a standing goal.** "Bug-compatible with DEC C" as a blanket
aim is the tarpit named; "behavior-compatible, with a differential oracle and an
explicit exception register" is the tractable version of the same intent.

---

## 2. What L1 behavior-compat concretely requires (the enumerable set)

This is the load-bearing content: the classes of DEC-C-specific behavior VMS software
depends on. Each is derivable from public docs and each maps to a compiler knob, a
header shim, or a personality-layer feature. This list *is* the source architecture —
a register of behaviors, not a backlog of bugs.

### 2.1 Data model (the `vms-4b5` class — highest-frequency phantom-bug source)
- `int`==32, **`long`==32**, `long long`==64, `short`==16. Pointers 32-bit by
  default, 64-bit under `#pragma pointer_size 64` / `-mpointer-size=64` /
  `__INITIAL_POINTER_SIZE`. This is an **LLP64-shaped** model, *not* LP64 and *not*
  ILP32 — the same shape as Win64, which is exactly why LP64 (Linux/Alpha, Linux/x86)
  assumptions misfire.
- `size_t`/`ptrdiff_t` track the *active* pointer size (mixed 32/64 within one
  translation unit via `#pragma pointer_size` regions). VMS system headers toggle
  pointer size around individual declarations — this is pervasive, not exotic.
- **Architectural consequence:** every OVMX header, test, and hand-written port
  source must assume `long`==32. A standing lint (`sizeof(long)` assumptions,
  `%ld`-for-64-bit, `unsigned long` type-puns of 64-bit objects) would have caught
  `vms-4b5` at authoring time.

### 2.2 Struct/union layout, alignment, padding
- Default member alignment differs by heritage: **VAX C defaulted to byte alignment**
  (`#pragma member_alignment` off); DEC C on Alpha/Itanium/x86 defaults to *natural*
  alignment. On-disk and on-wire VMS structures (which OVMX already reproduces for
  RMS/SYSUAF/cluster) depend on the exact rule in force.
- `#pragma member_alignment {save|restore|__=n}`, `#pragma pack`, `#pragma nomember_alignment`.
- `#pragma required_pointer_size`, `__align`/`_align(n)`, `__unaligned` qualifier.
- **Consequence:** layout is a *first-class* compat surface, already exercised by
  OVMX's binary formats. The compiler must honor the same pragmas the headers use, or
  the structs the rest of OVMX carefully lays out won't match what the compiler emits.

### 2.3 Bitfields
- Allocation order, straddling rules, and the signedness of plain-`int` bitfields.
  DEC C's rules are documented and differ in edge cases from GCC's default. The
  `crtl_cc1fp_test.c` `packed_flags` case exercises this; it currently passes, but the
  rule must be pinned, not incidental.

### 2.4 Floating-point format (a genuine correctness fork, not a nicety)
- VAX formats **F/D/G/H** vs IEEE **S/T/X**. DEC C's `/FLOAT` qualifier selects the
  in-memory representation of `float`/`double`. On VAX, `D_float` was the default; on
  Alpha, DEC C defaults to IEEE `T_float` but can emit `G_float`/`D_float`.
- GCC's `alpha-dec-vms` backend carries VAX-float support (`-mfloat-vax`/`-mfloat-ieee`).
  Getting the *default* wrong silently changes every FP constant's bit pattern — a
  data-format divergence, not a rounding nicety. Ties directly to OVMX's VAX (ILP32)
  vs Alpha (LP64) split: the VAX leg and the Alpha leg have *different* default float
  formats, and this must be explicit per-arch, not inherited from GCC's host defaults.

### 2.5 Pragmas (the headers will not compile without these)
VMS system headers (`starlet`, `ssdef`, `descrip`, `rms`, `iodef`, `lib$routines`, …)
are saturated with DEC C pragmas. A compiler that ignores them doesn't "degrade
gracefully" — it produces wrong layout, wrong linkage, wrong symbol names. The
load-bearing set:
- `#pragma member_alignment`, `#pragma pack` (layout, §2.2)
- `#pragma pointer_size` / `#pragma required_pointer_size` (data model, §2.1)
- `#pragma extern_model {strict_refdef|relaxed_refdef|common_block|globalvalue}` —
  governs symbol emission/linkage; VMS's `globaldef/globalref/globalvalue` model.
- `#pragma linkage` / `#pragma use_linkage` — the VMS calling standard, register
  conventions, jacket routines (§2.7).
- `#pragma message` (diagnostic control — benign to map to GCC diagnostics).
- `#pragma builtins`, `#pragma inline`, `#pragma standard`, `#pragma dictionary`
  (CDD, rarely needed), `#pragma module` (image ident, §2.6).
- **Consequence:** OVMX needs a *pragma-recognition layer* that maps each to a GCC
  attribute / target hook / diagnostic — the single highest-leverage piece of the
  personality layer, because it is the gate on the headers compiling at all.

### 2.6 Predefined macros and language identity
- `__DECC`, `__DECC_VER`, `__VMS`, `__VMS_VER`, `__CRTL_VER`, `__ALPHA`/`__alpha`,
  `__vax`, `__ia64`/`__x86_64` (where applicable), `__INITIAL_POINTER_SIZE`,
  `__D_FLOAT`/`__G_FLOAT`/`__IEEE_FLOAT`, `__STDC_VERSION__` handling.
- Real VMS source branches heavily on `#ifdef __DECC` and `__VMS_VER` ranges. If our
  compiler defines `__GNUC__` but not `__DECC`, source takes the *wrong* preprocessor
  path before codegen even begins. This is a **preprocessor-identity** decision with a
  clean-room/authenticity dimension: do we present as DEC C (`__DECC`) or as GCC
  (`__GNUC__`), or both? (See §4 — this is a genuine fork.)

### 2.7 Language extensions and storage classes
- `globaldef` / `globalref` / `globalvalue` storage classes (map to the extern_model
  / symbol-vector machinery OVMX's LINK.EXE already implements).
- `readonly` / `noshare` qualifiers; `variant_struct` / `variant_union`;
  `$`-in-identifiers (the entire `decc$`/`lib$`/`sys$` surface depends on this — OVMX
  already relies on it, and the `vms-f97` patches decorate `..en` names accordingly).
- `__unaligned`, `__align`, `_BASED`/based pointers (rare), `main`-flags globalvalue
  (`__gcc_main_flags`, already handled by the port).

### 2.8 CRTL / runtime behavior (below the compiler, but part of "behaves like VMS")
- `DECC$*` feature logicals that change runtime semantics (filename parsing, case
  sensitivity, `stat` behavior, stream vs record I/O). These are a *runtime* compat
  surface (OVMX's `decc$`/RMS layer), not a codegen surface, but source depends on
  them and they belong on the same register so the boundary is explicit.
- `errno`/`vaxc$errno`/`$STATUS` duality; VMS condition values vs POSIX errno.

### 2.9 Char signedness, enum sizing, eval order (the L2 boundary)
- Default `char` signedness and `enum` underlying-type selection are documented →
  L1. Evaluation order within an expression, and aliasing outcomes under
  optimization, are *unspecified* → L2 (oracle-gated only where real source depends
  on them). Drawing this line explicitly is what keeps L2 from becoming the tarpit.

---

## 3. Is the `alpha-dec-vms` GCC port the right vehicle?

**Yes as the base, with three honest caveats — and it is the only realistic base.**

**Why it is right:**
- It is the *only* existing GCC lineage that already implements §2.1 (data model),
  §2.4 (VAX/IEEE float), §2.5 (several pragmas), §2.6 (some macros), §2.7 (the VMS
  symbol/linkage model), and the VMS calling standard. Reproducing that from scratch
  on a non-VMS GCC target, or in a bespoke compiler, is years of work.
- OVMX already depends on it: `LINK.EXE`/`IMGACT` are gap-probed against its real EVAX
  output, and the `vms-f97`/`vms-52c1` port patches already tune its name decoration.
- It is GPL and in-bounds to build/patch (same basis as the OpenSSH VMS port);
  clean-room stays intact because we read *GCC* source, never DEC C source.

**The three caveats (each a real risk, none disqualifying):**
1. **GCC's C front-end is not DEC C.** The backend gets the *machine model* right
   (widths, float, layout, linkage); it does **not** get DEC C's *front-end*
   semantics (pragma set completeness, `__DECC` identity, extension keywords, some
   diagnostic/eval behaviors). L1 compat therefore needs a **DEC C personality layer**
   *added to* GCC — it does not come for free with the backend.
2. **Upstream is retiring VMS targets.** `ia64-hp-vms` was removed in GCC 15; the
   `alpha-dec-vms` backend is unmaintained and could follow. OVMX is already pinned to
   **GCC 14.2.0** and carrying its own patch set — so we are effectively a *soft fork*
   already. This argues for making the fork explicit and minimal, not for switching
   base.
3. **No x86_64/aarch64 VMS GCC target exists.** OVMX's first-class arches include
   x86_64 and aarch64; there is no upstream GCC VMS target for them. OVMX must
   **author** the VMS-host layer for those arches from the `alpha-dec-vms` pattern
   (already the plan of record, `vms-da0` scout result). The personality layer (§4)
   is exactly the arch-portable part of that work — which is an argument *for*
   building it as a distinct layer rather than as `alpha-dec-vms`-only backend hacks.

**What is a dead-end:** trying to reach DEC C behavior-compat by *flags alone* on a
stock Linux GCC target (already rejected by the operator's oracle constraint — its OS
calls go musl→POSIX→Linux and force nothing VMS-authentic). And trying to reach it by
*bug-swatting the backend* per program (the tarpit). Both fail for the same reason:
they leave DEC C semantics *implicit*.

---

## 4. Options (the strategic fork)

All options assume the GCC 14.2.0 `alpha-dec-vms` base from §3. They differ in **where
DEC C behavior lives** and **how much we commit to.**

### Option A — "Backend-only": ride the port, fix divergences in the backend as found
Keep patching the GCC backend (as `vms-f97`/`vms-52c1` do) whenever a divergence
surfaces. No separate personality layer; no explicit behavior register.
- **Pro:** lowest up-front cost; matches current practice.
- **Con:** this *is* the tarpit. DEC C semantics stay implicit and per-arch;
  phantom bugs (`vms-4b5`) keep recurring; the x86/aarch64 arches each re-litigate the
  same behaviors; no convergence signal. **Not recommended** beyond the minimal name
  decoration already there.

### Option B — "DEC C personality layer" over the GCC base (a `-fvms-decc` persona)
Build an explicit, arch-portable compat layer: complete pragma recognition (§2.5),
data-model enforcement (§2.1), extension keywords (§2.7), predefined-macro identity
(§2.6), default-float per arch (§2.4), layout/bitfield rules (§2.2–2.3) — driven by a
**declarative behavior register** (one row per §2 behavior: public-doc citation → GCC
knob/attribute/hook → differential test).
- **Pro:** DEC C semantics become explicit, testable, and *shared* across
  alpha/vax/x86/aarch64. Phantom bugs become register entries caught by lint. Directly
  reusable when authoring the x86/aarch64 VMS layer. Bounds the work (§2 is finite).
- **Con:** real engineering; touches front-end (pragmas, keywords, macros) not just
  backend. Needs the differential oracle to be worth it.
- **This is the recommended mechanism.**

### Option C — "Header/flag shim only": `decc_compat.h` + fixed flag set
Provide VMS system headers plus a compatibility header that maps DEC C pragmas to GCC
attributes via the preprocessor, and a canonical flag set — no compiler-internal
changes. (Roughly what GNV did in places.)
- **Pro:** cheap; no compiler surgery; ships incrementally.
- **Con:** the preprocessor cannot express everything (`#pragma pointer_size` regions,
  extern_model, linkage, `__DECC` identity, extension keywords, default float).
  Gets ~60% of §2 and stalls. **Good as a *first increment* of Option B, not a
  destination.**

### Option D — Bespoke DEC C front-end / separate compiler
Write (or adapt a non-GCC front-end into) an actual DEC-C-semantics compiler.
- **Pro:** maximal fidelity ceiling, including L2/L3.
- **Con:** enormous; throws away the backend/linkage/float investment; loses the
  LINK.EXE gap-probe relationship; almost certainly never converges. **Not recommended.**

---

## 5. Recommendation

**Adopt Option B (a declarative DEC C personality layer over the GCC 14.2.0
`alpha-dec-vms` base), scoped to L1 behavior-compat as the standing contract, with L2
as an empirically-gated exception register and L3 reserved for per-incident operator
rulings. Ship it C-first (Option C as the first increment) and gate every row with a
differential oracle.** One line: **behavior-compatible with DEC C, by an explicit
register + oracle — never bug-compatible by whack-a-mole.**

Concretely, the source architecture that gets us out of the tarpit is **three
artifacts, not a bug queue:**

1. **A DEC C behavior register** (`docs/compat/decc-behavior.yaml`, one row per §2
   item): `id · behavior · public-doc citation · GCC mechanism (flag/attribute/hook/
   personality-feature) · differential test · status {absent|shim|implemented|
   oracle-verified}`. This is the single ledger for DEC C compat (INV-LEDGER-clean:
   one source, generated views), the same shape as the compatibility-surface register.
2. **A differential oracle** extending the existing lab-Alpha oracle program: compile
   a corpus with the OVMX compiler, capture DEC C's *documented* behavior (and, where
   available, real observed behavior on the OpenVMS Alpha V8.4 oracle) and diff
   *structurally* (widths, layout, float bits, symbol/linkage, macro expansion) — not
   textually. Every register row must be pinned by a diff, or it is a claim, not a fact.
3. **An authoring-time lint** for OVMX's own headers/tests/port-source that flags the
   §2.1 assumptions (`long`==64, 64-bit `unsigned long` type-puns, `%ld` on 64-bit)
   — the class that produced `vms-4b5`. Cheapest, highest-leverage single piece.

**Two decisions I am NOT taking — genuine operator forks:**

- **Preprocessor identity (§2.6): do we define `__DECC`, `__GNUC__`, or both?** Defining
  `__DECC` makes real VMS source take its intended `#ifdef __DECC` path (maximal source
  compat) but is an *identity claim* — the compiler asserting it is DEC C — with a
  clean-room/authenticity dimension (INV-0/Rule 8: we are OVMX, not DEC). Defining only
  `__GNUC__` is honest but sends source down GCC paths DEC C code never intended.
  My inclination: define `__GNUC__` truthfully **and** a distinct OVMX identity macro,
  and provide `__DECC` **opt-in** behind the personality flag with a documented "we
  present the DEC C *interface contract*, we are not DEC C" note — but this is an
  authenticity-posture call reserved to the operator.
- **How far L2 goes.** L1 is clearly in scope. Whether we ever pursue L2 result-compat
  for unspecified-behavior cases (eval order, aliasing) — and certainly any L3
  bug-compat — should be *demand-driven by real shipped VMS source that provably
  depends on it*, filed as individual exception-register rows with operator sign-off,
  never a standing goal. Absent a signal, we stop at L1.

**Sequencing (does not gate 0.6 or 1.0; this is `vms-da0` long-pole work):** (1) land
the §2.1 lint now (it is cheap and would have prevented `vms-4b5`); (2) stand up the
behavior register with §2.1/§2.4/§2.5 rows first (data model, float, the header-gating
pragmas) since those gate the VMS headers compiling at all; (3) grow the differential
oracle row-by-row against lab-Alpha; (4) reuse the whole layer when authoring the
x86_64/aarch64 VMS-host compiler, where it pays for itself a second and third time.

---

## 6. The honest tarpit assessment

Even done right, this is not free, and pretending otherwise would repeat the
`vms-4b5` error at the strategic level:

- **§2 is enumerable but not small.** It is dozens of behaviors, several with per-arch
  variants (VAX vs Alpha float and data model differ). The register makes it *bounded*
  and *shared*; it does not make it quick.
- **The oracle can only observe, never read.** L3 bug-compat, if ever pursued, depends
  on reproducing a DEC C bug we can only see behaviorally on the OpenVMS oracle — slow,
  and clean-room-constrained by construction. This is the strongest argument for
  refusing L3 as a standing goal.
- **Upstream drift is real.** GCC may drop `alpha-dec-vms` entirely; we are already a
  soft fork at 14.2.0. The personality layer *reduces* this exposure (it is our code,
  arch-portable) but the backend underneath is borrowed and aging.
- **The biggest risk is not the compiler — it is us.** `vms-4b5` was OVMX assuming
  LP64. The single most valuable thing this architecture does is make "what does VMS
  actually do here?" a *looked-up register fact with a citation and a test*, instead of
  an assumption rediscovered as a phantom bug three programs later. That is the whole
  point, and it is worth doing before, not after, the next phantom bug.
