# vms-e18e — ⭐ CN=3 on the LIVE PUBLIC PAGE

**2026-09-25. https://openvmx.3dl.dev/demo/cluster/ — loaded in headless
Chromium with NO query overrides at all**, so what is graded is the deployed
defaults a visitor gets. The two VAX nodes come from `vax.3dl.network`; nothing
was served locally.

## Result

`cn3-result.json`:

```json
{"pass": true, "hold_s": 767, "elapsed_s": 2438,
 "announced": {"OVMXA": true, "OVMXB": true},
 "oracle":    {"OVMXA": true, "OVMXB": true},
 "bugchecks": {}, "losses": {}}
```

Each OVMX node's own `SHOW CLUSTER`, typed at its own console in the page:

```
View of Cluster from system ID 1987 node: OVMXA    25-SEP-2026 16:50:38
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXA  | 00010003 | VMX V0.7        | MEMBER           |
| 1988   | 00010002 |                 | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |

View of Cluster from system ID 1988 node: OVMXB
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXB  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
| 1987   | 00010003 |                 | MEMBER           |
```

The oracle — the real VAX's own CNXMAN/OPCOM (`VAXC.console.log`) — proposed and
completed BOTH admissions itself.

**Stability**, three consecutive polls, each a fresh `SHOW CLUSTER` on both OVMX
consoles, with the real VAX's own NIC transmit counter climbing throughout:

```
t=1333s members={"OVMXA":3,"OVMXB":3} vaxcTx= 7131 alive=true
t=1716s members={"OVMXA":3,"OVMXB":3} vaxcTx= 9560 alive=true
t=2100s members={"OVMXA":3,"OVMXB":3} vaxcTx=11990 alive=true
```

12 m 47 s sustained, zero bugchecks, no `lost connection` / `quorum lost` /
`was removed from the cluster` on any console.

## What is deployed

| node | asset | sha256 |
|---|---|---|
| OVMXA initramfs | `openvmx.3dl.dev/demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | `c539347756c9739057adcaceecf9b7b111a824c4306a1fcc930a677e46e367fc` |
| OVMXA sysdisk | `openvmx.3dl.dev/demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | `5f305cc6f687e63863fcf6d233a30f13a384d0cf1ec1446f5d9fecedcb03bb32` |
| kernel | `openvmx.3dl.dev/boot/vmlinuz` | `829e74f8b4853ebdaed9768f9ab94f1156737d5dcc99ec8bdcde28fc39db30fa` |
| OVMXB | `vax.3dl.network/machines/dec/vax/browser/ovmx-vax-nodeB.img.gz` | `1e07f3de652e26b9645d675c104e316f0589097fcf83cb595b791c5b62479d75` |
| VAXC | `vax.3dl.network/machines/dec/vax/browser/vms73-nodeC-cluster.dsk.gz` | `45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15` |

All built from `3dl-dev/vms` **`7d053b50`** (V0.7-1 + #1312 + #1309).

## Honest scope

* Node C's `OPA0:` login did not come up inside the window, so the VMS-side
  evidence is VMS's own CNXMAN/OPCOM transcript rather than an SDA
  `SHOW CLUSTER` — the same oracle rd vms-b34 and rd vms-1ac used.
* `OVMXB`'s table header reads `01-JAN-2010`: the NetBSD/vax guest's TOD clock
  is unset (`WARNING: preposterous TOD clock time` at boot). Cosmetic,
  pre-existing.
* One run. The intermittency recorded in
  `tests/lab/captures/vms-e18e-cn3-browser-20260925/` was fixed by #1309, which
  this deployment carries, but a single live run is not a reliability
  measurement.
