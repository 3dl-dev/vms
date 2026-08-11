# Product Vision

## Mission

Make OpenVMS free. OpenVMX is a **clean-room, open-source OpenVMS-compatible environment on Linux** that runs real VMS applications and **joins real VMSclusters** — so organizations can migrate off VMS Software Inc (VSI) node-by-node, at zero downtime and zero license cost.

### Naming: product and substrate

Following the GNU/Linux convention, the project is two named layers, not one. **OpenVMX** is the VMS-compatible product — what a user logs into, and what the login banner, `SHOW SYSTEM`, `MONITOR`, and DCL report. **OVMX/Linux** (with the slash, like "GNU/Linux") is the Linux base layer it runs on: kernel, boot sequence, and distro tooling — the rough equivalent of the VAX/Alpha hardware OpenVMS itself ran on, except here the "hardware" is a Linux distribution. The split is never cosmetic: OVMX/Linux is what appears at early boot and in distro build metadata, before OpenVMX ever takes over the console. Single source of truth: `src/libvms/include/ovmx_identity.h`; layer mapping: `docs/architecture.md`.

## North Star

VSI monetizes two scarcities:

1. **Licensing** — you cannot legally run VMS without paying them.
2. **Portability** — a decades-old application only runs on their OS, so the customer stays hostage to the license.

OpenVMX attacks both by commoditizing them: reproduce a compatible surface with AI-driven development faster than a licensed vendor can defend it, and give it away. This is Sutton's bitter lesson applied to systems software — don't out-craft 40 years of DEC→Compaq→HP→VSI engineering; **commoditize it.** The strategic wedge is the community and mid-market VSI abandoned when it revoked OpenVMS Alpha community licensing: a ready-made base of adopters, evangelists, and compatibility oracles.

Realistic ambition: not "VSI dies," but **"VSI becomes irrelevant to everyone except the whales, and even the whales' next renewal gets harder because a credible free alternative exists."**

### The two rails (both required, converging at migration)

OpenVMX must be a drop-in cluster member that runs the customer's software. That needs two parallel pillars — neither defers:

1. **Cluster interop** (`vms-ci`) — *the migration path in.* An OVMX/Linux node speaks SCS / NISCA / MSCP / DLM well enough to join a customer's live VMScluster and appear in `SHOW CLUSTER`. Built **clean-room** by reverse-engineering the wire protocol against a ground-truth reference cluster — never from VSI/HPE source. Turns migration from a big-bang forklift into "add a free node, evacuate the VSI nodes one at a time, zero downtime."

2. **VMS compatibility / image activation** (`vms-913` + compat work) — *what lets OpenVMX run the migrated software.* Full system-service, RMS, DCL, and image-activation compatibility, with **no Unix/Linux leaks** visible to users or applications. Without this, a cluster node is just a protocol emulator.

They converge at the **rolling evacuation**: moving a live workload off a VMS node onto OpenVMX *is* running VMS images on OpenVMX inside the cluster (`vms-ci.6`, gated by both the DLM rung and image activation).

### The one metric that matters

> Can a real, unmodified VMS application build/run on OpenVMX — and can an OVMX/Linux node join a real VMScluster — with **zero Unix leaks** visible to its users?

95%-compatible = 0% adoption for the risk-averse enterprise, because the missing 5% is exactly the quadword-alignment, RMS-indexed-file, condition-handler edge case their 1987 application depends on. Grinding that long tail cheaply is precisely where AI-driven development compounds — and it is the whole game.

### Not near-term (stated honestly)

- VSI's tier-1 support, indemnification, and certifications for defense / utilities / exchanges — a free layer does not threaten a renewal backed by an SLA and a throat to choke.
- Deep clustering internals beyond membership / MSCP / DLM basics arrive incrementally, not at once.

## Key Decisions

| # | Question | Decision | Rationale |
|---|----------|----------|-----------|
| Q1 | Freestanding vs glibc for syscall layer? | Freestanding (no glibc) | VMS system services should not depend on Linux userspace; enables static linking for initramfs and maximum control over syscall interface |
| Q2 | Kernel modules vs pure userspace? | Both — kernel modules for VMS-specific semantics, userspace for portability | Access control, AST delivery, event flags, and filesystem versioning benefit from kernel support; userspace fallback keeps Docker mode viable |
| Q3 | Docker vs bare-metal deployment? | Both — Docker for development, QEMU bootable for production-like testing | Docker lowers the barrier to entry; bootable distro proves the full stack including kernel modules |
| Q4 | VMS compatibility vs Linux convenience? | VMS behavior wins when they conflict | The project's value proposition is VMS compatibility; deviating defeats the purpose |
| Q5 | Primary goal — standalone compatibility, or cluster interop? | **Cluster interop is the primary go-to-market; standalone compatibility is its required foundation** | Node-by-node, zero-downtime migration into a customer's existing VMScluster is the pitch enterprises actually say yes to; it attacks VSI's deepest moat from the inside |
| Q6 | How is the cluster protocol obtained? | **Clean-room RE of the wire + public OpenVMS docs only; NEVER disassemble or copy VSI/HPE source or binaries** | Interoperability RE is legally protected only if kept clean-room (US DMCA §1201(f), EU Software Directive Art. 6). This is a hard, unrecoverable-if-crossed invariant, including for AI agents |
| Q7 | Is image activation (`vms-913`) deferrable behind cluster work? | **No — it is a co-equal required pillar** | Full VMS compatibility (the whole direction) requires running the customer's real VMS images; a cluster node that can't host the migrated workload is worthless |

This table is the **source of truth** for product direction. If a spec or calculation contradicts a decision here, flag the conflict explicitly — the user resolves it.

## Scope

### Foundation — COMPLETE (Phases 1-7)
Freestanding syscall layer (x86_64 + aarch64), VMS system services + RTL, process management (PCBs/ASTs/event flags/access modes), logical name manager, VMS filesystem library, RMS (sequential/relative/indexed), DCL shell, kernel modules (`vms.ko`, `vmsfs.ko`), bootable distro (Docker + QEMU), multi-user SSH with SYSUAF auth. See `tracking/roadmap.md`.

### Rail A — Cluster interop (`vms-ci`)
Reference lab (SIMH VAX 7.3 cluster) → capture + dissect SCS/NISCA/MSCP → the OVMX/Linux node appears in real `SHOW CLUSTER` → MSCP-served disk → distributed lock manager → rolling evacuation. Clean-room throughout.

### Rail B — VMS compatibility / image activation (`vms-913` + compat)
IMGACT.EXE image activator, shareable images, INSTALL, GSMATCH; provable source compatibility (`vms-801`); authenticity / no-Unix-leaks (`vms-898`) enforced as a build-failing invariant, not polish.

### Reorientation (`vms-pivot`)
Reframe vision + roadmap (this doc), cluster-node architecture design + stale-implementation assessment, and re-triage of the existing backlog against the two rails.
