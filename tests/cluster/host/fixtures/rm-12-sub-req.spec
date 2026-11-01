%OVMX-CLUSTER-SPECIMEN-1
name:      rm-12-sub-req
class:     scs-msg
origin:    capture
capture:   rm5-remaster.pcap
frame:     29
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    cbb67f411097dfa517fbe17118d341a01ce8b73e791781267e88ea1319d3f364
%bytes
; RA5: the HOLDER VAX3 hands over a SUB-resource lock of RMT5 and the new master GRANTS it.
; REQ body[36:40] the parent HOLDER lkid, body[40:44] the parent NEW-MASTER lkid.
; ANS body[20:24] the handle the new master minted, body[24:28] echoed, body[28]=0x10,
; body[30:32]=0 0, body[32:36]=01 00 fa 00, body[36:56] twenty zeros (no value block).
@0    08 00 2b f7 30 9a 08 00 2b e4 13 32 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   2e 0e 81 0d 01 00 12 00 2e 0e 00 00 81 0d 00 00
@48   2e 0e 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 52 2f 0a 00 52 2f 9a 0c 47 0d 01 00 8d 22
@80   02 12 00 00 01 00 03 00 00 00 00 00 16 02 60 20
@96   a9 00 00 18 00 00 02 02 16 00 01 00 ce 00 00 11
@112  cc 04 00 02 00 00 03 02 53 32 49 24 53 59 53 54
@128  45 4d 24 56 41 58 33 20 20 20 1a 04 00 00 00 00
@144  00 00 fe 7f 70 00 00 00 00 00 00 00 6c 74 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 01 04 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 2e 1e 80 05
