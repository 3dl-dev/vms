%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-s4a-1
class:     scs-msg
origin:    capture
capture:   af4-op08-s4a.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    b12318806f5c1882919d72134999bc9e05aded72827b480316781b64d3da2134
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-s4a.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1988.
; 4 members (VAXC csid 1, OVMXA 2, OVMXB 3, OVMXC 4); OVMXA (slot 2) removed; the transition keeps slots 1,3,4
@0    52 54 00 00 df 0b 08 00 2b 1c 1d ad 60 07 bc 00
@16   aa 00 04 00 c4 07 01 01 aa 00 04 00 c5 07 4b 13
@32   a3 01 77 02 01 00 12 00 a3 01 00 00 77 02 00 00
@48   a3 01 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 bf 63 0a 00 e9 db 53 02 7f 01 0c 00 2e cb
@80   01 08 00 00 06 00 00 00 40 03 00 00 05 00 02 00
@96   03 35 00 00 03 02 00 00 80 e1 f0 25 81 35 bc 00
@112  20 40 e9 74 81 35 bc 00 00 c5 07 00 00 00 00 1a
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 04
@160  00 00 00 03 00 00 00 00 04 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 9e 0a ae 99
