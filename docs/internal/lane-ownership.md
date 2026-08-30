# OVMX Lane Ownership Registry (cross-session coordination bus)

> **Internal orchestration state — not product documentation.** This registry coordinates
> the OVMX agent-swarm's hot-file ownership, not the OVMX product. It lives under
> `docs/internal/`; do not cite it from user-facing docs.

**Every session — the main conductor AND any mustered peer — MUST read this before dispatching
into a lane, and MUST claim a lane before touching its hot files.** The merge-safe design (one
owner per hot file) only holds if ALL sessions honor one registry. Advisory (no hard enforcement).

Rule: one owner per hot-file surface at a time. To take a lane, set Owner + a live marker
(session / branch / rd item) + as-of. To release, clear it. If a lane shows an owner, do NOT
dispatch into its files — coordinate with that owner or wait. Re-derive the *live* status from
`ListAgents` + `gh pr list` each tick; the table below is the durable ownership map, not live status.

_See `docs/internal/orchestration-conductor.md` (runbook + operating model) and `docs/internal/conductor-state.md`
(the live pointer). Convergence program: `vms-1d6`. Release-eng: `vms-a84`._

| Lane | Owned surface (hot files) | Owner role | Domain |
|---|---|---|---|
| **Main / ACP conductor** | release train, `docs/roadmap*`, `docs/compat/**`, `tools/roadmap/**`, the executive/ACP faithfulness spine (`src/kernel-core/**` shared-core release-gating, `vms-040` boundary), IMGACT activation | the long-lived conductor | Linux runtime + executive + Files-11 ACP + release/roadmap |
| **Alpha (LP64)** | `tools/cross-alpha/**`, `src/imgact/arch/alpha/**`, the qemu-system-alpha run harness, GAP3/syscall-numbering (`vms-8954`) | peer sub-conductor | Alpha LP64 runtime + the 64-bit oracle |
| **VAX (NetBSD, ILP32)** | `src/kernel-netbsd/**`, `tools/cross-vax/**`, `tests/lab-vax/**`, `src/libvms/rtl` VAX codegen, VAX login/SYSUAF | peer sub-conductor | NetBSD-VAX SYSKRNL + VAX faithfulness |
| **GCC / toolchain** | `tools/cross-alpha-vms/**` (the alpha-dec-vms musl+GCC port), `src/vmslink/mk_decc_shr.sh` + DECC$SHR enumeration, the do-it-like-VMS ladder | peer sub-conductor | The OpenVMS GCC port + toolchain (Linux GCC = oracle) |
| **Demo / browser** | `~/projects/openvmx-site/**` (site), `~/projects/pcjs*` (PCjs VAX), `tools/webdemo/**`, the demo disks + S3/CloudFront | peer sub-conductor | The web-browser marketing demos (x86_64 + VAX panes) |
| **Cluster / VAT2** | `src/vmsscs/**`, `tools/cluster/`, the lab-1/lab-2 wire investigations | peer sub-conductor | SCS/NISCA cluster wire + rejoin/admission |

**Shared-core release-gating (main conductor).** Anything under `src/kernel-core/**`, the ODS-2
codec (`src/vmsfs/ods2/**`), or a cross-arch change rides the width gate {x86_64 + VAX ILP32 +
Alpha LP64 (+ aarch64)} and is release-gated by the main conductor, even when authored by a peer.
Joint changes that span substrates (e.g. the vms-165 VFS retirement: Linux + NetBSD + shared core)
land as ONE atomic PR so they cannot half-flip.

**Mustering a new lane.** The operator spins up a peer; it claims a NEW row here for its domain
(e.g. a networking lane for `src/vmstcpip/**` + `vms_bg.c`; a cluster-wire lane), reads `rd ready`,
and starts the tick loop. No two lanes own the same hot file.
