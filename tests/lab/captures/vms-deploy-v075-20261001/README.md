# V0.7-5 on the public page: 11 of 12 visitor runs CN=3 (2026-10-01)

`https://openvmx.3dl.dev/demo/cluster/` — headless Chromium, **no query parameters**, fresh context and
cold cache per run, **nothing typed into any console**, and the grader's panel-scrolling **off** (it
starved Node A; #1334). Node B and Node C are fetched from `vax.3dl.network`.

## What was deployed

| piece | source | live sha256 (16) |
|---|---|---|
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | build-cluster-demo at tag V0.7-5 (`42c8f890`) | `1eb40da3f82bd908` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | same; ODS-2 identity injected + read back | `4d3287f0ad2c77c8` |
| `boot/vmlinuz` | openvmx-site track-release at V0.7-5 | `b543a5c130472cb5` |
| `ovmx-vax-nodeB.img.gz` (vax.3dl.network) | run-boot.sh `sysboot-single` at V0.7-5 | `af73a61416ef3710` |

All four fetched back **over the public URL and hashed** before grading. Node A's initramfs and the
deployed kernel share vermagic `6.12.103-ovmx SMP preempt mod_unload` and executive `srcversion`
`3AED2EA29C4D763A2BC3C06` — the lock `vms.ko` needs, checked rather than assumed. Node B's identity is
injected at build time (`SCSNODE OVMXB, SCSSYSTEMID 1988, VOTES 0, EXPECTED_VOTES 1, VAXCLUSTER 2`) with
`CLUSTER_AUTHORIZE.DAT` group 257 — read back as `01 01` out of the file. Node C is the pinned real
VAX/VMS V7.3 volume, unchanged.

## Results

| case | order | gap | cpu | pass 1 | pass 2 | worst drift |
|---|---|---|---|---|---|---|
| page-order | A,B,C | 5 s | ×1 | **NOT CN=3** — Node B never started | CN=3 | 39 / 40 ms |
| all-at-once | A,B,C | 0.5 s | ×1 | CN=3 | CN=3 | 93 / 40 ms |
| page-order-throttled4 | A,B,C | 5 s | ×4 | CN=3 | CN=3 | 975 / 1072 ms |
| b-first | B,A,C | 3 s | ×1 | CN=3 | CN=3 | 213 / 57 ms |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 | CN=3 | 170 / 76 ms |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 | CN=3 | 188 / 224 ms |

**11 of 12.** Across all twelve runs: **zero** bugchecks, **zero** lost connections, **zero** restarts on
any node, and `ovmx_founded` empty — no OVMX node founded a cluster in any order at any speed. Both OVMX
nodes announce **OpenVMX V0.7-5** (23 occurrences; no older version), Node C is
`OpenVMS (TM) VAX Version V7.3`, and the real VAX admitted **OVMXA 12 times and OVMXB 11 times** in its
own words — one admission per node per successful run, and the missing one is the run below.

## The one failure: Node B's panel never started

`pass1/01-page-order-nodeB-never-started/` — Node B's console file is **0 bytes** and the in-page hub
counted **not one frame** from it across 586 s, while Node A and the real VAX clustered with each other
normally (`the real VAX admitted OVMXA`). No restart, no bugcheck, no lost connection on any node: the
machine never produced a byte, so there is nothing to read as a cluster event. Something in that run's
Node B panel did not come up.

A control says it is not the image or the deploy (`nodeB-fetch-probe.log`): five cold loads of the live
page, Node B started alone, watching the network —

```
iter 1..5: screenLen=7703  resp 200 58035101  bar="pcjs cluster node — real product confirmed (disk unit DUA0: ...)"
```

**5 of 5** fetched the full 58,035,101-byte image with HTTP 200 and booted to the same console length,
with the panel's own authenticity line confirming the product. So the graded miss is a one-off failure of
that panel to start, on a rig that re-downloads 88 MB of node images from scratch on every single run —
recorded as what it is rather than attributed to a cluster defect it shows no evidence of.
