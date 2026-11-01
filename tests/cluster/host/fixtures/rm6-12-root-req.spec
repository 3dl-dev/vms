%OVMX-CLUSTER-SPECIMEN-1
name:      rm6-12-root-req
class:     scs-msg
origin:    capture
capture:   rm6-remasters.pcap
frame:     545
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    bc862fa8ef071f6e0f4c1ac361d5fe154d61d9734f48bb5b453d2f9f2aaa4a0e
%bytes
; HELD-OUT topology (RA6 pod labrm6b, tree RMT7): O=VAX2, N=VAX3, H=VAX1, LOCKDIRWT 1/1/1,
; CSIDs not in node-number order. The holder ROOT record and the new master GRANT for it.
; ANS body[30:32]=f1 01 where RA5 had f0 01: the cell rd vms-380 records as UNNAMED and
; keeps opt-in, so this codec writes the grant 0 0 there and the gate records the divergence.
@0    08 00 2b fc 63 c1 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 01 04 4b 13
@32   65 34 e4 24 01 00 12 00 65 34 00 00 e4 24 00 00
@48   65 34 00 00 01 00 00 02 92 00 04 00 0a 00 03 00
@64   09 00 52 2f 09 00 52 2f b4 23 36 33 04 00 94 1f
@80   02 12 00 00 01 00 03 00 00 00 00 00 1e 02 20 20
@96   89 09 00 15 11 00 01 01 1e 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 03 04 52 4d 54 37 53 59 53 54
@128  45 4d 24 56 41 58 31 20 20 20 5c 00 00 00 00 00
@144  00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00
@160  52 4d 4c 56 42 2d 30 31 32 33 34 35 36 37 38 39
@176  01 00 00 00 00 00 01 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 0e 84 52 30
