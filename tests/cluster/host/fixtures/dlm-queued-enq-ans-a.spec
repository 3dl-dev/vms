%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-queued-enq-ans-a
class:     scs-msg
origin:    capture
capture:   order1-vax-first.pcap
frame:     1040
spec:      vms_cluster_codec_dlm.h "THE SAME RECORD WHEN THE MASTER QUEUED THE REQUEST INSTEAD" (rd vms-85f)
wire-len:  204
sha256:    2989a08ac5be99dd23d87f34b9a6f2d00a08d700abf80efec85ad39339a1e1a6
%bytes
; REAL captured frame 1040 of the same capture: VAX1's answer, sent 0 frames later.
; cat-0x82 op-0x01 with body[34] = 0xfb -- THE MASTER QUEUED IT. The request echoed,
; with body[0:4] (the CM envelope, swapped), body[8] (the category), body[20:24]
; (0x0f000272, the handle VAX1 minted), body[28:30] (0x14 0x00, the QUEUED record's
; flags), body[32:36] (00 00 fb 00, the record) and body[52:54] (zeroed) rewritten,
; and NOTHING ELSE -- the requester's own handle, the mode it asked for, the name and
; body[36:44] all ride back unchanged. This is the frame OVMX now builds.
@0    08 00 2b 22 85 c2 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   0d 1e c1 14 01 00 12 00 0d 1e 00 00 c1 14 00 00
@48   0d 1e 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 90 0f 09 00 90 0f 48 14 95 1d 03 00 1e 06
@80   82 01 49 53 01 00 02 00 00 00 00 00 72 02 00 0f
@96   b2 02 00 0e 14 00 05 00 00 00 fb 00 00 00 00 00
@112  00 00 00 00 00 00 03 10 55 43 58 24 00 00 45 54
@128  41 43 50 5f 0a 02 01 64 4b 31 20 20 20 20 20 00
@144  00 00 f9 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 20 00 81 02 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff cf e5 2f 39
