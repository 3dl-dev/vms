# The multicast fix on the wire: Node A's executive starts RECEIVING a real VMS node's HELLOs

**rd vms-147. In-browser cluster demo, headless chromium on `k3s-worker`
(`ovmx-lab/cluster-demo-proof`), 2026-09-22. One variable: the group → multicast
derivation.**

## The rig

| | |
|---|---|
| **Node A** | OpenVMX x86_64 under qemu-wasm (real Linux kernel + `vms.ko`), `SCSNODE=OVMXA`, `SCSSYSTEMID=1987`, `VOTES=1`, `EXPECTED_VOTES=2`, **group 257**. Boot artifacts built by `build-boot-artifacts.yml` from commit `5b2444d4` (the fix), config-injected by `tools/cluster-web-demo/inject-cluster-config.sh` + `inject-ods2-config.sh`. |
| **Node C** | a **real OpenVMS VAX V5.5-2H4** volume on pcjs (KA655), `SCSNODE=VAXC`, `SCSSYSTEMID=1989`, **group 257**, already a one-member VMScluster. Not OVMX, not modified. |
| **Wire** | the demo page's in-page JS L2 hub — a dumb repeater, no relay server. Node A's NIC is a **real QEMU `virtio-net-pci`**, so QEMU's `virtio_net_receive_filter()` enforces the guest's programmed MAC table: a frame the guest never asked for is dropped **before** the executive, and is counted nowhere. |

## Before (V0.7, shipped): `rx 0` for the whole run

Node A derived the HELLO multicast as `LE16(group)` → `ab:00:04:01:01:01`, and
enabled **that** address in the NIC filter. Node C, a real VMS node configured
for group 257, transmits to `ab:00:04:01:01:02`. Node A's own report:

    frames tx 94 (errors 0), rx 0 (dropped: nobuf 0, badclass 0)

`rx 0` with both drop counters at 0 is the signature of a filter drop one layer
below the executive — the frames reached the NIC and were never delivered.

## After (this fix): `rx` CLIMBS, 0 → 553 in one 8-minute window

`nodeA-run.log`, `result.json`, screenshot `nodeA-local-ports.png`:

    t=94s   *** Node A executive frames: tx=20  RX=60  (nobuf=0 badclass=60) ***
    t=192s  *** Node A executive frames: tx=68  RX=191 (nobuf=0 badclass=191) ***
    t=316s  *** Node A executive frames: tx=129 RX=345 (nobuf=0 badclass=345) ***
    t=477s  *** Node A executive frames: tx=208 RX=553 (nobuf=0 badclass=553) ***

and on the console an operator actually reads:

    PEA0:  open, link up
           hardware address 52-54-00-00-00-0A
           MTU 1500, channels 0, circuits 0
           cluster group 257 (CLUSTER_AUTHORIZE)
           frames tx 177 (errors 0), rx 481 (dropped: nobuf 0, badclass 481)

**That is the fix, measured:** the executive now transmits to, and filters for,
the address a real VMS node on the same group is actually using, so a real
OpenVMS node's HELLOs reach OVMX's port driver for the first time in this rig.

## CN=2 did NOT form, and the reason is a DIFFERENT, precisely measured thing

Every one of those 553 frames is counted `badclass` — the port receives it and
the codec cannot classify it. `hub-frames.json` holds the raw bytes of 60
frames off the hub (53 from VAXC, 7 from OVMXA). Fed to the executive's OWN
classifier (`vms_frame_classify`, `vms_hello_parse`):

    VAXC  len=128 family=0 (UNCLASSIFIED) sca_content=114 disc_class[36]=0x03  vms_hello_parse=E_CLASS
    OVMXA len=134 family=1 (DISCOVERY) cls=1 (HELLO) sca_content=120 disc_class[36]=0x05  vms_hello_parse=OK

Byte-for-byte, the same field positions, different values:

| abs | OVMXA (what OVMX implements, from the **V7.3** corpus) | VAXC (**V5.5-2H4**) |
|---|---|---|
| 0–5 dst | `ab 00 04 01 01 02` | `ab 00 04 01 01 02` — **same group address, the fix working** |
| 14–15 SCA length | `76 00` → 120 content | `70 00` → **114 content** |
| 16–21 dst logical | `ab:00:04:01:01:02` | `ab:00:04:01:01:02` |
| 22–23 | `01 00` | **`01 01`** |
| 24–29 src logical | `aa:00:04:00:c3:07` (sysid 1987) | `aa:00:04:00:c5:07` (sysid 1989) |
| 30–31 per-frame word | `a0 00` (multicast) | `a0 00` — same |
| 32–35 | `08 00 00 80` | `08 00 00 80` — same |
| **36 message class** | **`05`** (HELLO) | **`03`** |
| 40.. node name | `05 "OVMXA"` | `06 "VAXC  "` |
| 48–64 capability span | zeros | `80 01 ff 83 00 04 …  10 03` |
| 94–95 | `92 05` | **`90 05`** |
| 102–111 tail const | `bc 00 03 58 51 41 00 00 00 00` | identical |
| 120–125 HW MAC | own MAC | own MAC |
| 126–127 | `26 00` | **`21 00`** |
| 128–133 | `00 00 64 00 00 00` | *absent — the frame ends at 127* |

**So: the two nodes are now correctly addressed to each other and each other's
frames arrive — and they are speaking two different revisions of the NISCA
discovery protocol.** OVMX's codec is grounded entirely on an **OpenVMS VAX
V7.3** capture corpus (`docs/cluster-protocol-spec.md`; `SWVers: V7.3` in
`sda-scs-extract-vax1.txt`). Node C is **V5.5-2H4**, eight years earlier, and
its HELLO is a 114-byte-content, message-class-`0x03` frame OVMX has never seen
and does not decode. Node C likewise never engaged OVMX's V7.3-shaped HELLO.

That is the next blocker for an in-browser CN=2, it is a clean-room RE item
against a V5.5 oracle (or a V7.3 Node C volume), and it is **not** the address
defect this capture closes. The lab CN=2/CN=3 joins against real **V7.3** VAXes
are unaffected: they were, and remain, real.

## Files

| file | what |
|---|---|
| `nodeA-run.log` | the full driver log: boot, login, and every `SHOW CLUSTER/LOCAL_PORTS` sample |
| `result.json` | end-of-run state: `execLP {tx 208, rx 553, nobuf 0, badclass 553}`, hub frame census, both nodes' console tails |
| `hub-frames.json` | raw bytes of 60 0x6007 frames off the in-page hub (the V5.5 specimens above) |
| `nodeA-local-ports.png` | the demo page at the end of the run — Node A's own `SHOW CLUSTER/LOCAL_PORTS` |
