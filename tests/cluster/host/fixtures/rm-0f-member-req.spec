%OVMX-CLUSTER-SPECIMEN-1
name:      rm-0f-member-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     8
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    3dce9dbc07bdc824dc767ba2342bb6aa59d54312c3e7362c12ae36ac3d6534f4
%bytes
; RA5: the same op-0x0f step to a PLAIN MEMBER (the holder VAX3), which answers 82/15 with
; body[28:32] KEPT -- the other 5 of 65 real pairs. One step, two roles, one capture.
@0    08 00 2b e4 13 32 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 01 04 4b 13
@32   0e 2d cc 1f 01 00 12 00 0e 2d 00 00 cc 1f 00 00
@48   00 00 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   09 00 52 2f 09 00 52 2f d3 1e 17 2c 0a 00 e1 1f
@80   02 0f 03 00 01 00 03 00 00 00 00 00 13 02 20 20
@96   7b 04 00 0d 02 00 01 00 01 00 03 00 ff ff ff 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 2a 00 00 00 00 00
@144  38 93 f8 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 00 00 4f 0f 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 04 00 01 00 01 00 03 00
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
