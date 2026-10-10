# vms-cab op-0x0f answer: pre-registered prediction (2026-10-10)

This was committed BEFORE the held-out capture was taken.

## Derivation (all lab captures up to 2026-10-10 17:40, real VMS <-> VMS pairs only)

`op0f_classify.py` over every pcap in the lab library:

- **894 unique op-0x0f / 82-0x15 pairs.**
- **688 come from a responder with no observed interest in the resource.** "Interest" means it never sent or received a lock-acquiring request (0x01 / 0x06 / 0x07 / 0x0d) for that name. All 688 answer with the RULE: the request echoed, cat 0x82, op 0x15, body[28:32] = 0, body[32:36] = 01 00 fa 00. The comparison is byte for byte over body[4:132].
- **206 come from a responder with interest.**
  - 184 of them follow the RULE.
  - The other 22 keep the request's body[28:32] instead of clearing it. Those are the 83 the first scan counted; the rest of the 83 were pairing artefacts, and dedup plus pairing on body[4:8] removed them.
- **Who sends the 22.** 21 of the 22 have request body[28] = 3, and each comes from the responder that had been *sent* lock requests for the name, i.e. the resource's master. Where one request fanned out to two responders, the master kept body[28:32] and the non-master cleared it, 21 of 21.

**The 83 are explained:** the differing answers come only from a node that holds or masters the resource. OVMX answers only when its lock engine holds no lock on the resource (no granted, waiting or proxy lock: `vms_lock_dlm_name_in_use`), so it never stands in that class. Everything else is withheld and counted (`op0f_withheld_held`).

## Prediction for the held-out capture

**Setup.** VAX-only, in pod `vaxlab-3`, with OVMXE not running. tcpdump starts before the VAXes join, so interest is fully observed. Then:
1. Cycles of VAX1 boot, cluster mount of VAX1DATA with file I/O, DISMOUNT/CLUSTER, and SHUTDOWN with REMOVE_NODE.
2. The same cycle with VAX2 leaving.

**P1.** Every pair whose responder has no observed interest matches the RULE over body[4:132]. Zero misses. This is the exit status of `op0f_classify.py`.

**P2.** Every pair that differs from the RULE comes from a responder with interest.

If P1 fails even once, #1615 does not merge.
