%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-b36or-2
class:     scs-msg
origin:    capture
capture:   af4-op08-b36or.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    500cf62a35afcbbae849952f26f9e57098e34bf9e1ea47b7ef0289f2976e0b2b
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-b36or.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1026 (coordinator) -> 1025.
; 3 real VAX V7.3 members (VAX1 csid 1, VAX2 2, VAX3 readmitted as a NEW incarnation at csid 00010004); VAX3 (slot 4) removed a second time, VAX2 coordinating; the transition keeps slots 1,2
@0    aa 00 04 00 01 04 08 00 2b 9c d9 04 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   0f 26 74 35 01 00 12 00 0f 26 00 00 74 35 00 00
@48   0f 26 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 52 2f 08 00 52 2f 85 34 1e 25 0b 00 b0 1e
@80   01 08 58 63 08 00 00 00 40 03 00 00 05 00 01 00
@96   03 00 01 00 02 01 00 00 20 ca 91 dc 8c 30 bc 00
@112  c0 dc e5 79 8e 30 bc 00 00 01 04 00 00 00 00 06
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 05
@160  00 00 00 01 00 00 00 00 04 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 08 cf 00 87 00 00 00 00
