# The public 3-node demo redeployed at V0.7-3, graded as a visitor (2026-09-29)

`https://openvmx.3dl.dev/demo/cluster/` — headless Chromium, **no query parameters**, a fresh browser
context and cold cache per run, and **nothing typed into any console**. Node B and Node C are fetched
from `vax.3dl.network`; nothing is served locally. Graded by
`tools/cluster-web-demo/visitor-gate.mjs`, six cases varying click ORDER and SPEED plus CPU throttling.

## What was deployed

| piece | source | sha256 (16) |
|---|---|---|
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | build-cluster-demo at tag V0.7-3 (`9e3ea4cb`) | `70e788cb98fe00ee` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | same, ODS-2 identity injected + read back | `a9608bc6e0730d13` |
| `boot/vmlinuz` | openvmx-site track-release at V0.7-3 | `0e1a55f777af419e` |
| `ovmx-vax-nodeB.img.gz` (vax.3dl.network) | run-boot.sh `sysboot-single` at V0.7-3 | `551c231f0a5e6288` |

Every one of those four was fetched back **over the public URL** and hashed before grading: the live
bytes are the bytes that were built. `boot/` was left as the site's own tracker built it — its kernel
and this initramfs carry the same vermagic (`6.12.103-ovmx SMP preempt mod_unload`) and the same
executive `srcversion` (`12474AE97C7EEF04281F2D0`), so the version lock `vms.ko` requires holds.
Node C is the pinned real VAX/VMS V7.3 volume, unchanged.

## What the live consoles say

* **`OpenVMX V0.7-3`** on both OVMX nodes, 56 times across the runs. No `V0.7-2` anywhere.
* **`OpenVMS (TM) VAX Version V7.3`** on Node C.
* **No OVMX node ever founded a cluster** — `ovmx_founded` is empty in all 14 runs, in every click
  order, at every speed. That is the roster fix (#1325) holding in public.
* The real VAX admits them **in its own words**: `%CNXMAN, proposing addition of system OVMXA` /
  `... OVMXB`.

## Results

**Pass 1** (`pass1/`, 08:04–08:31) — 4 of 6 CN=3.

| case | order | gap | cpu | verdict |
|---|---|---|---|---|
| page-order | A,B,C | 5 s | ×1 | CN=3 |
| all-at-once | A,B,C | 0.5 s | ×1 | **NOT CN=3** — OVMXB restarted 33×, VAXC 3× |
| page-order-throttled4 | A,B,C | 5 s | ×4 | **NOT CN=3** — OVMXB restarted 5×, VAXC 2× |
| b-first | B,A,C | 3 s | ×1 | CN=3 |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 |

**Re-run of exactly those two failures** (`pass1/rerun-*`, 08:32–08:35): both **CN=3**.

**Pass 2** (`pass2/`, 08:36–08:44) — 5 of 6 CN=3; `b-first` failed with OVMXB restarted 5×, VAXC 4×.

**Total: 11 of 14 runs reached CN=3.** Every failure carries guest RESTARTS and nothing else — no
split brain, no founding OVMX node, no roster problem.

**Baseline for comparison** (`baseline-v072/`, 06:46–07:04, the same rig ~1 h earlier, against the
then-live V0.7-2 page): 5 of 6, the one failure also a restart storm (OVMXB 8×, VAXC 5×). So within
this sample V0.7-3's residual failure rate is **not distinguishable** from V0.7-2's. #1326 was measured
at ~12× rarer on the stall rig; this browser rig is a different, harsher condition and 14 runs is too
few to resolve it. Claiming an improvement from these numbers would be reading noise.

## What the restarts actually are (for the cluster lane)

`pass1/02-all-at-once/OVMXB.console.log` carries 34 `>> NetBSD/vax boot` banners. The text immediately
before them is not one thing:

```
[   1.0000000] Detecting hardware...        <-- one second into the NetBSD boot
...
%OVMX-I-MOUNTED, system disk DUA0: mounted  <-- still in early startup
...
%CNXMAN, this node has already re-incarnated and the cluster still refuses it:
         waiting to form or join an OpenVMS Cluster
```

A guest that restarts **one second into hardware detection** is not bugchecking — the emulated VAX is
halting and the KA655's halt action reboots it. Under a browser carrying three emulators, Node B is
being starved hard enough to halt, and each halt starts a fresh incarnation, which is how the third
excerpt happens: the cluster is still refusing the incarnation the previous life left behind.

None of that is a page defect and none of it is fixed here. It belongs to **rd vms-8c54** (the stalled
guest, open) and the incarnation-rejection observation under vms-735.

## The panels themselves are fine now

`repaint_stalls` — a panel that showed nothing new for a whole poll and then produced output the moment
it was scrolled into view (rd vms-0bc) — fired **3 times across 12 runs**, versus a panel-by-panel
count on every run before the embed fix. Note the counter was also made stricter in the same window, so
treat that as "the panels are no longer visibly stuck", not as a measured ratio.
