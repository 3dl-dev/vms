%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open09-merge-oracle
class:     scs-msg
origin:    capture
capture:   XI.pcap
spec:      Davis p. 7-40 Phase 1 contents; rd vms-f297 one-variable oracle
wire-len:  204
sha256:    9d065241d21bea64969aebc94020bb5032c35ba901561e4bfc1070f90aa27c7b
%bytes
; REAL captured frame 9 of tests/lab/captures/vms-f297-open-cells-20261008/XI.pcap
; VAX1 (LOCKDIRWT 1) admits VAX2 booted with LOCKDIRWT 0 and EXPECTED_VOTES 3:
; body[24] = 01, the MERGE rebuild (book p. 7-40: a zero-weight joiner among nonzero ones);
; body[96:98] = 03 = CEVOTES = max(EXPECTED_VOTES 3, votes 2); body[22:24] = 02 = (3 + 2) / 2.
@0    08 00 2b 92 0b 09 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 5b 13
@32   9e 00 4a 01 01 00 12 00 9e 00 00 00 4a 01 00 00
@48   9e 00 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0b 00 a9 20 0a 00 53 2f 08 01 5a 00 05 00 6c 1e
@80   01 09 03 00 03 00 00 00 40 02 00 00 03 00 02 00
@96   01 00 01 00 01 01 00 00 20 e3 32 cd 8c 3a bc 00
@112  e0 cb 4a a7 8d 3a bc 00 00 01 04 00 00 00 00 06
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02
@160  00 00 00 00 01 00 00 00 03 00 01 00 01 00 01 00
@176  02 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
