%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-joiner-oracle
class:     scs-msg
origin:    capture
capture:   e88-C3-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j) + rd vms-e88 "PARAMS body[18:20] = the sender's member count"
wire-len:  204
sha256:    d75150bcf788996219a28f3593cca3edf3be6e00da8408468c913f95fc2720d6
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 43 of
; tests/lab/captures/vms-e88-join-target-20260930/e88-C3-20260930.pcap) sent by a real
; OpenVMS VAX V7.3 JOINER (VAX2, SCSSYSTEMID 1026, VOTES 0) to a member (VAX1,
; 1025). A system in no cluster advertises a member count of ZERO:
;   body[18:20] (abs 90) = 00 00
;   body[22:24] (abs 94) = 00 00  VOTES 0
@0    aa 00 04 00 01 04 08 00 2b 13 f1 57 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   0b 00 0d 00 01 00 12 00 0b 00 00 00 0d 00 00 00
@48   0b 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0c 00 d0 c2 08 00 d8 3f 02 00 00 00 00 00 00 00
@80   01 01 00 00 00 50 00 00 00 00 00 00 01 00 00 00
@96   01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 80 4a 3f 0e 57 9f 00
@144  10 00 00 00 01 00 00 00 00 00 2a 00 0c 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
