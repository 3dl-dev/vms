# vms-629: a real VAX's op-0x08 barrier lookup, and the directory's answer (2026-10-05)

`cf1-op08.pcap` contains two records, trimmed verbatim by `pick08.py` from the dlmlab
**CF1** run. CF1 is a cold formation of two real OpenVMS VAX V7.3 nodes (VAX1 and VAX2,
`VOTES 1`, `EXPECTED_VOTES 2`, booted together):

1. **The request.** VAX1 sends VAX2 cat-0x02 **op 0x08**, naming `SYS$SYS_ID` + LE32(1025,
   its own SCSSYSTEMID) + 2×00 (a ROOT, hash at body[128:132]), at formation-barrier step ~7.
2. **The answer.** VAX2 replies with cat 0x82 **opcode 0x01**. The answer echoes the
   request, with body[34] = 0xf9 ("you master it").

`coldform-ev2-formation.pcap` (rd vms-6d3d) records 121 and 123 are the independent first
sample, showing the same shape with a different stale span.

**Why it matters.** In the cluster lane's runO scenario (OVMXA founds; a real VAXC proposes
a formation with OVMXA as a founding member; rd vms-f29):
- VAXC sends that op 0x08 for its own SYS$SYS_ID to OVMXA, its directory node.
- OVMX refused the op 0x08 under RULE C and never answered it.
- VAXC therefore held barrier release #7 forever.
- The cluster then admitted nobody.

The fix makes OVMX's directory role (rd vms-8219) answer an op 0x08 exactly as it answers
an op-0x01 root lookup.
