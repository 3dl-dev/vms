# V0.7-6 on the public page: the VAX console is quiet, 11 of 12 runs CN=3 (2026-10-02)

`https://openvmx.3dl.dev/demo/cluster/` — headless Chromium, **no query parameters**, fresh context and
cold cache per run, **nothing typed**, grader panel-scrolling off. Node B and Node C come from
`vax.3dl.network`. Two six-case passes of `tools/cluster-web-demo/visitor-gate.mjs`.

**The pass bar moved with this release.** A run passes only if the cluster formed **and** the OVMX/VAX
node's RAW console carries no NetBSD boot output: #1347 (rd vms-553) silenced the substrate at the source
(an `OVMX_QUIET` kernel option plus a quiet secondary bootstrap), and the pcjs embed stopped filtering
(baron-3dl/pcjs `92e01064`), so the console a visitor reads is the claim.

## What was deployed

| piece | source | live sha256 (16) |
|---|---|---|
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | build-cluster-demo at tag V0.7-6 (`bfde7f5b`) | `a09ef586acd78e1c` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2.gz` | same; ODS-2 identity injected + read back | `be23601ecca44080` |
| `boot/vmlinuz` | openvmx-site track-release at V0.7-6 | `b5437c43c6f61d75` |
| `ovmx-vax-nodeB.img.gz` (vax.3dl.network) | run-boot.sh `sysboot-single` at V0.7-6 | `118659c2df7f158e` |

All four fetched back **over the public URL and hashed** before grading. Node A's initramfs and the
deployed kernel share vermagic `6.12.103-ovmx SMP preempt mod_unload` and executive `srcversion`
`783E572A57AC4C4A0E906A1`. Node B carries `SCSNODE OVMXB, SCSSYSTEMID 1988, VOTES 0, EXPECTED_VOTES 1,
VAXCLUSTER 2` and `CLUSTER_AUTHORIZE.DAT` group 257 (`01 01`, read back out of the file); its build
applied `netbsd-ovmx-quiet.patch` and configured the kernel `OVMX_QUIET + MSGBUFSIZE`. Node C is the
pinned real VAX/VMS V7.3 volume, unchanged.

## The console, as a visitor reads it

`pass2/01-page-order/OVMXB.console.log`, verbatim from the top — ROM self-test, VMB, then the executive:

```
KA655-B V5.3, VMB 2.7
Performing normal system tests.
40..39..38..37..36..35..34..33..32..31..30..29..28..27..26..25..
...
Tests completed.
>>>B DUA0
(BOOT/R5:0 DUA0)
  2..
-DUA0
  1..0..

%OVMX-I-EXEC, VMS executive attached on /dev/vms

    OpenVMX V0.7-6 - OpenVMS-compatible

%OVMX-I-SYSDISK, mounting system disk DUA0:
%OVMX-I-MOUNTED, system disk DUA0: mounted
%OVMX-I-SCSNODE, node name OVMXB set from SYS$SYSTEM:OVMXVMSSYS.PAR
%PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
```

Nothing between VMB and the executive. **Zero NetBSD markers on Node B's console in all twelve runs** —
`substrate_noise` is `{}` in every one, and an independent grep for the six markers the V0.7-2 disk used
to print (`>> NetBSD/vax boot [`, kernel version line, NetBSD Foundation copyright, `MicroVAX 3800/3900`,
`total`/`avail memory =`, `Detecting hardware...`) returns 0 for every file. The build's own final boot of
the shipped disk says the same thing before it ever reached the page
(`nodeB-build-final-boot-console.txt`).

## Results

| case | order | gap | cpu | pass 1 | pass 2 | worst drift |
|---|---|---|---|---|---|---|
| page-order | A,B,C | 5 s | ×1 | CN=3 | CN=3 | 327 / 170 ms |
| all-at-once | A,B,C | 0.5 s | ×1 | CN=3 | CN=3 | 222 / 59 ms |
| page-order-throttled4 | A,B,C | 5 s | ×4 | **NOT CN=3** — Node A starved | CN=3 | 1665 / 2027 ms |
| b-first | B,A,C | 3 s | ×1 | CN=3 | CN=3 | 156 / 68 ms |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 | CN=3 | 74 / 32 ms |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 | CN=3 | 547 / 597 ms |

**11 of 12**, and across all twelve: zero bugchecks, zero lost connections, **zero restarts on any node**,
`ovmx_founded` empty, `silent_never_starts` 0 (the panel watchdog from rd vms-bfdd never had to fire).
Both OVMX nodes announce **OpenVMX V0.7-6** (23 occurrences, nothing older), Node C is
`OpenVMS (TM) VAX Version V7.3`.

## The one failure: Node A starved at ×4 CPU throttle

`pass1/03-page-order-throttled4-nodeA-starved/OVMXA.console.log` is 445 bytes and ends at
`Booting from ROM...` with the screen clear that follows it — the Linux guest started and printed nothing
more inside the 600 s window. Worst main-thread drift that run: **1665 ms**.

Two things it is **not**: it is not rd vms-4ff's IO-APIC panic (no `Kernel panic`, no `IO-APIC` anywhere in
the console, and the live page does carry the remedy — `console=ttyS0 loglevel=3 quiet no_timer_check`,
checked against the deployed `node-worker.js`), and it is not the grader (scrolling is off, and the same
case passed in pass 2 at an even higher drift of 2027 ms). It is the qemu-wasm guest getting too little CPU
when the browser is throttled 4× with three emulators running — the harshest row in the matrix, and the
only one that has ever failed this way.

## One observation, not a claim

Node B's console dates its banner ` 1-JAN-2010 13:07:47.62`: the emulated VAX has no battery-backed clock,
so the node starts at its epoch rather than today. Visible on the public page, nothing to do with this
deploy, and not filed as a defect here.
