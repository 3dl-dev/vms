# OVMX Roadmap — Wave Plan (swarm-dispatch execution)

> Purpose: a dependency-wired, outcome-scoped execution plan so `swarm-dispatch`
> can iterate the roadmap in **waves** across clean sessions. A *wave* is simply
> "the set of `rd ready` items at a point in time" — when a wave's items close,
> the dependency graph promotes the next wave to `ready`. There is no separate
> scheduler: **the dep graph IS the wave structure.** Wave labels (`wave-b1`,
> `wave-b2`, …) are a convenience for bulk dispatch, not the source of truth.

This plan supersedes the linear "Phase 8+" backlog. It organizes forward work as
the two co-required rails from `docs/product-vision.md`, converging at migration.

## How to run a wave

1. `rd ready` → the current wave (items with no open blockers).
2. `/swarm-dispatch` the ready items of one rail/wave in parallel (typed agents).
3. Each item is outcome-scoped + self-contained: an agent reads it cold and can
   execute to a verifiable done-condition, gated by CI (green by SHA).
4. Closing a wave's items promotes the next wave. Repeat.

**Invariants every item inherits** (do not restate per item):
- Containerized, aarch64-first. Test loop = arm64 podman; verify CI by SHA
  (`gh run list --workflow CI`). See `CLAUDE.md` + memory.
- VMS-native means: `LINK.EXE` (symbol-vector images) + `IMGACT.EXE` activation,
  **no `ld`/`ld.so`**. Toolchain internals: `docs/design-link-native-toolchain.md`.
- Clean-room rule 8 holds for any format/protocol work.

---

## RAIL B — Run their software (the road to VMS system calls)

The VMS system-service layer already exists (Phase 2: `$ASSIGN`/`$QIO`/`$ENQ`/
event flags/AST/lock-mgr/mailbox/security + `lib$`/`str$`/`mth$`/`ots$`/RMS).
Rail B delivers images to those services the VMS-native way and proves real VMS
software drives them. Spine epic: **`vms-ade`** (toolchain) → **`vms-913`**
(activation infra) → the system-call endpoint.

Foundation ✅ (closed): `vms-b49` spec · `vms-8d5` SV resolver · `vms-714` IMGACT
activation · `vms-142` 2-image E2E · `vms-78f` C-RTL decision · `vms-20b`
production linking (GOT/DATA-imports/`.vms$rel`) · `vms-99c` symbol-vector TLS ·
`vms-fa1` section-merge-by-flags · `vms-80a` **real libvmssys links+activates**.

### Wave B1 — the C runtime (`label: wave-b1`)
Everything above freestanding `libvmssys` needs libc. Decision `vms-78f` = wrap
musl as `DECC$SHR`.
- **`vms-61f.1`** Build `DECC$SHR.EXE`: whole-archive musl `libc.a` (+`-lgcc`
  soft-float builtins) into an OVMX shareable with a `.vms$sv` vector of libc
  universals via `LINK.EXE`. *Done:* OVMXDUMP lists `malloc/free/memcpy/snprintf/
  strlen/…` as universals; valid ET_DYN with PT_TLS for musl's TLS.
- **`vms-61f.2`** IMGACT drives musl runtime init on the `.vms$imp` path:
  program TP, `__init_tls` over the combined TLS image, the `__libc_start_main`/
  `__dls3`-equivalent init, malloc arena — mirroring musl's ldso bootstrap.
  *Done:* a consumer calling `malloc()`+`snprintf()` bound to `DECC$SHR` runs
  VMS-native (correct output, clean exit). *Blocked by `vms-61f.1`.*
- **`vms-a17`** ABS64 / `.rela.data` pointer-initializer relocs in LINK.EXE
  (enabler: higher libs have pointer tables). Independent — runs in B1.
- **`vms-80a` follow-on** productionize the atomics choice (`-mno-outline-atomics`
  in the OVMX-lib build vs shipping the 3 `__aarch64_*_acq_rel` helpers). Small.

### Wave B2 — migrate the OVMX libraries (`label: wave-b2`) — decomposes `vms-b65`
Per the library build order (`CLAUDE.md`): `libvmssys ✅ → vmsprocess → libvms →
{ vmslnm → vmsfs → vmsrms }`. Each item: **compile the real library, link
`LIBX$SHR.EXE` VMS-native with `LINK.EXE` + a symbol vector, activate it through
IMGACT and prove its universals resolve + run.** Wired sequentially per the DAG.
- `vms-b65.1` `vmsprocess$SHR` (needs libvmssys)
- `vms-b65.2` `LIBVMS$SHR` (needs vmsprocess + `DECC$SHR`) — the big runtime
- `vms-b65.3` `vmslnm$SHR` (needs LIBVMS)
- `vms-b65.4` `vmsfs$SHR`  (needs vmslnm)
- `vms-b65.5` `vmsrms$SHR` (needs vmsfs)

### Wave B3 — DCL + the system-call endpoint (`label: wave-b3`)
- **`vms-b65.6`** `DCL.EXE` built + activated **VMS-native** through IMGACT
  (login chain STARTUP→LOGINOUT→DCL); runs a scripted `.COM`/`SHOW TIME` session.
- **`vms-034`** (re-scoped) the above proven as a **CI gate** — VMS-native, not
  the DT_HASH crutch. Extends today's link-only job to "runs correctly."
- **`vms-sys`** ★ endpoint: a **real OpenVMS-built image** activates through
  IMGACT and its `SYS$…` system-service calls **dispatch into the OVMX service
  layer and execute**. Closes the loop "we can activate images" → "activated VMS
  software makes VMS system calls that OVMX answers." Likely decomposes further
  once B2 lands.

> **Deprecated bootstrap path (do not deepen):** the DT_HASH / `--export-dynamic`
> musl-as-interpreter approach (`vms-55f`, and `vms-034`'s original musl-crutch
> framing) is superseded by the VMS-native spine above, per the operator "all
> VMS" ruling recorded in `vms-55f` and memory `vms-native-toolchain-spirit`.
> `vms-55f` is deferred as bootstrap-only; `vms-034` is re-pointed to VMS-native.

---

## RAIL A — Cluster interop (`vms-ci`) — the migration path in
Already decomposed (`ci.0 ✅ → ci.1 ✅ → ci.2 → ci.3 → ci.4 → ci.5 DLM → ci.6
evacuation`; plus `vms-ci.8` node identity, `vms-ce7` satellite boot, kernel
lock-mgr items). Clean-room RE against the SIMH VAX lab (`~/vax/cluster`).
Labels: `wave-a1` = `ci.2`/`ci.8`/`ce7` (dissector + node identity), `wave-a2` =
`ci.3` (appear in `SHOW CLUSTER`), `wave-a3` = `ci.4`/`ci.5` (MSCP + DLM). Rail A
runs in parallel with Rail B; they **converge at `vms-ci.6`** (evacuation needs
both DLM and `vms-913` image activation).

## Compatibility & authenticity (Rail B breadth) — already decomposed
- **`vms-801`** provable source compatibility (corpus ladder `.4`–`.7`). `wave-compat`.
- **`vms-898`** authenticity / indistinguishability (DCL/SHOW/help/queue subs).
  `wave-auth`. Enforced partly as a build-failing invariant.

## Cross-cutting enablers (dispatch as needed, not a wave)
`vms-a17` (ABS64), multi-module TLS (>1 TLS obj/image), x86_64 LINK.EXE + IMGACT
(`vms-913.11`), INSTALL known-image DB (`vms-913.5`), system-disk boot
(`vms-913.7`/`.10`).

---

## The critical path (shortest line to VMS system calls)
```
vms-61f.1 → vms-61f.2 ─┐
                       ├─→ vms-b65.1 → b65.2 → b65.3 → b65.4 → b65.5 → b65.6
vms-a17 ───────────────┘                                                │
                                                          vms-034 (CI) ◀┤
                                                          vms-sys  ◀─────┘
```
Rail A (cluster) proceeds in parallel and joins at `vms-ci.6` for the north-star
rolling-evacuation demo.

---

# Swarm-dispatch DAG series

The work is shaped as an **ordered series of DAGs**, each a single
`/swarm-dispatch` unit sized to one clean session. A DAG's *entry* items are its
`rd ready` leaves; internal edges are real `rd` dependencies (skip `level=epic`
containers — dispatch their leaves). Each DAG ends at a **CI-green-by-SHA gate**.
Select a DAG by label (`dag-1` … `dag-5`; Rail A = `dag-a1` …).

**Rail B — main line (take in order):**

```
DAG-1  C runtime + linker enabler        [READY — take next]
   vms-61f.1 ─▶ vms-61f.2                 (build DECC$SHR ▶ IMGACT musl-init)
   vms-a17    (parallel, independent)     (ABS64 / .rela.data relocs)
   gate: DECC$SHR activation CI job green; ABS64 reloc test green
        │
        ▼
DAG-2  Core runtime migration
   vms-b65.1 ─▶ vms-b65.2                 (vmsprocess$SHR ▶ LIBVMS$SHR)
   gate: LIBVMS$SHR activates; a lib$/sys$ universal runs via IMGACT
        │
        ▼
DAG-3  File / RMS stack migration
   vms-b65.3 ─▶ vms-b65.4 ─▶ vms-b65.5   (vmslnm ▶ vmsfs ▶ vmsrms)
   gate: each $SHR activates through IMGACT
        │
        ▼
DAG-4  DCL VMS-native
   vms-b65.6 ─▶ vms-034                   (build+run DCL ▶ CI gate)
   gate: DCL runs a scripted .COM / SHOW TIME session VMS-native in CI
        │
        ▼
DAG-5  ★ VMS system-call endpoint
   vms-sys  (blocked by DAG-4 + vms-913)  → decompose into a SYS$ corpus ladder
   gate: a real VMS program's SYS$ calls dispatch into OVMX services + execute
```

**Rail A — cluster interop (parallel stream, own series):**

```
DAG-A1  vms-ci.2 (dissector+spec) ∥ vms-ci.8 (node identity) ∥ vms-ce7 (satellite boot)
   ▼
DAG-A2  vms-ci.3  (OVMX appears in real SHOW CLUSTER)
   ▼
DAG-A3  vms-ci.4 (MSCP-served disk) ─▶ vms-ci.5 (DLM $ENQ/$DEQ)
   ▼
        vms-ci.6  rolling evacuation  ← JOINS Rail B here (needs vms-913 activation)
```

**Independent streams** (dispatch opportunistically, no spine dependency):
`vms-801` source-compat corpus ladder · `vms-898` authenticity subs ·
cross-cutting enablers (multi-module TLS, x86_64 `vms-913.11`, INSTALL DB).

## DAG-1 detail (the next unit)
- **Fan-out (parallel, dispatch together):** `vms-61f.1` (whole-archive musl →
  `DECC$SHR.EXE` + `.vms$sv` libc vector) and `vms-a17` (ABS64/`.rela.data` in
  LINK.EXE). Independent — different files, no shared state.
- **Fan-in (after `vms-61f.1`):** `vms-61f.2` (IMGACT drives musl runtime init —
  TP + `__init_tls` + libc init + malloc arena — on the `.vms$imp` path).
- **Exit gate:** a consumer calling `malloc()`+`snprintf()` bound to `DECC$SHR`
  runs for real through `IMGACT.EXE` (new CI job), and the ABS64 reloc test is
  green — both by SHA. Unblocks DAG-2 (`vms-b65.1`).
