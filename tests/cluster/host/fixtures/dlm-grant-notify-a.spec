%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-grant-notify-a
class:     scs-msg
origin:    capture
capture:   order1-vax-first.pcap
frame:     3587
spec:      vms_cluster_codec_dlm.h "THE DEFERRED GRANT (op 0x02)" (rd vms-a4d7)
wire-len:  204
sha256:    41a2b663006aa60f2bbbaeab2e1a6c00767a60e709b73c9a979daf9bd1a0a8c4
%bytes
; REAL captured frame 3587 of tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap,
; on a private real OpenVMS VAX V7.3 cluster. VAX1 (aa:00:04:00:01:04), the MASTER
; of 'F11B..VAX1DATA', had answered 08:00:2b:22:85:c2's op-0x01 for PW with 0xfb
; QUEUED seven frames earlier (frame 3580, body[20:24] = ec03001c, body[24:28] =
; 2d010022, body[30] = 04). THIS frame is what the master sent next about that lock:
; cat-0x02 op-0x02, the SAME handle pair at body[20:28], body[4:8] all zero (no
; transaction -- nobody answers one), body[28:30] = 14 00, body[30] = body[31] = 04
; (the mode it had queued for), body[47] = 0 (no resource name: keyed by HANDLES),
; body[112:128] = 00000000 ffffffff 00000000 ffffffff. The residue spans
; (body[10:12], [16:20], [32:48], [48:112], [128:132]) are this sender's own stale
; message buffer and OVMX writes zeros there (rd vms-42de) -- body[16:20] reads
; 28440080 here, the S0 kernel address 0x80004428 showing through.
@0    08 00 2b 22 85 c2 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   01 24 bb 18 01 00 12 00 01 24 00 00 bb 18 00 00
@48   01 24 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   09 00 90 0f 09 00 90 0f 3b 18 82 23 00 00 00 00
@80   02 02 00 00 01 00 02 00 28 44 00 80 ec 03 00 1c
@96   2d 01 00 22 14 00 04 04 40 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  4c 76 03 00 7e 00 00 00 4c 76 03 00 77 00 00 00
@144  4c 76 03 00 70 00 00 00 4c 76 03 00 00 00 00 00
@160  00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 01 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 00 00 00 00
