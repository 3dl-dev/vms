%OVMX-CLUSTER-SPECIMEN-1
name:      ra7a-02-01-vax2-enq-valblk
class:     scs-msg
origin:    capture
capture:   trimmed.pcap.gz
frame:     25
spec:      vms_cluster_codec_dlm.h op-0x1a + GRANT valblk/seq (rd vms-26e)
wire-len:  204
sha256:    bb238abb0f650f12daf53994546dbaa365fc6f09ac9b230ea6f129abe9561ada
%bytes
; RA7 fire A, same capture. AFTER the tree moved, VAX2 (08-00-2b-f7-b8-42)
; asks the new master OVMXE for RA7T at CR with LCK$M_VALBLK: body[28]=0x11,
; body[30]=01 CR, body[24:28]=0x0b0003bf VAX2's own handle, name RA7T.
; Every byte cited.
@0    52 54 00 00 e5 01 08 00 2b f7 b8 42 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 02 04 4b 13
@32   8f 00 38 01 01 00 12 00 8f 00 00 00 38 01 00 00
@48   8f 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   05 00 b8 11 0d 00 52 2f 17 01 6e 00 0a 00 cd 1e
@80   02 01 00 00 01 00 02 00 00 00 00 00 19 02 40 20
@96   bf 03 00 0b 11 00 01 00 19 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 03 04 52 41 37 54 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  56 4d 58 20 56 30 2e 37 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 3c 25 6f eb
