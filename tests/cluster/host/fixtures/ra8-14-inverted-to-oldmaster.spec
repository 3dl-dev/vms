%OVMX-CLUSTER-SPECIMEN-1
name:      ra8-14-inverted-to-oldmaster
class:     scs-msg
origin:    capture
capture:   ra8-remaster.pcap
frame:     37
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    484a744aef32e7174a2db7c754177c5f9601fc919f121662e7fccbf94f11631e
%bytes
; RA8: the two op-0x14s of the MIDDLE remaster (CACHE$cmSYSDSK1, O=VAX2 3f00,
; N=VAX3 1ca8), whose supposedly role-determined cells are EXACTLY INVERTED from RA6 rule:
; the frame to the OLD MASTER carries [28]=0xd0 [34]=0xf9 where the rule wants 10/fa, and
; the frame to the other member carries [28]=0x00 [34]=0xfa where it wants 80/f9.
; Committed so nobody can restore the role rule without this frame reddening.
@0    08 00 2b 3e 3f 00 08 00 2b 2d 1c a8 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   0a 08 c5 08 01 00 12 00 0a 08 00 00 c5 08 00 00
@48   0a 08 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   0e 00 52 2f 09 00 ba c9 38 08 7d 07 07 00 73 bd
@80   02 14 a3 00 01 00 03 00 00 00 00 00 12 02 60 20
@96   d0 04 00 0f d0 04 00 0c a8 03 f9 0d 18 9d 87 00
@112  00 00 00 00 00 00 00 18 43 41 43 48 45 24 63 6d
@128  53 59 53 44 53 4b 31 20 20 20 20 20 2d 03 00 00
@144  25 25 25 20 20 20 20 28 00 00 00 00 20 6e 00 00
@160  65 20 56 41 58 33 20 20 20 61 74 20 32 31 2d 4f
@176  43 54 2d 32 30 32 36 00 31 34 3a 34 36 3a 30 30
@192  00 00 00 00 ff ff ff ff 5e 72 e2 9b
