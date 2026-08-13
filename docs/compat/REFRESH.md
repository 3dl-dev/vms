# Refreshing the Compatibility Surface Register — repeatable runbook

The register (`docs/compat/`) is a **living** artifact: its status column is a
*dated observation against `origin/main`*, not stored truth. This runbook is the
repeatable procedure for keeping it current, and how that procedure is wired into
the release-engineering train (pillar `vms-a84`).

There are two refresh modes — a cheap **incremental** update on every surface
change, and a periodic **full re-census**. Both end at the same gate:
`render_compat.py --check` green and the generated views regenerated.

---

## Mode 1 — Incremental update (per change; the common case)

Triggered by the **architecture-change cascade** (CLAUDE.md): any PR that changes
a public surface — `starlet.h`/`ssdef.h`, a descriptor/RMS/`vms.ko`/DCL/CLD
surface, boot/init, a cluster protocol, a toolchain/image format — updates the
relevant `docs/compat/facilities/<token>.yaml` **in the same PR**:

1. Edit the item(s): set `status`, `authenticity`, `evidence` (a path on the new
   tree), and `scope_1_0` if it differs from the facility default. `status:
   verified` **requires** `verified_against` (name the oracle/test).
2. Bump the facility's `last_reviewed`.
3. `python3 tools/compat/render_compat.py` and commit the regenerated
   `docs/compatibility-surface.md` alongside the YAML.
4. CI runs `render_compat.py --check`; an un-updated register is loud (evidence
   drift + staleness warnings), not silent.

Do **not** auto-flip status from a passing test — status is a human judgement of
coverage; a green local test on a `partial` facility keeps it `partial`.

---

## Mode 2 — Full re-census (periodic / per-release / on demand)

Re-measures every facility against `origin/main`. This is how the register was
first built and how it is periodically re-grounded. It is agent-driven because
measuring "how faithfully does OVMX implement X" means reading code, not running
a script. Automated form: the saved workflow **`compat-refresh`**
(`.claude/workflows/compat-refresh.js`) — invoke it (`Workflow`, opt-in) to run
the whole fan-out; or drive it by hand as below.

### The invariant that makes it trustworthy

**Measure against `origin/main` only.** The conductor checkout is pinned stale;
a bare relative grep reads old code and yields false conclusions. Every census
agent uses `git show origin/main:<path>` / `git grep <pat> origin/main`. (When
run this way the `--check` "evidence not found" warnings from a pinned checkout
are false positives — they vanish against a real `main` checkout.)

### The 6 census clusters

Fan out one census agent per cluster (sonnet). Each returns compact per-facility
rows: `facility · surface · VMS target · OVMX status · authenticity · evidence ·
notes`, naming gaps individually.

| Cluster | Facilities |
|---|---|
| Core RTL + system services | descriptors, status-codes, chf, lib/str/mth/ots, sys-* (time, eventflags, ast, logicalnames, process, procinfo, memory, io, mailbox, lock, fao-msg, security) |
| RMS + files + logicals | rms-api, ods2, filespec, devices, logical-namespace, fdl |
| DCL + utilities + queues | dcl-verbs, dcl-scripting, dcl-qualifiers, lexicals, utilities, help, queues |
| Security + sysmgmt + boot/install | sysuaf, privileges, rights-db, protection-acl, audit, sysgen, boot, install, accounting |
| Clustering + executive substrate | kernel-executive, scs, nisca, connection-manager, cluster-dlm, mscp-serve, cluster-logicals, shadowing |
| Toolchain + networking | object-format, image-activation, symbol-vectors, link, librarian, macro, message-compiler, mms-mmk, tcpip-services, decnet, lat, ssh, smg |
| Languages & compilers | compilers, language-rtl, calling-standard |

### Steps

1. **Census** — dispatch the 6 agents; each measures its cluster on `origin/main`.
2. **Populate** — transcribe each cluster's findings into `facilities/*.yaml`,
   mirroring `facilities/str.yaml`. Enumerate corpus-critical facilities (SYS$,
   LIB$, OTS$, RMS entry points, F$ lexicals, DCL verbs) at routine granularity;
   feature facilities at feature granularity. Every named gap is its own item.
3. **Audit** (credibility gate — do not skip):
   - Every `status: verified` must cite a **real oracle** (lab-1/2/Alpha, mined
     captures, a CI negctl gate, a fixpoint) — not a plain local self-test. A
     `partial`-coverage facility cannot be `verified`.
   - `authenticity: facade-risk` only where a surface reports success without
     doing the work / fakes shared state per-process. These join the Draper
     register.
4. **Render + check** — `render_compat.py` then `render_compat.py --check`
   (against a real `main` checkout). Zero errors.
5. **Land** — branch off `origin/main` (use a worktree; do not disturb the
   pinned checkout), commit the YAML + regenerated `.md`, PR, reap green.

---

## Release-engineering tie (pillar `vms-a84`)

The register ships **through** the release machinery, like everything else:

- **Snapshot at cut.** `tools/cut-release.sh` runs `tools/compat/snapshot.py
  --out-dir dist/release-<version>/`, which writes the tracked
  `docs/compat/snapshots/<version>.json` (per-item coverage, for trend) and a
  `compat-coverage.json` into the bundle, and prints the "Compatibility surface"
  Markdown block (counts + V1 met, and the delta vs the previous snapshot: surfaces
  advanced, facades resolved, regressions).
- **Notes.** `tools/gen_release_notes.py` embeds that block, so every release
  states its coverage and what moved since the last one — generated, never
  hand-written (vms-55a).
- **Gate.** `render_compat.py --check` runs in CI (drift gate); the release
  acceptance step additionally requires the cut's snapshot to be present and the
  register to validate clean.
- **Website.** The snapshot series feeds the coverage trend on
  `openvmx.3dl.dev`.

Net: a coverage number and a per-surface delta are a **standing release artifact**,
refreshed every cut, not a one-off audit.
