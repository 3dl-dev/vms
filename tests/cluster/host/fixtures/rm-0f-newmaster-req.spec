%OVMX-CLUSTER-SPECIMEN-1
name:      rm-0f-newmaster-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     9
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    5b5345fed0841dae2d89f52b758f7d42b2ebe3eda3df561f37b70e8a0a86d8fe
%bytes
; RA5 pod labrm5 2026-10-13, tree RMT5: the old master VAX1 sends the op-0x0f step to the
; chosen NEW master VAX2, which answers 82/15 with body[28:32] CLEARED -- the new-master
; role of rd vms-cab rule 2 (60 of 65 real op-0x0f pairs clear it).
@0    08 00 2b f7 30 9a aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   f8 ed a3 81 01 00 12 00 f8 ed 00 00 a3 81 00 00
@48   f8 ed 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 53 2f 08 00 54 2f b2 80 08 ed 03 00 e5 1f
@80   02 0f 03 00 01 00 03 00 00 00 00 00 13 02 20 20
@96   7b 04 00 0e 02 00 01 00 01 00 03 00 3a 9d 87 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 53 59 53 54
@128  45 4d 24 56 41 58 31 20 20 20 20 20 e1 01 00 00
@144  00 00 f8 7f 78 00 00 00 00 00 00 00 71 00 00 00
@160  84 96 f8 7f 00 00 00 00 00 41 00 00 00 00 bb 16
@176  9d 56 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
