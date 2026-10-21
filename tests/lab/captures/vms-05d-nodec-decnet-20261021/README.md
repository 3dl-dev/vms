# vms-05d: Node C ships DECnet-VAX installed, configured, unlicensed (2026-10-21)

rd vms-05d (parent vms-5843). Build recipe: `tools/lab-vax/build_nodeC_vms73_cluster.sh` with
`DECNET=1 FROM_GZ=<vms73-nodeC-cluster.dsk.gz>` (PHASE 3). Provenance and the licensing table are
in `tools/lab-vax/PROVENANCE-vms73-nodeC.md`, section "vms73 DECnet variant".

## Disk

`vms73-nodeC-decnet-cluster.dsk.gz`, sha256
`c78d3c58dbd59b4003c6ee598a3ef017e0925391ad19224f013859b320e91361` (29,998,193 bytes).
Uncompressed: 1,505,766,912 bytes, the same RA92 geometry as the input disk; sha256 in
`disk-sha256-precompress.txt`. Input: the published `vms73-nodeC-cluster.dsk.gz`
(`45355fd2…4fe15`, verified by the script before use). The build ran in an isolated pod,
`ovmx-lab/vms05d-lab`, and the output was kept at
`/lab/cluster-demo-nodeC-v73-decnet/` on the lab PVC.

## Build evidence (no license on the volume)

- `build-console.log`: the whole build console. It boots the input disk (real genesis,
  `%CNXMAN, now a VAXcluster member -- system VAXC`), runs `PRODUCT INSTALL DECNET_PHASE_IV`,
  `NETCONFIG`, NCP `DEFINE NODE`, and the SYSTARTUP_VMS edit, then reboots and shuts down cleanly.
- `build-reboot-startnet-nolicense.log`: the reboot after configuration. Startup runs
  `START/NETWORK DECNET` (STARTNET) and VMS prints its own failure:

  ```
  DECnet starting
  %RUN-S-PROC_ID, identification of created process is 20200213
  %NCP-W-OPEFAI, Operation failure
  Circuit = QNA-0
  %SYSTEM-F-NOLICENSE, operation requires software license
  %NCP-I-NOINFO, No information in database
  ```

  LMF also reports `%LICENSE-F-EMTLDB, license database contains no license records`.
- `ncp-list-known-nodes.log`: `NCP LIST KNOWN NODES` shows executor 1.965 (VAXC) and the remote
  nodes 1.1 (OVMXA) and 1.2 (OVMXB). `LIST EXECUTOR CHARACTERISTICS` shows nonrouting IV.
  `SHOW LICENSE` prints `%SHOW-I-NOLICENSE, no licenses exist`.
- `byol-shipped-disk-boot-unlicensed.log`: the shipped `.dsk.gz` booted again, from a fresh
  decompress on the bench below. The STARTNET failure is the same.

## Why 1.965 and not 1.3

STARTNET.COM refuses a cluster member whose DECnet address is not its SCSSYSTEMID. With 1.3 the
first lab boot printed:

```
%DECdtm-W-ADDRNEQ, DECnet address (1027) is not equal to SCSSYSTEMID (1989)
%DECdtm-F-NOSTART, DECdtm facility not started
...
DECnet address of local node must match VAXcluster system ID.
DECnet not started.
```

With 1.3, DECnet could never start, licensed or not. SCSSYSTEMID 1989 is 1.965.

## Bring-your-own-license proof (lab PAKs, scratch copy, PAKs redacted entirely)

Bench: VAXC was the shipped disk under SIMH (`ovmx-vaxlab:5` image, MicroVAX 3900). OVMXA was an
x86 OVMX V0.7-13 release image under KVM, with DECnet config injected by
`tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` from this branch, so it defines
VAXC 1.965. Both were on one L2 segment: SIMH's `udp:` Ethernet was bridged to qemu's socket-mcast
segment by `udp2mcast.py`, and `byol_sethost.py` drives the run. The OVMX image's SCSNODE is the
stock `OVMX`, so `F$GETSYI("NODENAME")` reads OVMX there. Its DECnet executor is OVMXA 1.1.

The lab's PAK set (`/lab/cluster/PAKS.COM`, never copied out) has DVNETRTG, not DVNETEND.

- `byol-licensed-startnet.log`: `LICENSE LOAD DVNETRTG`, `SHOW LICENSE`, then
  `@SYS$STARTUP:STARTNET` in the same session that STARTNET had already failed in at boot. The
  re-run prints harmless `DUPLNAM`/`DEVACTIVE` noise (NETACP is already running), then
  `DECnet event 4.10, circuit up`, and `NCP SHOW EXECUTOR` reports state on.
- `byol-remote-login-license-gates.log`: the DECnet license alone is not enough to log in to VAXC
  from another node. Without VAX-VMS the login gets "No license is active for this software
  product". With VAX-VMS but not VAXCLUSTER it is refused with
  `%LOGIN-F-NOVAXCLUSTER, DEC VMSCLUSTER license is not active`, which VAXC's own security audit
  records as "Remote interactive login failure" from OVMXA::SYSTEM.
- With DVNETRTG + VAX-VMS + VAXCLUSTER loaded:
  - `byol-sethost-ovmxa-console.log`: OVMXA `SET HOST VAXC` gets the real V7.3 banner. It logs in
    as SYSTEM, `F$GETSYI("NODENAME")` is `VAXC`, and `SHOW TERMINAL` shows `_RTA1:` with
    `Remote Port Info: OVMXA::SYSTEM`. `LOGOUT` gives `%REM-S-END`.
  - `byol-sethost-vaxc-console.log`: VAXC `SET HOST OVMXA` gets the OpenVMX V0.7-13 banner and
    logs in. NODENAME is `OVMX`, `SHOW TERMINAL` shows `_RTA0:` with `Remote Port Info:
    VAXC::SYSTEM`, and `LOGOUT` gives `%REM-S-END, control returned to node VAXC::`.
  - `byol-sethost-both-ways-decnet.pcap`: the 0x6003 frames of that run, from OVMXA's NIC.

So a visitor who loads the DECnet license can SET HOST from VAXC to an OVMX node. SET HOST into
VAXC also needs VAX-VMS and VAXCLUSTER licenses.

## Side findings (OVMX, not Node C)

- After an OVMX node is killed mid-session and rebooted, its first CTERM Connect Initiate to VAXC
  is silently dropped while VAXC still holds the dead session's link. OVMX reused the same NSP
  source link address (8448) across the reboot. It cleared once VAXC dropped the link.
- A long DCL line (about 95 characters) typed through OVMX's CTERM into a VAXC session at width 80
  stalled on the wrap redisplay, and the link ended "idle bound". Short lines work.
- The SYSTEM account on both Node C disks warns `Your password expires on Friday, 23-OCT-2026`.
