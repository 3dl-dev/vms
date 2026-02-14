# OVMX Resource Request: Phases 8-13 (Source Compatibility)

**From**: VMS PM
**To**: CPEO → CEO
**Date**: 2026-02-13
**Type**: Staff Queue — Practitioner (Claude Code compute time)
**Priority**: P1

---

## Request

OVMX is requesting allocation on the shared Claude Code Max 20x subscription to execute Phases 8-13: OpenVMS Alpha source compatibility. This is the core mission pivot — moving from "VMS-inspired environment" to "real VMS C programs compile and run."

The work spans ~20 weeks and consumes ~42% of 50% of the weekly Max 20x budget on average. Opus usage is concentrated in 6 of 20 weeks. The remaining 14 weeks are Sonnet/Haiku only.

---

## What This Delivers

| Phase | Milestone | Validation |
|-------|-----------|------------|
| **8** | Core API gaps closed | 50+ real VMS C programs compile and run |
| **9** | VMS C Runtime Library | Standard C I/O works with VMS file semantics |
| **10** | First real application | MMK (57-file VMS make utility) builds and runs |
| **11** | Fortran support | VMS Fortran programs compile via GFortran + preprocessor |
| **12** | BASIC support | DEC BASIC programs transpile to C and run |
| **13** | DECnet + BLISS | Network connectivity to real VMS systems; BLISS compilation |

---

## Resource Requirement

### Total Budget

| Model | Tokens | Est. API-equivalent cost |
|-------|--------|--------------------------|
| Opus | 15.4M | ~$520 |
| Sonnet | 59.5M | ~$730 |
| Haiku | 10.9M | ~$12 |
| **Total** | **85.8M** | **~$1,260** |

Model routing is optimized — Opus is reserved for 4 design spikes (CHF architecture, CRTL design, Fortran preprocessor design, DECnet NSP architecture). All implementation runs on Sonnet/Haiku.

### Weekly Schedule

20 weeks, targeting 50% of Max 20x weekly budget. Formatted for CEO capacity planning.

```
WEEK  PHASE  OPUS  SONNET  HAIKU  DESCRIPTION
────────────────────────────────────────────────────────────────
 1     8       —    low     low   Conformance harness + corpus fetch
 2     8       —    med      —    sys$fao, sys$synch, sys$getmsg
 3     8      HIGH  low      —    ← Opus spike: CHF architecture
 4     8      med   med      —    CHF impl + missing sys$ services
 5     8       —    low     med   Missing headers (mechanical)
 6     8       —    med      —    Missing RTL + conformance triage
 7     9      HIGH  low      —    ← Opus spike: CRTL architecture
 8     9      med   med      —    CRTL implementation
 9     9       —    med     med   Header audit + type headers
10     9       —    med      —    Fix audit findings, validate
11    10       —    med      —    MMK source audit
12    10       —    med      —    API gap closure for MMK
13    10       —    med      —    Build + validate MMK
14    11+12   HIGH  med      —    ← Opus spike: Fortran design; BASIC lexer
15    11+12    —    HIGH     —    Fortran + BASIC impl (parallel)
16    11+12    —    med     low   Bindings + RTL for both languages
17    11+13   low   med     low   Fortran validation; DECnet design
18     13      —    med      —    ← Opus spike: NSP architecture; impl
19     13      —    med      —    NET: device + FAL/DAP
20     13      —    med     med   BLISS integration + DECnet testing
────────────────────────────────────────────────────────────────

KEY:  — = none  |  low = <15%  |  med = 15-35%  |  HIGH = 35-50%
      Percentages are of the 50% allocation (i.e., 25% of full Max 20x)
```

### Opus Peak Weeks

Only 4 weeks require significant Opus allocation. These are the scheduling constraints:

| Week | What | Why Opus | Can it shift? |
|------|------|----------|---------------|
| 3 | CHF architecture | Novel: setjmp/longjmp stack unwinding + thread safety | No — blocks all of Phase 8 |
| 7 | CRTL design | Novel: stdio interception strategy, ripple effects across stack | No — blocks Phase 9 |
| 14 | Fortran preprocessor design | Novel: language extension semantics | Yes — can shift ±2 weeks |
| 18 | DECnet NSP architecture | Novel: protocol state machine from spec | Yes — can shift ±3 weeks |

Weeks 3 and 7 are on the critical path. Weeks 14 and 18 have scheduling flexibility.

### Non-Opus Weeks

14 of 20 weeks use only Sonnet/Haiku. These are flexible — they can be compressed, expanded, or interleaved with other portfolio work without impacting Opus capacity.

---

## What This Does NOT Require

- No founder hands-on time (all work is AI-side)
- No infrastructure requests (no new repos, services, or domains)
- No external dependencies (test corpus is publicly available)
- No cross-project coordination (OVMX is self-contained)

The only shared resource is Claude Code Max 20x compute time.

---

## Scheduling Flexibility

| Constraint | Flexibility |
|------------|-------------|
| Phase ordering (8→9→10→11/12/13) | **Fixed** — dependency chain |
| Week-to-week within a phase | **Flexible** — can pause/resume between weeks |
| Opus spike weeks (3, 7, 14, 18) | **Weeks 14/18 movable** ±2-3 weeks |
| Sonnet/Haiku weeks | **Fully flexible** — interleave with other projects |
| Overall duration (20 weeks) | **Can stretch to 30** if sharing with other projects |

### Interleaving Scenario

If other portfolio projects also need Max 20x time, OVMX can alternate weeks:

```
Example: OVMX gets 3 weeks on, 1 week off (other project)
 → 20 working weeks becomes ~27 calendar weeks
 → Still within budget, just slower

Example: OVMX gets Opus weeks 3,7 as scheduled, everything else flexes
 → Critical path protected, non-critical work fills gaps
```

### Minimum Viable Cadence

The project stays coherent at **2+ sessions/week**. Below that, context rebuild overhead dominates and efficiency drops. Recommended minimum: 3 sessions/week, even if short.

---

## Dependencies on Other Portfolio Work

None identified. OVMX does not block or depend on Aerocloak, Galtrader, or any other 3DL project. It can be scheduled purely based on available capacity.

If the CPEO identifies cross-portfolio synergies (e.g., shared infrastructure, blog content timing), those can be layered on without changing this request.

---

## Success Metrics

| Milestone | Target Date (from start) | Measurable |
|-----------|--------------------------|------------|
| Phase 8 complete | Week 6 | 50+ VMS programs compile |
| Phase 9 complete | Week 10 | stdio with VMS semantics works |
| Phase 10 complete | Week 13 | MMK builds and runs |
| Phases 11-12 complete | Week 17 | Fortran + BASIC programs compile |
| Phase 13 complete | Week 20 | DECnet connectivity proven |

---

## Reference

- Full technical roadmap: `/home/baron/projects/vms/docs/roadmap-source-compat.md`
- Current beads: `cd /home/baron/projects/vms && bd ready`
- Project CLAUDE.md: `/home/baron/projects/vms/CLAUDE.md`
- Cost analysis: $1,260 API-equivalent over 20 weeks (~$63/week average)
