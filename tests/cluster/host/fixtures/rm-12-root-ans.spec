%OVMX-CLUSTER-SPECIMEN-1
name:      rm-12-root-ans
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     28
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    9dab7ee92d2cce715fee9c1b60c9e5b38ea39a7d152b018100eb54b3e7c9fc8e
%bytes
; RA5 pod labrm5 2026-10-13, tree RMT5: the HOLDER VAX3 hands over its ROOT lock
; and the new master VAX2 GRANTS it. REQ body[24:28]=0x110000ce is VAX3 own root lkid
; (SDA before the move), body[20:24]=0x20600216 its holder EPID, body[28]=0x11, body[30:32]=01 01.
; ANS body[20:24]=0x020004cc is the master copy SDA then showed on VAX2, body[24:28] echoed,
; body[28]=0x10, body[32:36]=01 00 fa 00, body[36:52]=the LVB "RMLVB-0123456789",
; body[52:56]=LVB sequence 1, and body[30:32]=f0 01 -- the one cell rd vms-380 cannot name.
@0    08 00 2b e4 13 32 08 00 2b f7 30 9a 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 02 04 4b 13
@32   80 0d 2e 0e 01 00 12 00 80 0d 00 00 2e 0e 00 00
@48   80 0d 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 52 2f 0a 00 52 2f 47 0d 99 0c 07 00 a8 22
@80   82 01 00 00 01 00 03 00 00 00 00 00 cc 04 00 02
@96   ce 00 00 11 10 00 f0 01 01 00 fa 00 52 4d 4c 56
@112  42 2d 30 31 32 33 34 35 36 37 38 39 01 00 00 00
@128  45 4d 24 56 41 58 33 20 20 20 20 20 48 03 00 00
@144  00 00 00 00 00 00 04 00 00 00 00 00 00 00 00 00
@160  52 4d 4c 56 42 2d 30 31 32 33 34 35 36 37 38 39
@176  01 00 00 00 00 00 01 04 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
