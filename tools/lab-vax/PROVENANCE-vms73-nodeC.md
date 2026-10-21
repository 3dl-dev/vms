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

## vms73 DECnet variant — `vms73-nodeC-decnet-cluster.dsk.gz` (rd vms-05d)

Baron's ruling (2026-10-21, rd vms-05d): Node C gets DECnet-VAX Phase IV **installed and
configured but unlicensed**, so a visitor brings their own DVNETEND license. This is a
**separate** disk image next to `vms73-nodeC-cluster.dsk.gz`; the original disk is unchanged.

### Source

- Input: the published `vms73-nodeC-cluster.dsk.gz` itself (sha256
  `45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15`, the vms-2570 build above),
  pinned by `FROM_SHA256`. Starting from the built volume rather than re-personalizing
  `d0-newinst073.dsk` carries the cluster identity, the cluster password (an operator fact that is
  never typed or committed) and the SYSTEM password over bit-for-bit.
- DECnet kit: the V7.3 distribution CD's own PCSI kit,
  `DUA2:[DECNET_PHASE_IV_VAX073.KIT]DEC-VAXVMS-DECNET_PHASE_IV-V0703--1.PCSI`
  (`/lab/media/openvms073.iso`), installed with `PRODUCT INSTALL DECNET_PHASE_IV`.
- Built by `build_nodeC_vms73_cluster.sh` with `DECNET=1 FROM_GZ=... FROM_SHA256=...` (PHASE 3 of
  the script), inside an isolated `ovmx-lab` pod. The lab's live reference cluster was not touched.

### What PHASE 3 does, the VMS way

1. `PRODUCT INSTALL DECNET_PHASE_IV /SOURCE=DUA2:[DECNET_PHASE_IV_VAX073.KIT]`.
2. `MCR AUTHORIZE CREATE/PROXY` (creates NETPROXY.DAT, so STARTNET does not log
   "Error opening permanent proxy database").
3. `@SYS$MANAGER:NETCONFIG`: node name VAXC, address **1.965**, nonrouting endnode, no default
   DECnet or object accounts (MAIL, FAL, PHONE, NML, MIRROR, VPM all declined).
4. `NCP DEFINE NODE 1.1 NAME OVMXA` and `DEFINE NODE 1.2 NAME OVMXB` (the OVMX nodes of
   `tools/cluster-web-demo/mk_democonfig.py`'s `DEMO_ROSTER`).
5. Uncomments SYSTARTUP_VMS.COM's own `$!$ START/NETWORK DECNET` line, so STARTNET runs at every
   boot on the console.
6. **Registers no PAK.** It reboots and asserts that the boot console shows the cluster genesis and
   STARTNET's `%SYSTEM-F-NOLICENSE`. It then asserts the NCP permanent database and that no DVNET
   license is active, and shuts down cleanly before packaging.

### Why VAXC is 1.965, not 1.3

The first try used 1.3. Real VMS refused it. NETCONFIG warned "Your DECnet node address should
match your VAXcluster node address.", and at boot DECdtm logged
`%DECdtm-W-ADDRNEQ, DECnet address (1027) is not equal to SCSSYSTEMID (1989)`. STARTNET.COM, on a
cluster member, compares the executor address with `SCSSYSTEMID` and stops:
"DECnet address of local node must match VAXcluster system ID." / "DECnet not started."
With 1.3, DECnet could never start, licensed or not. SCSSYSTEMID 1989 is 1*1024 + 965, and
NETCONFIG offers 1.965 as its default. SCSSYSTEMID stays 1989 (frozen in the roster), so the DECnet
address follows it. The roster records VAXC as `"decnet": "1.965"`.

### Unchanged

`SCSNODE=VAXC`, `SCSSYSTEMID=1989`, group 257, `VOTES=1`, `EXPECTED_VOTES=1`, `ALLOCLASS=0`, the
cluster password, the SYSTEM password, the RA92 geometry (2,940,951 blocks), and pcjs bootability.

### Licensing

The shipped volume carries **no license PAK**. At boot LMF reports
`%LICENSE-F-EMTLDB, license database contains no license records`, and `SHOW LICENSE` prints
`%SHOW-I-NOLICENSE, no licenses exist`. The build asserts both. The lab proof of
bring-your-own-license ran on a scratch copy that was deleted afterwards. Its PAKs never left the
lab and are never committed, published or shipped.

What a visitor's own licenses unlock, as measured in the lab on 2026-10-21. The lab's PAK set has
DVNETRTG, not DVNETEND; either one starts a Phase IV endnode.

| Licenses loaded on VAXC | `@SYS$STARTUP:STARTNET` | `SET HOST OVMXA` from VAXC | `SET HOST VAXC` from an OVMX node |
|---|---|---|---|
| none (as shipped) | `%NCP-W-OPEFAI` / `Circuit = QNA-0` / `%SYSTEM-F-NOLICENSE` | no circuit | no circuit |
| DVNETEND or DVNETRTG | circuit QNA-0 up (event 4.10) | works (console login needs no base license) | connects, prints the banner and `Username:`, then `No license is active for this software product` |
| + VAX-VMS | same | works | refused: `%LOGIN-F-NOVAXCLUSTER, DEC VMSCLUSTER license is not active` |
| + VAX-VMS + VAXCLUSTER | same | works | works, and SHOW TERMINAL on VAXC shows `Remote Port Info: OVMXA::SYSTEM` |

VAXC is a VMScluster member. Real VMS LOGINOUT therefore allows a license-free login only on the
console (OPA0). An inbound network login needs the VAX-VMS and VAXCLUSTER licenses as well as the
DECnet license.

### Not changed, noted

At login the SYSTEM account prints `WARNING - Your password expires on Friday, 23-OCT-2026
18:44`. Both Node C disks carry this, since it was set by the 2026-09-23 personalization. The
password itself was not touched here.

Capture: `tests/lab/captures/vms-05d-nodec-decnet-20261021/`.
