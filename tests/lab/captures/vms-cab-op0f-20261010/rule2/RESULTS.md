# vms-cab op-0x0f revised rule: held-out result (2026-10-10)

The prediction (`PREDICTION.md`) was committed in 8e72221ff before these captures were taken.

The captures are three-node VAX-only: OpenVMS VAX V7.3 VAX1/VAX2/VAX3 on the dlmlab golden volume, run inside pod `vaxlab-3` on a private bridge. tcpdump started before the first boot.

| capture | arm | sha256 (uncompressed) |
|---|---|---|
| `heldout-E4.pcap.gz` | LOCKDIRWT 0/1/0; VAX1 SHUTDOWN with REMOVE_NODE | 08afcce58bade5e21ff24f552deed796bc70752e2b1903e1365f1006e3c64258 |
| `heldout-E5.pcap.gz` | LOCKDIRWT 0/0/0; VAX2 was to depart | df6da5682c50d2f1d6fdfddc3f0314e67bd4f28806f3749cf9f6fdcfb5042f25 |
| `heldout-E5b.pcap.gz` | E5 repeated with a settle delay | 53bebd6b92ca0d9beca47eb3f662491714e781641eb04f9466d4be6ddc740226 |

`python3 op0f_role_rule.py heldout-E4.pcap heldout-E5.pcap heldout-E5b.pcap`:

```
Counter({(True, True): 425, (False, True): 372})
```

Every key in that output is `(clear_form, matches_rule)`.

| | result |
|---|---|
| **P1** | holds: 797/797 pairs match the rule, zero misses |
| **P2** | holds: 425 new-master (clear) answers and 372 member (keep) answers |

**Honest note on E5.** In both E5 runs, VAX2 stopped responding while VAX3 was joining: 87% CPU, no console output, and it was removed by the survivors. That happened before the planned VAX2 departure, so the E5 arm contributes only 11 pairs. E4 alone carries P1 and P2. Both E5 runs used the stock dlmlab golden volume, with the VAX simulator's `set noasynch` removed so the boot finishes in minutes. The hang is not investigated here.

The first rule (`../PREDICTION.md`, `../RESULTS.md`, 287/287) still holds on its own data, but it was a two-node confound: every recipient there was the new master. This rule supersedes it.
