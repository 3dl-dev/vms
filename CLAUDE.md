# CLAUDE.md — OVMX Project Instructions

> OS-level instructions (session protocol, beads workflow, model routing, blog pipeline, rules) are inherited from `~/.claude/CLAUDE.md`. This file contains only project-specific configuration.

## Project

**OVMX**: OpenVMS-compatible environment for Linux. Implements VMS system services, DCL shell, RMS file system, and kernel modules — packaged as a bootable distro.

## Project Status

Phases 1-7 are complete. OVMX runs as a bootable QEMU VM — the single runtime target (Rule 9). (The root Dockerfile + docker-compose.yml, the dead-legacy glibc product container, were deleted by vms-71a; CI runs entirely on the QEMU path or plain build tooling now.) See `tracking/roadmap.md` for phase history and `docs/product-vision.md` for scope.

## Quick Reference

```bash
# Development build (shared libs, readline, tests)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure

# Static build (musl)
cmake -B build-static -DCMAKE_C_COMPILER=musl-gcc -DOVMX_STATIC=ON -DBUILD_TOOLS=ON
cmake --build build-static -j$(nproc)

# Bootable QEMU VM — THE runtime (see Project-Specific Rule 9).
# vms.ko provides the VMS executive; userspace reaches it via /dev/vms.
docker build -f Dockerfile.bootable -o dist .
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz

# Kernel modules (needs kernel headers)
make -C src/kernel
make -C src/kernel/vmsfs

# QEMU kernel module tests
tests/qemu/run_tests.sh
```

## Agent Roster

| Agent | Spec | Domain | Default Tier |
|-------|------|--------|-------------|
| Manager | `.claude/agents/manager.md` | `*` | inherit |
| Systems Engineer | `.claude/agents/systems-engineer.md` | `src/**` | sonnet |
| QA Engineer | `.claude/agents/qa-engineer.md` | `tests/**`, `.github/workflows/**` | sonnet |
| Technical Writer | `.claude/agents/technical-writer.md` | `docs/**` | sonnet |
| Implementer | `.claude/agents/implementer.md` | `*` | sonnet |
| Reviewer | `.claude/agents/reviewer.md` | `*` (read-only) | sonnet |
| Blog | `.claude/agents/blog.md` | `docs/blog/**` | sonnet |

**Routing rules:**
- System service / DCL / kernel / RMS / assembly implementation → Systems Engineer
- Testing / CI/CD / static analysis / build validation → QA Engineer
- Documentation / API reference / guides / admin docs → Technical Writer
- Blog outline → post authoring → Blog
- Everything else (prioritization, decisions, coordination) → PM

## Team

VMS uses a three-role worker structure managed by the OS scheduler (`os next` command).

### Profiles

Worker profiles live in `.claude/profiles/`:

- **manager.md** — persistent agent (long-running interactive session). Decomposes parent beads, assigns work to implementers/reviewers, reviews completed work, escalates blockers, reports status to CPEO.
- **implementer.md** — ephemeral agent (one bead per session). Receives a focused bead with domain context hint (Systems, QA, TechWriter). Writes code/tests/docs, commits, pushes branch, closes bead.
- **reviewer.md** — ephemeral agent (code review sessions). Reviews implementer branches for correctness, style, test coverage, domain fit before manager approves merge.

### Domain Routing

Beads carry a context hint indicating the domain specialty:

| Hint | Routed to | Example Work |
|------|-----------|--------------|
| **Systems** | Implementer (Systems focus) | VMS system service, DCL feature, kernel module, RMS implementation, assembly |
| **QA** | Implementer (QA focus) | Test infrastructure, CI/CD, static analysis, build validation |
| **TechWriter** | Implementer (TechWriter focus) | API documentation, command reference, user guides, blog drafts |

Manager reads domain hints from beads and assigns work accordingly.

### Workflow

1. Manager decomposes large beads and assigns domain context hints
2. `os next` picks a ready bead, reads the implementer profile, launches implementer session in a git worktree
3. Implementer executes the bead scope (code/tests/docs), pushes branch, closes bead
4. Manager (or separate reviewer session) reviews the branch against the bead scope
5. Manager approves merge (if tests pass, scope met, quality OK) or requests changes
6. Manager escalates blockers or design questions to CPEO via beads

## Task-Type → Model Mapping

| Task Type | Model | Rationale |
|-----------|-------|-----------|
| Novel VMS system service design, kernel module architecture, cross-component trade-offs | **Opus** | Requires deep VMS compatibility reasoning and multi-layer architectural synthesis |
| System service implementation, DCL features, test design, spec writing, documentation | **Sonnet** | Structured implementation within well-defined VMS constraints |
| CMakeLists updates, config file edits, mechanical refactoring, template-driven doc work | **Haiku** | Mechanical work with clear patterns |

## Token Optimization

**Standing order: minimize token utilization.** See `~/.claude/CLAUDE.md` for the full policy. Key points:
- Default to Haiku for mechanical work
- Use tools and `--json` parsing over LLM interpretation
- Concise subagent prompts with file refs, not pasted content
- One focused task per agent dispatch
- No verbose outputs — write the deliverable, not a summary of it

## Design Change Cascade

**Every design/architecture change MUST trigger these downstream beads:**

A "design change" is any bead that modifies:
- System service API (`starlet.h`, `ssdef.h`, public headers)
- VMS descriptor format (`descrip.h`, `descrip.c`)
- Kernel module interfaces (`vms.ko`, `vmsfs.ko`)
- DCL syntax or builtin behavior
- RMS data structures (FAB, RAB, NAM, XAB)
- Boot sequence or init process

```
Design Change (parent)
├── 1. API Compatibility Check (P1, blocked by parent)
│      Route to: Systems Engineer
│      Assess: Does this break existing code that uses the changed API?
│      Output: go/no-go + list of breaking changes
│
├── 2. Test Coverage Check (P2, blocked by #1)
│      Route to: QA Engineer
│      Assess: Are the affected code paths tested? New tests needed?
│      Output: test coverage report + new test bead if gaps found
│
└── 3. Documentation Update (P3, blocked by #2)
       Route to: Technical Writer
       Assess: Do docs reflect the change? (API reference, architecture.md, guides)
       Output: updated documentation or "no doc impact"
```

## Source of Truth Hierarchy

When artifacts disagree, resolve conflicts in this order:

1. **OpenVMS reference documentation** — VMS compatibility is the project goal; when in doubt, match VMS behavior
2. **docs/architecture.md** — system design decisions and trade-offs specific to OVMX
3. **CLAUDE.md** — project conventions and workflow (this file)
4. **Source code + tests** — implementation is authoritative for "what it does now"

## Artifact Conventions

- **C source**: C11 with GNU extensions, `-Wall -Wextra`, VMS naming (`sys$`, `lib$`, `str$`, `mth$`, `ots$`)
- **Headers**: Public headers in `src/*/include/`, system headers (`starlet.h`, etc.) expose VMS API
- **Assembly**: x86_64 and aarch64 in `src/libvmssys/arch/`
- **Kernel modules**: Standalone Makefiles in `src/kernel/` and `src/kernel/vmsfs/`
- **Specs and plans**: Structured markdown in `docs/`
- **Tracking**: Beads for active work. `tracking/` files are historical reference — do not delete, but create new work as beads
- **Tests**: Integration in `tests/integration/`, QEMU kernel tests in `tests/qemu/`, unit tests in `tests/libvmssys/`
- **Build configs**: CMakeLists.txt hierarchy, Dockerfile.bootable (builds the runtime), src/kernel/Dockerfile + tests/qemu/Dockerfile (build/test tooling). The root Dockerfile + docker-compose.yml (glibc product container, Rule 9) are deleted.

## Library Build Order (Dependency Graph)

```
libvmssys          (freestanding, no deps)
  └─ vmsprocess    (+ pthread)
       └─ libvms   (+ pthread, m)
            ├─ vmslnm   (+ pthread)
            │    └─ vmsfs
            │         └─ vmsrms
            └─ vmsdcl   (+ vmsfs, vmsprocess, optional readline)
```

## Repo Structure

```
vms/
├── CLAUDE.md              # This file — project instructions
├── README.md              # User-facing project overview
├── CMakeLists.txt         # Top-level build configuration
├── Dockerfile.bootable    # Bootable distro builder (QEMU mode)
├── boot.sh                # QEMU boot wrapper
├── src/                   # Source code
│   ├── libvmssys/         # Freestanding syscall layer (no glibc)
│   ├── libvms/            # VMS runtime (system services + RTL)
│   ├── vmsprocess/        # Process control blocks, ASTs, event flags
│   ├── vmslnm/            # Logical name manager + daemon
│   ├── vmsfs/             # VMS filesystem library
│   ├── vmsrms/            # Record Management Services
│   ├── vmsdcl/            # DCL shell (lexer, parser, executor, builtins)
│   ├── kernel/            # Kernel modules (vms.ko, vmsfs.ko)
│   └── ovmx_init/         # PID 1 boot orchestrator
├── tests/                 # Test programs
│   ├── libvmssys/         # Freestanding library unit tests (7)
│   ├── integration/       # End-to-end tests (4)
│   ├── qemu/              # Kernel module tests (5 programs, 62 assertions)
│   └── conformance/       # VMS compatibility tests
├── tools/                 # Utilities (vms_login, vms_help, vms_ssh_auth)
├── distro/                # Root filesystem configs, boot scripts
│   ├── boot/              # init-wrapper.sh, run-qemu.sh
│   └── rootfs/            # etc/ovmx/, etc/ssh/, vms/sys$manager/
├── docs/                  # Documentation
│   ├── architecture.md    # System architecture
│   ├── building.md        # Build instructions
│   ├── product-vision.md  # Product vision and key decisions
│   ├── agent-*.md         # Agent specifications
│   └── blog/              # Blog pipeline
├── tracking/              # Historical tracking (read-only reference)
│   ├── roadmap.md         # Phase completion history
│   ├── status.md          # Status snapshots
│   └── backlog.md         # Enhancement ideas (migrated to beads)
└── .beads/                # Beads database (git-tracked)
```

## Project-Specific Rules

1. **VMS compatibility first**: When OpenVMS reference docs and implementation ease conflict, match VMS behavior unless there's a compelling reason documented in `docs/architecture.md`.
2. **Preserve tracking/ files**: Do NOT delete `roadmap.md`, `status.md`, or `backlog.md`. These are historical records. Create new work as beads.
3. **Freestanding libvmssys**: Code in `src/libvmssys/` must compile with `-ffreestanding -fno-builtin` and no glibc dependency.
4. **Status codes**: All VMS APIs return VMS status codes (odd = success, even = error). Use `ssdef.h` constants.
5. **Dual architecture**: x86_64 is primary, aarch64 is supported. Test on both when feasible.
6. **Kernel module testing**: Use QEMU-based test infrastructure (`tests/qemu/`) for kernel modules — never load untested modules on the host.
7. **DCL compatibility**: DCL should match OpenVMS DCL behavior, not bash. When in doubt, reference OpenVMS documentation.
8. **Clean-room VMS RE (HARD INVARIANT)**: All reverse-engineered VMS formats and protocols are derived ONLY from (a) observing behavior (the reference lab wire / documented tool output — SDA/SYSGEN/SYSMAN/LINK) and (b) public OpenVMS documentation. This covers **both**:
   - **Cluster wire protocols** (SCS/NISCA/NISCS/MSCP/DLM) — from lab observation + the Cluster Systems manual, IDSM, `$SSDEF`/`$LCKDEF`, etc.
   - **The link/image toolchain** (object/image/symbol-vector/GSMATCH/ident formats for LINK.EXE + IMGACT) — from the VSI OpenVMS Linker Utility Manual, the I64/x86 porting guides, and other public docs. Where the public docs do NOT publish a byte-level layout, OVMX defines its **own** representation and LABELS it as an OVMX design choice — never presented as VMS-authentic (see `docs/design-link-native-toolchain.md`).

   NEVER disassemble, decompile, or copy VSI/HPE source or binaries; never paste leaked VMS source. This is what makes the interop RE legally protected (DMCA 1201(f), EU SW Directive Art. 6) — it is not optional.

   **Where the reference lab is (updated 2026-08-02 — the old `~/vax/cluster/` path is dead):**

   - **Lab-1, the hand-run lab: `/data/training/vax/cluster/`** (see its `README-lab.md`). Dev moved
     to the `workshop` VM on 2026-08-01 and the lab dataset moved to ZFS `tank/vax`, surfacing there
     via virtiofs. Every lab path in older docs and handoffs still says `~/vax` — swap the prefix.
     **Lab-1 is single-instance and is usually held by whatever investigation is live** (as of this
     writing, `vms-2f3`). Do not disturb a running lab-1 session.
   - **Lab-2, labs on demand: `tests/lab/`** (`vms-a5c`). A k3s StatefulSet where **one pod is one
     complete, isolated 2-node VMScluster** — each replica clones its own disks and builds its own
     `br0`+taps inside its own pod netns, so replicas cannot see each other or the cluster LAN and
     all of them reuse SCSSYSTEMID 1025/1026/1027 by design. `kubectl -n ovmx-lab scale sts/vaxlab
     --replicas=N` is the whole capacity story. **If you need a lab and lab-1 is busy, take a lab-2
     replica — do not queue.** Read `tests/lab/README.md` before driving one; it carries the console
     protocol, the four traps, and the wire comparison against lab-1 *including what that comparison
     does not prove*.

   Both labs are observation oracles under this rule, and neither changes what it permits.

9. **One runtime target: the kernel/QEMU path (HARD INVARIANT)**. Operator ruling 2026-07-28 — the
   Docker *runtime* layer is **dead**. OVMX has exactly **one** runtime: the real-kernel / QEMU path,
   where `vms.ko` provides the VMS **executive** and userspace reaches it through `/dev/vms`
   (`vms_kif`). Docker is **never** an OVMX runtime, never a supported way to "run OVMX", and never
   a target whose limitations get designed around.

   **This distinction is load-bearing — do not collapse it:**
   - **Docker as a RUNTIME target — FORBIDDEN.** The root `Dockerfile` and `docker-compose.yml`
     (glibc product container, SSH on 2222) were **deleted by vms-71a** — CI no longer builds or
     runs them. Do not recreate them, do not document them as a way to run OVMX, do not add new CI
     jobs on them.
   - **Docker/podman as BUILD or TEST tooling — FINE and expected.** `distro/Dockerfile.bootable`
     (builds the bootable QEMU image), `src/kernel/Dockerfile` (builds `vms.ko`), and
     `tests/qemu/Dockerfile` (QEMU test harness) *produce and test the real runtime*. Rule: "ALL
     deps containerized — NEVER install on host" depends on these. Keep them.

   **Why this keeps being forgotten, and the trap to avoid:** Docker containers have no `/dev/vms`.
   Because CI runs in Docker, the kernel executive is currently *unprovable in CI* — and so OVMX
   grew per-process userspace fakes (logical name tables, process table, event flags, mailboxes)
   that report success while sharing nothing. **The architecture drifted to fit the test harness.**
   Therefore:

   - **Never add a silent userspace fallback for an executive facility.** If `/dev/vms` is absent,
     the correct behavior is to **fail honestly** (`SS$_NOSUCHDEV`, as `sys_lock.c` already does) —
     never to fake per-process success. A silent fallback is the exact LARP bug class the
     authenticity invariants exist to kill (INV-6).
   - **An executive facility is not done until a test exercises it against a real `/dev/vms`.**
   - The standing gate `tests/integration/test_runtime_target.sh` enforces the mechanical parts of
     this rule. Do not add allowlist entries to it to make it pass.
