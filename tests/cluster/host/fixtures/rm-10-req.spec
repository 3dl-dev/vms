%OVMX-CLUSTER-SPECIMEN-1
name:      rm-10-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     19
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    6170a9f2402033da1526384528c5a756e71559dcfa2491d53ac9ab4245784125
%bytes
; RA5 pod labrm5 2026-10-13, tree RMT5: O=VAX1 -> N=VAX2, the third step, and its 82/15
; answer -- the request echoed with body[8]=0x82, body[9]=0x15, body[34]=0xfa and nothing else.
@0    08 00 2b f7 30 9a aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   f9 ed a4 81 01 00 12 00 f9 ed 00 00 a4 81 00 00
@48   f9 ed 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 53 2f 08 00 54 2f b3 80 09 ed 06 00 e5 1f
@80   02 10 03 00 01 00 03 00 00 00 00 00 13 02 20 20
@96   37 08 00 0b 64 00 00 00 00 00 f9 e0 06 9d 87 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 53 59 53 54
@128  45 4d 24 56 41 58 31 20 20 20 20 20 48 03 00 00
@144  00 00 f8 7f 7a 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 00 00 42 16 29 e4 00 00 04 00 04 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
