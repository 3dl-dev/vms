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

**Totals (re-derived after vms-263 + vms-1a8 + vms-309): 50 REAL · 4 PARTIAL · 0 top-level FACADE · 1 STUB (54 verbs).** ASSIGN (vms-263) moved PARTIAL→REAL and STOP (vms-1a8) moved FACADE→REAL; see their rows under REAL below.
Of the 4 named SET/SHOW sub-facades (ACCOUNTING/PASSWORD/VOLUME/LICENSE), PASSWORD (vms-e9e) and ACCOUNTING (vms-17d) moved to REAL, and **SET VOLUME (vms-309) moved FACADE→PARTIAL** (real mount-state verification + real qualifier grammar, honest per-qualifier refusal including /LABEL — no characteristic actually persists; see its section below) — SHOW LICENSE is still open, see the FACADE section below.

## REAL (49)

| Verb | Evidence |
|---|---|
| ACCOUNTING | `cmd_accounting()` reads the real per-user last-login record (`ovmx_accounting_get_lastlogin()`); no fabricated fallback. |
| ANALYZE | Forks/execs real `ANALYZE.EXE`; honest `%ANALYZE-F-NOIMG` if missing. |
| APPEND | Real `fopen`/append `fwrite` I/O. |
| **ASSIGN** | **vms-263 (Phase 2):** now calls the real `lnm_create()` against the logical-name manager for `/PROCESS` (the default) and `/SYSTEM /GROUP /JOB` (routed to the same executive-resident LNM$SYSTEM/GROUP/JOB tables `DEFINE` already uses) -- no longer writes into the DCL SYMBOL table. `/TABLE=name` (an arbitrary caller-named table) stays an honest `%DCL-W-NOTIMPL`/`SS$_UNSUPPORTED` refusal -- no table-by-name registry exists anywhere in `src/vmslnm`, for ASSIGN or DEFINE. Veracity gate: `tests/dcl/test_assign_real_lnm_veracity.sh` (fails on the old symbol-facade). |
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
| **SET** | Dispatcher — DEFAULT/PROMPT/VERIFY/TERMINAL/PROTECTION/PROCESS/FILE/UIC/WORKING_SET/TIME/ENTRY/QUEUE do genuine executive-backed work. **vms-e9e:** PASSWORD moved REAL. **vms-17d:** ACCOUNTING moved REAL. **vms-309:** VOLUME moved FACADE→PARTIAL (see PARTIAL bucket above and its own section below). No named top-level-bucket-affecting FACADE subcommand remains under SET. |
| **SHOW** | Dispatcher — PROCESS/SYSTEM/DEVICE/MEMORY/LOGICAL/STATUS read real executive state, deliberately blank rather than fabricate on failure. **Named FACADE subcommand still open: LICENSE** — see below. |
| SORT | Real line-read/`qsort`/write. |
| SPAWN | Real `fork`/`execl` subprocess with real `/NOWAIT`, `/OUTPUT` redirection. |
| **STOP** | **Fixed (vms-1a8):** the process-target forms (a process-name parameter, `/IDENTIFICATION=pid`) now resolve the target through the real executive process table and terminate it via `sys$delprc`, which enforces the DCL Dictionary's GROUP/WORLD privilege rule (`SS$_NOPRIV` for a same-group target without GROUP, refused with the target left alive) and returns the authentic `SS$_NONEXPR` for a nonexistent target. Bare `STOP` (no target) is unchanged: abnormal termination of the current image/command, unstacking to DCL. `STOP/QUEUE`, `/CPU`, `/NETWORK` are separate Dictionary entries, never implemented, and now draw the authentic `%DCL-W-IVQUAL` instead of silent acceptance. Ground-source: `tests/dcl/test_stop_facade_gate.sh` (host, proves no more `$STATUS=1` for an unreachable target) + `tests/qemu/test_syssvc_delprc.c` (real `vms.ko`: a named/`PID`-targeted process is actually created by one process and actually terminated by another, confirmed gone from both Linux and the executive's table; the privilege refusal leaves the target alive; nonexistent targets return `SS$_NONEXPR`). |
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
| ATTACH | Real `kill(SIGCONT)`/`waitpid()` process control, but only for the Ctrl-Y-interrupted process or a raw `/ID=pid`, not general job-tree terminal reassignment. |
| HELP | Really lists all 54 verbs with interactive "Topic?" recursion, but flat printf text (no HLB), with hardcoded sub-help for only SHOW/SET/DIRECTORY. |
| INQUIRE | Genuinely prompts/reads/sets a symbol, but `/NOPUNCTUATION` maps to "don't upcase input" instead of its real meaning (suppress trailing prompt punctuation) — right name, wrong semantics. |
| **SET** (VOLUME subcommand) | **vms-309:** real "is this a mounted volume" check (`/proc/mounts`, shared with MOUNT/DISMOUNT) and a real, structural qualifier grammar (all 23 Dictionary qualifiers, `%DCL-W-IVQUAL`/`IVKEYW` now reachable) — no characteristic, including `/LABEL`, actually persists (vmsfs has no write-back path for a live-mounted volume). Disclosed gap, not a lie: every qualifier draws a specific, honest `SS$_UNSUPPORTED` refusal instead of the old blanket `SS$_NORMAL`. See "vms-309 — SET VOLUME" below. |

## FACADE (0 top-level; 1 open named sub-facade under SHOW, 4 fixed)

No top-level verb is FACADE as of vms-1a8 (STOP moved to REAL above).

Named sub-facades (do not change the SET/SHOW top-level bucket, called out per
the same convention `docs/design-dcl-fidelity.md` §1 used):

| Subcommand | Evidence |
|---|---|
| SET AUDIT | **Fixed** (vms-6f4 Phase 0): now honestly refuses (`SS$_UNSUPPORTED`) instead of toggling `ctx->audit_enabled`, a per-process bool nothing else could observe. |
| SET ACCOUNTING | **Fixed (vms-17d): moved to REAL** — see §"vms-17d — SET ACCOUNTING moves FACADE to REAL" below. Was: `cmd_set_accounting()` toggled `ctx->accounting_enabled` (same dead-bool shape as the old SET AUDIT) and printed `%SET-I-INTSET` as if it controlled the real accounting writer, `ovmx_accounting_record_login()`, which recorded unconditionally regardless. |
| SET PASSWORD | **Fixed (vms-e9e): moved to REAL** — see §"vms-e9e — SET PASSWORD moves FACADE to REAL" below. Was: `cmd_set_password()` printed `%SET-I-PASSWORD, password change not fully implemented` (admits it) but returned `SS$_NORMAL` (fake success) and never touched SYSUAF. |
| SET VOLUME | **Fixed (vms-309): moved to PARTIAL** (a subcommand move does not change SET's own top-level bucket) — see §"vms-309 — SET VOLUME" below. Was: `cmd_set_volume()` printed `%SET-I-NOTIMPL` but returned `SS$_NORMAL` unconditionally — mounted device or not, real qualifier or garbage, every invocation. |
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
- SHOW LICENSE (Phase 2's fake-success sweep). SET PASSWORD is DONE
  (vms-e9e), SET ACCOUNTING is DONE (vms-17d), and SET VOLUME is DONE
  (vms-309, honest-errors scope — see its section below) — see their
  sections below. STOP's ignored target is DONE (vms-1a8, see the STOP row
  under REAL above).
- **Follow-up filed by vms-309, not yet an rd item (file from repo root,
  not a worktree — CLAUDE.md/MEMORY):** a real `SET VOLUME/LABEL` write-back
  needs a new `vmsfs.ko` ioctl (ioctl name TBD, e.g. `VMSFS_IOC_SETLABEL`)
  that updates `hb_volname` on a volume that is CURRENTLY mounted —
  recomputes `hb_checksum`, writes through the mount's own buffer head
  (`mark_buffer_dirty`, `src/kernel/vmsfs/vmsfs_blkdev.c`), and refreshes
  the cached `sbi->home` (`src/kernel/vmsfs/vmsfs_super.c`). `vmsfs.ko`
  currently declares no ioctl at all. Kernel module interface work —
  CLAUDE.md's Design Change Cascade applies (API compatibility check →
  test coverage check → doc update).
- HELP as a real hierarchical library, terminal-characteristic presentation
  fidelity (Phases 3-4).

## Phase 1 (vms-097) — the qualifier-grammar hole is now STRUCTURAL

Phase 1 landed the CDU/CLD-style machinery the Phase 0 canary stood in for
(`docs/design-dcl-fidelity.md` §4). `struct dcl_verb` now carries a per-verb
qualifier table (`struct dcl_qual_def *quals`, `src/vmsdcl/include/dcl/cdu.h`);
`dcl_validate_qualifiers()` (`src/vmsdcl/dcl_parser.c`) validates the parsed
line against it and dispatch (`src/vmsdcl/dcl_exec.c`) rejects a bad qualifier
with the authentic `%DCL-W-IVQUAL` / `%DCL-W-IVKEYW` **before the handler
runs**. Gate: `tests/dcl/test_ivqual_gate_structural.sh` (revert the `.quals`
wiring or the dispatch call and it goes red).

**14 verbs retrofit with qualifier tables this pass** (the ones whose handlers
read a bounded, enumerable qualifier set): TYPE, COPY, DELETE, RENAME, CREATE,
SEARCH, PURGE, DIRECTORY, PRINT, SUBMIT, SORT, DUMP, plus APPEND and
DIFFERENCES (which read no qualifiers, so their tables are empty and any
qualifier is now honestly rejected).

**Bucket effect.** None of the 14 changed fidelity *bucket* — all were already
REAL on core function; the qualifier hole was tracked architecturally (a
per-verb dimension the board did not down-grade for), not as a per-verb facade.
What changed is that `%DCL-W-IVQUAL`/`IVKEYW` went from **structurally
unreachable** (only the hand-rolled SET TERMINAL canary and RUN could reject a
qualifier) to **structurally reachable for these 14 verbs**. Each table lists
ONLY the qualifiers its handler actually implements (INV-DCL §3): a qualifier
real VMS accepts but OVMX does not yet implement is now honestly rejected with
IVQUAL (an over-restriction, not a lie) rather than silently swallowed.
DIRECTORY/DATE is declared a keyword qualifier honouring only `MODIFIED` (the
one timestamp OVMX surfaces from `stat(2)`); other DCL Dictionary keywords
(CREATED/EXPIRED/BACKUP/ALL) draw `%DCL-W-IVKEYW` rather than a faked mtime.

**Deferred to the Phase 1 follow-up** (verb-table population, `vms-097`
follow-on): the remaining ~38 verbs still have `quals == NULL` (legacy
accept-all) — notably the SET and SHOW **dispatchers**, whose qualifier space
is subcommand-dependent and needs a nested table design, and the
foreign/utility verbs (ANALYZE, BACKUP, MAIL, MONITOR, SYSGEN, SYSMAN, TCPIP,
LINK, INSTALL, PRODUCT, CONVERT, RUN, SPAWN, OPEN/READ/WRITE/CLOSE, the
logical-name verbs, etc.). VALREQ/NOVAL value-required enforcement is also
deferred (no grounded `SS$_` status constant yet); Phase 1 enforces the two
authentic errors with grounded codes: IVQUAL (`SS$_IVQUAL` 2288) and IVKEYW
(`SS$_IVKEYW` 2292).

## Engine A rollout tranche 2 (vms-7543) — 15/54 → 33/54 carry qualifier tables

Continues the Phase 1 keystone (`docs/design-vms-parity-map.md` §3). This pass
adds CDU/CLD qualifier tables to the **in-process, self-parsing** verbs Phase 1
left as legacy accept-all, so `%DCL-W-IVQUAL`/`IVKEYW` is now structural for
them too, and deepens per-verb coverage on the highest-value file/queue verbs.

**18 verbs newly retrofit with tables** (were `quals == NULL`):
- Populated (list exactly the qualifiers the handler reads):
  ASSIGN `{PROCESS,SYSTEM,GROUP,JOB,TABLE=}`, DEFINE `{PROCESS,SYSTEM,GROUP,JOB}`,
  DEASSIGN `{PROCESS,SYSTEM,GROUP,JOB,ALL}`, OPEN `{READ,WRITE,APPEND}`,
  SPAWN `{NOWAIT,OUTPUT=}`, INQUIRE `{NOPUNCTUATION}`, ATTACH `{IDENTIFICATION=}`,
  CONVERT `{FDL=}`, REPLY `{ENABLE=,DISABLE,TO=}`, RECALL `{ALL}`.
- Empty (read no qualifiers → any qualifier now draws IVQUAL):
  CLOSE, CONTINUE, EXIT, HELP, LOGOUT, PHONE, PIPE, WAIT.

Two of the populated tables also **repair latent bugs**: the parser splits
`/NOWAIT`→name=`WAIT`,negated and `/NOPUNCTUATION`→name=`PUNCTUATION`,negated,
so the handlers' literal `dcl_has_qualifier(cmd,"NOWAIT"/"NOPUNCTUATION")` reads
never matched (the qualifier silently did nothing). Declaring the literal name
in the table drives `dcl_validate_qualifiers()`'s NO-undo path, which
reconstructs the name so the read resolves. ATTACH's handler was moved from the
`/ID` abbreviation to the canonical `/IDENTIFICATION` (the validator
canonicalises `/ID` to it).

**Per-verb coverage deepened** (each qualifier does real work or is honestly
rejected — INV-DCL §3):
- **DIRECTORY 10 → 13**: `+/PROTECTION` (renders the VMS protection column),
  `+/VERSIONS=n` (limits versions listed per name group), `+/EXCLUDE=(spec)`
  (omits files matching, via the same wildcard engine as the positional pattern).
- **COPY 1 → 2**: `+/CONFIRM` (real Y/N prompt read from SYS$INPUT, mirrors
  DELETE/CONFIRM). `/CONTIGUOUS` and the other ~18 draw honest IVQUAL.
- **PRINT 1 → 3 / SUBMIT 1 → 3**: `+/NAME=job-name` (overrides the real
  `vms_queue_entry.job_name`), `+/HOLD` (submits then `vmsq_hold_entry()` → the
  entry is really HOLDING, visible in SHOW QUEUE). `/COPIES` etc. (no backing
  queue-entry field) draw honest IVQUAL; the stale `/COPIES` mention in
  cmd_print's header comment was corrected.

All error text/format is VMS-authentic (`%DCL-W-IVQUAL`/`IVKEYW`, DCL Dictionary
wording); no `OVMX$_` code is emitted on any of these standard-operation paths
(parity principle: indistinguishable in operation).

**Bucket effect: none** — every one of these verbs was already REAL on core
function; what changed is qualifier reachability and depth, the same
architectural (not per-verb-bucket) dimension Phase 1 tracked.

**Deferred, with the reason (kept `quals == NULL` on purpose)** — filed as
Engine A follow-ups under `vms-b9a`:
- **External-image delegators** (the child `SYS$SYSTEM:*.EXE` validates its own
  qualifiers authentically; a DCL-side table would wrongly reject valid ones):
  ANALYZE, INSTALL, LINK, MAIL, MONITOR, PRODUCT, SYSGEN, SYSMAN.
- **SET / SHOW / TCPIP umbrellas**: qualifier space is subcommand-dependent
  (`param[0]`); needs nested per-subcommand tables (Tier-2), not a flat one.
- **RUN**: self-validates through its own `run_process_qualifiers[]` layer.
- **MOUNT / DISMOUNT** and **BACKUP / LIBRARY**: full utility qualifier
  grammars, each a sized retrofit like SET VOLUME's 23-qualifier pass.
- **READ / WRITE / REQUEST / EDIT / ACCOUNTING**: carry script- or
  interactive-critical qualifiers (`/END_OF_FILE`, `/ERROR`, `/SYMBOL`, `/TO`,
  `/TPU`, …) that want real coverage, not just restriction.

**Gates** (fail on the pre-rollout state): `tests/dcl/test_ivqual_rollout_tranche2.sh`
(IVQUAL structural on the new verbs + positive controls),
`tests/dcl/test_directory_coverage.sh`, `tests/dcl/test_copy_confirm.sh`,
`tests/dcl/test_print_submit_coverage.sh`.

## Phase 2, TOP LIE #1 (vms-263) — ASSIGN moves PARTIAL to REAL

`cmd_assign()` (`src/vmsdcl/dcl_cmd_io.c`) used to write its equivalence
string into the DCL SYMBOL table via `dcl_sym_set()` for every invocation,
including the `/PROCESS` default — real work, wrong subsystem: an
`F$TRNLNM()`/`SHOW LOGICAL` lookup never saw what `ASSIGN` had just
"assigned." It now calls the real `lnm_create()` against the logical-name
manager, mirroring `cmd_define()`'s existing LNM path in the same file:

- `/PROCESS` (default) and `/SYSTEM /GROUP /JOB` all route to the four
  well-known tables — `LNM$SYSTEM`/`GROUP`/`JOB` are executive-resident
  (`lnm_create()` → `vms_kif` → `/dev/vms`, `src/vmslnm/lnm_client.c`) and
  already load-bearing for `DEFINE`, so ASSIGN now wires to them instead of
  refusing them (no per-process fallback, INV-6 — an unreachable executive
  fails honestly with `SS$_NOSUCHDEV`/`LNMFAIL`, never a fake success).
- `/TABLE=name` (an arbitrary caller-named table) is **still** an honest
  `%DCL-W-NOTIMPL`/`SS$_UNSUPPORTED` refusal — neither ASSIGN nor DEFINE has
  a generic table-by-name registry anywhere in `src/vmslnm` to resolve it
  against.
- `DEASSIGN` (`cmd_deassign()`) already called the real `lnm_delete()`
  before this pass (it was independently REAL, not part of the ASSIGN
  facade) and needed no change; it now removes exactly what the fixed
  `ASSIGN` creates.

Clean-room (Rule 8): ASSIGN's parameter order (equivalence-name THEN
logical-name, the opposite of DEFINE's logical-name THEN
equivalence-string), its default table (`LNM$PROCESS`), and its scope
qualifiers (`/PROCESS /JOB /GROUP /SYSTEM /TABLE`) are the public OpenVMS
DCL Dictionary's ASSIGN entry ("This command performs a subset of the
function of the DEFINE command").

Veracity gate (fails on the old symbol-facade, passes on the fix):
`tests/dcl/test_assign_real_lnm_veracity.sh` — `ASSIGN BAR FOO` then
`F$TRNLNM("FOO")` and `SHOW LOGICAL FOO` both show the real translation
`BAR`, `SHOW SYMBOL FOO` finds nothing (`%DCL-W-NOLCL`, proving the DCL
symbol table was never touched), and `DEASSIGN FOO` removes the logical
name (`SHOW LOGICAL FOO` afterward reports `%DCL-W-NOLOG`). The Phase 0
facade canary (`tests/dcl/test_facade_gate_phase0.sh`) moved from
`ASSIGN .../SYSTEM` (now wired, no longer a facade) to `ASSIGN .../TABLE`
(still honestly refused) so it keeps testing a real refusal rather than a
now-fixed path.

## vms-e9e — SET PASSWORD moves FACADE to REAL

`cmd_set_password()` (`src/vmsdcl/dcl_cmd_set.c`) used to print
`%SET-I-PASSWORD, password change not fully implemented` and
`Full SYSUAF.DAT rewrite is planned for a future release.` while returning
`SS$_NORMAL` — an `-I-` (success-toned) lie for a total no-op, worse than an
honest error under INV-DCL because it looks like it worked.

It now implements the real public OpenVMS DCL Dictionary "SET PASSWORD"
self-service exchange (Rule 8 citation:
<https://www.digiater.nl/openvms/doc/ia64-v8.3/opsys/vmsos83/9996/9996pro_205.html>,
<https://wiki.vmssoftware.com/SET_PASSWORD>): `Old password:` / `New
password:` / `Verification:`, no terminal echo, verified against the real
SYSUAF hash (`sysuaf_authenticate()`), and on match writes a real new hash
back through a new shared library function, `sysuaf_write_record()`
(`src/libvms/rtl/sysuaf.c`) — a second CALLER of the existing ONE writer
(`sysuaf_format_record()`, vms-9b7/INV-1), not a second SYSUAF format.
Mismatch, blank, and under-length (Dictionary default `PWDMINIMUM` of 6)
all honestly refuse without writing.

**Deviation from the item's initial framing, flagged per CLAUDE.md Rule 5:**
the item description anticipated a `/USER=` qualifier or a name parameter
for changing another account's password under SYSPRV. The public DCL
Dictionary entry fetched for this fix shows SET PASSWORD takes **no
parameters** and has exactly three qualifiers — `/GENERATE`, `/SECONDARY`,
`/SYSTEM` — none of which name another account. There is no DCL-level way
to change someone else's password; that is AUTHORIZE's job
(`tools/vms_authorize.c`'s `cmd_modify()`, already SYSPRV-gated). Ground
truth (source-of-truth hierarchy #1, CLAUDE.md) overrides the item's
initial assumption: implemented per the Dictionary, not per the
anticipated-but-unverified `/USER=` shape. `/SECONDARY` and `/SYSTEM`
(gated on SECURITY privilege, matching the Dictionary) both honestly refuse
— OVMX has no secondary-password field and no per-node system-password
subsystem.

Veracity: `tests/libvms/test_sysuaf_write_veracity.c` drives the exact
mechanism `cmd_set_password()` calls (`sysuaf_lookup` →
`sysuaf_authenticate` → `sysuaf_write_record`) against a real
`SYS$SYSTEM:SYSUAF.DAT` resolved through the same path translation
AUTHORIZE and LOGIN use, in an isolated temp DKA0: root — changes
`TESTUSER`'s password and proves the NEW password now authenticates, the
OLD one no longer does, and a bystander row is untouched. This is the
"host test driving `sysuaf_authenticate` before/after" the item allowed as
an alternative to a QEMU-login proof, since `sysuaf_write_record()` needs
no `/dev/vms` (plain file I/O plus the same host-tooling LNM fallback
AUTHORIZE/LOGIN already rely on, `src/vmslnm/lnm_defaults.c`).
`tests/dcl/test_set_password_veracity.sh` is the DCL-surface companion —
proves the facade text/status is gone (`SET PASSWORD/BOGUS` →
`%DCL-W-IVQUAL`, `/SECONDARY` and `/GENERATE` → honest `%DCL-W-NOTIMPL`, an
extra parameter → `%DCL-E-MAXPARM`) without needing a real identity, which
bare host ctest does not have (Rule 9/INV-6: no per-process identity
fake).

## vms-17d — SET ACCOUNTING moves FACADE to REAL

`cmd_set_accounting()` (`src/vmsdcl/dcl_cmd_set.c`) used to set
`ctx->accounting_enabled` — a **per-DCL-context** bool, freshly zero at the
start of every `vmsdcl` process and invisible to every other process — and
print `%SET-I-INTSET, accounting enabled/disabled` as though that had taken
effect. `SHOW ACCOUNTING` read the same per-process bool. Meanwhile the real
accounting writer, `ovmx_accounting_record_login()`
(`src/libvms/rtl/ovmx_accounting.c`, called from login/SSH), recorded
**unconditionally** — SET ACCOUNTING controlled nothing. INV-DCL's banned
fake-success class, same dead-bool shape as the old SET AUDIT facade
(vms-6f4 Phase 0) but for a command OpenVMS treats as real, no-privilege-gap
work rather than an unimplemented subsystem.

It now flips a **real, persisted, system-wide** flag:
`ovmx_accounting_set_enabled()`/`ovmx_accounting_is_enabled()`
(`src/libvms/rtl/ovmx_accounting.c`), backed by a one-line `"0"`/`"1"` state
file at `VMS_ACCOUNTING_STATE_PATH` (`SYS$MANAGER:ACCOUNTNG.ENB`,
`ovmx_layout.h`). `ovmx_accounting_record_login()` checks it before writing
a lastlogin record; `cmd_show_accounting()` (`src/vmsdcl/dcl_cmd_show.c`)
reads the identical flag — the dead `ctx->accounting_enabled` field has been
removed from `struct dcl_context` entirely, not just stopped-being-read.
Default with no state file yet is **enabled**, matching real OpenVMS (where
accounting runs from system startup via `ACC$START` unless a manager
explicitly disables it) — not the old per-process bool's implicit
"disabled," which was never a considered default, only an unwritten zero.

Qualifiers per the public OpenVMS DCL Dictionary SET ACCOUNTING entry (Rule
8 citation: <https://wiki.vmssoftware.com/SET_ACCOUNTING>, fetched for this
fix): `/ENABLE[=(class[,...])]` and `/DISABLE[=(class[,...])]`, each keyword
naming a resource class (`IMAGE`, `LOGIN_FAILURE`, `MESSAGE`, `PRINT`,
`PROCESS`) to start/stop tracking independently. OVMX has no per-class
accounting — only the single system-wide login record `ovmx_accounting.c`
already writes — so bare `/ENABLE` and `/DISABLE` flip that one real flag,
and a class list on either qualifier draws the authentic `%SET-W-NOTIMPL` /
`SS$_UNSUPPORTED` refusal instead of silently accepting granularity nothing
honours. `/ENABLE` and `/DISABLE` given together (both bare) resolves
`/ENABLE`-wins, the same precedence the pre-existing facade's `if`/`else if`
already had — OVMX has no basis to invent a "conflict" VMS message class
that isn't grounded in the Dictionary text.

Veracity: `tests/libvms/test_accounting_veracity.c` drives the exact
mechanism `cmd_set_accounting()` calls (`ovmx_accounting_set_enabled()`) and
the exact mechanism every login path calls
(`ovmx_accounting_record_login()`) against a real `SYS$MANAGER:` resolved
through the same path translation AUTHORIZE/LOGIN/DCL use, in an isolated
temp DKA0: root — proves a login while disabled writes no lastlogin record
and the identical call while enabled does, both confirmed by a fresh read
(`ovmx_accounting_get_lastlogin()`), not cached state (INV-6: a real,
system-wide gate, not a per-process fake). `tests/dcl/test_set_accounting_veracity.sh`
is the DCL-surface companion — proves the facade text/status is gone
(`SET ACCOUNTING/BOGUS` → `%DCL-W-IVQUAL`, `/ENABLE=(IMAGE)` → honest
`%SET-W-NOTIMPL`/`SS$_UNSUPPORTED`) and that `SHOW ACCOUNTING` agrees with
`SET ACCOUNTING` within one session, without needing a real identity (bare
host ctest has none, Rule 9/INV-6).
`tests/dcl/test_show_quick.sh` was updated to `SET ACCOUNTING/DISABLE` before
asserting `SHOW ACCOUNTING`'s text: with a real, persisted flag shared by
every script in the same `dcl-integration` ctest run (one real `/vms`
mount, see `run_dcl_tests.sh`'s own non-hermetic-ordering note), the old
assumption — "a fresh per-process bool defaults to disabled" — no longer
holds, so the assertion now pins its own precondition instead of relying on
another script's incidental last write.

## vms-309 — SET VOLUME moves FACADE to PARTIAL

`cmd_set_volume()` (`src/vmsdcl/dcl_cmd_set.c`) used to print
`%SET-I-NOTIMPL, SET VOLUME requires a mounted VMSFS volume` and return
`SS$_NORMAL` **unconditionally** — for every invocation, mounted device or
not, real qualifier or garbage, even bare `SET VOLUME` with no device at
all. An `-I-` (success-toned) message for a total no-op is INV-DCL's
banned class.

**Scope decision (flagged per CLAUDE.md Rule 5 — the item anticipated a
REAL outcome for `/LABEL`, or PARTIAL "if only `/LABEL`"; this PR lands
neither, and says why here).** The item's own fallback authorized scoping
to honest errors if "`/LABEL` needs vmsfs plumbing that doesn't exist."
It doesn't exist. Two independent findings converge on the same
conclusion:

1. `cmd_mount()`'s own `/LABEL`-equivalent parameter, in the same source
   file, already carries the comment "Volume label -- informational only;
   vmsfs does not read it back."
2. `src/kernel/vmsfs/` — the kernel module MOUNT actually `mount(2)`s, and
   the thing "a mounted vmsfs volume" in the item's own wording refers to
   — declares **no ioctl at all** (`grep -rn ioctl src/kernel/vmsfs/*.c`
   is empty). It reads the home block (`hb_volname`, `vmsfs_ondisk.h`)
   into `sbi->home` exactly once, at mount (`vmsfs_super.c`), and never
   re-reads it. There is no in-kernel path to rewrite a label on a volume
   that is currently mounted, and patching the raw block device
   underneath a live mount from userspace would not be an honest
   substitute for one — it would race the kernel's own buffer cache for
   that block (invisible to the live mount, or silently clobbered on the
   kernel's next write-back), which is the exact silent-corruption failure
   mode INV-6/INV-DCL exist to prevent, not an honest refusal.

A real `/LABEL` therefore needs a new `vmsfs.ko` ioctl (recompute
`hb_checksum`, write through the mount's own buffer head, refresh
`sbi->home`) — kernel module interface work, CLAUDE.md's Design Change
Cascade-sized, not a facade-kill patch. Filed as a follow-up (see the
"Still open" list above; not yet an rd item — file from repo root per
CLAUDE.md/MEMORY, not from a worktree).

**What actually shipped.** `cmd_set_volume()` now:
- Validates the full 23-qualifier Dictionary grammar structurally
  (`dcl_validate_qualifiers()`, the Phase 1 machinery SET PASSWORD/SET
  ACCOUNTING already use) — an unknown qualifier draws the authentic
  `%DCL-W-IVQUAL`/`SS$_IVQUAL`, not silent acceptance.
- Checks the device is a genuinely **mounted** volume the same way
  MOUNT/DISMOUNT do — `mount_point_is_mounted()` against `/proc/mounts`,
  the kernel's own real, cross-process mount table (promoted from
  `static` in `dcl_cmd_misc.c` to a shared declaration in
  `src/vmsdcl/include/dcl/dcl_cmd.h` so SET VOLUME can call the exact same
  function, not a re-derived copy). Not mounted → the authentic
  `SS$_DEVNOTMOUNT`, matching the Dictionary's own parameter description
  ("the name of one or more MOUNTED Files-11 volumes"), not the old
  success-toned NOTIMPL.
- For a genuinely mounted volume, every qualifier — `/LABEL` included —
  draws a specific, honest `%SET-W-NOTIMPL`/`SS$_UNSUPPORTED` refusal
  naming exactly what didn't happen, instead of a blanket fake success.
  A bare `SET VOLUME device:` with no qualifier on a mounted device is a
  real (if pointless) no-op — the Dictionary doesn't forbid it, and OVMX
  genuinely verified the device names a mounted volume and changed
  nothing else, claiming nothing more.

Clean-room (Rule 8): syntax, access requirement, and the 23-qualifier list
from the public OpenVMS DCL Dictionary SET VOLUME entry
(<https://wiki.vmssoftware.com/SET_VOLUME>,
<https://www.digiater.nl/openvms/doc/ia64-v8.3/opsys/vmsos83/9996/9996pro_225.html>,
both fetched for this fix). `/LABEL=volume-label`: "Assigns a 1-12
character ANSI name to the volume ... remains in effect until it is
changed explicitly; dismounting the volume does not affect the label."

**Bucket: PARTIAL, not REAL.** Real, substantial work — genuine mount-state
verification and a genuine structural qualifier grammar, both new — with a
disclosed gap: no SET VOLUME qualifier persists any characteristic,
`/LABEL` included. Not STUB (`docs/design-dcl-fidelity.md`'s STUB bucket is
"an immediate, honest refusal with no logic behind it" — PHONE's shape;
SET VOLUME's mount-check + per-qualifier structural validation is real
logic, not an immediate blanket refusal).

Veracity gates: `tests/dcl/test_set_volume_veracity.sh` (ctest, no
`/dev/vms`) proves the two branches reachable without a real mount — a
bogus qualifier draws `%DCL-W-IVQUAL`/2288 regardless of mount state, and
`SET VOLUME` against a device nothing in this environment ever mounts
draws the authentic `SS$_DEVNOTMOUNT`/2688, never `SS$_NORMAL`/1. The
mounted-volume branch below that check — including `/LABEL`'s honest
refusal, which needs a real mount to reach at all — is the paired
POSITIVE in `tests/qemu/test_mount_e2e.sh` (extended, not a new script):
against a genuinely-mounted `DKA100:`, a bogus qualifier still draws
IVQUAL, `/LABEL=` draws `%SET-W-NOTIMPL` (never the old `%SET-I-NOTIMPL`),
and a bare `SET VOLUME DKA100:` with no qualifier reports no error.
`tests/dcl/test_set_quick.sh`'s trailing bare `SET VOLUME` (no device)
assertion moved from the old facade text to the real
`%DCL-E-NODEVICE`/`SS$_BADPARAM` refusal.
