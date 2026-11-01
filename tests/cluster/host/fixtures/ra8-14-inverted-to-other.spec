%OVMX-CLUSTER-SPECIMEN-1
name:      ra8-14-inverted-to-other
class:     scs-msg
origin:    capture
capture:   ra8-remaster.pcap
frame:     39
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    89e359f0b22fc2d1a12734628b504e3457aa02037762cbb668d10d33881dfe22
%bytes
; RA8: the two op-0x14s of the MIDDLE remaster (CACHE$cmSYSDSK1, O=VAX2 3f00,
; N=VAX3 1ca8), whose supposedly role-determined cells are EXACTLY INVERTED from RA6 rule:
; the frame to the OLD MASTER carries [28]=0xd0 [34]=0xf9 where the rule wants 10/fa, and
; the frame to the other member carries [28]=0x00 [34]=0xfa where it wants 80/f9.
; Committed so nobody can restore the role rule without this frame reddening.
@0    aa 00 04 00 01 04 08 00 2b 2d 1c a8 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 03 04 4b 13
@32   3c 44 9e 37 01 00 12 00 3c 44 00 00 9e 37 00 00
@48   3c 44 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0e 00 52 2f 0b 00 ba c9 06 37 a2 43 05 00 c9 c0
@80   02 14 00 00 01 00 03 00 00 00 00 00 13 09 00 1e
@96   c9 04 00 01 00 00 00 00 01 00 fa 00 00 00 00 00
@112  00 00 00 00 00 00 00 18 43 41 43 48 45 24 63 6d
@128  53 59 53 44 53 4b 31 20 20 20 20 20 2d 03 00 00
@144  00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 93 01 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 5e 72 e2 9b
