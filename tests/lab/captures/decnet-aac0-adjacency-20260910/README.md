# First live OVMX ⇄ real-VMS DECnet Phase IV adjacency (rd vms-aac0)

**2026-09-10.** An OVMX-on-NetBSD/vax node, running the DECnet router mode
(`DECNETD.EXE --router`, rd vms-0a9) with the RSLIST-tail encode fix (rd vms-df5),
advertised as an L1 router and a **real OpenVMS VAX V7.3 endnode selected it as
its designated router** — the first live OVMX↔real-VMS Phase IV adjacency.
Rung 2 of the DECnet north-star (rd vms-e4dc), parent rd vms-30e.

Captured on an ISOLATED own-lab (k3s pod vaxlab-3, private br0) — the shared
oracle nodes (vaxlab-0/1/2) were untouched.

## The two gates (both PASS)

### Adjacency — the real VMS endnode selects OVMX (`aac0-PASS.pcap`)
The real VAX1 (DECnet 1.1) endnode-hello's designated-router field flips from
`rtr 0.0` to `rtr 1.42` (OVMX) ~10s (2 hellos) after OVMX's router started:
```
BEFORE  aa:00:04:00:01:04 > ab:00:00:03:00:00  endnode-hello src 1.1 blksize 1498 rtr 0.0   hello 15
AFTER   aa:00:04:00:01:04 > ab:00:00:03:00:00  endnode-hello src 1.1 blksize 1498 rtr 1.42  hello 15
```
(Independently re-decoded: VAX1 advertised rtr 0.0 ×15, then rtr 1.42 ×9.)
Real VMS OPCOM on VAX1: `DECnet event 4.10, circuit up` + `event 4.15, adjacency
up, Circuit QNA-0, Adjacent node = 1.42`.

### Acceptance — real VMS accepts the corrected frame; event-4.4 storm gone
OVMX's router-hello is now the correct **27-byte** message (DN length prefix
`0x1b`), where the earlier 18-byte frame was rejected `DECnet event 4.4, packet
format error` (×112). Post-fix, VAX1's event-4.4 count = 0.
```
aa:00:04:00:2a:04 > ab:00:00:04:00:00  DN(0x6003)  router-hello l1rout vers 2 src 1.42 pri 64 hello 5
  routing msg: 0b 02 00 00  aa 00 04 00 2a 04  02  da 05  40  00  05 00  00  08 00 00 00 00 00 00 00 00
               RFLAGS TIVER      ID (1.42)     II  BLKSZ  PRI AR  TIMER  MPD └── 9-byte RSLIST tail ──┘
```

## Why the 18-byte frame was wrong (rd vms-df5)

OVMX's original router-hello stopped after MPD (18 bytes), matching tcpdump's
LENIENT decoder struct `rhellomsg`. A conformant Phase IV router-hello carries a
mandatory RSLIST tail after MPD: for a lone router with no known router peers
(n=0) that is 9 bytes — `08` (RSLIST-length = 8+7n) + a 7-byte reserved Name field
(zero) + `00` (RSLIST-count = 7n). Omitting it → too short → event 4.4 → the real
node never selected OVMX. The fix appends the tail (fixed message 18→27).

Grounding (clean-room, Rule 8): the WIRE FORMAT is triple-grounded — (1) the Linux
kernel `net/decnet/dn_dev.c` real encoder, (2) a **real OpenVMS VAX V7.3 router-hello
specimen** captured on this lab (`real-router-hello-specimen.hex`, byte-identical),
and (3) this real-VMS **acceptance** run. The `dnet_router_hello.c` encoder C is
OVMX's own independent implementation, not a copy of any GPL source; the bytes on
the wire are the (uncopyrightable) protocol. The BCT3MULT hypothesis was refuted —
AREA→TIMER is direct in both the real specimen and OVMX.

## Safety — never crashed a peer
Throughout the wrong-frame run (112 rejections) AND after the fix, the real VMS
node **never bugchecked** — it logged the format error and kept running. The
honest-omit-ungrounded discipline meant the wrong frame was *short* (safely
rejected), never a guessed field that could crash the decoder.

## Files
- `aac0-PASS.pcap` / `aac0-PASS.txt` — the before→after capture (VAX1 rtr flip +
  the OVMX 27-byte frame). Re-decode: `tcpdump -r aac0-PASS.pcap -nne`.
- `real-router-hello-specimen.hex` — the real VAX V7.3 router-hello oracle the
  fix was byte-matched against.
