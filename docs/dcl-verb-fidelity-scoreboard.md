# DCL Verb Fidelity Scoreboard (Phase 0, vms-6f4)

> Re-derived 2026-08-11 by reading every verb handler in `src/vmsdcl/dcl_cmd_*.c`,
> `dcl_backup.c`, and `dcl_library.c` against the 54-verb table in
> `src/vmsdcl/dcl_builtin.c`. This is a **raw observation**, not a frozen
> conclusion (CLAUDE.md continuation rule) — re-derive it again before trusting
> it on a later date, the same way this pass re-derived
> `docs/design-dcl-fidelity.md` §1's `~34/~13/~5/~2` estimate rather than
> copying it.
>
> **Why the count moved from `docs/design-dcl-fidelity.md`'s `~34 REAL · ~13
> PARTIAL · ~5 FACADE · ~2 STUB` to `48 · 4 · 1 · 1` below:** that document's
> own audit predates several fixes already in `main` before this session
> started — MOUNT (`vms-651`, real `mount(2)` through a setuid helper) and
> PRODUCT (now shells out to a real `PRODUCT.EXE` instead of the fat builtin
> the design doc describes) were both already REAL, not FACADE, when this
> pass read them. Three more were fixed **in this session** (vms-6f4 Phase 0,
> the canaries: SET TERMINAL's qualifier validation, ASSIGN's
> `/SYSTEM /JOB /GROUP /TABLE` refusal, SET AUDIT's honest refusal) and are
> counted in their post-fix state below. The design doc's `~` tildes were an
> estimate by its own admission; this is the same board, counted for real.

**Bucket definitions (INV-DCL, `docs/design-dcl-fidelity.md` §3):**

| Bucket | Meaning |
|---|---|
| **REAL** | Implements genuine VMS semantics for what it claims. May have honestly-disclosed gaps, but does the real thing and never claims success for something it didn't do. |
| **PARTIAL** | Real, substantial work with a meaningful but *disclosed* gap — incomplete, not dishonest. |
| **FACADE** | Prints a plausible VMS success message and/or returns a success status while doing little or nothing that persists or that any other process could observe. The banned class INV-DCL exists to kill. |
| **STUB** | Near-total placeholder — typically an immediate, honest refusal with no logic behind it. |

**Totals: 48 REAL · 4 PARTIAL · 1 FACADE · 1 STUB (54 verbs).**

## REAL (48)

| Verb | Evidence |
|---|---|
| ACCOUNTING | `cmd_accounting()` reads the real per-user last-login record (`ovmx_accounting_get_lastlogin()`); no fabricated fallback. |
| ANALYZE | Forks/execs real `ANALYZE.EXE`; honest `%ANALYZE-F-NOIMG` if missing. |
| APPEND | Real `fopen`/append `fwrite` I/O. |
| BACKUP | Genuine archive/restore/list I/O (OVMX-specific saveset format, not VMS-binary-compatible, but real). |
| CLOSE | Closes a real per-process file channel OPEN created. |
| CONTINUE | Real `SIGCONT`/`waitpid` on the Ctrl-Y-interrupted child. |
| CONVERT | Real line-by-line copy/record count; `/FDL` honestly disclosed as accepted-but-ignored, not silently dropped. |
| COPY | Real `fopen`/`fread`/`fwrite` file copy. |
| CREATE | Real `mkdir`/`fopen` with ODS-2 name validation. |
| DEASSIGN | Calls the real `lnm_delete()` against the logical-name manager. |
| DEFINE | Calls real `lnm_create_multi()`; handles multi-valued search lists correctly (vms-420). |
| DELETE | Real `unlink()`, real queue-entry delete (`vmsq_delete_entry()`), real symbol delete. |
| DIFFERENCES | Real line-by-line file diff. |
| DIRECTORY | Real `opendir`/`stat`-based listing. |
| DISMOUNT | Mirrors MOUNT: real privilege check, real `/proc/mounts` check, real `umount(2)` via helper. |
| DUMP | Real `fread` hex/ASCII dump. |
| EDIT | Calls `edt_run()`, a genuine line-mode EDT editor (`dcl_editor.c`). |
| EXIT | Sets real session exit-request/status flags consumed by the executor. |
| INSTALL | Forks/execs real `INSTALL.EXE`, which writes the actual KFE known-image database (vms-913.5). |
| LIBRARY | Genuine library file I/O (`dcl_library.c`, own binary format, functionally real). |
| LINK | Forks/execs `cc` to genuinely produce a linked executable. |
| LOGOUT | Real logout line, real `sys$sndopr` OPC record, sets real exit flags; no fabricated username. |
| MAIL | Forks/execs real `MAIL.EXE`. |
| MONITOR | Forks/execs real `MONITOR.EXE`. |
| MOUNT | Real PRV$M_MOUNT check via the executive, real device resolution, real `mount(2)` via setuid helper, real LNM entry (`vms-651`). |
| OPEN | Real `fopen()` into a per-process channel table. |
| PIPE | Real `pipe(2)`/`fork`/`exec` pipeline of DCL subprocesses. |
| PRINT | Real submission into the queue manager (`vmsq_submit`) to SYS$PRINT. |
| PRODUCT | Forks/execs real `PRODUCT.EXE` for INSTALL/SHOW; honest `%PCSI-NOTIMPL` otherwise. |
| PURGE | Real version-purge via `vmsfs_purge_versions()`. |
| RECALL | Real readline history integration; honest refusal if readline absent. |
| READ | Real `fgets()` from an open channel or SYS$INPUT. |
| RENAME | Real `rename(2)`. |
| REPLY | Real `sys$sndopr()` OPC message; no fabricated operator name. |
| REQUEST | Real `sys$sndopr()` OPC request. |
| RUN | Real image activation (`dcl_activate_image()`/`run_detached()`) with its own honour/refuse qualifier layer. |
| SEARCH | Real line-by-line string search over a real file. |
| **SET** | Dispatcher — DEFAULT/PROMPT/VERIFY/TERMINAL/PROTECTION/PROCESS/FILE/UIC/WORKING_SET/TIME/ENTRY/QUEUE do genuine executive-backed work. **Named FACADE subcommands still open: ACCOUNTING, PASSWORD, VOLUME** — see below. |
| **SHOW** | Dispatcher — PROCESS/SYSTEM/DEVICE/MEMORY/LOGICAL/STATUS read real executive state, deliberately blank rather than fabricate on failure. **Named FACADE subcommand still open: LICENSE** — see below. |
| SORT | Real line-read/`qsort`/write. |
| SPAWN | Real `fork`/`execl` subprocess with real `/NOWAIT`, `/OUTPUT` redirection. |
| SUBMIT | Real submission into the queue manager to SYS$BATCH. |
| SYSGEN | Forks/execs real `SYSGEN.EXE`. |
| SYSMAN | Forks/execs real `SYSMAN.EXE`. |
| TCPIP | Real `ioctl`s for interface/route config, real `/etc/hosts` writes, honest `%TCPIP-W-PRIVREQ` when unprivileged. |
| TYPE | Real `fopen`/`fgets` display with real `/PAGE` paging. |
| WAIT | Real `sleep()` for the parsed delta-time. |
| WRITE | Real `fprintf` to SYS$OUTPUT/SYS$ERROR or an open channel. |

## PARTIAL (4)

| Verb | Evidence |
|---|---|
| **ASSIGN** | Writes into the DCL symbol table (`dcl_sym_set`), not the LNM manager DEFINE uses — real work, wrong table. **Fixed this session (vms-6f4 Phase 0):** now honestly refuses `/SYSTEM /JOB /GROUP /TABLE` with `SS$_UNSUPPORTED` instead of silently discarding them (was FACADE before). Wiring the real LNM path is Phase 2. |
| ATTACH | Real `kill(SIGCONT)`/`waitpid()` process control, but only for the Ctrl-Y-interrupted process or a raw `/ID=pid`, not general job-tree terminal reassignment. |
| HELP | Really lists all 54 verbs with interactive "Topic?" recursion, but flat printf text (no HLB), with hardcoded sub-help for only SHOW/SET/DIRECTORY. |
| INQUIRE | Genuinely prompts/reads/sets a symbol, but `/NOPUNCTUATION` maps to "don't upcase input" instead of its real meaning (suppress trailing prompt punctuation) — right name, wrong semantics. |

## FACADE (1 top-level; 4 named sub-facades under SET/SHOW)

| Verb | Evidence |
|---|---|
| **STOP** | `cmd_stop()` ignores its target parameter/qualifiers entirely and unconditionally tears down the *current* session, returning `SS$_NORMAL` as if the named target were stopped. |

Named sub-facades (do not change the SET/SHOW top-level bucket, called out per
the same convention `docs/design-dcl-fidelity.md` §1 used):

| Subcommand | Evidence |
|---|---|
| SET AUDIT | **Fixed this session** (vms-6f4 Phase 0): now honestly refuses (`SS$_UNSUPPORTED`) instead of toggling `ctx->audit_enabled`, a per-process bool nothing else could observe. |
| SET ACCOUNTING | Still open — toggles `ctx->accounting_enabled` (same dead-bool shape as the old SET AUDIT) and prints `%SET-I-INTSET` as if it succeeded. |
| SET PASSWORD | Still open — `cmd_set_password()` prints `%SET-I-PASSWORD, password change not fully implemented` (admits it) but returns `SS$_NORMAL` (fake success) and never touches SYSUAF. |
| SET VOLUME | Still open — prints `%SET-I-NOTIMPL` but returns `SS$_NORMAL` for an operation that does nothing. |
| SHOW LICENSE | Still open — `cmd_show_license()` prints two invented, unconditional LMF-style rows (fixed 0/0/100 Avail/Actv) with no disclosure at all — the least-honest of the four. |

## STUB (1)

| Verb | Evidence |
|---|---|
| PHONE | `cmd_phone()` immediately returns an honest `%PHONE-I-NOTAVAIL, PHONE facility is not available`; no logic beyond the refusal. This is an *honest* stub — the bucket describes implementation depth, not honesty (an honest one-line refusal is not the problem INV-DCL targets). |

## What Phase 0 fixed vs. what remains

**Fixed in this pass (vms-6f4 Phase 0), each with a CI-gated regression test
(`tests/dcl/test_ivqual_gate_phase0.sh`, `tests/dcl/test_facade_gate_phase0.sh`):**
- SET TERMINAL rejects an unknown qualifier with `%DCL-W-IVQUAL` instead of
  silently accepting it (the canary for the qualifier-grammar hole described
  in `docs/design-dcl-fidelity.md` §2 — the hole itself, `struct dcl_verb`
  carrying no per-verb qualifier table, is still open architecture-wide;
  Phase 1 closes it for all 54 verbs).
- ASSIGN honestly refuses `/SYSTEM /JOB /GROUP /TABLE` instead of silently
  discarding them.
- SET AUDIT honestly refuses instead of toggling a dead per-process bool.

**Re-derived as already fixed before this session (not this pass's work, but
worth recording so the board stays accurate):**
- MOUNT (`vms-651`) and PRODUCT (shells to real `PRODUCT.EXE`) — both listed
  as FACADE in `docs/design-dcl-fidelity.md` §1, both REAL as read today.

**Still open (out of Phase 0's bounded scope — see Phase 1/2 in
`docs/design-dcl-fidelity.md` §5):**
- The qualifier-grammar hole itself, for the other 53 verbs (Phase 1,
  `vms-097`).
- STOP's ignored target, SET ACCOUNTING/PASSWORD/VOLUME, SHOW LICENSE (Phase
  2's fake-success sweep).
- HELP as a real hierarchical library, terminal-characteristic presentation
  fidelity (Phases 3-4).
