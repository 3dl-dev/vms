# Design — DCL Surface Fidelity (the table-driven grammar retrofit)

> **Authorization (operator, 2026-08-11).** DCL hardening is **executive-sized work** —
> the same class as the `vms.ko` executive retrofit (`vms-6b8`), which took days and was
> big work, not a patch sweep. It ships **incrementally as `0.3-x` point releases**
> ("0.3-x is everything we accrete on our march"). **Clustering e2e stays `0.4`** and runs
> in parallel — it is a thin serial chain of hard blockers (~25 deep) and contends with
> this pillar for no files.
>
> This doc is the artifact; the rd items point at it. It records what was **observed** in
> the audit (raw), not a frozen verdict — re-derive counts before acting on them.

## 1. The finding

An `Explore` audit of `src/vmsdcl/` (2026-08-11) classified all 54 top-level verbs:
**~34 REAL · ~13 PARTIAL · ~5 FACADE · ~2 STUB.** The file/queue/process **core is
genuinely implemented**; the `%FACILITY-S-IDENT` error-format layer and the entire `SHOW`
layer are **honest** (deliberate blanks over fabrication). The rot is concentrated in two
places, and the first is architectural.

## 2. Root cause: the facade *is* the architecture

**The qualifier grammar is unvalidated by construction.** `struct dcl_verb`
(`src/vmsdcl/include/dcl/cdu.h`) carries a single boolean `CDU_F_QUALIFIER` flag —
**there is no per-verb declaration of legal qualifiers, value-types, defaults, or
placement anywhere.** The name "CDU" (Command Definition Utility) is aspirational: the
header defines flags but no command definitions.

`parse_qualifier()` (`dcl_parser.c:42-105`) takes any token the lexer tagged
`TOK_QUALIFIER`, upcases it into `cmd->qualifiers[]` with `present=1`, and **never
consults the verb** — it always returns 0 (accept). Dispatch (`dcl_exec.c:1195-1208`)
calls `verb->handler(cmd)` with no qualifier check in between. Handlers read qualifiers by
**positive lookup** (`dcl_has_qualifier`), which only ever asks "is X present?" A qualifier
no handler asks about is silently dropped.

**`%DCL-W-IVQUAL` is therefore structurally unreachable.** `SET TERMINAL/FDAFS` →
`SS$_NORMAL`, no error, no effect. This is not a missing check on one command; it is the
design, and it affects **all 54 verbs**.

**The proof it is architectural, not incidental:** `RUN` had to hand-roll its own
honour/refuse layer (`run_refuse_unhonourable`, `dcl_cmd_process.c:940+`) *because the
shared machinery offers no way to reject a qualifier*. Its own comment concedes
`dcl_has_qualifier()` "is used by commands that have no qualifier table at all." RUN is the
exception that indicts the rule.

The second locus is a cluster of **fake-success commands** — plausible output / `-S-`/`-I-`
status while doing little or nothing: ASSIGN (creates a DCL *symbol*, not a logical name,
and silently discards `/SYSTEM /JOB /GROUP /TABLE`), MOUNT (registers a name → `getcwd()`,
ignores the volume), PRODUCT SHOW HISTORY (`time(NULL)` reported as the install date),
SET AUDIT/ACCOUNTING/PASSWORD (toggle dead per-process bools), SHOW LICENSE (invented LMF
rows), HELP (`printf` masquerading as a hierarchical library), STOP (ignores its target and
tears down the shell), PHONE / SET HOST / SET VOLUME (`SS$_NORMAL` on an unimplemented op),
INQUIRE `/NOPUNCTUATION` (right name, wrong semantics).

## 3. The anti-facade invariant (extend INV-6 to the surface)

INV-6 ("never fake per-process success; fail honestly, `SS$_NOSUCHDEV`") was pointed only
at the executive / `/dev/vms` layer. The DCL surface — the first thing a human touches — was
never brought under it. **INV-DCL** states the same rule one layer up:

> A DCL command must either implement the VMS semantics of the syntax it accepts, or reject
> that syntax with the authentic VMS error (`%DCL-W-IVQUAL`, `IVKEYW`, `%…-NOTIMPLEMENTED`).
> **Silent qualifier acceptance and fake-success are the two banned classes.** A command
> that prints `-S-`/`-I-` while doing nothing is a worse tell than an honest error, because
> it appears to work — the uncanny-valley ocean.

## 4. Target architecture — table-driven command definitions (CDU/CLD-faithful)

This is how VMS actually does it: DCL is a thin parser over `DCLTABLES`, built from `.CLD`
command definitions by the Command Definition Utility. Each verb *declares* its qualifiers,
value-types, defaults, negations, and placement; the parser validates the line against the
declaration before any handler runs.

Retrofit `struct dcl_verb` to carry a **qualifier table** (name, value-type
`none|value|list|keyword`, default, `/NO` form, placement). The parser then, for free and
uniformly across all 54 verbs, produces: `%DCL-W-IVQUAL` (unknown qualifier),
`%DCL-W-IVKEYW` (bad keyword value), value-type / required-value checks, and
mutually-exclusive-qualifier conflicts. Handlers stop parsing strings and read a validated,
typed structure. **One structural change closes the universal silent-accept hole.**

Clean-room (Rule 8): the CDU/CLD grammar and `DCLTABLES` structure come from the public VMS
**Command Definition Utility** manual and the **DCL Dictionary** plus observed behaviour.
Qualifier **names** and error text are grounded to the oracle/docs and cited — never invented.

## 5. Phased plan (outcomes, not layers)

- **Phase 0 — Make the instrument honest** *(do first; mirrors R2.0's corpus fix).* Commit a
  per-verb DCL-fidelity scoreboard (REAL/PARTIAL/FACADE/STUB) **and** a CI gate that asserts
  `%DCL-W-IVQUAL` fires on a bogus qualifier and that named facades return honest errors. A
  metric that cannot fail is not a gate. Converts "lies everywhere" into a ranked,
  falsifiable, regression-protected worklist.
- **Phase 1 — The keystone: CLD-style qualifier grammar + parser validation.** `IVQUAL`
  becomes reachable; the universal silent-accept dies across all verbs. Design-cascade-sized
  (touches `dcl_parser.c`, dispatch in `dcl_exec.c`, the verb struct, and every handler's
  qualifier reads). Highest leverage on the board.
- **Phase 2 — Eliminate the fake-success commands** *(each an outcome, ranked by the audit's
  TOP LIES):* ASSIGN → real LNM + `/TABLE`; MOUNT → real volume; SET AUDIT/ACCOUNTING/PASSWORD;
  PRODUCT history; SHOW LICENSE; STOP target semantics; PHONE / SET HOST / SET VOLUME;
  INQUIRE `/NOPUNCTUATION`.
- **Phase 3 — HELP is a real hierarchical library** (HLB / `Information available:` / `Topic?`
  navigation). Folds `vms-1c5` (A2).
- **Phase 4 — Presentation fidelity.** Login/logout sequence (`vms-46b`, A4 — gated on shared
  logical names `vms-d37`), terminal-characteristic V7.3 names (`vms-2cb`).
- **Cross-cut veracity.** Every replaced facade ships with a test that would **fail on the
  facade** (the INV-DCL gate), so the scoreboard cannot be re-faked.

## 6. Relationship to the existing board

The DCL-presentation items (`vms-1c5` HELP, `vms-46b` login, `vms-2cb` terminal names) hang
under **`vms-898`, the authenticity epic that `docs/roadmap-v1.md` §5 froze until 1.0** —
which is why this surface stalled. This ruling **promotes DCL surface fidelity to a funded
`0.3-x` pillar**, out from under the freeze. §5's own exemption already covers it: "work a
meta-layer item [only] when it gates a claim the release actually makes" — an interactive
session that lies is the most-touched claim OVMX makes. The above items are folded/depended
under the new pillar epic; the keystone (Phase 1) is new and owns the architecture.

## 7. Release mapping

`0.3-x` accretion (0.3-2, 0.3-3, …) ships each phase as it lands, through the release
machinery (`vms-a84`). `0.4` remains clustering e2e, in parallel. No file contention: this
pillar owns `src/vmsdcl/**`; clustering owns `src/vmsscs/**`.
