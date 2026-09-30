# V0.7-4 on the public page: the visitor matrix passes, 12 of 12 (2026-09-30)

`https://openvmx.3dl.dev/demo/cluster/` — headless Chromium, **no query parameters**, fresh context and
cold cache per run, **nothing typed into any console**. Node B and Node C come from `vax.3dl.network`.
Two full six-case passes of `tools/cluster-web-demo/visitor-gate.mjs`, varying click ORDER and SPEED
plus CPU throttling.

## What was deployed

| piece | source | live sha256 (16) |
|---|---|---|
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | build-cluster-demo at tag V0.7-4 (`e2c8ddda`) | `368f58bf6ce03c92` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | same; ODS-2 identity injected + read back | `350913d80ed5eff4` |
| `boot/vmlinuz` | openvmx-site track-release at V0.7-4 | `3ba04cf906ec9658` |
| `ovmx-vax-nodeB.img.gz` (vax.3dl.network) | run-boot.sh `sysboot-single` at V0.7-4 | `143827e84e7ae814` |

All four were fetched back **over the public URL and hashed** before grading. Node A's initramfs and the
tracker's kernel carry the same vermagic (`6.12.103-ovmx SMP preempt mod_unload`) and the same executive
`srcversion` (`0012D5AF8A9F03F1AAB83B8`) — the lock `vms.ko` requires, checked rather than assumed.
Node B's identity is injected at build time: `SCSNODE OVMXB, SCSSYSTEMID 1988, VOTES 0,
EXPECTED_VOTES 1, VAXCLUSTER 2`, `CLUSTER_AUTHORIZE.DAT` group 257 (`01 01` on the wire). Node C is the
pinned real VAX/VMS V7.3 volume, unchanged.

## Results — `clean-pass1/`, `clean-pass2/`

| case | order | gap | cpu | pass 1 | pass 2 | worst main-thread drift |
|---|---|---|---|---|---|---|
| page-order | A,B,C | 5 s | ×1 | CN=3 | CN=3 | 373 / 95 ms |
| all-at-once | A,B,C | 0.5 s | ×1 | CN=3 | CN=3 | 29 / 207 ms |
| page-order-throttled4 | A,B,C | 5 s | ×4 | CN=3 | CN=3 | 1189 / 1006 ms |
| b-first | B,A,C | 3 s | ×1 | CN=3 | CN=3 | 89 / 184 ms |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 | CN=3 | 207 / 139 ms |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 | CN=3 | 219 / 532 ms |

**12 of 12.** Across all twelve runs: **zero** bugchecks, **zero** lost connections, **zero** restarts on
any node, and `ovmx_founded` empty — no OVMX node founded a cluster in any order at any speed. Both
OVMX nodes announce **OpenVMX V0.7-4** on their own consoles (24 occurrences, no older version); Node C
is `OpenVMS (TM) VAX Version V7.3`. That is the first clean sweep of the matrix on a public deploy, and
the release it is running is the one that closed the stalled-guest CNXMGRERR (#1327, 20/20 on the stall
rig).

## The first two passes were my instrument, not the product — `graded-with-the-broken-instrument/`

Graded with the gate as it stood, V0.7-4 scored **3 of 6 twice**, every failure `no SCA frames from
OVMXA`: Node A booted, mounted its disk, and then stopped — at a different point each time
(`pass1-02-all-at-once/OVMXA.console.log` stops after `%OVMX-I-MOUNTED`; other runs stopped at
`Booting from ROM...`, before the kernel banner). One run captured the real thing behind it
(`pass2-04-b-first-kernel-panic/OVMXA.console.log`):

```
[    0.000000] ..MP-BIOS bug: 8254 timer not connected to IO-APIC
[    0.000000] Kernel panic - not syncing: IO-APIC + timer doesn't work!
[    0.000000]  setup_IO_APIC+0x836/0x850
```

That is the kernel's early timer-routing check failing inside qemu-wasm, at `[0.000000]`, before
anything OVMX runs — and it is timing-sensitive, so it fails when the guest's worker is starved.

**What was starving it was the gate.** To read a pcjs panel it scrolled the panel into view first, three
canvas-heavy cross-origin iframes every 15 s, which existed only because an offscreen panel used to stop
repainting (rd vms-0bc, since fixed in the embed). Turning that off is the only change between the two
sets of numbers above: **6 of 12 with it on, 12 of 12 with it off**, same pod, same hour, same page.
Scrolling is opt-in now (`REVEAL=1`), never for grading.

A separate control says the same thing from the other side (`clean-pass1/nodeA-boot-probe.log`): six
cold loads of the live page, all three nodes started as a visitor would, Node A classified by its own
console — **6 of 6 reached `%CNXMAN`**.

The IO-APIC panic is still worth having on the record: it is one observed early-boot panic under heavy
main-thread contention, and the kernel names its own remedy (`no_timer_check`, which skips exactly the
check that panicked). Filed rather than shipped blind — a cmdline change to the public page needs its
own measurement, and with the instrument fixed the condition no longer reproduces here.

## Pre-deploy baseline — `pre-deploy-v073/`

The live V0.7-3 page, graded on the same pod an hour before the deploy with the same (then-unfixed)
instrument: 5 of 6, the one failure the same `no SCA frames from OVMXA`. So the V0.7-3 numbers in
`vms-deploy-v073-20260929/` carry the same instrument load, and the honest comparison between the two
releases is the one made with the fixed gate — which only V0.7-4 has had.
