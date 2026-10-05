%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-lockdirwt2-oracle
class:     scs-msg
origin:    capture
capture:   fcb-L2-params.pcap
spec:      docs/cluster-protocol-spec.md 4(j) "LOCKDIRWT -- GROUNDED by controlled reconfiguration" (rd vms-fcb)
wire-len:  204
sha256:    8c9d73cc6bba011e7300691be8ed361d6ab199729ecdd1b074696737f961518f
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 1 of
; tests/lab/captures/vms-fcb-lockdirwt-20261004/fcb-L2-params.pcap) sent by a real OpenVMS VAX V7.3
; VAX2 (SCSSYSTEMID 1026, the joiner), booted conversationally with SYSBOOT> SET LOCKDIRWT 2
; and read back with SYSBOOT> SHOW LOCKDIRWT on its own console (L2-vax2.console.log).
;
; THE FIELD: body[26:28] (abs 98) = 02 00 -- the sender's LOCKDIRWT 2.
;   body[18:20] (abs 90) = 00 00  member count (rd vms-e88)
;   body[22:24] (abs 94) = 00 00  VOTES (sec 4(j))
@0    aa 00 04 00 01 04 08 00 2b 92 0b 09 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 5b 13
@32   13 00 15 00 01 00 12 00 13 00 00 00 15 00 00 00
@48   13 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0a 00 53 2f 0a 00 ea 74 02 00 00 00 00 00 00 00
@80   01 01 00 00 00 50 00 00 00 00 00 00 01 00 00 00
@96   01 00 02 00 00 00 00 00 00 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 80 4a 3f 0e 57 9f 00
@144  10 00 00 00 01 00 00 00 00 00 2a 00 2a 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
