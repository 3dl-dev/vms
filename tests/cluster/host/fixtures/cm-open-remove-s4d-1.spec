%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-s4d-1
class:     scs-msg
origin:    capture
capture:   af4-op08-s4d.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    1567acc0ef92bb624c8f062215ca1aff6c456230926c93d2a7f0746d79a14533
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-s4d.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1987.
; 4 members (VAXC csid 1, OVMXA 2, OVMXB 3, OVMXC 4); OVMXC (slot 4) removed; the transition keeps slots 1,2,3
@0    52 54 00 00 df 0a 08 00 2b 1c 1d ad 60 07 bc 00
@16   aa 00 04 00 c3 07 01 01 aa 00 04 00 c5 07 4b 13
@32   6e 02 55 03 01 00 12 00 6e 02 00 00 55 03 00 00
@48   6e 02 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   05 00 3a ea 09 00 e8 db 20 03 39 02 0f 00 1d cb
@80   01 08 58 63 06 00 00 00 40 03 00 00 05 00 02 00
@96   03 00 00 00 03 03 00 00 a0 9d 33 94 80 35 bc 00
@112  00 73 96 e2 80 35 bc 00 00 c5 07 00 00 00 00 0e
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 03 00 01 01 14 04 00 0e 00 00 01 00 00
@176  00 14 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 43 b7 71 75
