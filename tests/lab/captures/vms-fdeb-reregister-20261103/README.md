# vms-fdeb: OVMX's op-0x0d directory re-registration, answered by a real V7.3

This is the lab fire for #2017. The cluster lane ran it in its own pod, `ovmx-ci/fdeb-gate`.
The payload came from openvmx-site run 41581050721 (preflight + capture, `ref=work/vms-fdeb-02-0d-reregister`).
Node B came from vms run 41578788593, and its manifest names commit 323c05998.
The visitor gate (`tools/cluster-web-demo/visitor-gate.mjs`, from the same ref) ran both orders 10 times each, alternating, with the page's 0x6007 frame tap on.

| file | |
|---|---|
| `tally.txt` | per run: verdict, bugcheck lines in the consoles, OVMX op-0x0d sent and VAXC 82/0d answers (counted from each run's `frames.jsonl`) |
| `c-first-legacy-r1-frames.jsonl.gz` | every 0x6007 frame of run 1. It is gzipped so the corpus censuses that walk `*.jsonl` keep their pins |
| `c-first-legacy-r1-*.console.log` | the three consoles of run 1 |
| `c-first-legacy-r2-stall-VAXC.console.log` | VAXC in the failing run 2 (see below) |

## On the wire (run 1; frame indices as `transition_dlm_census.frames()` reads the decompressed file)

```
1723  VAXC  -> OVMXA 01/0c  release 4 of epoch 5 (OVMXA's addition)
1724..1728 VAXC -> OVMXA/B 02/0d   VAXC re-registers its own resources
1734  OVMXA -> VAXC  02/0d  OVMXA re-registers RMS$ (fid on OVMXASYS), the resource class of the vms-8cb5 crash
1736  VAXC  -> OVMXA 82/0d  the record echoed, body[34] = 0xf9
1759  OVMXA -> VAXC  01/0b  step report 5, sent only after the answer
```

## Tallies

| order | runs | CN=3 + every node usable | bugchecks | 02/19 | runs where OVMX sent 02/0d to VAXC | VAXC answered 82/0d |
|---|---|---|---|---|---|---|
| c-first-legacy | 10 | 9 | 0 | 0 | 5 | 5 of 5 |
| page-order-throttled4 | 10 | 9 | 0 | 0 | 0 (one run sent 1 to OVMXA) | -- |

OVMX sends a record only for a root it masters when a transition reaches release 4.
In most orders it masters nothing at that point, so most runs carry none.

## The two failures are not this change

Both failures are in run 2, which ran 05:20 to 06:41.
In each, VAXC stopped transmitting about 75 to 85 s into the run, and its console froze mid-line on `CNXMAN, completing VAXcluster state transition`.
There is no bugcheck text and no op 0x19.
OVMX sent no op-0x0d in either run.
VAXC's last frames are an ordinary OVMXB lookup, a 0xf9 answer, and the matching removal, the same pattern VAXC accepted many times earlier in those runs.
The same signature shows up 7 times in the cluster lane's p0-8cb5-gate pods, which ran builds without this change (pre-fix 500f19a1a and #2015) on the same node during the same hours.
So this is the existing VAXC startup stall, and this change does not cause it.
