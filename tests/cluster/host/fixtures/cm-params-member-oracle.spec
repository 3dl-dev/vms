%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-member-oracle
class:     scs-msg
origin:    capture
capture:   e88-C3-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j) + rd vms-e88 "PARAMS body[18:20] = the sender's member count"
wire-len:  204
sha256:    6fc26c2ecce23fa6957e308b262dec8b0a5d784a3db86f6b916f4b549213550a
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 623 of
; tests/lab/captures/vms-e88-join-target-20260930/e88-C3-20260930.pcap) sent by a real
; OpenVMS VAX V7.3 MEMBER (VAX3, SCSSYSTEMID 1027, the founder) to a real
; JOINER (VAX2, 1026) in a cluster of two members {VAX3, VAX1}. It is the
; frame that ended the joiner's wait in experiment C3: VAX2 had been held off
; from VAX3 at the bridge for five minutes while it had a connection to VAX1
; alone, VAX1 advertising 2 members, and VAX2 sent no membership request to
; anybody; 9.8 s after THIS record arrived it sent one -- to VAX3.
;
; THE FIELD: body[18:20] (abs 90) = 02 00 -- the sender's cluster member
; count. Every member PARAMS in the four e88 trios carries its own count (1
; alone, 2 with two); every joiner PARAMS carries 0.
;   body[22:24] (abs 94) = 01 00  VOTES 1 (sec 4(j))
;   body[12]    (abs 84) = 0x21   nonzero on every member record, 0 on every
;                                 joiner record: NOT decoded, NOT used.
@0    08 00 2b 13 f1 57 08 00 2b 00 d2 ff 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   0b 00 0b 00 01 00 12 00 0b 00 00 00 0b 00 00 00
@48   0b 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0c 00 d7 3f 0d 00 52 2f 02 00 00 00 00 00 00 00
@80   01 01 00 00 21 50 00 00 00 00 02 00 01 00 01 00
@96   01 00 01 00 00 62 24 16 04 34 bc 00 80 64 03 32
@112  04 34 bc 00 03 00 00 00 00 00 00 00 00 00 02 00
@128  01 00 c0 42 00 00 c0 42 00 62 24 16 04 34 bc 00
@144  10 00 00 00 01 00 00 00 00 00 2b 00 bf 01 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 08 00 08 00
@176  00 00 ff ff ff ff ff ff ff ff 04 00 01 00 49 4e
@192  54 45 52 6e 65 74 20 20 20 20 3c 73
