%OVMX-CLUSTER-SPECIMEN-1
name:      rm-0e-ans
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     6
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    f962a20c09774cfaa394a1f46b34a7c4a6b32145054c92ce7f486d44a0a1f82e
%bytes
; The new master VAX2 answers the 0x0e: the request echoed, body[8]=0x82, body[9]=0x15,
; body[34] 0xf9 -> 0xfa, and NOTHING else (60/60 real 0x0e pairs).
@0    aa 00 04 00 01 04 08 00 2b f7 30 9a 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 5b 13
@32   a2 81 f8 ed 01 00 12 00 a2 81 00 00 f8 ed 00 00
@48   a2 81 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 54 2f 08 00 53 2f 08 ed b1 80 04 00 e4 1f
@80   82 15 00 00 01 00 03 00 00 00 00 00 13 02 20 20
@96   44 08 00 22 64 00 00 00 00 00 fa e0 32 9c 87 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 53 59 53 54
@128  45 4d 24 56 41 58 31 20 20 20 20 20 db 01 00 00
@144  00 00 fe 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 c1 05 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
