# VMS Command-Surface & Tools Parity Map

> **Operator charge (2026-08-11):** "we killed the lies, which is good, but we still have a
> hollow experience. we need significantly more command surface and depth, true to VMS … we
> can't have facades, we must have real tools, parity with VMS. map that out."
>
> Killing facades got OVMX to **honest**. Parity is the next bar: the real command surface at
> real depth. This map is target × current × the sequenced gap. **Current state is measured
> strictly against `origin/main`** (the working checkout is ~101 commits behind — the pinned-tree
> trap; every number below is from `git show origin/main:`). Targets are clean-room from the
> public DCL Dictionary and VSI utility manuals. Re-derive before acting.

## 0. The honest denominator (what's already good — do not re-solve)

The facade-kill campaign is effectively **complete**: an `origin/main` sweep of all 54 DCL verbs
found **zero** fake-`-S-` successes; unimplemented paths return authentic `%FAC-severity-IDENT`
NOTIMPL/NOTAVAIL. The CLD qualifier-grammar **keystone is built and wired into dispatch**
(`dcl_validate_qualifiers` at `dcl_exec.c:1203`, before the handler). The utility ecosystem has
real coverage (AUTHORIZE, SYSGEN, SYSMAN, MONITOR, MAIL, INSTALL, LINK, LIBRARY, BACKUP, PRODUCT,
HELP, INITIALIZE). 35 F$ lexicals are present. **The problem is not lies anymore — it's breadth
and depth.**

## 1. The shape of the gap — two engines under a breadth expansion

```
                 ┌─────────────────────────────────────────────┐
   BREADTH  ───▶ │  ~54 verbs today  →  ~150 VMS verbs          │  (SET/SHOW umbrellas
                 │  35 F$ lexicals   →  ~43                      │   are ~30 sub-verbs each)
                 └─────────────────────────────────────────────┘
   DEPTH ─┬─▶ Engine A: QUALIFIER GRAMMAR   — CLD tables, 15/54 verbs retrofit → all 54,
          │                                   + real qualifier coverage per verb
          └─▶ Engine B: LOGICAL-NAME / ROOTED-DISK COMPOSITION — the namespace, not aliases
                                              (search lists, concealed/rooted, DISK$label, LD)
```

Everything else (utilities, presentation) hangs off these. **The two engines are the spine
because they are what every other command composes through.**

## 2. DCL command surface — breadth

**Current (`origin/main`):** 54 builtin verbs — ~30 REAL, ~14 PARTIAL, 8 DELEGATED to real
SYS$SYSTEM images, 1 honest-stub (PHONE), 0 facade.
**Target (DCL Dictionary):** ~150 distinct commands. The delta is not just missing verbs — the
**SET** and **SHOW** umbrellas are each ~30+ sub-verbs, most unimplemented, and several DCL verbs
are actually families (DELETE vs DELETE/SYMBOL vs DELETE/ENTRY vs DELETE/KEY; STOP vs STOP/QUEUE).

Biggest missing surface, by group: **scripting** (`CALL`/`SUBROUTINE`, `GOSUB`, `ON`/`SET ON`,
full symbol scoping) is foundational to command procedures and only partial; **queues**
(`START/STOP/INIT/ASSIGN QUEUE`, queue manager); **volumes/devices** (`ALLOCATE`/`DEALLOCATE`,
`SET DEVICE`, `SET VOLUME` depth); **SET/SHOW** long tail; **networking** (`SET HOST`).

## 3. Engine A — qualifier grammar (depth per verb)

**The keystone landed but is a partial rollout.** `dcl_validate_qualifiers` only fires for verbs
that declare a non-NULL `quals` table (`dcl_parser.c:452` short-circuits otherwise). **Only 15 of
54 verbs carry a table**; the other 39 (SET, SHOW, ASSIGN, RUN, MOUNT, …) are still legacy
accept-all — `parse_qualifier` (`dcl_parser.c:54`) stores any `/token` unconditionally.
`SET TERMINAL/FDAFS` *is* now rejected — but by a **hand-rolled per-command allow-list**
(`terminal_known_qualifiers[]`, `dcl_cmd_set.c:246`), not the general grammar; SET PASSWORD/
ACCOUNTING/VOLUME each build a private one-off shim.

Two depth deficits, even on retrofit verbs:
- **Rollout:** 39 verbs need real CLD tables so IVQUAL/IVKEYW is structural everywhere (retires
  the per-command canaries).
- **Coverage:** present tables honor a fraction of VMS's qualifiers — DIRECTORY 10 of ~30,
  COPY 1 of ~20, PRINT 1 of ~40, SUBMIT 1 of ~30, SORT 1 of ~20. Parity means the qualifiers
  *do things*, not just parse.

*This is the `vms-b9a` pillar; `vms-097` (keystone) is "built, 15/54" — the remaining work is the
rollout + per-verb depth, not the mechanism.*

## 4. Engine B — the logical-name & rooted-disk composition layer (the experience spine)

This is where OVMX feels like aliases, not a namespace. **The transparency layer is cosmetic:
the concealed/rooted attributes are dead constants** — `LNM$M_CONCEALED` (`lnmdef.h:49`),
`NAM$M_CNCL_DEV`/`NAM$M_ROOT_DIR` (`nam.h:58-59`), `DVI$_CONCEALED`, `SS$_CONCEALED` are all
**defined and read nowhere**.

| VMS mechanism | Target | `origin/main` state |
|---|---|---|
| `SYS$SYSROOT` = **two-element search list of concealed rooted devices** (`[SYS0.]` specific over `[SYSCOMMON.]` common) | the one structure that makes a common/specific (cluster-shareable) system disk work | **flattened**: single `SYS$SYSDEVICE:[SYS0.]`, attr `0` (not concealed, not a search list). `SYS$COMMON`/`SYS$SPECIFIC` **absent** (`lnm_defaults.c:110`) |
| `SYS$SYSTEM`/`LIBRARY`/`MANAGER`/`HELP` = `SYS$SYSROOT:[…]` (ride the root) | redefine the root, everything follows | **pre-flattened** full paths `SYS$SYSDEVICE:[SYS0.SYSCOMMON.…]` that **dodge composition** — redefining `SYS$SYSROOT` changes nothing (`lnm_defaults.c:119-146`) |
| Search-list read | `$TRNLNM` returns index 0,1,2…; file open tries each | engine exists but **every read collapses to `equiv[0]`** (`vms_kif.c:1411`) — see `vms-ed7`/`vms-b12` |
| `DISK$<label>` auto-logical on MOUNT | reference a disk by volume label, unit-independent | **absent** — `cmd_mount` mounts but defines no logical (`dcl_cmd_misc.c:1739`); `SET VOLUME/LABEL` stubbed |
| Concealed rooted logical as virtual disk root (`FOO:[000000]`) | subtree presents as a disk | **absent** — attrs read nowhere; `DEFINE` can't even set them (see §4.1) |
| **LD** (Logical Disk) — container file as `LDA0:` | file-as-disk virtualization | **absent** — no LDDRIVER/LD anywhere |

### 4.1 The create-side: `DEFINE` can't build these
`cmd_define` (`dcl_cmd_io.c`) parses **neither** `/TRANSLATION_ATTRIBUTES=(CONCEALED,TERMINAL)`
**nor** access modes — it hardcodes `LNM_ATTR_TERMINAL` + `LNM_MODE_USER` on every create. So
there is **no way to create a concealed/rooted logical from DCL** — the direct reason
`SYS$SYSROOT` is flattened: the tool to make it real rejects nothing and applies nothing. (The
structure is otherwise correct: `DEFINE name equiv[,equiv…]`, search-list second arg fixed by
vms-420; `ASSIGN` reversed order honored.) Also: always-`TERMINAL` is the opposite of VMS's
non-terminal default and would break iterative translation the moment TERMINAL is honored.

*This is `vms-ed7` (composition epic): `vms-b12` search-list read, `vms-d8e` concealed/rooted +
DEFINE create-side, `vms-240` TERMINAL/iterative, `vms-69e7` proof = real `SYS$SYSROOT`. **Add:
`DISK$label` on MOUNT, and the LD driver, as disk-transparency items under it.***

## 5. F$ lexical functions

**35 present, ~8 absent** vs the DCL Dictionary: `F$SETPRV`, `F$CSID`, `F$DELTA_TIME`,
`F$FID_TO_NAME`, `F$LICENSE`, `F$MULTIPATH`, `F$CUNITS`. Honesty gap: the unknown-lexical path
(`dcl_lexical.c:2906`) returns an **empty string** rather than a `%DCL` error — the one place the
lexical layer still quietly fakes. F$SETPRV is the notable miss (privilege manipulation in
procedures).

## 6. Utility ecosystem

**Real on `origin/main`:** AUTHORIZE, SYSGEN, SYSMAN, MONITOR, MAIL, INSTALL, LINK, LIBRARY,
BACKUP, PRODUCT, HELP, INITIALIZE, TCPIP-config.
**Genuinely absent / stub:** **MESSAGE** (compiler — how VMS software emits condition codes),
**PATCH**, **standalone SDA** (only ANALYZE/SYSTEM interactive mode exists), a real **CDU/.CLD
compiler** (the qualifier tables are hand-written C — a real CDU would *generate* Engine A),
**ANALYZE/RMS_FILE** (absent), **ANALYZE/OBJECT** (stub), **PHONE**.

**The 10 absences that most break the "real VMS" illusion** (utility agent, ranked): AUTHORIZE
✓done · the logical-name system ✗ (Engine B) · BACKUP ✓ · MOUNT/INITIALIZE ~ · MAIL ✓ · MONITOR ✓
· SYSGEN+AUTOGEN ~ · SDA ✗standalone · INSTALL+LMF ~ · LINKER+LIBRARIAN ✓. **The single biggest
illusion-breaker still open is Engine B**, and the highest-value new tool is a real **CDU** —
because CDU *is* the supported way to build Engine A and to let VMS software install its own DCL
commands (`SET COMMAND`).

## 7. The program — tiered and sequenced

This is bigger than the `vms-b9a` DCL pillar; **`vms-b9a` becomes a phase of it.** Proposed
top-level program: **"VMS command-surface & tools parity"**, three tiers by what a session
touches first, feeding R2 (running real VMS software needs the tools + lexicals + namespace
underneath it).

- **Tier 1 — the every-session spine (do first):**
  1. **Engine B** (`vms-ed7`) — real `SYS$SYSROOT`/concealed/rooted/search-list + `DEFINE`
     create-side + `DISK$label` + LD. This is the "hollow experience" fix; nothing else composes
     without it.
  2. **Engine A rollout** (`vms-b9a`/`vms-097`) — CLD tables on all 54 verbs + qualifier depth on
     the top file/process commands (DIRECTORY, COPY, DELETE, SET DEFAULT, SET FILE…).
  3. F$ lexical completion + honest unknown-lexical error.
- **Tier 2 — system-management depth:** SET/SHOW long tail; queues (START/STOP/INIT QUEUE);
  ALLOCATE/DEALLOCATE/SET DEVICE/SET VOLUME depth; SDA standalone; real CDU/.CLD; MESSAGE.
- **Tier 3 — dev/specialist:** ANALYZE/RMS + /OBJECT, PATCH, DEBUG depth, MMS/CMS, PHONE, DCL
  scripting completeness (CALL/SUBROUTINE/ON/symbol scoping) if not pulled into Tier 1.

**Sequencing vs clustering:** this program owns `src/vmsdcl/**` + `src/vmslnm/**` + `src/vmsrms/**`
+ `tools/**`; clustering owns `src/vmsscs/**`. No contention — runs as the **0.3-x accretion
lane** in parallel to 0.4 clustering, exactly as DCL fidelity already does. Ships incrementally.

## 8. Roadmap position

This is the concrete build-out of "0.3 = a real VMS system" past honesty into depth, and it is the
substrate R2 ("run every VMS app we can find") stands on — an app that calls `F$SEARCH` over a
search-list logical, or installs under a concealed root, needs Engines A+B real. Recommend adopting
it as a named program that parents `vms-b9a` (Engine A) and `vms-ed7` (Engine B), with new Tier-2/3
epics filed as they're funded.
