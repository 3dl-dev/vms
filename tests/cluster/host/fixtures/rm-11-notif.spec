%OVMX-CLUSTER-SPECIMEN-1
name:      rm-11-notif
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     25
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    cc385f54d433e2684e96e547f41e3aed81ac7616a714e1d729e5ca15b5611cc0
%bytes
; O -> every other member. body[4:8]=0: a NOTIFICATION, never answered (5/5 real).
@0    08 00 2b e4 13 32 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 01 04 4b 13
@32   10 2d d1 1f 01 00 12 00 10 2d 00 00 d1 1f 00 00
@48   10 2d 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 52 2f 09 00 52 2f d8 1e 19 2c 00 00 00 00
@80   02 11 03 00 01 00 03 00 00 00 00 00 2f 08 00 04
@96   ce 00 00 11 00 00 d0 01 00 00 00 00 52 4d 4c 56
@112  00 00 00 00 00 00 03 04 52 4d 54 35 01 00 00 00
@128  00 00 00 00 00 00 00 00 e0 73 9b 87 e0 73 9b 87
@144  31 00 f4 04 00 00 00 00 00 00 0b 00 00 00 00 00
@160  10 00 01 00 02 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
