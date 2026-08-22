# GCC-oracle lane — OpenVMS GCC port × OVMX faithful-surface gap analysis

> **Status:** DRAFT / first gap-analysis pass (2026-08-22). Lane epic: `vms-da0`. Pillar: toolchain (`vms-ade`).
> Owner: GCC production-compiler lane (conductor peer). Release-gates shared-core via the 3-way convergence gate.

## Charge (operator, relayed via ACP conductor 2026-08-22)

**North star.** The *real* OpenVMS GCC port **builds from source AND runs on OVMX**, over OVMX's **own faithful VMS surface** — genuine DECC$SHR / C-RTL, RMS file I/O over the ACP, LINK.EXE consuming genuine VMS object/image format + symbol-vector/GSMATCH, MMK/MMS + DCL build drivers, the VMS calling standard + condition handling. **Linux GCC is only ever a correctness oracle.**

**Faithfulness is a ladder.** We never adapt GCC *down* to OVMX's surface; we build OVMX's surface *up* until the real VMS port just builds. Each rung is a verifiable outcome: "the surface now supports X, and Y more of the port builds against it." Every surface decision is oracle-grounded to real OpenVMS behavior — **lab-Alpha** (OpenVMS Alpha 8.4, LP64) and the **VAX lab** (V7.3, ILP32), clean-room per Rule 8.

**No cheating.** Never vendor-and-hack a Linux toolchain component to fake a milestone. A rung is done when the real VMS port builds *more of itself* — never when "a Linux binary ran." This document exists because the prior cc1-in-guest approach (vendor Alpine cc1 + whole-archive musl + hack LINK.EXE IE/TPOFF32 TLS relaxations so a Linux binary executes) was exactly that cheat; #708 reverted the LINK hacks.

## Goalpost — the real OpenVMS GCC port

**CONFIRMED (conductor/operator 2026-08-22):** anchor on GCC upstream **`alpha*-dec-*vms*`** — the only cleanly-obtainable *real* in-tree GPL OpenVMS GCC port, targeting OpenVMS **Alpha**. Zero authoring required; Alpha is first-class here, so this is the faithful *starting* rung, not a detour.

- **Anchor version: GCC 8.5.0** (last release-series with substantive AdaCore / Tristan Gingold maintenance of `config/vms/`). The `alpha*-dec-*vms*`, `ia64-hp-*vms*`, and generic `*-*-*vms*` `config.gcc` stanzas are **present and NOT obsolete** through 8.5.0 → 14.2.0 (verified against real `config.gcc`), so **14.2.0** is available later if a modern C++ host compiler is wanted.
- **Obtained (oracle only, never vendored):** `/tmp/gcc-oracle`, sparse/blobless clone of `releases/gcc-8.5.0` (`gcc/config/{vms,alpha,ia64}`, `libgcc/config/{vms,alpha}`, `config.gcc`). ~104 MB (trimmable to ~2 MB).
- **Pointer model:** the Alpha port defaults `LONG_TYPE_SIZE`/`POINTER_SIZE` **32**, goes **LP64 under `-mpointer-size=64`** (`config/vms/vms.h:54-60`) — the authentic OpenVMS Alpha dual 32/64-bit pointer model. `__CRTL_VER`/`__VMS_VER` default `70320000` = **V7.3-2** (`config/alpha/vms.h:305-306`); lab-Alpha oracle is V8.4 (behavior grounds to 8.4).
- **Excluded:** `ia64-hp-vms` (operator dropped Itanium). VSI x86-64 OpenVMS GCC — no in-tree `x86_64*vms*` stanza exists; source not a cleanly-separable clean-room goalpost. **VAX/VMS GCC obsoleted at GCC 3.3 — there is no modern VAX GCC goalpost** (the VAX lane is not a GCC target).
- **Build model reality (scout C):** no top-level `descrip.mms`/`.com` ships — the modern port is built by a **cross `configure`+`make`** (GNV-style), producing a compiler that *runs* on VMS. So "builds+runs on OVMX" has two phases (see Sequencing): **(P1) the port's runtime/output runs on OVMX-Alpha** over the faithful surface; **(P2) GCC itself builds *on* OVMX** — needs a self-hosting Unix-ish build env (shell/make/bootstrap cc) on OVMX, a deeper rung.
- **Explicit ladder sequencing (do not forget):** `alpha-dec-vms` on **OVMX-Alpha** first → generalize the surface → **author an x86_64 VMS-host layer later** (top-of-ladder rung). The self-host northstar (`vms-df7`: GCC builds the shipped x86_64 kernel) ultimately needs an x86_64 VMS-host compiler for which *no existing port exists* — that is a **later clean-room-authoring rung** (Rule 8: author the x86_64 VMS-host layer grounded to real OpenVMS behavior, **never lifted from VSI**). It does **not** block or distract the first march; it is captured here so sequencing stays explicit. (Conductor is flagging this future-scope item to the operator.)

## Method

1. Obtain the real port source (sparse, oracle-only — never vendored into OVMX).
2. Enumerate the port's faithful-surface **requirements** (what it needs from DECC$SHR / RMS / object-link / condition / build-drivers).
3. Map OVMX's **current** surface.
4. **Gap = requirement − current**, each row oracle-grounded to real OpenVMS (lab-Alpha LP64 / VAX ILP32).
5. Gaps become outcome-shaped **rd rungs** under `vms-da0`. Executive-facility gaps route to the conductor for genuine backfill + the 3-way gate.

## Coordination & clean-room (conductor, 2026-08-22)

- **Alpha-lane alignment.** This toolchain work rides the **OVMX-Alpha runtime surface**, which the Alpha lane owns. If the alpha-dec-vms build surfaces OVMX-Alpha **runtime/executive** gaps, **route them to the conductor** — do not fix Alpha-runtime internals unilaterally.
- **Obtainability vs. behavior.** Obtaining GPL alpha-dec-vms *source* to build is fine (no-vendoring `/tmp` sparse scout). But the DECC$SHR / RMS / object-format / condition **behavior** OVMX's surface is built to must be **oracle-grounded clean-room** (lab-Alpha 8.4), **never inferred from VSI binaries** (Rule 8).

---

## Gap matrix

### Axis 1 — C-RTL / DECC$SHR

**OVMX current surface** (scout A, 2026-08-22 — cites into `src/vmslink/mk_decc_shr.sh`):

- DECC$SHR is a **whole-archive of musl `libc.a` (1345 members) + libgcc**, linked by LINK.EXE into one ELF `ET_DYN` OVMX shareable with a **372-universal `.vms$sv` symbol vector** (369 PROCEDURE + 3 DATA: `stdin/stdout/stderr`), STRICT (zero deferred externals). Grown append-only (GSMATCH LEQUAL).
- Breadth present: full stdio, str/mem, math (+20 libgcc IEEE-quad soft-float helpers), POSIX file ops, pthreads, signals, time, sockets, dlopen.
- The only non-musl universal is `ovmx_get_libc` (`ovmx_libc_stub.c` — exposes musl's hidden `__libc` for IMGACT's TLS re-drive); plus `__init_libc`, `__copy_tls`, `__init_tp` glue.

**What the port actually requires** (scout C — `gcc/config/vms/vms.c`, `vms-crtlmap.map`, `libgcc/config/vms/`): the compiler **transparently aliases every standard C name to `decc$<name>`** (`vms.c:194`, applied as an IDENTIFIER transparent-alias) with a ~900-entry mapping table (`vms-crtlmap.map`) and name-decoration rules: `MALLOC`→`_malloc64`, `64`→`_NAME64`/`_X32`/`_X64` long-pointer triples, `FLOAT32/64/128` prefix chars + `MATH$XXX_{S,T,X}`, `BSD44`→`decc$__bsd44_`, `G_MASK`→`decc$ga_`/`decc$gl_`, `32ONLY` (getopt/getarg, dropped under LP64). Required bootstrap/runtime entry points: `decc$main`, `_malloc32`/`_malloc64`, `get_errno_addr`/`get_vms_errno_addr`/`vaxc$errno`, `decc$to_vms` (Unix↔VMS path xlat), `C$_EXIT1` (exit-status). `fwrite`/`fwrite_unlocked` are treated as **non-standard on VMS** and disabled as builtins.

**The core gap has two layers: (1) NAMING — the port's objects reference `decc$`-prefixed universals; OVMX's DECC$SHR exports bare musl names (`fprintf`, not `decc$fprintf`), so the port would not even *link*. (2) SEMANTICS — even bound, the musl backend has POSIX file semantics, not VMS.**

| Requirement (DEC C RTL) | OVMX current | Gap | Oracle |
|---|---|---|---|
| **CRTL universals are `decc$`-prefixed** (transparent alias + decoration rules) | DECC$SHR exports **bare musl names**; no `decc$*` universal in the 372-entry vector | **NAMING gap — the port cannot link against OVMX's DECC$SHR.** OVMX must export the `decc$`-prefixed vector honoring the exact `vms.c` decoration rules. **#1 highest-leverage rung.** | Alpha 8.4 DECC$SHR vector |
| Text-library includes `rtldef`, `starlet_c` on the system include path; include canonicalization (lowercase basename+`.h`, no dots in dir names); `$` legal in identifiers | OVMX has `starlet.h`/`ssdef.h` etc. as ordinary headers; no `rtldef`/`starlet_c` text-library concept on the include path | Provide the port's expected include surface (`rtldef`/`starlet_c`) | lab |
| File I/O routes through RMS (`fopen`/`open`/`creat` → FAB/RAB → ACP) | musl→POSIX→Linux syscall **passthrough**; `OVMX_RMS_IO` is an app-level seam in LINK.EXE/TCC.EXE only, **not** in the C-RTL | **File I/O in the C-RTL bypasses RMS entirely** (INV-6 / executive-boundary veneer). A compiler's `decc$fopen` must reach RMS (which exists — Axis 3). | Alpha 8.4 DECC$SHR |
| File versioning on create (`foo.o;2`) | ABSENT in CRTL (no `creat` exported; musl `fopen`/`open` write plain paths). Versioning exists only in `src/vmsrms`/`src/vmsfs` via `sys$create`, unreached by the C-RTL | Version-on-close missing from the CRTL path | VAX/Alpha lab |
| FAB/RAB direct access from the CRTL; record formats (rfm/rat, `"rfm=var"` fopen args) | ABSENT (no `fab$`/`rab$`/`cc$rms_*` universals; musl `fopen` takes no VMS optional-arg form) | No CRTL-side FAB/RAB or record formats | lab |
| `decc$feature_*` switches (DECC$UNIX_LEVEL, EFS_CHARSET, POSIX-vs-VMS filename mode) | ABSENT / not emulated (roadmap-only) | Feature-switch layer missing | lab |
| `vaxc$errno` dual VMS status | ABSENT (roadmap Phase 9) | Missing | lab |
| VMS filespec parsing / logical translation on `fopen`/`open` (`dev:[dir]file.ext;v`) | ABSENT in CRTL (musl raw POSIX paths). Parsing lives in `vmsfs`/`vmsrms`, invoked via services, not via DECC$SHR | CRTL doesn't parse VMS filespecs or translate logicals | lab |

*Notable in-vector absences already visible:* no `creat`, no `mkstemp` (only `mkstemps`/`mktemp`), no `decc$*`, no `getopt`/`regcomp`, no LFS `open64`.

> **Implication for the port:** the alpha-dec-vms host layer + libgcc call the DEC C RTL expecting VMS semantics (versioned temp/listing files, RMS record I/O, VMS filespecs). OVMX's DECC$SHR gives libc breadth but the *wrong* file semantics — the biggest single ladder region. Whether the CRTL must *route through RMS* or merely *present VMS semantics over the ACP* is the first design rung (see Open Questions).

### Axis 2 — LINK / object–image / symbol-vector format

**OVMX current** (scout B — `src/vmslink/link.c`, `src/vmslink/include/ovmx_image.h`):

- **Consumes** stock **ELF64 `ET_REL`** only (`link.c:359-366`) + `ar`/`.OLB` archives whole (in-process, no `ld -r`). Reloc set is curated dual-arch (aarch64 + x86_64) — x86_64 has `R_X86_64_64/PC32/PLT32/GOTPCREL(X)/REX_GOTPCRELX` + TLSDESC/gnu2 quartet; `default: die("unsupported .text relocation")` (`link.c:1283`). **PLT32 written identically to PC32 — no PLT stub; cross-image calls out of scope** (`link.c:1268-1281`).
- **Emits** ELF `ET_DYN` (`link.c:2447`) + OVMX-original `.vms$*` sections: `.vms$sv` (symbol vector, bound by **INDEX** not name), `.vms$imp` (imports bound to `(soname, index)`), `.vms$wimp` (by-name weak, activation-resolved), `.vms$rel`/`.vms$tls`/`.vms$ehf`. GSMATCH major/minor. Also emits executables (`--executable` → crt0 + `PT_INTERP`, same emit path).
- **No VMS OBJ path anywhere** — grep `gsd|tir|evax|egsd|etir|emh|obj-evax` in `src/vmslink/` = 0 hits.

**What the port actually does** (scout C): GCC itself `#define OBJECT_FORMAT_ELF` but the comment says "not really Elf … makes compiling crtstuff.c easier" (`alpha/vms.h:20-22`) — GCC emits **assembly**; the actual **VMS OBJ (GSD/TIR/EGSD/ETIR) records are produced by the binutils `alpha-dec-vms` gas/BFD backend** (binutils not cloned → byte layout UNVERIFIED). The **link step is the native VMS LINKER**, invoked literally as `$ link` by the `vms-ld.c` wrapper, driven by a generated **`.opt` option file**: `IDENT=` (image ident, ≤15 chars), `GSMATCH`, `symbol_vector=(…)` (universal exports), `cluster=`/`collect=` (sequentialize DWARF2 + `eh_frame` psects), **`PSECT_ATTR=LIB$INITIALIZE,GBL`** (always), `/share` vs `/exe`, `.cld` command-definition files, and `SYS$LIBRARY` redefinition via `SYS$TRNLNM`/`SYS$CRELNM`. EH/debug objects pulled in: `vms-dwarf2eh.o`, `vms-dwarf2.o`.

| Requirement | OVMX current | Gap | Oracle |
|---|---|---|---|
| Object format the port emits ↔ what LINK consumes | GCC emits asm → **gas(alpha-dec-vms) emits VMS OBJ** for the VMS LINKER. OVMX LINK eats **ELF `ET_REL`** | **DESIGN RUNG (open question 3):** either (a) OVMX assembles to ELF (a toolchain-config choice — object format is toolchain-internal, not GCC source) + LINK stays ELF-in, or (b) LINK grows a faithful VMS-OBJ (GSD/TIR) front-end. (a) is far cheaper and arguably not "adapting GCC down"; needs conductor/operator call. | Alpha 8.4 obj/LINK |
| **LINK driven by the `vms-ld` `.opt` grammar** (IDENT/GSMATCH/symbol_vector/cluster/collect/`PSECT_ATTR=LIB$INITIALIZE`) | OVMX LINK has native `.vms$sv` **symbol vectors + GSMATCH** already — but no `.opt` option-file front-end, no `PSECT_ATTR`/`cluster`/`collect` | **Tractable rung:** teach LINK.EXE (or a `vms-ld`-compatible shim) the `.opt` grammar and map `symbol_vector=`/`GSMATCH`/`IDENT` onto the existing `.vms$sv`/GSMATCH machinery. The primitives exist. | Alpha 8.4 LINKER `.opt` |
| `SYS$LIBRARY` logical + `SYS$TRNLNM`/`SYS$CRELNM` at link time | OVMX has logical-name services (`vmslnm`) | Confirm the link-time logical redefinition path works | lab |
| Cross-image PLT/GOT call stubs | stubbed — intra-link only (`link.c:1276-1281`) | The port links against shareable images (DECC$SHR, libgcc shareable) → **cross-image call stubs likely required** | lab |

### Axis 3 — RMS file semantics

**OVMX current** (scout B — RMS is **broad and real**, not the gap it was assumed to be):

- **Versioning IMPLEMENTED:** `rms_next_version()` = highest+1 (`rms_core.c:1147-1163`); explicit `;N` honored (`rms_core.c:1166+`); ODS-2 directory records carry descending versions, duplicate-version rejected (`ods2_path.c:60-119`, `ods2_edit.c:586-731`).
- **FAB/NAM/RAB present** (`src/vmsrms/include/rms/*.h`); org/rfm/mrs/mrn/rat/fsz round-tripped.
- **Record formats:** FIX, VAR, VFC, STM, STMLF, STMCR all supported (`rms_seq.c:56,254`); orgs SEQ/REL/IDX (Prolog-3 indexed).
- **Temp / delete-on-close IMPLEMENTED:** `FAB$M_TMP/TMD/DLT`; close unlinks on DLT/TMD (`rms_core.c:2092-2137`).

| Requirement | OVMX current | Gap | Oracle |
|---|---|---|---|
| Assembler/BFD sequential + temp scratch files | full SEQ + temp/delete-on-close present | **Primitives exist** — gap is reaching them *from the C-RTL* (Axis 1), not RMS itself. Predicted first GCC wall (`design-gcc-vms-oracle-lane.md:122`) lands here but the mechanism is present | Alpha 8.4 RMS |
| Backing store fidelity | POSIX-fd backend + RMS-metadata **sidecar** + genuine ODS-2 writer path | Confirm sidecar vs. pure-ODS-2 doesn't leak non-VMS behavior the port observes | lab |

> **Cross-axis finding:** RMS is strong; DECC$SHR (Axis 1) doesn't route to it. The highest-value early ladder region is **binding the C-RTL file layer onto the existing RMS/ACP stack**, not building RMS.

### Axis 4 — Process / spawn (driver pipeline cpp→cc1→as→ld)

**OVMX current** (scout B — three divergent fork-based creation paths):

1. `lib$spawn` (`lib_misc.c:207-360`) — `fork()` + exec of **`/bin/sh -c`** (a Bourne shell, **not DCL**); `/NOWAIT` returns without wait.
2. `sys$creprc` (`sys_process.c:608-823`) — `fork()`+`execve()`, double-fork for detached; **registers the child name** via `vms_kif_setprn()` so `$GETJPI`/`SHOW SYSTEM` see it, but privs/UIC/username/quotas are **process-local only** (no executive row).
3. DCL `SPAWN`/`RUN` (`dcl_cmd_process.c:1138,1645-1718`) — third path with duplicated `/OUTPUT`+`/NOWAIT` handling.

| Requirement | OVMX current | Gap | Oracle |
|---|---|---|---|
| Driver spawns a stage, learns exit status, spawns next | synchronous `waitpid` status works | **`/NOWAIT` completion channel absent:** `lib$spawn`'s `efn`/`astadr`/`astprm` are **silently discarded** (`lib_misc.c:275,282`) — no EF, no termination AST on exit. The async-AST + mailbox machinery exists (`vms_ast.c`, `vms_mbx.c`); it just isn't wired to `lib$spawn` completion. Filed: `vms-e9a`. Executive-facility → **route to conductor** | Alpha 8.4 spawn |
| Unify the three creation paths | divergent (sh-exec vs creprc vs DCL) | Unify onto `sys$creprc` + real registration (shared kernel-core, 3-way gate) | lab |

### Axis 5 — Condition handling / calling standard / image init

**OVMX current** (scout B):

- **Condition dispatch = thread-local handler-stack emulation** (`lib_signal.c`) — `lib$establish/revert/signal/stop` over a `_Thread_local` stack; handlers invoked as ordinary calls. **`SYS$SETEXV` does not exist** (0 hits). `sys$unwind` (`sys_condition.c:44-68`) only pops the handler chain — **`newpc` ignored, no machine-frame transfer** ("no frame to transfer control to").
- **Image init:** ELF `.init_array` runs at activation (`imgact.c:584-599,1128-1135`); `.vms$ehf` eh_frame registered via `__register_frame` **before** ctors (needed for GCC EH). **LIB$INITIALIZE PSECT-ordered init NOT reproduced** — plain C++ `.init_array` ctors work; a runtime relying on the VMS `LIB$INITIALIZE` PSECT list gaps.

**What the port actually requires** (scout C — `libgcc/config/alpha/vms-unwind.h`, `vms-gcc_shell_handler.c`, `alpha/vms.h`): the **OpenVMS Alpha calling standard** — procedure descriptors `PDSCDEF` (kinds `PDSC$K_KIND_FP_STACK`/`FP_REGISTER`, register save area, `pdsc$l_ireg_mask`, PV via frame ptr r29); `CUMULATIVE_ARGS = {int num_args; enum avms_arg_type atypes[6]}` (I64/FF/FD/FG/FS/FT, **max 6 register args**); headers `vms/pdscdef.h`, `libicb.h`, `chfctxdef.h`, `chfdef.h`, `ssdef.h`. **CHF:** `chf$signal_array`, `chf$mech_array`, `CHFCTX`, dispatcher detected via `SYS$GL_CALL_HANDL`. **Invocation-context unwind services:** `LIB$GET_INVO_HANDLE`, `LIB$GET_INVO_CONTEXT`, `LIB$GET_PREV_INVO_CONTEXT`. Static handler `__gcc_shell_handler` returns `SS$_RESIGNAL` when no user handler. **`LIB$INITIALIZE` section:** `.section LIB$INITIALIZE,GBL,NOWRT` (`alpha/vms.h:297`). Exit-status `C$_EXIT1` + `STS$V_MSG_NO`/`STS$M_INHIB_MSG` mapping; `MASK_RETURN_ADDR` masks to 32 bits.

| Requirement | OVMX current | Gap | Oracle |
|---|---|---|---|
| **OpenVMS Alpha calling standard** — PDSC procedure descriptors, r29 PV, `CUMULATIVE_ARGS` 6-reg model, invocation-context (`LIB$GET_INVO_*`) | *(OVMX-Alpha runtime — pending confirmation; likely absent as VMS-authentic PDSC)* | **OVMX-Alpha runtime/ABI surface → route to conductor** (Alpha lane owns runtime internals). Deep rung. | Alpha 8.4 calling std |
| VMS condition handling — CHF (`chf$signal_array`/`chf$mech_array`/CHFCTX), dispatcher via `SYS$GL_CALL_HANDL`, `SYS$SETEXV` vectors, real frame unwind (`newpc` transfer) | handler-stack emulation; **no `SYS$SETEXV`**, `sys$unwind` ignores `newpc` (no frame transfer) | **Real executive condition dispatch/unwind is a genuine forcing target** (`design-gcc-vms-oracle-lane.md:104-108`). Executive-facility → route to conductor | Alpha 8.4 CHF |
| `LIB$INITIALIZE` PSECT-ordered constructors (`.section LIB$INITIALIZE,GBL`, always emitted in `.opt` via `PSECT_ATTR`) | `.init_array` only; no LIB$INITIALIZE PSECT collection | Add LIB$INITIALIZE PSECT collection in LINK (ties to Axis 2 `.opt` rung) | lab |
| libgcc VMS unwind / `.eh_frame` (`vms-dwarf2eh.o`, `cluster=`/`collect=` psect sequencing) | `__register_frame` wired pre-ctors (`imgact.c`, vms-70d) | Confirm the port's EH lands on this path once `.opt` cluster/collect is honored | lab |
| Exit-status POSIX↔VMS mapping (`C$_EXIT1`, `STS$` bits, `-mvms-return-codes`) | *(pending confirmation)* | Provide `C$_EXIT1` globalvalue + status mapping | lab |

### Axis 6 — Build drivers (MMS/MMK, DCL .com, configure/make)

**What the port actually uses** (scout C): the modern port builds by **cross `configure`+`make`** (GNV-style), NOT MMS. GNU make fragments: `x-vms` (no `collect2`: `USE_COLLECT2=`, `LN=cp`), `t-vms` (crtlmap gen + `vms.o`/`vms-c.o`), `t-vmsnative` (builds native `ld`/`ar` wrappers from `vms-ld.c`/`vms-ar.c` when `gnu_ld != yes`). libgcc: `crt0.o` from `vms-ucrt0.c` compiled `-mpointer-size=64`; `vms-dwarf2{,eh}.o` from `.S`. **No full-GCC `descrip.mms` ships in-tree** — only `libiberty/configure.com` + `makefile.vms` DCL scaffolding (UNVERIFIED that a self-hosting MMS build exists). Build-time needs: binutils→`alpha-dec-vms` (gas + VMS BFD) *or* native VMS MACRO/LINKER; the DEC C RTL text-libraries (`rtldef`, `starlet_c`) on the include path; the native VMS LINKER at link.

| Requirement | OVMX current | Gap | Oracle |
|---|---|---|---|
| **P1: the port's *output + runtime* runs on OVMX-Alpha** (crt0, libgcc, the `decc$` CRTL it calls, `$ link` via `.opt`) | surface gaps per Axes 1/2/5 | The near-term march — build OVMX's surface up so a *cross-built* alpha-dec-vms compiler's output activates+runs on OVMX-Alpha | Alpha 8.4 |
| **P2: GCC *builds on* OVMX** (`configure`+`make` running on OVMX-Alpha) | needs a self-hosting Unix-ish build env (shell, make, a bootstrap cc, `ar`/`ranlib`) on OVMX-Alpha | Deeper rung — GNV-class build environment on OVMX; sequence *after* P1. (tcc bootstrap stays labeled until then.) | Alpha 8.4 / GNV |

### Sequencing — P1 before P2

"The port builds+runs on OVMX" splits: **P1** = build OVMX's surface up until an alpha-dec-vms compiler (cross-built on a build host as the oracle reference) *runs* on OVMX-Alpha and its runtime/output is faithful; **P2** = run the port's own `configure`+`make` *on* OVMX-Alpha (self-hosting build env). P1 is the near-term ladder; P2 is the self-host endgame (`vms-df7`), later. This doc's rungs are P1 unless marked.

---

## Ladder rungs (rd, to file under `vms-da0` after the reframe lands)

Each rung is a verifiable outcome. **Owner:** `L` = this lane (LINK / CRTL-vector / include surface); `C` = conductor (executive / OVMX-Alpha runtime facility, rides the 3-way gate). Rough dependency order.

| # | Rung (outcome) | Owner | Axis |
|---|---|---|---|
| **R1** | **DECC$SHR exports the `decc$`-prefixed CRTL universal vector**, honoring the `vms.c` decoration rules (`decc$`, `_malloc32/64`, `_NAME64`/`_X32/_X64`, float-model prefixes, `BSD44`, `G_MASK`, `32ONLY`). *Verify:* an alpha-dec-vms `.o` referencing `decc$fprintf`/`decc$main`/`_malloc32`/`get_vms_errno_addr`/`vaxc$errno` **resolves** against DECC$SHR (zero unresolved `decc$*`). **The first rung to climb — highest leverage; nothing links without it.** | L | 1 |
| **R2** | **CRTL file I/O routes through the RMS/ACP stack** — `decc$fopen`/`open`/`creat`/`tmpfile` reach FAB/RAB → ODS-2-over-ACP (RMS already supports versioning/records/temp; Axis 3). *Verify:* a `decc$`-CRTL create yields a versioned RMS file over the ACP, not a raw Linux path. **Must not reinstate a veneer (INV-6).** Design question 2 (route-through vs `libvmscrtl` shim) decided here. | L (+C for ACP) | 1↔3 |
| **R3** | **LINK honors the `vms-ld` `.opt` grammar** — `symbol_vector=`/`GSMATCH`/`IDENT`/`cluster=`/`collect=`/`PSECT_ATTR=LIB$INITIALIZE` mapped onto native `.vms$sv`/GSMATCH. *Verify:* the port's `$ link` step (vms-ld wrapper → OVMX LINK) turns port `.o`s into an OVMX image. | L | 2 |
| **R4** | **Object-format decision recorded + implemented** (open question 3): assemble-to-ELF (cheap, toolchain-internal) vs faithful VMS-OBJ (GSD/TIR) front-end. *Verify:* a port `.o` → OVMX image via the chosen path. **Conductor/operator design call.** | L→C call | 2 |
| **R5** | **The port's include surface** — `rtldef`/`starlet_c` text-libraries on the include path + include canonicalization + `$`-in-identifiers. *Verify:* the port's host sources `#include` resolve unchanged on OVMX. | L | 1 |
| **R6** | **`lib$spawn` `/NOWAIT` completion (EF + termination AST)** wired (`vms-e9a`) so a `cpp→cc1→as→ld` driver spawns a stage, learns its status, spawns the next. *Verify:* the driver pipeline runs a multi-stage compile with real completion signalling (not just synchronous `waitpid`). | C | 4 |
| **R7** | **LIB$INITIALIZE PSECT-ordered constructors** collected in LINK (ties R3). *Verify:* LIB$INITIALIZE-ordered init runs for the port's runtime. | L | 2/5 |
| **R8** | **OpenVMS Alpha calling standard on OVMX-Alpha** — PDSC procedure descriptors, r29 PV, CHF (`chf$signal_array`/`mech_array`/CHFCTX), invocation-context (`LIB$GET_INVO_*`), real `sys$unwind` frame transfer. *Verify:* libgcc `vms-unwind` condition-handling + a thrown-through-frames unwind works on OVMX-Alpha. **Deep OVMX-Alpha runtime rung → conductor.** | C | 5 |
| **R9** | *(P2, endgame)* **GNV-class self-hosting build env on OVMX-Alpha** — shell/make/bootstrap-cc/`ar` so the port's own `configure`+`make` runs *on* OVMX. Sequenced after P1; retires the tcc bootstrap. | L+C | 6 |
| **R10** | *(top-of-ladder, later — captured so it's not forgotten)* **Author an x86_64 VMS-host GCC layer**, clean-room, grounded to real OpenVMS behavior (no VSI lift), for the x86_64 self-host northstar (`vms-df7`). Not near-term; does not block P1. | L+C | goalpost |

**First march:** R1 (decc$ vector) → R2 (CRTL→RMS) → R3/R4 (LINK `.opt` + object format) — that chain gets a cross-built alpha-dec-vms compiler's objects linking into a runnable OVMX-Alpha image over a faithful surface. R6/R8 unblock the driver pipeline and condition-handling depth.

## Open questions

1. **Goalpost arch** — anchor on `alpha-dec-vms` → OVMX-Alpha first. **RESOLVED (conductor/operator 2026-08-22): yes.** x86_64 VMS-host layer captured as a later clean-room rung.
2. **CRTL↔RMS binding** — does DECC$SHR's file I/O *route through* RMS/ACP (replace the musl file layer with an RMS-backed one), or does OVMX present VMS file semantics at the RMS/ACP layer and interpose a `libvmscrtl` shim? First design rung; must not reinstate a veneer (INV-6). *(Highest-value early region — RMS is strong, the CRTL just doesn't reach it.)*
3. **Object format** — CONFIRMED (scout C): GCC emits assembly; the binutils `alpha-dec-vms` gas/BFD backend produces VMS OBJ (GSD/TIR) for the native VMS LINKER. OVMX's LINK consumes ELF `ET_REL`. **Design call (conductor/operator):** (a) configure OVMX's assembler to emit ELF (object format is toolchain-internal, not GCC source — cheap, arguably *not* "adapting GCC down") + LINK stays ELF-in, or (b) LINK grows a faithful VMS-OBJ (GSD/TIR) front-end. My read: (a), with the faithfulness effort spent on the `.opt`/symbol-vector/GSMATCH *link surface* (R3) rather than the object byte-format. Confirm.
