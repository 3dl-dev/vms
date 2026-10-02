%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-s4b-2
class:     scs-msg
origin:    capture
capture:   af4-op08-s4b.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    70fdaaee084c08c519acdcd06a31a5782714d826de91b55a1db1300717e06554
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-s4b.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1990.
; 4 members (VAXC csid 1, OVMXA 2, OVMXB 3, OVMXC 4); OVMXB (slot 3) removed; the transition keeps slots 1,2,4
@0    52 54 00 00 df 0c 08 00 2b 1c 1d ad 60 07 bc 00
@16   aa 00 04 00 c6 07 01 01 aa 00 04 00 c5 07 4b 13
@32   ef 00 b1 01 01 00 12 00 ef 00 00 00 b1 01 00 00
@48   ef 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 2e f8 0b 00 e8 db 95 01 d3 00 13 00 11 cb
@80   01 08 00 00 06 00 00 00 40 03 00 00 05 00 02 00
@96   03 00 00 00 03 02 00 00 a0 5f e2 b8 81 35 bc 00
@112  c0 b4 ec 06 82 35 bc 00 00 c5 07 00 00 00 00 16
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 03 00 20 20 20 04 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 23 c8 53 a6
