# OpenVMX Documentation Index

The map of `docs/`. It splits into a **user-facing spine** (listed in full below)
and a large **internal / design** bucket (design records, audits, oracle and
compatibility data) that is characterized, not enumerated.

## User Guides

- [Getting Started](getting-started.md) — first boot to a DCL prompt
- [Install Guide](install-guide.md) — `PRODUCT INSTALL` a kit onto a target volume (steps checked in CI against the real e2e gate)
- [Upgrade Guide](upgrade-guide.md) — in-place upgrade preserving site config and user data (CI-checked against the real e2e gate)
- [Building](building.md) — all build modes, CMake options, kernel modules
- [Cluster Configuration Guide](cluster-configuration-guide.md) — stand up a 2-node cluster via the pre-seeded `OVMXVMSSYS.PAR` (VMS-way SYSGEN/AUTOGEN authoring is post-0.6)
- [TCP/IP Configuration Guide](tcpip-configuration-guide.md) — the `TCPIP$CONFIG` plane, `TCPIP$` logicals, interfaces
- [Multi-Architecture Guide](building-multiarch.md) — x86_64, Alpha (LP64), VAX, and aarch64 targets
- [DCL Command Reference](dcl-commands.md) — the built-in verb set (`src/vmsdcl/dcl_builtin.c`)
- [Adding an OVMX Kernel Module](adding-an-ovmx-kernel-module.md) — extending `vms.ko`

## Reference

- [System Services API](api-system-services.md) — the `sys$` service reference
- [Runtime Library API](api-rtl.md) — `lib$` / `str$` / `mth$` / `ots$` reference
- [Compatibility Surface Register](compatibility-surface.md) — per-surface status across 9 domains (generated from `docs/compat/`)
- [Compatibility Contract](compatibility-contract.md) — what "compatible" means and how it is enforced
- [Capability Reference](capabilities-v0.6.md) — the shipped capability set at a glance

## Roadmap & Releases

- [Release Roadmap to 1.0](release-roadmap-to-1.0.md) — **the roadmap of record**: milestone ladder (0.3 → 1.0), the 1.0 gate set, and current status (reconciled from `rd`)
- [Releasing](releasing.md) — release engineering and the co-release gate
- **Release notes** — per-milestone notes: `release-notes-0.2.md`, `release-notes-0.5*.md`, `RELEASE-NOTES-0.3*.md`, and [`RELEASE-NOTES-0.6.md`](RELEASE-NOTES-0.6.md)

## Internal / Design

These are engineering records, not user documentation. They are the authoritative
provenance for design decisions and clean-room reverse-engineering work, but the
guides above are the entry points for using OpenVMX.

- [Architecture](architecture.md) — component layers, product/kernel split, boot sequence, data flow
- [Product Vision](product-vision.md) — scope and the commoditization objective
- [Contributor Guide](../AGENTS.md) — repository conventions and contributor surface (`AGENTS.md`, repo root)
- **`design-*.md`** (~60 records) — per-feature design records: the executive core, Files-11 ACP, image activation, the distributed lock manager, DECnet, the native link toolchain, and more.
- **`audit-*.md`** — targeted audits (ILP32/VAX width, message idents, executive boundary).
- **Clean-room RE provenance** — `cluster-protocol-spec.md`, `decnet-provenance-register.md`, `draper-faithfulness-register.md`, `research-alpha-dlm-wire.md`.
- **Compatibility & parity tracking** — `dcl-verb-fidelity-scoreboard.md`, `qualifier-audit.md`, `conformance-gap-report.md`, `roadmap-source-compat.md`, `vms-source-code-corpus.md`, and the source YAML under `docs/compat/`.
- **Oracle data** — golden captures and normalization under `docs/oracle/`.
- **Runtime & process** — `runtime-target.md` (the one-runtime rule), `roadmap-reconcile-workflow.md`, and the internal orchestration state under `internal/` (`conductor-state.md`, `orchestration-conductor.md`, `lane-ownership.md`).
