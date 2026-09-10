# DECnet Phase IV bpf(4) datalink wire-proof (rd vms-9b0)

**2026-09-10.** First proof that OVMX's `DECNETD.EXE`, over the `src/libdatalink`
**NetBSD bpf(4)** backend, actually transmits real DECnet Phase IV frames on a
**live NetBSD/vax Ethernet interface** — not a socketpair self-test, not a
hand-crafted frame. Rung 1 of the DECnet north-star (rd vms-e4dc); parent rd
vms-30e.

## What was proven

An OVMX-on-NetBSD/vax node (SIMH MicroVAX 3900, node **1.42** area 1, name
`OVMXV3`) ran our `DECNETD.EXE --address 1.42 --iface qt0`. The engine opened
`/dev/bpf`, bound the interface, built endnode-HELLO frames from executive/config
state, and `write(2)`-egressed them. An **independent host `tcpdump` on the lab
bridge `br0`** captured them.

### Proof line (`ovmx-hello.pcap`, `sha256 eeb0468b…`)

```
14:45:37.120848 aa:00:04:00:2a:04 > ab:00:00:03:00:00, ethertype DN (0x6003),
  length 64: endnode-hello endnode vers 2 eco 0 ueco 0 src 1.42 blksize 1498 rtr 0.0 hello 5 data 2
```

- **7** such HELLOs from our node over the capture window.
- Source MAC **`aa:00:04:00:2a:04`** = the DEC Phase IV algorithmic MAC for node
  **1.42** (`AA-00-04-00` + little-endian `(area<<10)|node` = `0x042A` → `2a 04`) —
  i.e. the wire address is derived from the `--address` (NCP-executor) value, not
  invented (INV-6).
- Destination **`ab:00:00:03:00:00`** (all-endnodes multicast), **ethertype
  `0x6003`**.
- Independent egress corroboration: the pod's bridge FDB learned
  `aa:00:04:00:2a:04 dev tap3` (our node's tap), so the frame genuinely left the
  guest onto `br0`.

### Heterogeneous / live oracle (bonus)

The **same** capture holds the two REAL OpenVMS VAX V7.3 lab nodes' HELLOs —
`src 1.1` (`aa:00:04:00:01:04`) and `src 1.2` (`aa:00:04:00:02:04`), `hello 15` —
interleaved with ours (`hello 5`). Our node's NIC RX-counter advanced (it received
their HELLOs): the bpf backend carries DECnet **bidirectionally** on a real,
mixed OVMX ⇄ real-VMS Phase IV segment. No adjacency forms — all three are
endnodes (`rtr 0.0`, no designated router present) — which is the expected,
correct, and safe outcome for this rung.

## Scope boundary (deliberate)

This rung is **HELLO-egress-only**. `vax3` was netns-isolated on a new `tap3`
bridged into the vaxlab-1 pod's `br0`; the two real VMS nodes (`vax1`/`vax2`) were
**never** driven — only their routine multicast HELLOs were passively received. A
multicast endnode-HELLO is the exact frame those nodes already exchange every 15s,
so it cannot bugcheck them. Initiating **adjacency / NSP connect / routing
exchange** toward a live VMS node is a crash-vector reserved for the next rungs
(rd vms-aac0 adjacency, rd vms-a70 SET HOST bracket), to be dev-tested in
isolation first (⭐⭐ never crash a peer).

## Executive-L2-seam decision

DECnet's datalink stays **userspace bpf(4)**, NOT routed through the executive
(no `SCS_DATALINK_VIA_EXECUTIVE` analog). This is the ratified ruling **rd
vms-a1c** (operator, 2026-08-31): Option B — a userspace Phase IV engine over a
raw socket / bpf, behind a VMS-authentic surface (NETACP/NCP/`executor.dat`),
because there is no in-kernel DECnet stack to ride (unlike AF_INET). Building an
executive L2 seam for DECnet would *contradict* that ruling. The SCS cluster's
executive datalink (ethertype 0x6007) is a separate, in-kernel path and is
unaffected.

## Provenance

- `DECNETD.EXE`: cross-built for `elf32-vax` from `origin/main` @ `05ba08d9`
  (V0.6-13 tip) with `tools/cross-vax/toolchain-vax-netbsd.cmake`. A **fully
  static** build (859256 B, no shared-lib deps) was used so it runs directly in
  the NetBSD install ramdisk. The HELLO path uses `vmsdecnet_engine` +
  `ovmx_datalink` (both static libs, strong-referenced); the FAL RMS/SYSUAF weak
  seam is unused for HELLO and resolves to 0.
- Runtime host: the NetBSD/vax 10.1 install ramdisk (`boot.fs`) under SIMH inside
  the k3s `ovmx-lab` `vaxlab-1` pod. Zero release-rail capacity used; the only
  workshop-host touch was the ~2-min cross-build.

### Runtime deviations worth recording (for the next rungs)

1. **NIC driver is `qt0`, not `qe0`** on NetBSD 10.1 — SIMH's DELQA (`xq`) is
   driven by the `qt` driver, not `qe`. Pass `--iface qt0`.
2. **`boot.fs` ustarfs volume-size** — the install loader's ustarfs assumes a
   1.44 MB floppy and demands "insert disk 2" for the 2.4 MB image. Fix: splice a
   zero-length `USTAR.volsize.<N>` tar record between `boot.vax` and `netbsd.gz`
   so ustarfs spans the whole image; then it boots clean to sysinst → shell.
3. **ramdisk root is full** (~1.9 MB, no MFS/tmpfs in the install kernel) — hot-
   attach a scratch MSCP disk, `disklabel -R` + `newfs` + mount it, and stage the
   binary there to exec.

## Files

- `ovmx-hello.pcap` — the capture (`sha256 eeb0468b…`). Re-decode:
  `tcpdump -r ovmx-hello.pcap -nne`.
- `ovmx-hello.txt` — text decode (`sha256 08e84b06…`).
- `vax3-console.log` — full SIMH console transcript of the vax3 boot + run.
