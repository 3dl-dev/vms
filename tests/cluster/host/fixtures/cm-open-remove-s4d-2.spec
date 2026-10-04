%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-s4d-2
class:     scs-msg
origin:    capture
capture:   af4-op08-s4d.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    2f6a941b6e109173c614ed2076d535efd2906780303f387bef3293b247abd11e
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-s4d.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1988.
; 4 members (VAXC csid 1, OVMXA 2, OVMXB 3, OVMXC 4); OVMXC (slot 4) removed; the transition keeps slots 1,2,3
@0    52 54 00 00 df 0b 08 00 2b 1c 1d ad 60 07 bc 00
@16   aa 00 04 00 c4 07 01 01 aa 00 04 00 c5 07 4b 13
@32   a2 01 76 02 01 00 12 00 a2 01 00 00 76 02 00 00
@48   a2 01 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 4d 5c 0a 00 e9 db 52 02 7e 01 0e 00 29 cb
@80   01 08 00 00 06 00 00 00 40 03 00 00 05 00 02 00
@96   03 00 00 00 03 03 00 00 a0 9d 33 94 80 35 bc 00
@112  00 73 96 e2 80 35 bc 00 00 c5 07 00 00 00 00 0e
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 03 00 01 04 00 04 00 f8 7f 00 c0 b1 b0
@176  83 41 00 60 ee 78 de ff ff ff 00 00 5c 00 01 00
@192  00 00 00 00 00 00 00 40 9e ec 26 ef
