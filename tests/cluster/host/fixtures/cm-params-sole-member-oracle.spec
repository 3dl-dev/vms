%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-sole-member-oracle
class:     scs-msg
origin:    capture
capture:   e88-B-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j) + rd vms-e88 "PARAMS body[18:20] = the sender's member count"
wire-len:  204
sha256:    78138da53186e31ac124acf95c0317e400df4cf7241d212c1ad90277f6310b09
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 47 of
; tests/lab/captures/vms-e88-join-target-20260930/e88-B-20260930.pcap) sent by
; a real OpenVMS VAX V7.3 that is the ONLY member of its cluster (VAX1,
; SCSSYSTEMID 1025, the founder) to a real joiner (VAX2, 1026): the member's
; reciprocation a joiner waits for before it asks for admission.
;
;   body[18:20] (abs 90) = 01 00  the sender's member count: itself alone
;   body[22:24] (abs 94) = 01 00  VOTES 1 (sec 4(j))
@0    08 00 2b 13 f1 57 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 5b 13
@32   0f 00 0f 00 01 00 12 00 0f 00 00 00 0f 00 00 00
@48   0f 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   09 00 fe 7f 0b 00 52 2f 02 00 00 00 00 00 00 00
@80   01 01 00 00 21 50 00 00 00 00 01 00 01 00 01 00
@96   01 00 01 00 20 cc 75 68 03 34 bc 00 20 cc 75 68
@112  03 34 bc 00 02 00 00 00 00 00 00 00 25 00 60 00
@128  c0 be f8 7f 7e 00 00 00 20 cc 75 68 03 34 bc 00
@144  10 00 00 00 01 00 00 00 00 00 2b 00 7c 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
