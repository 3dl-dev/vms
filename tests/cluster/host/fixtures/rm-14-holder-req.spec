%OVMX-CLUSTER-SPECIMEN-1
name:      rm-14-holder-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     36
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    65b7911e770fe76c31a5614cc4c0d44ed16e638e3790fc3c0f5d7072f8214403
%bytes
; The same step to the HOLDER. body[28:32]=0x1a00042f and body[36:40]=0x008798b5 (a VAX
; S0 kernel address) -- the same span, different residue. Answered 82/15 with body[34] f9->fa.
@0    08 00 2b e4 13 32 08 00 2b f7 30 9a 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 02 04 4b 13
@32   84 0d 32 0e 01 00 12 00 84 0d 00 00 32 0e 00 00
@48   84 0d 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0a 00 52 2f 0a 00 52 2f 4b 0d 9d 0c 06 00 3a 21
@80   02 14 bf 00 01 00 03 00 00 00 00 00 1a 02 40 20
@96   c8 04 00 04 2f 04 00 1a 4d 03 f9 20 b5 98 87 00
@112  00 00 00 00 00 00 03 04 52 4d 54 35 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 48 00 00 00 00 00
@144  00 00 25 0d 0a 4d 65 73 00 00 00 00 20 66 00 00
@160  6d 20 75 73 65 72 20 53 59 53 54 45 4d 20 6f 6e
@176  20 56 41 58 33 0d 0a 01 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff ea ea 4e c5
