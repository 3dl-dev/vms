# OVMX Compatibility Surface Register — Design

> **Operator charge (2026-08-13):** "maintain a recording of every functional / compatibility
> surface that exists in VMS and its status in OpenVMX, of systems in scope for 1.0. RTL surface,
> clustering features, etc. This is both an internal artifact, and something we can render on the
> website. It should be comprehensive and also serve as a quick way to determine compatibility
> with corpus software."

## 1. What this is (and what it is not)

The **Compatibility Surface Register** is the single, comprehensive, machine-parseable map of
*every VMS functional/compatibility surface* × *its status in OVMX*, filtered to what is in scope
for 1.0. It answers three questions from one source of truth:

1. **Status** — for each catalogued VMS surface, what is its status in OVMX, and how faithfully
   is it done (real vs facade)? (Counts, never a percentage — see §7a.)
2. **Corpus compatibility** — given a program that calls `LIB$TPARSE`, `SYS$QIO`, `SET HOST`, …,
   is it supported today? (Keyed by canonical symbol/command/feature id → status.)
3. **Roadmap** — what remains for 1.0, per surface.

**What belongs here — and what does not.** This matrix catalogues **VMS compatibility
surfaces**: things a VMS program or user observes or depends on — a callable (`SYS$`/`LIB$`/…),
a command, a file format, a wire protocol, a device, a documented behaviour. Each item's `vms:`
field must describe *a VMS thing*, never an OVMX symbol, ioctl, or internal mechanism.

The **roadmap is broader than compatibility**, and its non-compatibility parts live elsewhere,
**not on this matrix**:

- **OVMX engineering milestones** — self-hosting/bootstrap (tcc gen2==gen3, BUILD.COM),
  reproducible builds → the self-hosting program (`vms-678`) and the release roadmap.
- **Architecture / platform bring-up** — which CPU targets OVMX runs on (x86_64/aarch64/alpha/vax)
  → tracked as a **release dimension** by the release machinery, not as a VMS API surface.
- **Internal implementation & housekeeping** — init wiring, deleted hacks, dead-code residue,
  refactor leftovers. These are notes at most; never their own "surface" row. (`ssh$pid1-wiring`
  — "we removed our own `start_sshd()` call" — was exactly this mistake and was deleted.)

If you cannot name the VMS manual, service, command, or format an item corresponds to, it is not
a compatibility surface — it belongs on the roadmap, not in this register.

It is **not** a replacement for the three existing narrower artifacts; it is their union and
their index:

| Existing artifact | Relationship |
|---|---|
| `docs/compatibility-contract.md` (vms-801.1, 2026-02-19) | Prior art; routine-level for the C source-compat surface only, and **stale** (declares clustering/DECnet/TCPIP permanently out of scope — all now active 1.0 workstreams; Alpha now first-class). Superseded as the coverage record by this register; retained as the source-compat *acceptance-criteria* prose. |
| `docs/design-vms-parity-map.md` (vms-8ad) | The DCL-command-surface slice, with sequencing. This register absorbs its status data and links back to it for the *plan*. |
| `docs/draper-faithfulness-register.md` | The **authenticity** axis — where facades still hide (INV-6/INV-DCL). This register carries an `authenticity` field per item; `facade-risk` rows are the join key back to the Draper register. |

## 2. Design principles

- **Data, not prose.** The source of truth is structured YAML (`docs/compat/`), one file per
  facility. Everything human-facing — the internal markdown register and the website table — is
  **generated** from it. No hand-maintained parallel tables that drift.
- **Continuation, not frozen truth** (`~/.claude/CLAUDE.md`). Each item carries `last_reviewed`
  and `evidence` (a file path). Status is a *dated observation against `origin/main`*, re-derivable
  from the evidence — never a standing belief. The generator's `--check` mode re-verifies evidence
  files still exist and flags stale reviews.
- **Grounded to `origin/main`.** The working checkout is pinned ~100 commits behind (the
  pinned-tree trap). Every status was measured with `git show origin/main:` / `git grep … origin/main`,
  never a bare relative grep.
- **Two orthogonal axes.** `status` = *how much* is implemented (coverage). `authenticity` = *how
  honestly* (does it do the real thing, or fake success). A routine can be `implemented` +
  `facade-risk` — that is a bug, and it is exactly what the Draper register hunts. Keeping the axes
  separate is what prevents "green" from meaning "faithful."
- **Comprehensive at facility level, routine-level where corpus lookup needs it.** Facilities that
  corpus software binds to by symbol (SYS$, LIB$, STR$, MTH$, OTS$, RMS entry points, F$ lexicals,
  DCL verbs) are enumerated at item granularity. Feature facilities (clustering, DECnet, boot,
  install) are enumerated at feature granularity. Both live in the same schema.

## 3. Data model

Source of truth: `docs/compat/`.

```
docs/compat/
  domains.yaml              # the 9 domains, facility order, controlled vocabularies
  facilities/
    str.yaml                # one file per facility
    sys-eventflags.yaml
    lib.yaml
    rms.yaml
    scs-cluster.yaml
    ...
```

### 3.1 Controlled vocabularies (defined in `domains.yaml`, enforced by the generator)

**`status`** — coverage (mutually exclusive, ordered worst→best):

| value | meaning |
|---|---|
| `absent` | Not present. (A design doc may exist — that is `designed`, below.) |
| `designed` | A design record exists; no implementation yet. |
| `stub` | Declared/callable, but honestly returns unsupported / no-op (e.g. `SS$_UNSUPPORTED`). |
| `partial` | Some routines/cases/qualifiers real; material gaps remain. |
| `implemented` | Broadly real for the common cases. |
| `verified` | `implemented` **and** checked against a VMS oracle (lab-1/lab-2/lab-Alpha or a conformance test). |

**`authenticity`** — faithfulness (orthogonal to `status`):

| value | meaning |
|---|---|
| `real` | Does the real thing. |
| `advisory` | Accepts/tracks but does not enforce, **honestly** (e.g. privileges tracked in PCB, no mode transition). Documented, not a lie. |
| `facade-risk` | Returns plausible success without doing the work, or reports per-process state as shared. An **INV-6/INV-DCL bug** — cross-listed to the Draper register. |
| `n/a` | Not a behavioural surface (e.g. a struct layout, a status-code block). |

**`scope_1_0`** — 1.0 inclusion (the "systems in scope for 1.0" filter):

| value | meaning |
|---|---|
| `in` | In scope for 1.0. |
| `stretch` | Wanted, not a 1.0 blocker. |
| `out` | Permanently out of scope (VAX FP formats, DECwindows, ACMS, RDB, Galaxy, ia64, kernel-mode). |
| `undecided` | Scope call not yet made by the operator → raised as an `rd gate`. |

### 3.2 Facility file schema

```yaml
facility: STR$                       # canonical facility / namespace token
name: String Manipulation RTL
domain: programming-interfaces       # one of the 9 domains in domains.yaml
vms_ref: "OpenVMS RTL (STR$) Manual" # public-doc citation (clean-room provenance)
scope_1_0: in                        # facility-level default; items may override
tier: 1                              # optional: source-compat tier (1/2/3) from the contract
plan_ref: vms-b9a                    # optional: rd item / design doc that owns the remaining work
summary: "Descriptor-based string routines."
last_reviewed: 2026-08-13
items:
  - id: str$copy_dx                  # canonical symbol / command / feature — the lookup key
    kind: routine                    # routine|command|lexical|feature|protocol|struct|status-block|utility|subsystem
    vms: "Copy source string to a CLASS_S/CLASS_D descriptor"
    status: implemented
    authenticity: real
    evidence: src/libvms/rtl/str_routines.c
    scope_1_0: in                    # optional; inherits facility default if omitted
    verified_against: null           # oracle citation when status: verified
    notes: ""
```

## 4. Generated outputs

`tools/compat/render_compat.py` reads `docs/compat/` and emits:

1. **`docs/compatibility-surface.md`** — the internal comprehensive register: per-domain sections,
   per-facility tables, rollup counts (status + authenticity breakdown, and V1
   met/in-progress/not-started counts per facility/domain — no percentages),
   and a top-line dashboard. Regenerated, never hand-edited.
2. **`build/compat-surface.json`** — the machine export: the flat list of every item plus rollups,
   consumed by (a) the website and (b) the corpus-compat lookup tool. This is the artifact that
   makes "is symbol X supported?" a lookup.

`--check` mode (for CI): validates every facility file against the vocabularies, requires
`verified_against` when `status: verified`, warns when an `implemented`/`verified` item's
`evidence` file is absent on the current tree (drift signal), and warns on `last_reviewed` older
than a threshold. It does **not** auto-flip status — status is a human observation — but it makes
drift loud.

## 5. Website rendering

The website repo (`3dl-dev/openvmx-site`, GH Pages, `openvmx.3dl.dev`) consumes
`build/compat-surface.json` and renders an interactive, filterable table: filter by domain /
status / scope, a search box keyed on `id` (the corpus-lookup UX), and a coverage dashboard. The
JSON is the contract between the repos; the product repo remains the single source of truth.

## 6. The 9 domains (facility taxonomy)

Grounded in the current header/source inventory on `origin/main`:

- **A. Programming interfaces** — descriptors; status/condition codes; SYS$ system services (time,
  event flags, ASTs, logical names, process control, process/system info, memory, I/O+QIO,
  mailboxes, locks, security services, FAO, message); RTL LIB$/STR$/MTH$/OTS$/SMG$; condition
  handling (CHF); RMS programmatic (FAB/RAB/NAM/XAB + entry points + orgs + record formats).
- **B. File system & storage** — ODS-2 on-disk + versioning + file IDs; filespec/wildcard/parse;
  devices (DVI, MOUNT/INIT); logical-name namespace (search lists, concealed/rooted, DISK$); FDL.
- **C. Command language & environment** — DCL verbs; DCL scripting/flow ($STATUS, symbols, CALL,
  ON, DECK); qualifier grammar (CLD/CDU); F$ lexicals; utilities; HELP; queues/batch/print.
- **D. System management & security** — SYSUAF/accounts/login; privileges; rights DB/identifiers;
  protection/UIC/ACLs; auditing/break-in; SYSGEN/AUTOGEN; boot; install/PCSI; accounting.
- **E. Clustering** — SCS; NISCA/NISCS (+ cluster-over-IP); connection manager/membership/quorum;
  cluster-wide DLM; MSCP/TMSCP serving; cluster-wide logical names/global sections; volume shadowing.
- **F. Toolchain & images** — object format; image format + activation (IMGACT, shareable/installed
  images); symbol vectors/GSMATCH/ident; LINK.EXE; LIBRARIAN; MACRO/assembler; MESSAGE compiler;
  MMS/MMK; self-hosting compiler.
- **G. Networking** — TCP/IP Services (UCX QIO + C sockets + EWA0:); DECnet Phase IV; LAT; SSH.
- **H. Executive** — the VMS executive itself: access modes, the process model (PCB), scheduling,
  and the executive-backed IPC underneath the system services. (Architecture/platform support and
  OVMX self-hosting are roadmap concerns — release dimensions and the self-hosting program — not
  compatibility surfaces, and are deliberately absent here.)
- **I. Languages & compilers** — compilers (Fortran/COBOL/BASIC/Pascal/MACRO/Ada/PL/I/BLISS/…),
  their language RTLs (`FOR$`/`COB$`/`BAS$`/`PAS$`), and the OpenVMS Calling Standard. OVMX has
  one language today: C, via tcc.

## 7. Maintenance — keeping it a living artifact

- **Design-change cascade hook.** The CLAUDE.md architecture-change cascade gains a 4th downstream
  step: *Compatibility Register Update* — any change to a public surface updates the relevant
  facility YAML in the same PR. The `--check` gate makes an un-updated register visible.
- **Ownership.** Rolls up under the VMS Parity Program (`vms-8ad`); each domain has an owning
  epic/pillar already (RTL/source-compat `vms-801`, DCL `vms-b9a`, clustering `vms-ci`/`vms-694`,
  networking `vms-67f`/`vms-30e`, toolchain `vms-ade`, authenticity `vms-898`). The register
  references those via `plan_ref` rather than duplicating their plans.
- **Re-derive, don't recall.** `last_reviewed` dates are the trust signal. A row is only as good as
  its last measurement against `origin/main`.

## 7a. No percentages — an inventory, and a V1 commitment set

**Operator ruling (2026-08-13): we do not put a percentage on the compatibility
surface.** A percentage needs a known denominator, and the total VMS compatibility
surface has none — it is fixed, vast, and **not version-scoped**; picking V1
targets does not shrink it, and it cannot be counted. Any "X% compatible" — even a
"1.0-scope coverage index" — fabricates a denominator and conflates *what we chose
for V1* with *the actual surface*. An earlier revision of this register did exactly
that; it was wrong. So the register reports **two count-based views, and no
percentage of the whole:**

1. **The inventory (the actual surface).** Absolute counts of catalogued surfaces
   by status and authenticity. It is stated as **incomplete by construction** — an
   inventory that grows as surfaces are identified, never a census of all VMS.
   There is deliberately no "N% done" line here, because the denominator is
   unknown.
2. **V1 readiness (a set we define).** Of the surfaces **committed to V1**
   (`scope_1_0: in` — an enumerable list *we* own), counts of *met*
   (implemented/verified), *in progress* (partial), and *not started*
   (absent/stub/designed), plus how many committed surfaces still carry
   facade-risk. This is honest because the denominator is a commitment list we
   control — but it is reported as **counts against that list**, and framed so it
   can never be read as a fraction of VMS. Surfaces that are `out`, `stretch`, or
   `undecided` are *not* in the V1 denominator but *remain in the inventory*, so
   deferring work never improves the numbers.

**The Languages & Compilers frontier** is why the distinction bites. OVMX has one
language (C, via tcc); Fortran/COBOL/BASIC/Pascal/MACRO/Ada/… are `absent`, most
`undecided` pending an operator scope call (`vms-082`) — the "run corpus software"
goal (R2) leans on COBOL/Fortran, so they are not automatically out. Cataloguing
them **added 22 absent surfaces to the inventory** and did *not* flatter any V1
count. If those `undecided` surfaces are ruled **into** V1, they join the V1
denominator at status `absent` and the *not-started* count jumps — exactly the
honest signal a scope decision should produce.

**The guard, stated plainly:** cataloguing more of the real VMS surface should make
the picture look *less* complete, never more. If adding real surface makes a number
improve, someone scoped the new surface out (or invented a denominator) to protect
the score — stop and fix it.

## 8. Roadmap position

This is the connective tissue over the whole 1.0 program: it turns "run every VMS app we can find"
(R2) into a measurable, per-symbol coverage number, and gives the website a live, honest coverage
story. It supersedes the coverage role of the stale compatibility contract without discarding the
contract's acceptance-criteria prose.
