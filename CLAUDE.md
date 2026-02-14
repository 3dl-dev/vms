# CLAUDE.md — OVMX Project Instructions

> OS-level instructions (session protocol, beads workflow, model routing, blog pipeline, rules) are inherited from `~/.claude/CLAUDE.md`. This file contains only project-specific configuration.

## Project

**OVMX**: OpenVMS-compatible environment for Linux. Implements VMS system services, DCL shell, RMS file system, and kernel modules — packaged as a bootable distro.

## Project Status

Phases 1-7 are complete. The project is functional as a Docker container and bootable QEMU VM. See `tracking/roadmap.md` for phase history and `docs/product-vision.md` for scope.

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

# Docker container (SSH on port 2222)
docker compose up --build
ssh system@localhost -p 2222    # password: MANAGER

# Bootable QEMU VM
docker build -f Dockerfile.bootable -o dist .
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz

# Kernel modules (needs kernel headers)
make -C src/kernel
make -C src/kernel/vmsfs

# QEMU kernel module tests
tests/qemu/run_tests.sh
```

## Agent Roster

The PM agent coordinates work across specialized agents. Each has a spec in `docs/`.

| Agent | Spec | Container | Role |
|-------|------|-----------|------|
| PM | CLAUDE.md | (beads via OS) | Prioritize, track, route work |
| Systems Engineer | docs/agent-systems.md | (native gcc/cmake) | Implement system services, DCL, kernel modules, RMS |
| QA Engineer | docs/agent-qa.md | (docker/qemu) | Test infrastructure, CI/CD, static analysis |
| Technical Writer | docs/agent-writer.md | (markdown) | API docs, command reference, guides |
| Blog | docs/agent-blog.md | (markdown) | Devblog posts (Baron's voice) |

**Routing rules:**
- System service / DCL / kernel / RMS / assembly implementation → Systems Engineer
- Testing / CI/CD / static analysis / build validation → QA Engineer
- Documentation / API reference / guides / admin docs → Technical Writer
- Blog outline → post authoring → Blog
- Everything else (prioritization, decisions, coordination) → PM

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
- **Build configs**: CMakeLists.txt hierarchy, Dockerfile, Dockerfile.bootable, docker-compose.yml

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
├── docker-compose.yml     # OVMX product container (SSH on port 2222)
├── Dockerfile             # OVMX product build (Docker mode)
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
