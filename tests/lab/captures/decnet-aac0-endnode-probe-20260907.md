# DECnet Phase IV — first OVMX ↔ real-VAX wire interaction (vms-aac0)

**Date:** 2026-09-07 · **Pod:** vaxlab-2 (shared with the cluster lane by agreement; DECnet
0x6003 is protocol-orthogonal to SCS 0x6007) · **Oracle:** lab VAX node **1.1 = VAX1**
(VAX/VMS V7.3), a pure Phase IV **endnode** (`rtr 0.0`, no router on the segment).

## What ran
Static `DECNETD.EXE` (glibc-static x86_64, built from origin/main `4e9605ff`, md5-verified in
pod) run on `br0` as endnode **1.42** (name OVMX) for 45 s with a `0x6003` capture:

    DECNETD.EXE --address 1.42 --name OVMX --iface br0 --duration 45

## Result (see the two sibling artifacts)
- **OVMX emitted faithful endnode-hellos on the real lab wire** — `src 1.42`, MAC
  `aa:00:04:00:2a:04`, byte-shaped identically to the oracle's own hello.
- **OVMX's engine heard and registered the real VAX** — `DECNETD-I-ADJINIT, heard 1.1`;
  OVMX `SHOW ADJACENT NODES` lists `1.1 initializing`.
- **The VAX did NOT adopt OVMX** — node 1.1's wire `rtr` field stayed `0.0` throughout, and it
  kept HELLOing normally. **No crash, no disruption** to the VAX or the co-resident CN=3 cluster
  (ovmx-never-crashes-a-peer satisfied for endnode-hellos).

## Interpretation (DECnet Phase IV semantics)
Two endnodes on a LAN with no router do **not** form a hello-based adjacency with each other —
endnode-hellos go to the all-routers multicast (`ab:00:00:03:00:00`) for *routers* to hear; a
peer endnode ignores them. So the engine's endnode-hello TX (all the live loop does today) can
never, by itself, make an endnode VAX list OVMX.

The provenance register done-bar (§4.3/4.4) requires the VAX to list OVMX **"as a reachable
endnode"** — not as a router. Getting there faithfully needs (a) OVMX defined in the VAX node
DB (`NCP SET NODE 1.42 NAME OVMX`) and (b) OVMX genuinely *reachable* — i.e. a live NSP/NICE
responder on the datalink, which today exists only socketpair-wired (not bound to the live
AF_PACKET engine). That overlaps the task-to-task work (vms-c23 landed the NSP connection
service but not its live-datalink binding).

## Bottom line
The **engine half of the Phase-1 proof is met**: OVMX's userspace Phase IV engine runs on a real
lab segment against a real VAX oracle and forms an adjacency entry for it. The **bidirectional
"VAX lists OVMX as reachable"** half needs a design/scope decision (wire the live NSP engine to
the datalink now, vs. stage it) plus reliable VAX-console access. Teed up to the operator.
