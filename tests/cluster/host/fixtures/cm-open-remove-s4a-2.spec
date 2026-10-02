%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-s4a-2
class:     scs-msg
origin:    capture
capture:   af4-op08-s4a.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    ec4f0c2745fe0e2c71d1787c9a35a6470bc9d7b6153340f2c6af34d62001274f
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-s4a.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1990.
; 4 members (VAXC csid 1, OVMXA 2, OVMXB 3, OVMXC 4); OVMXA (slot 2) removed; the transition keeps slots 1,3,4
@0    52 54 00 00 df 0c 08 00 2b 1c 1d ad 60 07 bc 00
@16   aa 00 04 00 c6 07 01 01 aa 00 04 00 c5 07 4b 13
@32   ef 00 b0 01 01 00 12 00 ef 00 00 00 b0 01 00 00
@48   ef 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 50 fb 0b 00 e8 db 94 01 d3 00 16 00 0b cb
@80   01 08 00 00 06 00 00 00 40 03 00 00 05 00 02 00
@96   03 00 00 00 03 02 00 00 80 e1 f0 25 81 35 bc 00
@112  20 40 e9 74 81 35 bc 00 00 c5 07 00 00 00 00 1a
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 03 00 00 00 00 04 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 b3 50 fb 55
