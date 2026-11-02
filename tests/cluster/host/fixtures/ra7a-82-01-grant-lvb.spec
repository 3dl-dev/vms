%OVMX-CLUSTER-SPECIMEN-1
name:      ra7a-82-01-grant-lvb
class:     scs-msg
origin:    capture
capture:   trimmed.pcap.gz
frame:     8
spec:      vms_cluster_codec_dlm.h op-0x1a + GRANT valblk/seq (rd vms-26e)
wire-len:  204
sha256:    31c3cc83eb77ea017a4e588905abf9bb45cf56257abe694345ccf267436849f1
%bytes
; RA7 fire A (rd vms-6ad1, pod lab6ad1, build ef0b7e3fc), capture
; tests/lab/captures/vms-6ad1-ra7-residue-20261021/fireA/trimmed.pcap.gz.
; The REAL VAX master VAX1 (aa-00-04-00-01-04) GRANTS OVMXE's CR request on
; RA7T: body[20:24]=0x0700065f the handle VAX1 minted, body[24:28]=0x01000361
; OVMXE's own, body[28]=0x10 + body[32:36]=01 00 fa 00 the grant record,
; body[36:52]="RMLVB-0123456789" the value block the first holder wrote, and
; body[52:56]=1 its SEQUENCE (VAX1's SDA: Valblk RMLVB-0123456789, Seqnum 1).
; EVERY byte is the real node's own, so every byte is cited.
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   29 00 2a 00 01 00 12 00 29 00 00 00 2a 00 00 00
@48   29 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   07 00 b8 11 0d 00 52 2f 09 00 0a 00 01 00 03 00
@80   82 01 00 00 01 00 02 00 00 00 00 00 5f 06 00 07
@96   61 03 00 01 10 00 00 00 01 00 fa 00 52 4d 4c 56
@112  42 2d 30 31 32 33 34 35 36 37 38 39 01 00 00 00
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 3c 25 6f eb
