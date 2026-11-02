%OVMX-CLUSTER-SPECIMEN-1
name:      ra7a-02-1a-valblk
class:     scs-msg
origin:    capture
capture:   trimmed.pcap.gz
frame:     15
spec:      vms_cluster_codec_dlm.h op-0x1a + GRANT valblk/seq (rd vms-26e)
wire-len:  204
sha256:    8bc10692457106ca9489ede9d1492d0a8b03c71591517c2683ea70dcd94b1c69
%bytes
; RA7 fire A, same capture. During the remaster of RA7T onto OVMXE, the OLD
; master VAX1 sends the NEW master an op-0x1a (rd vms-26e) about the lock the
; new master itself holds: body[4:8]=0 (a notification), body[20:24]=0x0700065f
; VAX1's handle for it (its grant above), body[24:28]=0x01000361 OVMXE's own,
; body[36:52]="RMLVB-0123456789" and body[52:56]=1 -- VAX1's RSB block and
; sequence. body[47]=0: the frame names no resource. Every byte cited.
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   2d 00 2f 00 01 00 12 00 2d 00 00 00 2f 00 00 00
@48   2d 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   07 00 b8 11 0d 00 52 2f 0d 00 0d 00 00 00 00 00
@80   02 1a 03 00 01 00 02 00 00 00 00 00 5f 06 00 07
@96   61 03 00 01 00 00 d0 01 00 00 00 00 52 4d 4c 56
@112  42 2d 30 31 32 33 34 35 36 37 38 39 01 00 00 00
@128  53 44 53 4b 31 20 20 20 20 20 2a 00 00 00 00 00
@144  00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 00 00 14 0b 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 6d 1b 50 48
