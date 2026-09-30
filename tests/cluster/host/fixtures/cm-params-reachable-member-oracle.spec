%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-reachable-member-oracle
class:     scs-msg
origin:    capture
capture:   e88-C3-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j) + rd vms-e88 "PARAMS body[18:20] = the sender's member count"
wire-len:  204
sha256:    e93e198ee50c33a75e2e212d5db0754babc5cd908a233efe4b64b6c754576b02
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 40 of
; tests/lab/captures/vms-e88-join-target-20260930/e88-C3-20260930.pcap) sent by
; a real OpenVMS VAX V7.3 MEMBER (VAX1, SCSSYSTEMID 1025, VOTES 1) to a real
; joiner (VAX2, 1026) that could reach it and NOT the other member: the record
; the joiner held for five minutes without asking anybody for admission.
;
;   body[18:20] (abs 90) = 02 00  the sender's member count: two members
;   body[22:24] (abs 94) = 01 00  VOTES 1
@0    08 00 2b 13 f1 57 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 5b 13
@32   0a 00 0d 00 01 00 12 00 0a 00 00 00 0d 00 00 00
@48   0a 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   08 00 d8 3f 0c 00 d0 c2 02 00 00 00 00 00 00 00
@80   01 01 00 00 21 50 00 00 00 00 02 00 01 00 01 00
@96   01 00 01 00 00 62 24 16 04 34 bc 00 80 64 03 32
@112  04 34 bc 00 03 00 00 00 00 00 00 00 0f 02 40 20
@128  00 00 00 00 31 20 20 20 80 64 03 32 04 34 bc 00
@144  10 00 00 00 01 00 00 00 00 00 2b 00 40 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 05 b2 f8 9b
