# Provenance — Node C V7.3 cluster demo disk (rd vms-2570, decision rd vms-d24)

This note records what is actually known about the source of the `vms73-nodeC-cluster.dsk.gz`
image `build_nodeC_vms73_cluster.sh` produces, and the authorization to ship it publicly. It
does not assert anything beyond what is recorded here — no licensing chain-of-custody is
invented (Rule 8 / clean-room; AGENTS.md).

## Source

- Base media: `/lab/media/openvms073.iso` (OpenVMS VAX V7.3 distribution CD image, file
  timestamp 2012-02-08, 547,375,104 bytes) and a fresh, never-personalized post-restore system
  disk `/lab/cluster/data/d0-newinst073.dsk` (RA92, 1,505,766,912 bytes / 2,940,951 blocks),
  both staged on the `ovmx-lab` cluster-interop lab's shared PVC (`vax-lab-pvc`, k3s namespace
  `ovmx-lab`, mounted identically across the `vaxlab-0..4` pods).
- This is the SAME base image `tests/lab/*/clean-cluster/FORMATION-NOTES.md` (built
  2026-07-28) used to construct the lab's own from-scratch, license-free 2-node V7.3 reference
  cluster — that document's SS5 ("Licensing — what the cluster actually required") found the
  fresh disk carries **no PAKs** and that VMScluster formation on V7.3 does not require one
  (`%LOGIN-I-NOVAXCLUSTER` is informational, not a hard block; confirmed again independently
  during this build, 2026-09-23).
- No provenance/licensing metadata beyond the above (file dates, sizes, and the lab's own prior
  measured finding) exists in the lab. This build does not add or assume anything further.

## Authorization to ship publicly

Baron authorized public shipment of a V7.3 Node-C disk for the browser cluster demo,
2026-09-23 — see rd **vms-d24** ("DECISION (Baron 2026-09-23): demo Node C = real VAX/VMS V7.3
(option A) — authorized for public ship; supersedes V5.5 path"). That decision superseded the
V5.5 path (`build_nodeC_vms55_cluster.sh`, `tools/lab-vax/`), whose own equivalent
authorization is recorded next to its build script and in rd vms-12a.

## What this build does NOT do

- It does not download, embed, or commit any VMS media to this repository (clean-room rule;
  AGENTS.md). `build_nodeC_vms73_cluster.sh` only ever reads from operator-supplied `BASE_DSK`
  / `ISO` paths.
- It does not publish the built disk anywhere. Publishing to `vax.3dl.network`
  (`baron-3dl/pcjs`) is Baron-manual (the binary-push guardrail blocks automated pushes of
  images this size) — proof runs serve the built disk locally.
- It does not alter the lab's own live group-1 reference cluster (`/lab/cluster/data/d0.dsk`,
  the `vaxlab` StatefulSet's running volumes) — it clones the separate, never-personalized
  `d0-newinst073.dsk` into its own working directory and configures group **257** (the demo's
  group, distinct from the lab's group 1).

## Built + verified 2026-09-23 (rd vms-2570)

- `SCSNODE=VAXC`, `SCSSYSTEMID=1989`, group `257`, `VOTES=1`, `EXPECTED_VOTES=1`, `ALLOCLASS=0`
  — matches `tools/cluster-web-demo/mk_democonfig.py`'s `DEMO_NODE_C` exactly.
- Real genesis proven twice: once in an isolated SIMH instance inside `vaxlab-4` (own tap,
  own bridge; SDA `SHOW CLUSTER` showed a solo CSB `VAXC`, CSID `00010001`, `Curr. coord. CSID
  00010001`), and again booting the packaged `.dsk.gz` under pcjs's browser VAX machine
  (`machines/dec/vax/browser/ovmx-cluster.html`) — same `%CNXMAN, now a VAXcluster member --
  system VAXC` real console output both times.
- pcjs bootability: **confirmed**. The V7.3 disk boots under pcjs's KA655 emulation
  (`ovmx-cluster.html`) exactly like the V5.5 disk did; no blocker.
- SHA-256 of the built disk (this build): recorded in
  `tests/lab/captures/vms-2570-nodec-v73-20260923/README.md`, alongside the console evidence
  and the in-browser Node A + Node C exchange capture.
