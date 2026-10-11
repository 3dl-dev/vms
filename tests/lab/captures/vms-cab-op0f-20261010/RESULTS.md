# vms-cab op-0x0f answer: held-out result (2026-10-10)

The prediction (`PREDICTION.md`) was committed in bcd2eb828 before either capture was taken.

Two VAX-only captures, OpenVMS VAX V7.3, VAX1 and VAX2, pod `vaxlab-3`, with OVMXE not running. Each pod was restarted before its capture, and tcpdump started before the VAXes joined.

| capture | arm | sha256 (uncompressed) |
|---|---|---|
| `heldout1.pcap.gz` | boot and join; VAX1 file I/O on `$2$DUA1:`; DISMOUNT/CLUSTER, MOUNT/CLUSTER; VAX1 SHUTDOWN with REMOVE_NODE | 8f4412968055995060e18dcdfdc481a00c3dfaafad65e0204b8321bd097cd652 |
| `heldout2.pcap.gz` | the same with VAX2 doing the I/O, the cluster dismount/mount and the departure | 8c8da4f3b91c37b1c0ae1a629fd0cb4d26e5081182da14e57cdd21ba71bb262b |

`gunzip -k heldout*.pcap.gz && python3 op0f_classify.py heldout1.pcap heldout2.pcap`:

```
pairs 297
no-interest responders (OVMX answers): 287 of 287 match the rule
interest responders (OVMX withholds): 10 match, 0 differ
```

| | result |
|---|---|
| **P1** | holds: 287/287, zero misses |
| **P2** | holds, but vacuously. No answer differed from the rule, so these captures exercise no master-kept answer. That class is known only from the 22 derivation pairs, and OVMX withholds in it. |
| bugchecks | none on either VAX |
| departures | both completed (VAX1 removed 17:52:13, VAX2 removed 18:00:39) |
