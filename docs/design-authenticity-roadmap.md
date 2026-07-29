# OVMX Authenticity Roadmap — Indistinguishable from OpenVMS

> Status: **PROPOSED** (pillar `vms-898`, objective: total VMS commoditization).
> Awaiting operator approval to build the rd work-item tree. This document is the
> plan of record; on approval it becomes the parent spec for the re-decomposed
> `vms-898` epic. No rd items exist yet for Milestones 1–3 — this doc IS the
> pre-approval artifact.
>
> **Operator framing (2026-07-27):** "The entire UX has a bunch of tells. Help
> facility isn't true to form, SET/SHOW are shallow, MONITOR/SYSGEN/SYSMAN feel
> absent — it's obviously a VMS LARP shim." Target chosen: **full surface parity,
> long haul** — a tiered roadmap with the "10-minute greybeard" bar as Milestone 1.
>
> **Trademark ruling (2026-07-27):** authenticity has a HARD legal ceiling —
> "OpenVMS"/"VMS" are VSI **trademarks**, which clean-room RE does not license.
> OVMX **brands as OVMX**; VMS-compatible identity lives only in machine-interop
> surfaces; human surfaces are badged **"OVMX — OpenVMS-compatible."** If VSI objects,
> fall back to hacker-tradition trademark-dodge spelling ("0penVMS-compatible", cf.
> `un*x`). See the Invariants section (INV-0). This is a discipline, not a milestone:
> the fix for "how does each increment hold hard and true" is to mechanize fidelity as
> standing gates so drift is forbidden, not swept up later.

## 1. Thesis: authenticity is depth, not presence

The recon pass (2026-07-27, Explore inventory) shows the skeleton is **substantially
built**, contradicting the "nonexistent" read: 55 DCL verbs, real queue DB, real
EDT, real BACKUP, 35 F$ lexicals, authentic `%FACILITY-SEVERITY-IDENT` message
*formatting*, and MONITOR/SYSGEN/SYSMAN/AUTHORIZE as 500–975-line images.

So the tell is **not absence — it is uneven depth**. A verb that exists and prints
three hardcoded lines is a *worse* tell than an absent one: a VMS-experienced user
types `MONITOR SYSTEM` expecting a live refreshing screen and gets a photograph.

**Definition of done for the epic:** OVMX survives *knowledgeable probing*. Not "does
the verb exist" but "what does a VMS person type in the first 10 minutes / first hour
/ first week that exposes the shim." Every item below is ranked by **tell-probability**
(how early and how surely a knowledgeable user hits it), not by feature-checklist
completeness.

## 2. Grounded current-state — the real tells (file:line)

| Tell | Reality today | Location |
|---|---|---|
| `SHOW SYSTEM` | Hardcoded `OpenVMS V7.3`; prints exactly **one fabricated process row**, never the real process list | `dcl_cmd_show.c:276` |
| HELP | Shim — no `.HLB` tree; per-verb one-liners + 3 stale hardcoded sublevels. Real `HELP.EXE` exists but the HELP **verb never calls it** | `dcl_cmd_misc.c:718`; `tools/vms_help.c` |
| Message idents | Format authentic; catalog ~50; some idents **invented** (`QMANERR`, `SUBMITERR`) rather than real VMS idents | `dcl_messages.c:36` |
| `F$GETSYI` | Recognizes **4** item codes (NODENAME, VERSION, HW_NAME, BOOTTIME); everything else returns empty | `dcl_lexical.c:1267` |
| Batch queue | Real *store*, **no executor** — `SUBMIT` accepts a job, nothing ever dequeues/runs it | `vmsqueue.c:222` |
| `SHOW LICENSE/CLUSTER/NETWORK` | near-empty stubs | `dcl_cmd_show.c:893+` |
| MOUNT / DISMOUNT | in-memory device-table update, no real volume | `dcl_cmd_misc.c:1584` |
| `SET TERMINAL` | self-labeled "(stub)" | `dcl_cmd_set.c:157` |
| `SHOW AUDIT / VOLUME`, `SET AUDIT / VOLUME` | acknowledge-only | `dcl_cmd_show.c`, `dcl_cmd_set.c` |
| RUN | native `fork/execl` of a Linux binary, not the VMS image activator | `dcl_cmd_process.c:650` |

**Not tells (leave alone):** AUTHORIZE has no DCL verb — that is *authentic* (real VMS is
`RUN SYS$SYSTEM:AUTHORIZE`). Message *format* is already correct. Queue store schema is real.

## 3. The oracle & purity guardrail (shapes every value item)

Most authenticity work is **VMS-authentic values and formats**: message idents, SYSGEN
parameter names/defaults, F$GETSYI item codes, SHOW column layouts, MONITOR class
displays. Per the project purity rule (`[[vms-purity-guardrail]]`, CLAUDE.md invariant
#8) these **cannot be self-certified** — green CI ≠ VMS-correct.

Every value-bearing item below pins to an **oracle** and lands with operator sign-off:

- **Public OpenVMS documentation** — System Services Ref, DCL Dictionary, Utility
  Routines, System Management Utilities (SYSGEN/SYSMAN/MONITOR/AUTHORIZE manuals),
  HELP message docs. Authoritative for *published* values and output formats.
- **The `~/vax` reference lab** (`[[vax-cluster-lab-authority]]`) — a real VMS instance
  for *empirically observable* values (SDA/SYSGEN/SYSMAN dumps, actual command output
  to diff against). Operator has granted standing autonomy over this lab.
- **Operator sign-off** for anything neither doc-published nor lab-observable, recorded
  on the item. Where no oracle exists, OVMX defines its own value, **labels it an OVMX
  design choice**, and files a `vms-purity` revisit item (`[[empirical-not-gate]]`).

Without this, we would ship a *convincing* LARP instead of a *correct* one — which
defeats the commoditization goal. The oracle is not optional overhead; it is the
difference between reproduction and imitation.

## 4. Fidelity rubric — tell-probability tiers

- **Tier 0 — first-90-seconds:** login banner, `SHOW SYSTEM`, `SHOW PROCESS/FULL`,
  `HELP`, `DIRECTORY` output, error-message idents. Hit immediately, cheap to fix.
- **Tier 1 — live behavior:** `MONITOR` live screen, batch executor, `SHOW
  DEVICE/MEMORY/CLUSTER` off real state, `F$GETSYI` breadth.
- **Tier 2 — sysadmin depth:** `SYSGEN` param DB, `SYSMAN` SMI+DO, `AUTHORIZE` depth
  (← SYSUAF/`vms-846` plugs in here), MOUNT real volumes.
- **Tier 3 — long tail:** TPU/EVE, PHONE, ACME/LDAP, MAIL fidelity, ANALYZE depth,
  RTL/system-service breadth, full lexical surface, ACCOUNTING records, DECnet/NCP.

Milestones map to tiers: **M1 = Tier 0**, **M2 = Tier 1 + Tier 2**, **M3 = Tier 3**.

---

## 4.5 Invariants & the trademark ceiling (standing gates)

These are **not** milestone tasks — they are standing gates every future build (authenticity
or otherwise: toolchain, cluster, DECnet, self-host) must pass, so the clone stays faithful
*as OVMX grows toward parity*. This is the answer to "how does each incremental milestone
toward the kneecap hold hard and true": mechanize fidelity so drift is **forbidden at merge
time**, not swept up in a later sweep. Authenticity is a discipline, not a phase.

### INV-0 — The trademark ceiling (HARD legal constraint)

Clean-room invariant #8 (CLAUDE.md) protects **copyright** — code and formats (DMCA 1201(f),
EU interop). "OpenVMS"/"VMS" are **trademarks** held and enforced by VSI, a separate body of
law that clean-room RE does not touch. Authenticity therefore has an un-escapable ceiling:

- **OVMX brands as OVMX.** Product name, marketing, and the answer to a human asking "what is
  this" are always OVMX — never presented as VSI's product *as to source*. Passing-off is the line.
- **VMS-compatible identity lives only in machine-interop surfaces.** Version/identity tokens
  software reads to interoperate (`F$GETSYI VERSION`/`SYI$_VERSION`, cluster negotiation) may
  report a VMS-compatible value — **functional interoperability**, the Wine / Samba / ReactOS
  posture, not source-branding.
- **The three "indistinguishable"s:** (1) to *software* — full send, legal, the real moat-breaker;
  (2) *faithful to a human* VMS user (at home, muscle memory intact) — yes; (3) making a human
  believe it *is* VSI's shipping product — **no**, never a goal. Win by being a legal, free clone
  that eats the moat in the open, not by deceiving as to source.
- **Badge:** human-facing surfaces read **"OVMX — OpenVMS-compatible."** Operator posture
  (2026-07-27): if VSI dings "OpenVMS-compatible," fall back to trademark-dodge spelling in the
  hacker tradition (cf. `un*x`/`*nix` vs AT&T's UNIX) — e.g. "0penVMS-compatible." The tell is
  real and un-escapable; it is *also* the legal shield, and that trade is accepted.
- Not legal advice — posture reasoning. Banner wording and public launch copy warrant real
  IP-counsel review before the give-it-away ships.

Promote to a project invariant (CLAUDE.md, sibling to #8). Tracked as an rd decision item.

### INV-1 — System-identity single source of truth

One module owns system identity; every surface reads it — never a hardcoded `V7.3` in N places
again. Identity has **two version numbers plus an iron rule** (D1, resolved 2026-07-27):

- **OVMX product version (brand — ours).** OVMX tags its *own* version: **V0.1** at first release,
  **V1** eventually. Human-facing surfaces show this, badged per INV-0 — e.g.
  `OVMX V0.1 — OpenVMS-compatible`. This is the honest "what is this" answer (SHOW SYSTEM header,
  login banner).
- **VMS-compatibility version (machine — true-to-arch).** The token software reads to interoperate
  (`F$GETSYI VERSION`/`SYI$_VERSION`) is chosen **true to the running arch**: on **x86-64**, the real
  VSI x86-64 version (**V9.2-x**), which happens to give full interop. Derived from arch, not a
  global constant.
- **Iron rule — never lie to the metal.** OVMX reports the arch it actually runs on. Where an arch
  has a VMS lineage (x86-64), the compat version matches it. Where it does **not** (**ARM**), OVMX
  does not fake a VMS arch — it presents its own identity honestly. **OVMX-on-ARM is a new frontier**
  (OpenVMS never ran on ARM): a headline capability to celebrate, not a tell to paper over.

Every surface is classified **human** (→ OVMX brand badge) or **machine** (→ true-to-arch compat
token) and reads the one identity module.

**Login banners are logicals, not printfs** (refinement, 2026-07-28, operator correction during
implementation). On real VMS the login banner is *not* compiled into LOGINOUT — a manager defines
**`SYS$ANNOUNCE`** (displayed before the `Username:` prompt) and **`SYS$WELCOME`** (displayed after
authentication) at boot, in `SYS$MANAGER:SYLOGICALS.COM` (OVMX: `SYLOGICALS.CONF`). An equivalence
string beginning with `@` names a **file** whose contents are displayed, which is how a site gets a
multi-line banner. Both ship **undefined**, exactly as VMS does: with no `SYS$WELCOME`, LOGINOUT
prints its built-in banner — and that built-in is the only thing the identity module supplies.

This matters more than the version string it replaced. A greybeard types
`DEFINE/SYSTEM SYS$WELCOME "..."` within the first ten minutes; a hardcoded `printf()` swallows it
silently, which is a *worse* tell than a wrong version — the verb appears to work and does nothing.
Implemented in `ovmx_banner.h`; the INV-1 gate asserts LOGINOUT and the SSH daemon keep resolving
the logical rather than regressing to a compiled-in greeting.

**Logical names are themselves an authenticity surface** (`vms-d37`, blocks A4). Wiring the banner to
`SYS$WELCOME` exposed that OVMX logical-name tables are **per-process**: `lnm_get_manager()` builds an
in-process table, and nothing but the daemon itself reads `SYLOGICALS.CONF`. Demonstrated across two
DCL processes:

```
proc 1:  DEFINE/SYSTEM CROSSPROC HELLO  /  SHOW LOGICAL CROSSPROC
         ->    "CROSSPROC" = "HELLO" (LNM$SYSTEM)
proc 2:  SHOW LOGICAL CROSSPROC
         -> %DCL-W-NOLOG, no logical name match
```

`DEFINE/SYSTEM` reports success and the definition dies with the process. This is not adjacent
infrastructure to be fixed later — logicals are *how a VMS system is configured*, so this is the
epic's own thesis (uneven **depth**, not absence) and squarely INV-6 territory: a facility that looks
implemented and silently isn't. It is the reason **A4 cannot honestly close on the banner work alone**
— the banner is correctly wired *to* the logical, but a sysadmin still cannot set it.

The in-process facility is otherwise genuinely deep and should not be rebuilt: all four tables
(`LNM$PROCESS/JOB/GROUP/SYSTEM`), hierarchical search order, `/TABLE=`, `/PROCESS`, `/SYSTEM`,
`DEASSIGN/SYSTEM`, and table attribution in `SHOW LOGICAL` all work. The single missing piece is
cross-process sharing.

### INV-2 — Message-ident fidelity gate
No emitted message may use an invented ident. New idents require oracle + operator sign-off. A
**CI lint** fails the build on any ident outside the verified catalog. (A3 seeds the catalog;
the gate is permanent.)

### INV-3 — Output-format conformance gate
Every user-facing display command carries a golden-reference test diffed against real VMS output.
New SHOW/DIRECTORY/utility output cannot merge without one. (A5 establishes the convention.)

### INV-4 — No-Linux-leak gate
The leak-detection suite runs on every VMS-facing surface: no errno text, Linux path, glibc
string, or bash-ism may reach a VMS-facing output. Extends the existing Unix-leak suite.

### INV-5 — Oracle / purity gate
Already a project invariant (`vms-purity`, `[[vms-purity-guardrail]]`): any VMS-authentic value
or format anywhere pins to the oracle (public docs / `~/vax` lab) with operator sign-off.
Authenticity is its largest consumer.

### INV-6 — Anti-LARP declared-stub gate
A shallow implementation must **declare itself** a stub (source annotation + auto-filed depth
item). Silent breadth-without-behavior is the LARP smell; making shims announce themselves
converts uneven depth from a hidden tell into visible, tracked work. No future feature may
quietly add a shim — this is the direct antidote to how OVMX accumulated 55 verbs of uneven depth.

---

## 5. Milestone 1 — survives the 10-minute greybeard (Tier 0)

The near-term deliverable. Cheap, visible, highest tells-per-dollar. Milestone done
when a VMS-experienced user poking around interactively for ~10 minutes finds no
obvious shim in the banner, `SHOW`, `HELP`, or error output.

### A1 — `SHOW SYSTEM` lists the real process table
- **Outcome:** `SHOW SYSTEM` prints every live process (not one fabricated row) with
  authentic columns: PID, Process Name, State, Pri, I/O, CPU time, Pages, and the
  correct banner line (node, version, uptime, load).
- **Done when:** with ≥3 processes running (e.g. DCL + a SPAWN + a detached daemon),
  `SHOW SYSTEM` lists all of them from the real PCB/process table; a test asserts row
  count and column format against a captured real-VMS `SHOW SYSTEM` layout.
- **Files:** `src/vmsdcl/dcl_cmd_show.c:240`, process table from `vmsprocess/`.
- **Oracle:** DCL Dictionary `SHOW SYSTEM`; `~/vax` lab output for exact column widths.
- **Purity:** version string — see Decision D1. Column layout → lab-verified.
- **Size:** 1 session.

### A2 — HELP facility is a real hierarchical library
- **Outcome:** the HELP verb drives a real hierarchical help library — `HELP`,
  `HELP SHOW`, `HELP SHOW SYSTEM`, `HELP SET TERMINAL /qualifier` all resolve through a
  key/subkey tree; retire the inline hardcoded shim.
- **Done when:** `HELP <verb>` and `HELP <verb> <subtopic>` return structured multi-level
  help for every builtin verb, sourced from a help library (`.HLB`-style or the wired
  `HELP.EXE`), with the authentic "Information available:" / "Topic?" interaction; a test
  covers a 3-level lookup and the "no such topic" path.
- **Files:** `src/vmsdcl/dcl_cmd_misc.c:707`; `tools/vms_help.c`; help library format
  (reuse `dcl_library.c` help-lib support at `:529`).
- **Oracle:** HELP output format from public docs / `~/vax`.
- **Decision D2:** wire the HELP verb to `HELP.EXE`, or build an in-process `.HLB`
  reader? (Recommend: wire to `HELP.EXE` + a real help library file — reuses existing
  code, matches VMS's `SYS$HELP:*.HLB` model.)
- **Size:** 1–2 sessions (library content authoring is the bulk).

### A3 — Message facility: real idents, no invented ones
- **Outcome:** every error OVMX emits uses a **real** VMS message ident; the invented
  ones (`QMANERR`, `SUBMITERR`, …) are replaced; catalog expanded to cover the facilities
  OVMX actually raises (DCL, MOUNT, SUBMIT, RMS, SYSTEM, LOGIN, …).
- **Done when:** an audit lists every ident OVMX can emit, each cross-checked against the
  oracle and marked verified/OVMX-design; no ident lacks provenance; a test asserts a
  sample of high-traffic errors match real VMS text exactly.
- **Files:** `src/vmsdcl/dcl_messages.c:36` and all `dcl_error(...)` call sites.
- **Oracle:** OpenVMS System Messages manual; `~/vax` `HELP/MESSAGE`.
- **Purity:** **operator sign-off on the ident list** (values). Design-change cascade.
- **Size:** 1–2 sessions.

### A4 — Login/logout presentation fidelity
- **Outcome:** the interactive login sequence matches VMS: `SYS$ANNOUNCE` banner,
  `Welcome to OpenVMS...`, `Last interactive login on ...`, `Last non-interactive
  login`, new-mail count line, `SYS$WELCOME`, correct `$` prompt and node name.
- **Done when:** a fresh SSH login reproduces the real VMS LOGINOUT sequence line-for-line
  (modulo site text); a test captures the login transcript and diffs the fixed portions.
- **Files:** `tools/vms_login.c`; `src/ovmx_init/` login path; logical names
  `SYS$ANNOUNCE`/`SYS$WELCOME`.
- **Oracle:** LOGINOUT behavior from public docs / `~/vax`.
- **Size:** 1 session.

### A5 — `SHOW` / `DIRECTORY` output-format sweep
- **Outcome:** the highest-traffic display commands match VMS output byte-shape:
  `DIRECTORY` (columns, `;version`, blocks, `/FULL` fields, `Total of n files, m blocks`
  trailer), `SHOW PROCESS`, `SHOW PROCESS/FULL`, `SHOW DEFAULT`, `SHOW TIME`, `SHOW
  LOGICAL` format.
- **Done when:** for each command, OVMX output diffs clean against a captured real-VMS
  reference (fixed fields; dates/sizes normalized); tests assert the format.
- **Files:** `dcl_cmd_file.c` (DIRECTORY), `dcl_cmd_show.c`.
- **Oracle:** `~/vax` captured output; DCL Dictionary.
- **Size:** 1–2 sessions.

---

## 6. Milestone 2 — survives a working sysadmin (Tier 1 + Tier 2)

Milestone done when a VMS admin can perform real system-management tasks (monitor the
system live, tune parameters, run SYSMAN commands, manage users and queues) and the
utilities actually work.

### B1 — MONITOR is a live refreshing screen
- **Outcome:** `MONITOR SYSTEM` (and `MONITOR PROCESSES/TOPCPU`, `MONITOR IO`,
  `MONITOR MODES`, `MONITOR POOL`, `MONITOR DISK`) render the authentic MONITOR display
  — full-screen, refreshing on the interval, with the correct class layout, CUR/AVE/MIN/MAX
  columns, and real metrics mapped from `/proc` and the process table.
- **Done when:** `MONITOR SYSTEM` refreshes live with the real screen layout for ≥2
  classes; `/INTERVAL` and `/summary` honored; screen layout diffs against real VMS.
- **Files:** `tools/vms_monitor.c` (832).
- **Oracle:** OpenVMS MONITOR Utility manual (class display layouts); `~/vax`.
- **Purity:** class display field set — doc-verified.
- **Size:** 2–3 sessions.

### B2 — Batch/print executor (JBC daemon)
- **Outcome:** a job controller daemon actually **dequeues and executes** `SUBMIT`ted
  `.COM` procedures and `PRINT` jobs; `SYS$BATCH` runs jobs to completion; `SHOW QUEUE`
  reflects live PENDING→EXECUTING→COMPLETED transitions driven by real execution; log
  files (`.LOG`) are produced.
- **Done when:** `SUBMIT x.COM` runs the procedure, produces `X.LOG`, and the entry
  reaches COMPLETED; a test submits a job and asserts side effects + log.
- **Files:** `src/vmsqueue/` (add executor), `dcl_cmd_process.c` (SUBMIT/PRINT).
- **Oracle:** queue/JBC behavior from public docs.
- **Size:** 2–3 sessions. (Design-change: new daemon in the boot sequence.)

### B3 — SYSGEN real parameter database
- **Outcome:** `SYSGEN` maintains a real parameter DB: `SHOW <param>`, `SET <param>
  value`, `USE ACTIVE/CURRENT/DEFAULT`, `WRITE ACTIVE/CURRENT`, with the authentic
  parameter set (MAXPROCESSCNT, WSMAX, GBLPAGES, GBLSECTIONS, NPAGEDYN, VIRTUALPAGECNT,
  BALSETCNT, SCSNODE, SCSSYSTEMID, VOTES, EXPECTED_VOTES, …) and correct min/max/default/unit.
- **Done when:** `SYSGEN SHOW MAXPROCESSCNT` and a `SET`/`WRITE`/`USE` round-trip work
  against a persisted param file; param names/defaults verified against oracle; test covers
  a round-trip.
- **Files:** `tools/vms_sysgen.c` (548); param store.
- **Oracle:** OpenVMS System Management Utilities (SYSGEN); `~/vax` `SYSGEN SHOW/ALL`.
- **Purity:** **parameter names, defaults, min/max need operator sign-off** (values).
- **Size:** 2 sessions.

### B4 — SYSMAN SMI + DO
- **Outcome:** `SYSMAN` provides the System Management utility surface: `SET
  ENVIRONMENT`, `DO <command>`, `PARAMETERS SHOW/USE/WRITE`, `CONFIGURATION`, `SYS_LOADABLE`,
  the authentic prompt and command grammar.
- **Done when:** `SYSMAN> DO SHOW SYSTEM` executes in the target environment; `PARAMETERS`
  delegates to the SYSGEN DB (B3); test covers a `DO` round-trip.
- **Files:** `tools/vms_sysman.c` (643).
- **Oracle:** OpenVMS SYSMAN manual; `~/vax`.
- **Size:** 2 sessions. Depends on B3.

### B5 — F$GETSYI / F$GETJPI / F$GETDVI item-code breadth
- **Outcome:** the info lexicals recognize the real item-code sets, not a handful —
  F$GETSYI (VERSION, NODENAME, HW_NAME, HW_MODEL, ARCH_NAME, PAGE_SIZE, PHYSICAL_MEMORY,
  ACTIVECPU_CNT, MAXPROCESSCNT, CLUSTER_MEMBER, BOOTTIME, …), F$GETJPI (PID, USERNAME,
  PRCNAM, STATE, PRI, MODE, IMAGNAME, WSSIZE, …), F$GETDVI (DEVCLASS, DEVTYPE, FREEBLOCKS,
  MAXBLOCK, MOUNTCNT, …).
- **Done when:** each lexical returns correct values for a documented core set (≥20 items
  each); test covers representative items; unknown items error authentically, not silently.
- **Files:** `src/vmsdcl/dcl_lexical.c:1247+`; backed by real `sys$getsyi/getjpiw/getdvi`.
- **Oracle:** DCL Dictionary lexical entries; `$SYIDEF/$JPIDEF/$DVIDEF`.
- **Purity:** item-code values → header fidelity (ties to `vms-1f9`/`vms-da9` sweeps).
- **Size:** 2 sessions.

### B6 — `SHOW DEVICE/MEMORY/CLUSTER/LICENSE` off real state
- **Outcome:** these commands read actual system state instead of stubs — `SHOW DEVICE`
  from the real device table, `SHOW MEMORY` from real page/pool accounting, `SHOW CLUSTER`
  from cluster state (or authentic single-node output), `SHOW LICENSE` from a real PAK
  store (or authentic "no licenses loaded").
- **Done when:** each reflects real state and diffs clean against reference format.
- **Files:** `dcl_cmd_show.c:534/633/907/893`.
- **Size:** 1–2 sessions.

### B7 — MOUNT / DISMOUNT real volume semantics
- **Outcome:** `MOUNT` actually associates a device with a Files-11/host-backed volume
  (not just an in-memory table entry); mounted state is real, visible to `SHOW DEVICE`,
  and survives across processes; `DISMOUNT` releases it.
- **Done when:** `MOUNT` a volume, write a file through it, `SHOW DEVICE` reflects the
  mount, `DISMOUNT` releases; test covers the lifecycle.
- **Files:** `dcl_cmd_misc.c:1584`; `vmsfs/` device layer.
- **Size:** 2 sessions.

### B8 — AUTHORIZE depth (← SYSUAF / `vms-846` plugs in here)
- **Outcome:** AUTHORIZE presents the real utility depth on a real RMS-indexed SYSUAF:
  ADD/MODIFY/REMOVE/SHOW/LIST/COPY/RENAME/DEFAULT, qualifiers (`/PRIVILEGES`, `/FLAGS`,
  `/UIC`, `/DIRECTORY`, `/DEVICE`, `/PWDMINIMUM`, …), and the authentic SHOW record layout.
- **Done when:** the full ADD→SHOW→MODIFY→REMOVE cycle works against the RMS SYSUAF and
  SHOW output matches the real AUTHORIZE record format.
- **Files:** `tools/vms_authorize.c` (862); SYSUAF from `vms-846` epic.
- **Dependency:** consumes the entire `vms-846` SYSUAF-to-RMS migration.
- **Oracle:** OpenVMS AUTHORIZE manual; `~/vax` `AUTHORIZE SHOW`.
- **Size:** 2 sessions (after 846 lands).

---

## 7. Milestone 3+ — the long tail (Tier 3)

Depth for the surfaces a power user or long-term operator eventually reaches. Each is an
independent sub-epic; sequence by demand.

- **C1 — TPU/EVE editor.** `EDIT/TPU` invokes a real TPU/EVE screen editor (today `EDIT`
  is EDT line-mode only). Large.
- **C2 — MAIL fidelity.** Folder model, `SEND/READ/REPLY/FORWARD/DIRECTORY`, `MAIL$`
  logicals, mail file (`.MAI`) format, external transport hooks.
- **C3 — PHONE.** Real interactive PHONE (today a stub).
- **C4 — ACME / LDAP / external auth.** The ACME agent framework (absent) for pluggable
  authentication.
- **C5 — ANALYZE depth.** `ANALYZE/RMS`, `ANALYZE/DISK`, `ANALYZE/IMAGE`, `ANALYZE/AUDIT`,
  `ANALYZE/SYSTEM` (SDA) fidelity.
- **C6 — RTL / system-service breadth.** Fill the long tail of `LIB$`/`SYS$`/`STR$`/`OTS$`
  routines a real image links against (drives real-app compatibility). Ties to `vms-801`.
- **C7 — Full lexical surface.** Remaining F$ lexicals (F$LICENSE, F$MULTIPATH,
  F$DELTA_TIME, F$MATCH_WILD, …) and full item-code coverage.
- **C8 — ACCOUNTING real records.** `SYS$ACCOUNTING` binary record format, real
  accounting collection, `ACCOUNTING` report utility.
- **C9 — DECnet / NCP / network utilities.** (Overlaps `vms-eat` DECnet Phase IV.)
- **C10 — Remaining SET/SHOW depth.** `SET AUDIT/VOLUME/SECURITY`, `SHOW AUDIT`, security
  auditing behind them (today acknowledge-only).

## 8. Sequencing & dependencies

```
M1 (Tier 0)  A1 SHOW SYSTEM   A2 HELP   A3 messages   A4 login   A5 output-sweep
                 |  (all independent; parallelizable)
                 v
M2 (Tier 1/2)  B1 MONITOR   B2 batch-exec   B5 F$GETSYI   B6 SHOW-state   B7 MOUNT
               B3 SYSGEN ──> B4 SYSMAN
               vms-846 (SYSUAF→RMS) ──> B8 AUTHORIZE depth
                 |
                 v
M3+ (Tier 3)   C1..C10  (independent sub-epics, demand-ordered)
```

- **M1 items are mutually independent** — build in parallel.
- **B4 depends on B3** (SYSMAN PARAMETERS delegates to the SYSGEN DB).
- **B8 depends on the whole `vms-846` epic** (already planned; see its own reshape note).
- **A3 (messages), B3 (SYSGEN params), B5 (item codes), B8 (AUTHORIZE record)** are
  the value-bearing items that require **operator sign-off** per §3.

## 9. How `vms-846` (SYSUAF→RMS) fits

`vms-846` is **not** a standalone authenticity play — it is the substrate for **B8**.
Real VMS keeps users in an RMS-indexed `SYSUAF.DAT`; a text file is a tell only AUTHORIZE
and `SHOW USERS` expose. So 846 stays its own epic (record → seed → readers → AUTHORIZE →
provision → cutover) and **B8 is its authenticity payoff**. `vms-846.1` (binary record)
is already done. 846 also needs its own reshape (missing cutover child, encoding contract,
integration test) — tracked separately from this roadmap.

## 10. Open operator decisions (block specific items, not the tree)

- **D1 — Versioning [RESOLVED 2026-07-27].** OVMX tags its **own** product version (**V0.1** first
  release → **V1**), shown on human surfaces badged "OpenVMS-compatible" (INV-0). The machine-facing
  VMS-compat token is **true-to-arch**: **V9.2-x on x86-64** (full interop). **Iron rule: never lie
  to the metal** — ARM has no VMS lineage, so OVMX-on-ARM presents its own identity honestly and is
  a celebrated new frontier, not a faked VMS arch. Folded into INV-1. Affects A1, A4, B5.
- **D2 — HELP delivery:** wire the HELP verb to `HELP.EXE` + a help-library file (recommended),
  or build an in-process `.HLB` reader? Affects A2.
- **D3 — Message ident list:** operator sign-off on the verified vs OVMX-design ident set. A3.
- **D4 — SYSGEN parameter set:** operator sign-off on param names/defaults/min/max. B3.

## 11. rd tree to build on approval

```
vms-898  EPIC: Authenticity — OpenVMS-compatible, indistinguishable to software   (re-scoped parent)
├── INV  Invariants: standing gates (DO NOW — cheapest, highest-leverage on the board)
│   ├── INV-0  Trademark ceiling: brand OVMX, VMS-compat only in machine interop   [decision, vms-purity+legal]
│   ├── INV-1  System-identity SSOT (dual identity)   [seeds A1/A4/B5]
│   ├── INV-2  Message-ident fidelity CI-lint gate   [pairs with A3]
│   ├── INV-3  Output-format golden-conformance gate   [pairs with A5]
│   ├── INV-4  No-Linux-leak gate (extend leak suite)
│   ├── INV-5  Oracle/purity gate (exists — vms-purity)
│   └── INV-6  Anti-LARP declared-stub gate
├── M1  Milestone: survives the 10-minute greybeard   (container)
│   ├── A1  SHOW SYSTEM real process table
│   ├── A2  HELP hierarchical library
│   ├── A3  Message idents: real, no invented   [operator sign-off]
│   ├── A4  Login/logout presentation fidelity
│   └── A5  SHOW/DIRECTORY output-format sweep
├── M2  Milestone: survives a working sysadmin   (container)
│   ├── B1  MONITOR live screen
│   ├── B2  Batch/print executor (JBC daemon)
│   ├── B3  SYSGEN parameter DB   [operator sign-off]
│   ├── B4  SYSMAN SMI + DO   (blocked by B3)
│   ├── B5  F$GETSYI/JPI/DVI item-code breadth   [purity: ties vms-1f9/da9]
│   ├── B6  SHOW DEVICE/MEMORY/CLUSTER/LICENSE off real state
│   ├── B7  MOUNT/DISMOUNT real volumes
│   └── B8  AUTHORIZE depth   (blocked by vms-846 epic)
└── M3  Milestone: long tail   (container)
    └── C1..C10  (independent sub-epics, created as demand warrants)
```

Each item carries: outcome, done-condition, files, oracle source, purity flag, size —
per §§5–7. Design-change cascade fires for A3, B2 (new daemon), B3, B7, B8.
