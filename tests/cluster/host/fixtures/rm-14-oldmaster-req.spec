%OVMX-CLUSTER-SPECIMEN-1
name:      rm-14-oldmaster-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     37
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    47208aa10994fa4781844bff9bbca5d5e3899a5eb40e8acf4f48138a79a76fcb
%bytes
; RA5: the new master reports the tree held to the OLD master, and the old master answers
; 82/15 as a verbatim echo (its body[34] was already 0xfa).
@0    aa 00 04 00 01 04 08 00 2b f7 30 9a 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   a5 81 fb ed 01 00 12 00 a5 81 00 00 fb ed 00 00
@48   a5 81 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   08 00 54 2f 08 00 53 2f 0b ed b4 80 07 00 b1 40
@80   02 14 52 45 01 00 03 00 45 0d 00 00 9e 01 00 32
@96   c8 04 00 06 10 00 00 00 1a 00 fa 00 00 00 00 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 00 00 00 00
@128  00 00 00 00 31 20 20 20 20 20 0b 00 00 00 00 00
@144  01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 20 00 44 0a 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
