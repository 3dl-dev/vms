%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dirhash-answer-vax1
class:     scs-msg
origin:    capture
capture:   dirhash-L1.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY HASH -- body[128:132]" (rd vms-4fb)
wire-len:  204
sha256:    5579554216163158f3e3cfe1528d99eb1b92f8d8f47db6c6ea3281133c053b30
%bytes
; REAL captured frame 4 of tests/lab/captures/vms-4fb-dir-hash-20261004/dirhash-L1.pcap: VAX1's cat-0x82 answer to the DLMTA request from VAX2 (body[28:40] rewritten, body[34]=0xf9).
; Private three-node OpenVMS VAX V7.3 cluster, scenario L1 (rd vms-4fb README).
; body[8:10] = 82 01; body[36:44] parent span = 75a0870000000000;
; body[46] = 03; body[128:132] = e36f3300 (the directory hash value).
@0    08 00 2b 92 0b 09 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   30 36 9d 1f 01 00 12 00 30 36 00 00 9d 1f 00 00
@48   30 36 00 00 01 00 00 02 92 00 04 00 0a 00 03 00
@64   09 00 db cb 0a 00 52 2f d1 1e 66 35 05 00 40 c2
@80   82 01 a3 00 01 00 01 00 00 00 00 00 17 02 40 20
@96   b8 00 00 2e 00 76 a0 87 00 00 f9 e0 75 a0 87 00
@112  00 00 00 00 00 00 03 05 44 4c 4d 54 41 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 9b 16 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 41 02 00 00 00 01 00 01 00 01 00 01 00
@176  03 00 00 60 ee 78 de 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff e3 6f 33 00
