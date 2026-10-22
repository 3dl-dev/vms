%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-queued-enq-ans-b
class:     scs-msg
origin:    capture
capture:   r3-ovmx-first.pcap
frame:     8246
spec:      vms_cluster_codec_dlm.h "THE SAME RECORD WHEN THE MASTER QUEUED THE REQUEST INSTEAD" (rd vms-85f)
wire-len:  204
sha256:    1bf7b68f7cac0e8e1113c1da00a14f2e9fe7ebe11d9b61ff17d8118e3dbf5327
%bytes
; REAL captured frame 8246: that second master's queued answer. Same fifteen
; offsets rewritten, and it shows the two cells whose determinant is NOT grounded
; taking their OTHER real values -- body[28:30] = 0x54 0x02 (not 0x14 0x00) and
; body[52] = 0x02 (not 0). OVMX writes the commonest form (0x14 0x00 / 0x00); the
; difference is a deviation row in frame-grounding.tsv, not a guess.
@0    08 00 2b 03 7e 61 08 00 2b 7a 2a ac 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   b5 11 e2 0b 01 00 12 00 b5 11 00 00 e2 0b 00 00
@48   b5 11 00 00 01 00 00 02 92 00 04 00 0a 00 03 00
@64   09 00 52 2f 09 00 52 2f a0 0b 74 11 05 00 3d 21
@80   82 01 00 00 01 00 03 00 00 00 00 00 52 05 00 04
@96   75 02 00 0c 54 02 04 00 00 00 fb 00 00 00 00 00
@112  00 00 00 00 00 00 00 0e 4c 4e 4d 24 02 00 4c 4f
@128  47 49 43 41 4c 53 20 20 20 20 2a 00 00 00 00 00
@144  38 2b f9 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 00 00 d4 01 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 63 c4 3c 47
