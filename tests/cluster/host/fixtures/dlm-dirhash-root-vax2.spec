%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dirhash-root-vax2
class:     scs-msg
origin:    capture
capture:   dirhash-L1.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY HASH -- body[128:132]" (rd vms-4fb)
wire-len:  204
sha256:    3d7b23a9a62c4ea82bdcd10cd311949e6cc6753db206467c5b03e1a3c8ca4cbe
%bytes
; REAL captured frame 3 of tests/lab/captures/vms-4fb-dir-hash-20261004/dirhash-L1.pcap: a ROOT request for DLMTA from VAX2 (1026, LOCKDIRWT 0) to the directory node VAX1 (1025, LOCKDIRWT 3).
; Private three-node OpenVMS VAX V7.3 cluster, scenario L1 (rd vms-4fb README).
; body[8:10] = 02 01; body[36:44] parent span = 0000000000000000;
; body[46] = 03; body[128:132] = e36f3300 (the directory hash value).
@0    aa 00 04 00 01 04 08 00 2b 92 0b 09 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 5b 13
@32   9c 1f 30 36 01 00 12 00 9c 1f 00 00 30 36 00 00
@48   9c 1f 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 52 2f 09 00 db cb 66 35 d0 1e 05 00 40 c2
@80   02 01 a3 00 01 00 01 00 00 00 00 00 17 02 40 20
@96   b8 00 00 2e 10 00 05 00 17 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 03 05 44 4c 4d 54 41 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 9b 16 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 41 02 00 00 00 01 00 01 00 01 00 01 00
@176  03 00 00 60 ee 78 de 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff e3 6f 33 00
