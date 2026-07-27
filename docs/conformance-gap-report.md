# Conformance Gap Report — Eight-Cubed Corpus

**Item:** vms-801.4 (scoped baseline + triage task; NOT the 80% milestone)
**Date:** 2026-07-27
**Corpus:** `tests/corpus/tier1-examples/` — 229 Eight-Cubed VMS C examples (see `tests/corpus/PROVENANCE.md`)
**Harness:** `tests/conformance/run_corpus.sh`, run inside the `Dockerfile` builder stage (Ubuntu 24.04, cmake+gcc, `-DBUILD_TOOLS=ON`) via `podman build --target builder` + `podman run`. Build artifacts and per-program compile/run logs written to `/tmp` inside the container (never into the repo).

## 0. Harness bug found + fixed

`run_corpus.sh`'s `LIB_FLAGS` used plain `-lvmsrms -lvmsfs -lvms -lvmsprocess`. The OVMX runtime libraries are built as OpenVMS-style shareable images (`OUTPUT_NAME "LIBVMS$SHR"`, `SUFFIX ".EXE"`, `PREFIX ""` — see `src/libvms/CMakeLists.txt` etc.), so `ld` looks for `libvms.so`/`libvms.a` and never finds them. Before the fix, **every single corpus program failed at the link step** (`cannot find -lvmsrms` etc.), i.e. the harness silently reported ~0% across the board regardless of actual VMS-API coverage. `run_conformance.sh` (the older, smaller harness) already carries the correct fix (`-l:LIBVMS\$SHR.EXE` style); `run_corpus.sh` (added later in vms-801.3) never got it. Fixed in `tests/conformance/run_corpus.sh` (this branch) to mirror `run_conformance.sh`'s linking. This is a harness-only fix — no `src/` changes.

## 1. Fresh baseline (this run)

| Metric | Count | % of 229 | Definition |
|---|---|---|---|
| **Compile %** | 122 / 229 | **53.3%** | Passed the C front-end (source parses + type-checks to an object); may still fail at link |
| **Link+run %** | 50 / 229 | **21.8%** | Produced a runnable binary (compiled AND linked) |
| **Run-pass %** | 34 / 229 | **14.8%** | Ran and exited 0 |

Full status breakdown:

| Status | Count | % |
|---|---|---|
| compile-fail (source-level error) | 107 | 46.7% |
| link-fail (compiles, linker can't resolve a symbol) | 72 | 31.4% |
| run-pass (exit 0) | 34 | 14.8% |
| run-fail (exit non-zero) | 10 | 4.4% |
| run-crash (killed by signal) | 6 | 2.6% |

**Note on the stale baseline:** the task brief flagged the previously recorded 52%/14% as stale (predating vms-810's 18 RTL functions and vms-812's 33 headers). The fresh numbers above (53.3% compile / 14.8% run-pass) land almost exactly on top of the stale ones despite those two landings. That is consistent with vms-810/812 having closed a *different* slice of gaps than what's currently blocking the corpus (see §2/§3 below — the biggest single-symbol wins remaining are not among what 810/812 shipped), not with the harness being broken during the stale measurement (this run's fix was necessary to get *any* signal at all — a broken-harness measurement would read ~0%, not 52%, so the stale number was almost certainly taken before the regression that broke `LIB_FLAGS`, or taken with a hand-patched command).

## 2. Root-cause triage methodology

179 programs fail (107 compile-fail + 72 link-fail). Of those, **18 are architecture-gated, not an OVMX API gap** (see §3.0) — they hit a source-level `#error "...G_FLOAT..."` / `#error "...Alpha specific..."` directive from the original VAX/Alpha-only example code, before ever reaching an OVMX API call. These 18 are excluded from the symbol-gap counts in §3 so the counts reflect only genuinely fixable OVMX gaps; they contaminate naive symbol matching (e.g. `lib$polyg`'s implicit-declaration warning still fires after the `#error`, but fixing `lib$polyg` would not unblock the program).

That leaves **161 genuinely gap-blocked programs**, categorized by root cause (a program can hit more than one):

| Category | Programs touched (any) | Programs where this is the ONLY blocker |
|---|---|---|
| missing_function (RTL/syscall not implemented or not declared) | 113 | 74 |
| type_mismatch (signature/type incompatible with real VMS) | 39 | 12 |
| missing_constant ($-namespaced macro/status code not defined) | 28 | 1 |
| missing_header (`.h` doesn't exist yet) | 32 | 32 |

119 of 161 (74%) have exactly one blocking category — fixing that one category fully unblocks the program. 33 need two categories fixed, 9 need three.

## 3.0 Architecture-gated (NOT an OVMX gap) — 18 programs

Original Eight-Cubed source hits a VAX/Alpha/IA64-only `#error` before any OVMX code path is reached. Recommend excluding these from the vms-801.4 milestone denominator (or accepting them as permanent non-portable failures) rather than filing OVMX gap beads:

| Program | `#error` reason |
|---|---|
| lib_cvtf_from_internal_time, lib_cvtf_to_internal_time, lib_emodg, lib_mult_delta_time, lib_polyg, lib_wait, ots_cnvout, ots_cvt_t_x, ots_divc, ots_mulc, ots_powc, ots_powcj | Requires `CC/FLOAT=G_FLOAT` (VAX G-float default on Alpha; not applicable to x86_64/aarch64 IEEE float) |
| sys_dclcmh, sys_get_arith, sys_getenv, sys_glx_lock | "Alpha specific code" |
| sys_ieee | Requires an `/IEEE_MODE` qualifier |
| sys_power | "IA64 specific code" |

If a fix is ever wanted: 11 of the 18 are the G_FLOAT family and might unlock together if the corpus is compiled with a macro shim that satisfies the `#if`/`#error` guard (needs inspection of the actual guard, not just the message) — a single investigation could resolve up to 11 programs at once. The remaining 7 are genuinely non-portable.

## 3.1 missing_header — 32 programs unblocked, 22 headers

Every one of these 32 programs is blocked *solely* by the header (100% single-blocker rate for this category — once the header exists with real content, the program still needs its declarations to compile clean and link, but nothing else was observed failing).

| Header | Programs unblocked |
|---|---|
| capdef.h | 3 |
| nsadef.h | 3 |
| devdef.h | 2 |
| dvsdef.h | 2 |
| fscndef.h | 2 |
| lkidef.h | 2 |
| quidef.h | 2 |
| seciddef.h | 2 |
| acldef.h | 1 |
| acmedef.h | 1 |
| clidef.h | 1 |
| fibdef.h | 1 |
| fpdef.h | 1 |
| pal_services.h | 1 |
| pqldef.h | 1 |
| prdef.h | 1 |
| prtdef.h | 1 |
| prxdef.h | 1 |
| rmidef.h | 1 |
| secsrvmsgdef.h | 1 |
| smgdef.h | 1 |
| tbkdef.h | 1 |

## 3.2 missing_function — 113 programs touched, 74 solely blocked by this, 183 distinct symbols

Long tail — most missing functions block exactly one program. Top repeats (≥2 programs):

| Symbol | Programs unblocked |
|---|---|
| lib$sfree1_dd | 5 |
| lib$sys_asctim | 3 |
| lib$set_logical | 2 |
| lib$set_symbol | 2 |
| lib$sget1_dd | 2 |
| sys$asctoid | 2 |
| sys$cpu_transitionw | 2 |
| sys$create_user_profile | 2 |
| sys$getutc | 2 |
| sys$purgws | 2 |

The remaining ~173 distinct symbols each block exactly 1 program (full list of 183 symbols with per-symbol program names in the triage data referenced in §5; representative sample: `lib$add_times`, `lib$addx`, `lib$analyze_sdesc`, `lib$analyze_sdesc_64`, `lib$asn_wth_mbx`, `lib$ast_in_prog`, `lib$attach`, `lib$callg`, `lib$char`, `lib$convert_date_string`, `lib$crc`, `lib$crc_table`, `lib$create_dir`, `lib$currency`, `lib$cvt_dx_dx`, `lib$cvt_vectim`, `lib$delete_logical`, `lib$delete_symbol`, `lib$digit_sep`, `lib$disable_ctrl`, `lib$do_command`, `lib$ediv`, `lib$emul`, `lib$enable_ctrl`, `lib$expand_nodename`, `lib$ffc`, plus a long tail of `sys$*` system services and a handful of already-partially-declared symbols reported as `undefined reference` at link time (e.g. `LIB$AB_UPCASE`, `SYS$CLRAST` — declared somewhere but not exported from the shareable image, worth checking as export-table gaps rather than pure "not implemented" gaps).

**Implication:** this is the highest-value category by raw unblock count (74 programs solely blocked), but it's shallow-and-wide — no single fix moves the needle much. Best strategy is batching by source library (e.g., all remaining `lib$` RTL string/VM/queue routines together, all remaining `sys$` process/security system services together) rather than one-symbol-at-a-time beads.

## 3.3 missing_constant — 28 programs touched, 1 solely blocked, 34 distinct symbols

Dominated by two easy, high-value wins:

| Symbol | Programs unblocked |
|---|---|
| **TRUE** | 16 |
| **FALSE** | 14 |
| OSS$M_RELCTX | 2 |
| (30 others) | 1 each |

`TRUE`/`FALSE` are not defined anywhere in OVMX's public headers (`grep -rn "define TRUE\|define FALSE" src/*/include` returns nothing). These are standard DEC C RTL boolean macros the Eight-Cubed examples assume are ambient. Defining them (in whichever header is the appropriate VMS home — worth checking against public OpenVMS docs for where DEC C declares them, per the project's clean-room-RE rule) would touch up to 16+14=~24 programs at once (union, since some programs use both), though most also carry other blockers (only 1 program is blocked *solely* by TRUE/FALSE — the rest need additional fixes too).

Remaining 32 distinct symbols are VMS status codes (`SS$_*`), item-list codes (`SYI$_*`, `JPI$_*`, `XAB$*`), and mode bits (`FAB$M_*`, `CHP$M_*`, `OSS$M_*`) each blocking exactly 1 program — e.g. `SS$_ALIGN`, `SS$_DEVALRALLOC`, `SS$_DUPIDENT`, `SS$_EXITFORCED`, `SS$_LKWSETFUL`, `SS$_LOWPREC`, `SS$_NOCALLPRIV`, `SS$_NOIMPERSONATE`, `SS$_NOLOG`, `SS$_NOMOREPROC`, `SS$_NOOPER`, `SS$_NOSUCHCPU`, `SS$_USERDISABLED`, `SYI$_ACTIVE_CPU_BITMAP`, `SYI$_AVAIL_CPU_BITMAP`, `SYI$_MAX_CPUS`, `RMS$_ACC_RUJ`, `RMS$_JNLNOTAUTH`, `XAB$C_ITM`, `XAB$K_ITMLEN`, `XAB$K_SETMODE`, `LIB$M_CLI_CTRLT`, `CHP$M_ALTER`, `CHP$M_OBSERVE`, `FAB$M_ASY`, `FAB$M_RU`, `FAB$M_UFO`, `ISS$C_ID_NATURAL`, `SIGUSR1` (POSIX signal, likely just a missing `#include <signal.h>` transitively).

## 3.4 type_mismatch — 39 programs touched, 12 solely blocked, 37 distinct issues

Two flavors:

**(a) Signature mismatches** — OVMX's declared prototype doesn't match what real VMS (and the example code) expects. Top offenders:

| Function | Programs unblocked | Detail |
|---|---|---|
| sys$bintim | 5 | `starlet.h` declares `uint64_t *timadr`; callers pass `GENERIC_64 *` (a struct/union type). Was directly observed: `lib_add_times.c` passes `&time1` where `time1` is `GENERIC_64` |
| sys$getjpiw | 5 | pointer-type arg mismatch |
| lib$get_input | 2 | pointer-type arg mismatch |
| sys$dclast | 2 | pointer-type arg mismatch |
| sys$faol | 2 | pointer-type arg mismatch |
| ots$scopy_dxdx, ots$scopy_r_dx, sys$crelnm, sys$getmsg, sys$getsyiw, sys$putmsg, sys$qio, sys$setimr, sys$setprv | 1 each | pointer-type arg mismatch |

**(b) Wrong argument count** — OVMX's prototype has a different arity than the examples call with:

| Function | Programs | Note |
|---|---|---|
| sys$crembx | 4 (sys_cancel, sys_crembx, sys_delmbx, sys_sndopr) | genuinely 4 distinct programs |
| sys$assign | 2 (sys_assign, sys_getdviw) | genuinely 2 distinct programs |
| sys$close, sys$connect, sys$create, sys$disconnect, sys$erase, sys$flush, sys$get, sys$put, sys$rewind | 1 program each, but **all 9 are the SAME single program** (`sys_rms_seq.c`) — this is one systemic RMS fastio signature issue, not 9 independent gaps. Fixing OVMX's RMS `sys$get`/`sys$put`/`sys$close`/etc. fastio call signatures to match real VMS arity would unblock `sys_rms_seq.c` in one shot. |
| lib$delete_file, lib$find_file, lib$rename_file, sys$brkthruw, sys$chkpro, sys$delprc, sys$setpri, sys$suspnd | 1 each | independent |

**(c) Unknown types** — 5 programs hit `unknown type name '__int64'` (MS/DEC-C compiler builtin, not a GNU C keyword — needs a `typedef __int64` shim, likely alongside `int64_t`) plus 3 programs (`OPCDEF`, `PRVDEF`, `XABITMDEF`) hit an unknown struct-typedef name, probably the RMS-adjacent header gaps in §3.1 rather than a separate fix.

## 4. Recommended bead-filing order (by leverage, not raw count)

1. **TRUE/FALSE macros** — 2-line header fix, touches up to ~24 programs (highest ROI-per-line in the whole report).
2. **22 missing headers (§3.1)** — 32 programs, 100% single-blocker, mechanical (define the item-list/struct constants per public VMS docs).
3. **sys$bintim + sys$getjpiw signature fixes** — 10 programs between two functions, but touches an existing public API (`starlet.h`) — needs the design-change cascade per CLAUDE.md (API Compatibility Check → Test Coverage → Docs) since it's a `starlet.h` change.
4. **sys_rms_seq.c's 9-symbol RMS fastio arity mismatch** — 1 program but a systemic signature issue worth fixing as a unit; also `starlet.h`-adjacent, same cascade requirement.
5. **sys$crembx arity (4 programs) + sys$assign arity (2 programs)**.
6. **missing_function long tail (113 programs, 183 symbols)** — batch by source library (`lib$` RTL vs `sys$` system services) rather than per-symbol beads; 74 programs are solely blocked by this category so it's the single biggest total-programs lever, just diffuse.
7. **Arch-gated 18 (§3.0)** — policy decision, not implementation: exclude from the 80% milestone denominator, or invest one investigation into the G_FLOAT `#error` guard (up to 11 programs) if the operator wants them counted.

## 5. Raw data

Full per-symbol → program-name mappings (`triage_clean.json`, all categories, no top-N truncation) and the raw harness JSON (`corpus_result.json`) were generated during this run in the scratch directory and are not checked into the repo (ephemeral working data, per the containerized/no-repo-artifacts build invariant). Re-derive by running the fixed `tests/conformance/run_corpus.sh` inside the builder image — see §0/§1 for the exact command shape. The dontguess entry for this run (see item audit trail) carries the full triage JSON for reuse without re-running the harness.
