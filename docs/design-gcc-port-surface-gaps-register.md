# GCC-port surface gap register — DECC$SHR/CRTL (vms-3e4) + RMS (vms-126)

> **Status:** CANONICAL single-ledger doc for the GCC-port gap register (merged
> 2026-09-17, `vms-b3a6` wave 2). This file absorbs the substance of two now-deleted
> duplicates: `design-gcc-vms-oracle-lane.md` (the lane charter / base-pick / GO-NO-GO
> decision record, 2026-08-20) and `design-gcc-vms-port-surface-gaps.md` (the original
> Axis 1-6 gap analysis, 2026-08-22, PR #709) — both `git rm`'d as single-ledger
> duplicates once folded in below (§0 = charter, §2a/§3a = the Axis-4/6 material the
> register itself never carried). The **numbers/rung-state snapshot in §1-§3 is from
> 2026-08-31** and is itself superseded for *link-surface sufficiency* by the
> 2026-09-11 F2a host-surface trio (`design-gcc-port-host-surface-demands.md` rung-1,
> `-gaps.md` rung-2, `-gate.md` rung-3, GREEN) — those three remain **separate** docs
> (distinct measurement rungs of `vms-078`, not register duplicates) and are the
> current word on host-surface sufficiency. Re-derive live gap counts and rung state
> from the compat register (`docs/compat/facilities/*.yaml`) + `rd dep tree vms-da0`,
> not from the symbol tallies below. Grounded on `origin/main` (a stale local
> checkout is a standing confounder — see project MEMORY.md). Ladder epic: `vms-da0`.
> Ladder target: `vms-fd1` (blocked by `vms-3e4`, `vms-126`(done), `vms-5b7e`(done),
> `vms-032`, `vms-3320`, `vms-c5d`).

## 0. Charter — GO/NO-GO, base pick, oracle framing (merged from `design-gcc-vms-oracle-lane.md`)

**Epic `vms-da0`.** Peer conductor lane, parallel to the 0.5/1.0 mainline, does not
gate it. Parent `vms-df7` (OVMX self-hosts its own kernel).

**Governing constraint (operator, 2026-08-20):** *"GCC and the self-hosting
toolchain are the oracle, and OVMX must bend to it — not the other way around."*
The compiler is a real VMS program; every OS-interface call it makes (RMS I/O,
`LIB$SPAWN`, condition handling, logical names, the calling standard) encodes what
real VMS provides. When it demands a facility OVMX fakes or lacks, OVMX is
corrected **toward VMS**, never the compiler shimmed toward OVMX/POSIX (the exact
INV-6 anti-pattern the authenticity program exists to kill).

**GO/NO-GO verdict: GO** (2026-08-20). Base pick, decided against three
alternatives:

| Candidate | Verdict | Why |
|---|---|---|
| Stock Linux-musl GCC | REJECTED | OS calls go musl→POSIX→Linux; forces nothing VMS-authentic |
| GNV "gcc" (VSI PCSI kit) | EXCLUDED | wrapper scripts around proprietary VSI/HPE DEC C, not GCC; needs proprietary source |
| VSI OpenVMS x86-64 GCC | NOT VIABLE | no obtainable source; no `x86_64-*-vms*` mainline target |
| **`alpha-dec-vms` OpenVMS GCC port, built on OVMX-Alpha** | **PRIMARY BASE** | the only cleanly-obtainable GPL VMS GCC port (GPLv3+); mainline GCC still configures `alpha-dec-vms` |

**Ladder reconcile (operator, 2026-08-22):** the deliverable is the *existing*
`alpha-dec-vms` port building on OVMX-Alpha unchanged (`vms-fd1`), not a Linux GCC
adapted down and not an OVMX-authored target as the first brick. Authoring an
x86_64/aarch64 VMS-host GCC layer from the `alpha-dec-vms` pattern (no upstream
port exists for those arches) is an explicitly **LATER, distinct rung** (`vms-9894`,
blocked by `vms-fd1`, clean-room per Rule 8 — nothing lifted from VSI/GNV). Linux
GCC is a correctness oracle only, never shipped or run.

**Predicted vs. actual first wall:** the charter predicted RMS file/temp semantics
as the first OS-facility wall the port's toolchain would hit. **The forcing
function proved the prediction wrong** (2026-08-20, from the F1 GNU `as` build):
the actual first wall was LINK.EXE dropping static constructors for symbol-vector
images (`.init_array` RELSKIP'd) — the `LIB$INITIALIZE` ctor-collection facility,
surfaced at F1 (assembler activation) rather than F2 (the compiler itself). RMS
turned out to be strong, not the first wall (§3). This is why the doc frames
"predicted wall" and "confirmed-absent gap" as separate evidence classes — a
prediction is not a measurement.

**The loop (method):**

```
pick/scout the VMS-host GCC upstream (F1 first: GNU as)
  -> cross-build it VMS-host as an OVMX image (LINK.EXE, the TCC.EXE pattern)
    -> ACTIVATE under IMGACT against real /dev/vms
      -> it demands a VMS facility OVMX fakes/lacks   <- THE WALL = the oracle speaking
        -> bend OVMX toward VMS: backfill the facility GENUINELY
             (public VMS docs + lab observation; NEVER a per-process fake)
          -> verify under the tests/qemu KE harness (real /dev/vms), 3-way convergence gate
            -> release-gate the shared-core change through the main/ACP conductor
              -> repeat
```

Endgame (post-1.0): GCC + kbuild build the shipped kernel in-guest, closing `vms-df7`.
The kernel-build output itself is native ELF via an in-guest GNU `ld`/BFD,
deliberately bypassing LINK.EXE (a kernel is not an OVMX symbol-vector image); the
**toolchain programs** (`as`, `gcc`, `cc1`) remain OVMX symbol-vector images
(LINK.EXE-linked, the TCC.EXE pattern) — no conflict between the two object paths.

## Why this doc exists

`vms-3e4` and `vms-126` were filed 2026-08-22 as vague, unscoped `inbox` items. Nine
days of ladder work landed since then (P1 milestone `#833`, multi-.o link+activation
`#969`, the crtl_rms N=7 CRTL/RMS runtime proof `#958`, three zlib-driven CRTL rungs
`#869`) — but almost none of it flowed back into `vms-3e4`/`vms-126` as tracked
sub-items; only two rungs (`vms-e39`=R1, `vms-dfb`=R2) were ever filed under them, and
both are stale (rd status still `inbox`/`blocked` though the work landed — see
**Status reconcile** below). This doc is the current, concrete symbol-level picture:
what's exported, what's proven at runtime, and what's still a real gap — each row
cited to a file or a commit, not recalled.

**Evidence classes used below:**
- **PROVEN (runtime)** — a real alpha-dec-vms cross-compiled port object ran to a
  verified `$STATUS` on real qemu-alpha over `/dev/vms`/ACP. Strongest.
- **PROVEN (link)** — links zero-deferred against the genuine DECC$SHR/LIBOTS$SHR,
  not yet run (or run under qemu-user only, which is *not* the executive oracle).
- **CONFIRMED ABSENT** — grepped `origin/main`, zero hits; a real, currently-open gap.
- **INFERRED** — no direct measurement; reasoned from adjacent evidence, labeled as such.

---

## 1. DECC$SHR / CRTL surface (vms-3e4)

### 1.1 What's exported today

- **Vector composition** (`src/vmslink/mk_decc_shr.sh`): DECC$SHR.EXE is
  musl `libc.a` (whole-archive) + `libgcc.a`, linked by LINK.EXE into one OVMX ET_DYN
  shareable exposing a `.vms$sv` symbol vector. Two branches: the original
  x86_64/aarch64 generic branch (bare musl names), and a dedicated **ALPHA/EVAX
  branch** (vms-7b96) that generates the `decc$`-prefixed vector the real port needs.
- **Alpha decc$ vector, PROVEN (link+partial runtime):** 538 universals + a separate
  **LIBOTS$SHR** (11 `OTS$` universals — Alpha's software integer divide/remainder)
  from real musl-alpha + libgcc, **zero residual undefs** (`88df8e75`/`ac2c4f07`,
  vms-838a, #770). Generated from `decc_crtl_map.txt`, itself derived from the port's
  own `vms-crtlmap.map` (GPL, enumeration-only per Rule 8) — 597 of its ~900 names
  matched against what musl-alpha actually defines (`vms-e39` note, 2026-08-22: 384
  exported at that snapshot; grown since by the items below).
- **Grown since the #770 baseline** (all cited, all Alpha-branch, generic branch
  untouched):
  - `decc$free` + the weak-alias-equate export class (`6fd04bcb`/`8f8ccddc`, #795,
    vms-47f8) — musl aliases like `free`=`weak_alias(__libc_free,free)` previously
    dropped by the port's cc1 (no weak-alias emission), fixed at the export side.
  - `stdin`/`stdout`/`stderr` as **DATA** universals, not PROCEDURE (`31fe2ed9`, #789,
    vms-bdd) — these are plain/undecorated in the port's own object references.
  - `___errno_location` (`3c3d4091`, #869) — musl's errno accessor, un-decorated by
    cc1 because it isn't a DEC-C-RTL-surface name; added to reach parity with the
    generic branch.
  - `unixio.h` on the port's include surface (`3c3d4091`, #869) — declare-only,
    covers the `open/close/read/write/lseek/dup/dup2/isatty/unlink/access/
    ftruncate/fsync/chdir/getcwd` class `zconf.h`'s `#ifdef VMS` path expects.
  - **Weak-alias/thunk-descriptor export fix** (`ccf3982e`, #958, vms-1ef) — the
    deepest fix in the register: a weak `decc$_malloc64` forwarder's exported PV
    now carries the strong `__libc_malloc_impl` descriptor (carrying the real
    `size_classes` anchor cell), general for any weak-redirected thunk, not a
    malloc special-case. Root cause was link-side redirect-incompleteness
    (`link.c:3712`), not cc1 or IMGACT. ~8 binary-grounded measure-first flips before
    landing — see `vms-1ef` history for the full saga if this class regresses.
  - `--use LIBOTS$SHR.EXE` wired into the canonical consumer link recipe
    (`build-joint-image.sh`) alongside `--use DECC$SHR.EXE` (#869) — any port
    program that divides/mods now resolves `OTS$DIV_*`/`OTS$REM_*`.
- **Special bootstrap symbols, PROVEN (runtime, P1 milestone `#833`):**
  `decc$main` (image-activation→CRTL bootstrap, argv/environ/exit), `C$_EXIT1`
  (globalvalue, oracle-pinned `0x0035A009` from lab-Alpha 8.4, `vms-e39` note
  2026-08-22T17:01Z), `_malloc32`/`_malloc64`, `get_errno_addr`/
  `get_vms_errno_addr`/`vaxc$errno` (`src/vmslink/ovmx_decc_crtl.c`, per-thread via
  pthread key — DECC$SHR is deliberately kept a non-TLS producer).
- **PROVEN (runtime) surface breadth**, from the joint-e2e port programs that
  actually ran to full success on real qemu-alpha `/dev/vms`
  (`tools/cross-alpha-vms/joint-e2e/`):
  - `joint_main.c` (P1 smoke test): `decc$main`, `malloc`, `tprintf`.
  - `crtl_rms_test.c`: `malloc`/`free`, `memset`/`memcmp`, `fopen`/`fwrite`/
    `fread`/`fclose`, `printf`/`fprintf`, `stderr` DATA universal — **N=7 full
    success, `$STATUS=0x0035a039`** (vms-1ef, 2026-08-30).
  - `crtl_rms2_test.c`: adds `fputs`/`fgets`/`fseek`/`ftell`/`rewind`,
    `sprintf`/`snprintf`/`sscanf`, `strtol`/`atoi`/`getenv`, `qsort` (function-
    pointer comparator), `strlen`/`strcpy`/`strcat`/`strncmp`/`strchr`/`strstr`,
    `calloc`/`realloc`/`free`.
  - `mf_main.c`+`mf_util.c` (multi-.o, `#969`, vms-bdd): a **3-object STRICT
    link** (crt0+main+util) with a cross-`.o` call pulling `malloc`/`free`/
    `memcpy`/`strlen`/`strcmp` from DECC$SHR — **N=5 full success,
    `$STATUS=0x0035A029`**, real qemu-alpha + real `/dev/vms` over the mounted
    ODS-2 ACP.
  - **zlib 1.3.1** (`3c3d4091`, #869): all 15 real TUs cross-compiled at the
    port's own flags, **PROVEN (link) only** — 0 `%LINK-F-UNDEF` out-of-box, not
    yet run on qemu-alpha.
  - `crtl_cc1fp_test.c` (`2b1c80d1`/`ef249e5a`, #871): FP arithmetic (double/float,
    `divt`/`mult`/`muls`/`adds`), unsigned64↔double conversion, `sqrt`/`pow`
    (real DECC$SHR/musl-libm calls, not libgcc-inlined), `printf`/`sprintf`/
    `sscanf` FP formatting, struct/union/bitfield static aggregates,
    function-pointer dispatch tables, large `switch` jump tables, recursion —
    **PROVEN (link) only**, 7 bound imports (`decc$tsqrt`, `decc$tpow_2`,
    `decc$tprintf`/`tsprintf`/`tsscanf`, `decc$malloc`, `decc$main`), 0 UNDEF.

### 1.2 Confirmed-absent gaps (DECC$SHR/CRTL)

| # | Gap | Evidence | rd item |
|---|---|---|---|
| R5 | **Include surface** — `rtldef`/`starlet_c` text-libraries on the CRTL include path, include-name canonicalization, `$` in identifiers — **LANDED (vms-714c, 2026-09-08)**: `src/libvms/include/vms/{ssdef,chfdef,pdscdef}.h`, a canonicalized `vms/` include-surface shim, additive and separate from OVMX's internal CHF/PDSC headers (owned by vms-1fa/vms-2e72). PROVEN by `tools/cross-alpha-vms/include-surface/run_include_surface_proof.sh` (CI gate `gcc-port-include-surface`): the GENUINE unpatched upstream `libgcc/config/alpha/vms-gcc_shell_handler.c` (GCC 14.2.0) compiles to real VMS/Alpha assembly against the shim, with a baseline control proving the SAME file fails unresolved (`vms/chfdef.h: No such file or directory`) without it. `$`-in-identifiers needed no fix — the alpha-dec-vms cross cc1 already accepts it by default (only host-side x86_64/aarch64 builds of `$`-bearing OVMX C need `-fdollars-in-identifiers`, unaffected by this item). **Residual, tracked separately:** `libgcc/config/alpha/vms-unwind.h` needs a `CHFCTX` type + `chfctxdef.h` (currently absent) and more `chf$mech_array`/fuller struct fidelity — folds into the deeper CHF/condition-handling dispatch rung (`vms-2e72`), not this item. | `grep -rn "rtldef\|starlet_c" origin/main -- src/` = 0 hits (pre-fix); shim + CI proof land the row | `vms-714c` |
| R7 | **LIB$INITIALIZE PSECT multi-constructor collection** for executable images — `link.c:4143` hard-codes `xfer_count == 1` ("no LIB$INITIALIZE handlers"); OVMX's real ctor path is ELF `.init_array` (`link.c:295-299`, explicitly documented as *not* a PSECT-collection reproduction) | `link.c:4143`, `link.c:295-299` | `vms-43c` |
| — | **`decc$feature_*` runtime switches** (`DECC$UNIX_LEVEL`, `DECC$EFS_CHARSET`, POSIX-vs-VMS filename mode) — not emulated at all; plus the crtlmap coverage delta (597-of-~900 as of the last audit, 2026-08-22) has never been re-measured against current DECC$SHR | `grep -rn "decc\$feature" origin/main -- src/` = 0 hits | `vms-06d` |
| R8 | **Real VMS condition-handling dispatch** — `SYS$SETEXV`, `chf$signal_array`/`chf$mech_array`/`CHFCTX`, `LIB$GET_INVO_HANDLE`/`LIB$GET_INVO_CONTEXT`/`LIB$GET_PREV_INVO_CONTEXT`, real frame-transfer unwind (`sys$unwind`'s `newpc` is currently ignored — `sys_condition.c:44-68`). `src/libvms/rtl/lib_signal.c` is a thread-local handler-stack **emulation**, not CHF dispatch. The narrower `SS$_HPARITH` FP-trap bridge (`vms-db3`/GAP3, done) proves the *pattern* (`lib$signal`→handler-chain search→`$STATUS`) works for one condition, but the port's own `libgcc/config/alpha/vms-unwind.h` needs the full CHF/invocation-context machinery for real EH/error unwinding. **Rung-1 landed** (`docs/design-chf-condition-handling.md`): real `SYS$SETEXV` primary/secondary/last-chance vectors, authentic dispatch search order, and a real establisher-frame/depth mechanism array replace the stub; rungs 2–5 (frame-transfer `SYS$UNWIND`, Alpha ICB primitives, HW-exception unification, libgcc EH) are children of `vms-2e72` | `lib_signal.c`, `sys_setexv.c`, `sys_condition.c:44-68`, `docs/design-chf-condition-handling.md`, `vms-db3` (done, narrower) | `vms-2e72` |

### 1.3 Data-model gotcha found along the way (NOT a compiler bug — RESOLVED)

`crtl_cc1fp_test.c` was originally read as evidence that the alpha-dec-vms **cross
cc1** (GCC 14.2.0) "silently miscompiles" a branch controlled by a union
type-punned member at `-O0`. **Root-cause (`vms-4b5`) proved that WRONG — it is not
a codegen bug at all, it is the OpenVMS C data model.** On alpha-dec-vms `long` is
**32-bit** (only `long long` and, with `-mpointer-size=64`, pointers are 64-bit — an
LLP64-shaped model matching real DEC C / VSI C), and the test punned a 64-bit
`double` through an `unsigned long` member — capturing only the low 32 bits. A
branch comparing that (zero-extended) 32-bit value against the 64-bit constant
`0x3FF0000000000000` is provably-false, and GCC correctly folds an out-of-range
comparison away even at `-O0` (an in-range constant, or a runtime comparand, keeps
the branch). Widening the member to `unsigned long long` makes the pun full-width
and the branch codegens and evaluates correctly (real `ldq` + `cmpeq` + branch).

**Lesson (the load-bearing one for the whole GCC-port lane): the alpha-dec-vms
target is NOT LP64.** Any port source that assumes `sizeof(long)==8` will produce
"phantom compiler bugs" like this one. This is a behavior-compat class, not a bug
class — see `docs/design-decc-bug-compat-architecture.md`. `crtl_cc1fp_test.c` now
uses `unsigned long long` and branches on the punned member as a positive
regression. `vms-4b5` closed as not-a-bug (`fixed`), root-caused
(`ef249e5a`/`2b1c80d1`, #871).

---

## 2. LINK / object-format surface (supports both axes — not separately gated)

**Object-format decision reversed from both earlier docs — corrected here
(2026-09-17).** The charter (§0 above, then `design-gcc-vms-oracle-lane.md` §2a.3)
originally resolved "gas emits ELF … `obj-evax.c` is the RMS-descriptor reference
to borrow the discipline from, **not** the output writer (EVAX objects are a
dead-end for the ELF kernel path)". The original gap analysis (2026-08-22, Axis 2,
Open Question 3) likewise framed object format as undecided, "leaning toward (a)
[assemble to ELF]". **Both are stale.** What was actually built is the opposite:
LINK.EXE reads the port's **genuine native EVAX (Alpha/VMS) object format
directly** — no ELF-assemble detour for Alpha at all. This is verified, not a
design lean: `docs/compat/facilities/object-format.yaml` rates
`object-format$evax-object-ingest` **status: verified, authenticity: real** (byte-exact
round-tripped against real `alpha-dec-vms-as` fixtures, `src/vmslink/test/run_evax_*.sh`),
and `docs/compat/facilities/link.yaml` rates `link$evax-backend` **status: verified,
authenticity: real**. The "EVAX is a dead-end" framing applied only to the
in-guest-kernel-build ELF path (§0's architecture-fork note, which still holds —
`vmlinux` output bypasses LINK.EXE and stays ELF); it never applied to, and is now
disproved for, the GCC port's own object ingest. It is the load-bearing piece under
nearly everything in §1.1 and §3 above:

- `src/vmslink/evax_read.{c,h}` (bead `vms-cbe`) is a real, clean-room GSD/EGSD/
  TIR/ETIR object-format reader, grounded to `binutils-2.43 bfd/vms-alpha.c` per
  Rule 8 (`link.c:44,3194,3483,3806`). LINK.EXE reads genuine VMS OBJ from the
  `alpha-dec-vms` binutils backend (`tools/cross-alpha-vms/build-toolchain.sh`
  vendors binutils 2.43 with its VMS/Alpha backends) directly — no ELF-assemble
  detour for Alpha.
- LINK.EXE emits a real Alpha `.vms$sv` **shareable** from `alpha-dec-vms` objects
  (`vms-c65`/#742) and a real executable transfer entry (crt0→`__main`).
- Weak/strong override machinery (`#899`) + the weak-alias-equate export class
  (`#795`) + the thunk-descriptor redirect fix (`#958`) together make cc1's
  no-weak-alias-emission gap (originally `vms-47f8`) a non-issue for consumers.
- STRICT (zero-deferred, no `--allow-undefined`) multi-object linking is proven
  (`#969`).

**Still open, LINK-side:** the `.opt`-grammar `vms-ld` front-end (`IDENT=`/
`symbol_vector=`/`cluster=`/`collect=`/`PSECT_ATTR=`) that the port's own driver
literally shells out to was never built — moot for every rung proven so far because
`build-joint-image.sh` drives LINK.EXE directly with equivalent flags
(`--transfer`, `--use`, `GSMATCH` env var), bypassing the port's own `vms-ld`
wrapper. This only becomes a real gap at **P2** (the port's own `configure`+`make`
running self-hosted on OVMX, invoking its own `vms-ld` literally) — not scoped here,
tracked structurally by `vms-da0`'s R9/R10 rungs in the original doc.

---

## 2a. Process/spawn surface (Axis 4, `vms-e9a`) — merged from the original gap analysis

The compiler driver's `cpp→cc1→as→ld` pipeline spawns a stage, must learn its exit
status, and spawn the next. OVMX had (2026-08-22) three divergent process-creation
paths: `lib$spawn` (fork/exec of a shell, **not executive-registered** — the INV-6
fake), `sys$creprc` (real, executive-registered), and DCL `SPAWN` (a third,
duplicated re-exec path). The original doc flagged this as **`/NOWAIT` completion
channel absent**: `lib$spawn`'s `efn`/`astadr`/`astprm` were silently discarded —
no EF, no termination AST — filed as `vms-e9a`.

**Landed since (re-derived from `vms-e9a`'s own PR history, not recalled):**
- **B0 (PR #919, merged 2026-08-29):** `lib$spawn` + DCL `SPAWN` routed onto the
  executive-registered `sys$creprc` (`src/libvms/rtl/lib_misc.c`); the unregistered
  fork/exec fake excised. Conductor ruling on record: SPAWN-requires-executive is
  *authentic* Rule-9 behavior (real VMS `$CREPRC` needs the executive), not a
  regression — plain-host now fails honestly (`%DCL-F-CREPRC`) absent `/dev/vms`.
- **B1 (PR #974, merged 2026-08-31):** the `/NOWAIT` EF + termination-AST completion
  channel itself — new shared kernel-core facility `VMS_IOCTL_SPAWN_NOTIFY`,
  mirrored across the required NetBSD/kernel-module places (four-places rule),
  proven by `tests/qemu/test_kmod_spawn_notify.c` (14/14) on real `/dev/vms`.

**Still open:** **B2** — the GCC driver's own VMS-host process-creation hook
actually calling `LIB$SPAWN`/`$CREPRC` for its `cpp→cc1→as→ld` pipeline (the
payoff step; the executive mechanism it needs now exists). **B3** (mailbox-pipe
concurrent-stage transport) is a deferred stretch rung, not required for the
sequential pipeline. Tracked: `vms-e9a` (rd status may still read stale per project
convention — re-derive from the item's own history, not the status field).

---

## 3. RMS surface (vms-126)

### 3.1 What's landed and proven

RMS was already assessed as **strong, not the predicted first wall** in the
2026-08-22 doc (Axis 3) — that assessment holds and has grown:

- **Core primitives** (`src/vmsrms/`): FAB/NAM/RAB structures; versioning
  (`rms_next_version()` = highest+1, explicit `;N` honored); record formats FIX/
  VAR/VFC/STM/STMLF/STMCR; orgs SEQ/REL/IDX; temp/delete-on-close
  (`FAB$M_TMP/TMD/DLT`).
- **RMS ENGINE reaches the ACP, PROVEN (runtime):** the public RMS services
  `sys$create`/`sys$open`/`sys$connect`/`sys$put`/`sys$get`/`sys$close`/
  `sys$extend`/`sys$erase` issue genuine `$QIO`s (IO$_ACCESS/CREATE/READVBLK/
  WRITEVBLK/MODIFY/DELETE) to the executive Files-11 ACP over `/dev/vms` — no
  POSIX bypass — proven by `tests/qemu/test_syssvc_rms_acp.c` (single-version
  create/put/get/close/reopen/extend/erase, RFM VAR/STMLF/FIX) and, for the
  compiler-driver **workload** (multi-version create, directory enumeration via
  `sys$parse`+`sys$search`, `rms_file_attr`, erase), by
  `tests/qemu/test_syssvc_rms_workload.c` (vms-1b5). This is the RMS ENGINE the
  compiler's file ops route THROUGH.
- **⚠ CORRECTION (2026-08-31, vms-1b5, trace-grounded):** an earlier revision of
  this bullet claimed the alpha PORT image's `crtl_rms_test.c`
  `fopen`/`fwrite`/`fread`/`fclose` "genuinely reaches RMS/ACP — not a POSIX
  bypass", citing the N=7 runtime proof (`vms-1ef`, #958). That is **overstated**.
  DECC$SHR is whole-archived musl-alpha; those calls are musl POSIX whose `open()`
  is a raw Alpha `callsys` into the Linux-Alpha kernel → the process's **ramfs**
  CWD, never `/dev/vms` (`mk_decc_shr.sh:875-876`, `decc_crtl_map.txt:16-18`:
  "Semantics are musl/POSIX UNTIL R2 routes the file entries through RMS/ACP
  (vms-dfb)"). The N=7 gate checks console text + `$STATUS` only; it would pass
  **identically over ramfs**, so it does not prove route-through. `vms-dfb`
  ("R2: DECC$SHR C-RTL file layer routes through RMS/ACP") was closed on this
  overstated evidence — the port-image CRTL→RMS binding is **still open**, tracked
  as `vms-47e` (build a CRTL→RMS file layer per `ovmx_link_rms_io.c`, produce
  LIBVMSRMS$SHR, `--use` it in the port link, and verify PORTTEST lands on the
  ODS-2 volume independently). The RMS ENGINE readiness above is genuine; the
  PORT's binding TO it is the remaining R2 work.
- **CRTL→RMS stdio VENEER, PROVEN (runtime) + landed (`vms-47e`, 2026-08-31):**
  the genuine veneer now exists as a reusable TU — `src/vmsrms/crtl_rms_stdio.c`
  (`ovmx_crtl_fopen`/`fwrite`/`fread`/`fclose` → `sys$create`/`$connect`/`$put`/
  `$get`/`$close`, byte-exact FIX mrs=0 put / mrs=1 get, the mechanical mirror of
  the proven `ovmx_link_rms_io.c`, fail-honest with NO POSIX fallback). It is
  proven **un-fakeably** by `tests/qemu/test_syssvc_crtl_rms_veneer.c`: a file
  written through the veneer's `fopen`/`fwrite` is then read back by a **DIFFERENT
  reader** — the executive ACP directory search (`sys$parse`+`sys$search`, genuine
  ODS-2 File ID via `rms_search_fid`) and the on-disk header (`rms_file_attr`,
  IO$_ACCESS) — which see it on the real Files-11 volume with a `;1` version. A
  ramfs POSIX write can produce NEITHER an ODS-2 File ID NOR a version, so this is
  the teeth the alpha N=7 same-CRTL round-trip lacks. This closes the veneer
  MECHANISM; it is the exact source the alpha DECC$SHR re-point will whole-archive.
- **REMAINING (the `vms-47e` child — the ALPHA port wiring, a build sub-project,
  NOT a same-session re-point):** re-pointing the alpha PORT IMAGE's
  `decc$fopen`/… onto the veneer needs infrastructure that does not exist yet in
  the alpha-dec-vms port world. Trace-grounded blocker (2026-08-31): the
  alpha-dec-vms port musl (`tools/cross-alpha-vms/musl-arch`) has **no `vms_kif`
  and no `vms_ioctl`** — its syscall backend is a raw Alpha `callsys` into the
  Linux-Alpha kernel — and **LIBVMSRMS$SHR is built only for the host executive
  arch, never for the alpha-dec-vms port toolchain** (the port link `--use`s only
  DECC$SHR + LIBOTS). So the child must, in order: (1) build the RMS engine +
  `vms_kif`/ACP `ioctl(/dev/vms)` transport for the alpha-dec-vms musl world as an
  alpha LIBVMSRMS$SHR; (2) compile `crtl_rms_stdio.c` into the alpha DECC$SHR and
  suppress musl-alpha's own `decc$fopen`/… from the export vector so the veneer's
  aliases win (`mk_decc_shr.sh` ALPHA branch); (3) `--use LIBVMSRMS$SHR` in the
  port link + stage it in SYS$SHARE for IMGACT to bind; (4) upgrade the
  `alpha-crtl-rms` boot gate (`tools/cross-alpha/run-module-gp-activation-alpha.sh`)
  to write onto a WRITABLE mounted ODS-2 volume and add the independent-reader
  step (a `$ DIRECTORY DKA0:[…]PORTTEST.DAT` / `sys$search` after the write). Until
  then the alpha port image's file writes still reach the Linux-Alpha VFS, and the
  N=7 gate remains a same-CRTL round-trip (honest label; the gate name is a
  required-check and is left stable — the claim is corrected here, not the name).
- **Locking, landed in V0.6:** RMS file-level share arbitration behind the DLM
  (`vms-50e`, #932) and RMS record-level locking behind the DLM (`vms-0dd`, #935)
  — real cross-node lock semantics, not local-only.

### 3.2 Confirmed gaps (RMS)

Nothing in the original Axis-3 "primitives absent" list stands anymore — the
remaining gap is **workload coverage**, not missing RMS mechanism. The
compiler-driver workload is proven **at the RMS ENGINE level** over the real ACP
by `tests/qemu/test_syssvc_rms_workload.c` (vms-1b5), and the **CRTL→RMS stdio
veneer** that binds `fopen`/`fwrite`/`fread`/`fclose` to those services is now
landed + proven un-fakeably against the ODS-2 volume by
`tests/qemu/test_syssvc_crtl_rms_veneer.c` (vms-47e, see the §3.1 veneer bullet).
The remaining hole is **narrow and specific**: wiring that veneer into the alpha
PORT image's DECC$SHR (a build sub-project — the alpha-dec-vms port world has no
`vms_kif`/RMS substrate yet; the `vms-47e` child, §3.1). Until that lands, the
alpha port image's own file writes are still musl-POSIX → Linux-Alpha VFS.

> **rd-ID caveat (Rule 10):** this table's "rd item" column reads `vms-1b5`, but
> in rd `vms-1b5` is actually the *decc$feature* item; the RMS-beyond-stdio item
> is **`vms-2e72`**. Doc↔rd cross-wiring for the conductor to reconcile.

| Gap | Status | rd item |
|---|---|---|
| Directory enumeration (`opendir`/`readdir`/`stat`-class) over RMS/ACP | **ENGINE PROVEN** — `sys$parse`+`sys$search` wildcard enumeration with genuine ODS-2 File IDs (`rms_search_fid`) + `rms_file_attr`, `test_syssvc_rms_workload.c` (B/C). PORT CRTL `opendir`/`readdir` still musl-POSIX → `vms-47e` | `vms-1b5`/`vms-2e72` |
| Listing-file (`.LIS`) semantics | The compiler driver's diagnostic/preprocessed-output path — a `.LIS` is just a sequential file; the create/put/version/enumerate machinery it needs is engine-proven above. No distinct RMS gap; folds into the PORT-CRTL binding | `vms-47e` |
| Multi-stage temp-file lifecycle (create→use→delete) | **ENGINE PROVEN** — create/put/get/close/reopen/erase (`test_syssvc_rms_acp.c`) + driver-shaped create→version→enumerate→erase→restore pipeline (`test_syssvc_rms_workload.c`, A/D) | `vms-1b5`/`vms-2e72` |
| RMS-layer version bump (`;N` → `;N+1` on a second create) | **ENGINE PROVEN** — `sys$create` of the same name twice yields coexisting `;1`/`;2`, versionless open resolves `;2`, explicit `;1` still resolves `;1` (the teeth a POSIX overwrite cannot fake), `test_syssvc_rms_workload.c` (A). Reaching it through the PORT CRTL specifically → `vms-47e` | `vms-1b5`/`vms-2e72` |

---

**Update since the 2026-08-31 snapshot (re-derived from `vms-126`'s own PR
history, 2026-09-10):** `vms-126` landed further beyond the veneer above — PR
#1122 added `fwrite` chunking for writes >0xFFFF bytes (successive `sys$put`,
byte-transparent to the `mrs=1` `$GET` read loop) and `ovmx_crtl_tmpfile`
(`FAB$M_TMD` delete-on-close), proven by
`tests/qemu/test_syssvc_crtl_rms_bigwrite.c`. Descoped to two children: `vms-667b`
(a `FAB$M_TMD` delete-on-close ordering bug found along the way) and `vms-a11`
(`fseek`/`ftell`/`r+`/`w+` random access over the sequential RMS stream, deferred).
Re-derive current status with `rd show vms-126` / `rd show vms-667b` / `rd show vms-a11`
rather than trusting this note as it ages.

---

## 3a. Build drivers surface (Axis 6) + P1/P2 sequencing — merged from the original gap analysis

**What the port actually uses:** the modern `alpha-dec-vms` port builds by cross
`configure`+`make` (GNV-style), not MMS — no top-level `descrip.mms`/`.com` ships
for the full compiler. So "the port builds+runs on OVMX" is two phases:

- **P1 (near-term, this register's scope):** the port's *output/runtime* — a
  cross-built `alpha-dec-vms` compiler's produced objects/images — runs on
  OVMX-Alpha over the faithful surface (§1-§3 above). This is the ladder target
  `vms-fd1`.
- **P2 (self-host endgame, later, feeds `vms-df7`):** GCC's own `configure`+`make`
  runs *on* OVMX-Alpha itself — needs a GNV-class self-hosting build environment
  (shell, make, a bootstrap `cc`, `ar`/`ranlib`) on OVMX-Alpha. Sequenced strictly
  after P1; retires the tcc-bootstrap-only story for the toolchain lane.

**R9** (GNV-class self-hosting build env, P2) and **R10** (the `vms-9894`
x86_64/aarch64 clean-room authoring rung, §0) are both later-ladder rungs, captured
here so they are not forgotten, and neither blocks the P1 march.

---

## 4. Prioritized "what to implement first" list

Ordered by leverage toward `vms-fd1` (the port builds+runs unchanged), not by
rd-item creation order:

1. **`vms-1b5` (RMS beyond-stdio)** — closes the last plausible "first wall"
   region the design doc predicted; the mechanism exists, this is proof-of-reach
   for the driver's actual usage pattern. Needs alpha toolchain + qemu-alpha to
   verify.
2. **`vms-06d` (decc$feature switches + crtlmap re-audit)** — cheapest remaining
   DECC$SHR item (host-container only, no boot), and the crtlmap re-audit tells
   us the true remaining symbol-count gap instead of a stale 2026-08-22 number.
3. **`vms-714c` (include surface)** — cheap, host-verifiable, unblocks the port's
   own headers compiling unchanged (a "builds unchanged" bar item, not just
   "links unchanged").
4. **`vms-43c` (LIB$INITIALIZE PSECT collection)** — needed once a real port
   object actually emits a literal `LIB$INITIALIZE` PSECT (libgcc EH init,
   static C++ ctors compiled by the real port, not OVMX's own C++-first-light
   `.init_array` proof which used a different codegen path). Verify with the
   alpha cross toolchain.
5. **`vms-4b5` (cc1 union-pun-branch "miscompile")** — RESOLVED / not-a-bug: it
   was the OpenVMS 32-bit-`long` data model, not a codegen fault (§1.3). The real
   standing risk it exposed is broader — DEC C behavior/data-model compatibility,
   tracked in `docs/design-decc-bug-compat-architecture.md`.
6. **`vms-2e72` (real CHF/condition-handling dispatch)** — the deepest, most
   architectural remaining rung (OVMX-Alpha runtime/ABI territory, conductor-owned
   per the original doc's Axis-5 routing). Lowest urgency only because nothing
   proven so far has needed it yet (no C++ EH or signal-class error path has been
   exercised through a real port object); it will become urgent the moment one is.

## 5. Status reconcile (existing stale rd items)

`vms-e39` (R1, DECC$SHR decc$-vector export) and `vms-dfb` (R2, CRTL→RMS
route-through) already exist as children of `vms-3e4`/`vms-126` from the 2026-08-22
filing pass. Both are **effectively done** per the evidence in §1.1/§3.1 above (R1:
landed+CI-green on #711, grown substantially since; R2: empirically proven at
runtime by `#958`) but rd status still shows `inbox`/`blocked`. Progress notes citing
this evidence were left on both items (2026-08-31) recommending the conductor close
or retarget them; not closed here — this was a read-only analysis pass, and by-SHA
gate/close authority for this lane belongs to the conductor per those items' own
history.

**`vms-47e` (CRTL→RMS binding) — split (2026-08-31).** This PR lands + proves the
reusable CRTL→RMS stdio veneer (`src/vmsrms/crtl_rms_stdio.c`) with an un-fakeable
independent-ODS-2 proof (`tests/qemu/test_syssvc_crtl_rms_veneer.c`), on the host
executive harness where a real `/dev/vms` + mounted ODS-2 + independent ACP reader
exist today (§3.1 veneer bullet). The remaining **alpha PORT-image wiring** is a
build sub-project (no alpha-dec-vms `vms_kif`/RMS substrate exists yet) — the
four-step plan in the §3.1 REMAINING bullet. Conductor to file it as the `vms-47e`
child (this lane is read-only on rd): "vms-47e.1 — wire crtl_rms_stdio.c into the
alpha DECC$SHR: build alpha LIBVMSRMS$SHR (+ vms_kif/ACP ioctl transport for the
alpha-dec-vms musl world), suppress musl-alpha's decc$fopen/… so the veneer aliases
win, `--use` it in the port link + stage in SYS$SHARE, and upgrade the alpha
crtl_rms boot gate to write onto a WRITABLE ODS-2 volume + assert an independent
`$ DIRECTORY`/`sys$search` sees PORTTEST.DAT with a real File ID + `;1`."

## 6. New rd items filed (this pass, 2026-08-31)

| id | Title | Parent | Verify needs alpha toolchain/runtime? |
|---|---|---|---|
| `vms-714c` | R5: port include surface (rtldef/starlet_c) | `vms-3e4` | No (host-container) |
| `vms-43c` | R7: LIB$INITIALIZE PSECT multi-constructor collection | `vms-3e4` | Yes (activation proof) |
| `vms-06d` | decc$feature_* switches + crtlmap coverage re-audit | `vms-3e4` | No (audit); maybe (feature semantics) |
| `vms-2e72` | Real CHF/condition-handling dispatch | `vms-3e4` | Yes (deep runtime rung) |
| `vms-1b5` | RMS beyond-stdio (dir ops, listing files, temp lifecycle, version bump) | `vms-126` | Yes |
| `vms-4b5` | cc1 union-pun-branch miscompile | `vms-da0` | Partial (repro is compile-only; toolchain container needed) |

Pre-existing rungs merged in from the deleted docs (not filed this pass, carried
forward for single-ledger completeness): `vms-e9a` (Axis 4, spawn — B0/B1 landed,
B2 open, §2a), `vms-9894` (R10, later x86_64/aarch64 authoring rung, §0), R9
(GNV-class self-host build env, §3a — not yet filed as a distinct rd item).

---

*Doc history: merged 2026-09-17 (`vms-b3a6` wave 2) from
`design-gcc-vms-oracle-lane.md` (charter, §0) and `design-gcc-vms-port-surface-gaps.md`
(original Axis 1-6 analysis, §2a/§3a + the EVAX correction in §2), both deleted as
single-ledger duplicates. This file remains the one canonical GCC-port gap-register
doc; the F2a host-surface trio (`design-gcc-port-host-surface-{demands,gaps,gate}.md`)
are separate, non-duplicate measurement rungs and stay as their own docs.*
