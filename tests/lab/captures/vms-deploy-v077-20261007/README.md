# V0.7-7 on the public page: 12 of 12, every run quiet (2026-10-07)

`https://openvmx.3dl.dev/demo/cluster/` — headless Chromium, **no query parameters**, fresh context and
cold cache per run, **nothing typed**, grader panel-scrolling off. Node B and Node C come from
`vax.3dl.network`. Two six-case passes of `tools/cluster-web-demo/visitor-gate.mjs`, whose pass bar is
"the cluster formed **and** the OVMX/VAX node's raw console carries no NetBSD boot output".

## Why everything was rebuilt

V0.7-7 replaces OVMX's placeholder status-code and structure constants with the **real OpenVMS V7.3
header values** (#1403, vms-f811 steps 1–8). Those numbers changed, so a node built before the tag and one
built after it are not safe on the same interconnect. Both OVMX nodes were rebuilt from the tag; Node C is
a real VAX/VMS V7.3 machine, which is where the values came from. Cluster changes in the payload: vms-f29
(an OVMX node as founding member with a real VAX joining it), vms-8219 (lock-directory answers), vms-4fb.

| piece | source | live sha256 (16) |
|---|---|---|
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | build-cluster-demo at tag V0.7-7 (`fe3b5bb6`) | `421081463eb9b27e` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | same; ODS-2 identity injected + read back | `705c91ed8f176014` |
| `boot/vmlinuz` | openvmx-site track-release at V0.7-7 | `8e6c36e4f28a1b29` |
| `ovmx-vax-nodeB.img.gz` (vax.3dl.network) | run-boot.sh `sysboot-single` at V0.7-7 | `46c049661362cdaf` |

All four fetched back **over the public URL and hashed** before grading. Node A's initramfs and the
deployed kernel share vermagic `6.12.103-ovmx SMP preempt mod_unload` and executive `srcversion`
`EE2CC441D860F243FC7755A`. Node B: `SCSNODE OVMXB, SCSSYSTEMID 1988, VOTES 0, EXPECTED_VOTES 1,
VAXCLUSTER 2`, `CLUSTER_AUTHORIZE.DAT` group 257 (`01 01`, read back out of the file).

## Results — every row, both passes

| case | order | gap | cpu | pass 1 | pass 2 | worst drift |
|---|---|---|---|---|---|---|
| page-order | A,B,C | 5 s | ×1 | CN=3 | CN=3 | 166 / 101 ms |
| all-at-once | A,B,C | 0.5 s | ×1 | CN=3 | CN=3 | 71 / 361 ms |
| page-order-throttled4 | A,B,C | 5 s | ×4 | CN=3 | CN=3 | 2050 / 1378 ms |
| b-first | B,A,C | 3 s | ×1 | CN=3 | CN=3 | 330 / 219 ms |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 | CN=3 | 34 / 183 ms |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 | CN=3 | 288 / 734 ms |

**12 of 12.** In every run: `quiet` true and `substrate_noise` `{}`; `ovmx_founded` empty; **zero**
bugchecks, **zero** lost connections, **zero** restarts on any node; `silent_never_starts` 0. Both OVMX
nodes announce **OpenVMX V0.7-7** (24 occurrences, nothing older) and Node C is
`OpenVMS (TM) VAX Version V7.3`. The real VAX admitted **both** nodes in **all twelve** runs
(`vaxc_admitted` is `['OVMXB','OVMXA']` in every `result.json`, and its OPCOM form
`proposed addition of node OVMXA`/`OVMXB` appears 12 times each).

Note on the ×4-throttled row: it passed in both passes here, at 2050 ms and 1378 ms of worst
main-thread drift — the highest drift ever recorded on this rig, and the row that failed on V0.7-6.

## The quiet claim, checked twice

* **Before publishing:** `nodeB-build-final-boot-console.txt` is the build's own final boot of the exact
  disk that shipped — KA655 ROM banner, VMB, then `%OVMX-I-EXEC` / `OpenVMX V0.7-7`. `netbsdNoise()` over
  it returns `{}`.
* **On the live page:** an independent grep for the six markers the V0.7-2 disk used to print
  (`>> NetBSD/vax boot [`, kernel version line, NetBSD Foundation copyright, `MicroVAX 3800/3900`,
  `total`/`avail memory =`, `Detecting hardware...`) totals **0 across all twelve Node B consoles**.

The page filters nothing (baron-3dl/pcjs `92e01064`), so that is the raw console in both cases.

## Rail note

The Node B build sat **Pending for 63 minutes**: `k3s-worker` was at 92 % of its memory *requests* with
another lane's 16 GiB `run-on-rail` job holding a slot, and this deploy runs with requests == limits (the
node OOM-rebooted on 2026-10-01, so overcommitting memory is not on the table). It scheduled and completed
unattended once that job finished. Nothing was shrunk below what the build needs to make it fit sooner.
