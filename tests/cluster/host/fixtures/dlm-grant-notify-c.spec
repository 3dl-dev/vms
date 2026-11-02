%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-grant-notify-c
class:     scs-msg
origin:    capture
capture:   heldout1.pcap.gz
frame:     16959
spec:      vms_cluster_codec_dlm.h "THE DEFERRED GRANT (op 0x02)" (rd vms-a4d7, rd vms-427)
wire-len:  204
sha256:    2abf3b8f837219b5e80a1aa778d6157fda67e3cd71398ada2267013d98f26277
%bytes
; REAL captured frame 16959 of tests/lab/captures/vms-cab-op0f-20261010/heldout1.pcap.gz,
; on a private real OpenVMS VAX V7.3 cluster (rd vms-427): a deferred grant that CARRIES
; the resource's value block. The master aa:00:04:00:01:04 had granted 'F11B$s*' at
; frame 16948 with body[36:56] = 01000000 01000000 00000000 00000000 1a020000 (block,
; sequence 0x21a), then answered aa:00:04:00:02:04's op-0x01 for PW (frame 16950,
; body[28:30] = 39 02) with 0xfb QUEUED at frame 16951. THIS frame is the deferred
; grant: the same handle pair at body[20:28], body[30] = body[31] = 04, and at
; body[36:52] the SAME block with body[52:56] = 1b020000, the next sequence. Section 7
; of tools/cluster/dlm_late_grant_census.py counts the population (41 of 42 the same
; block; zero in none where the last grant carried one).
@0    aa 00 04 00 02 04 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   83 26 5c 1a 01 00 12 00 83 26 00 00 5c 1a 00 00
@48   83 26 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 ed 7e 09 00 ed 7e dc 19 04 26 00 00 00 00
@80   02 02 04 00 01 00 01 00 00 00 73 e5 44 01 00 1e
@96   80 01 00 1a 14 00 04 04 00 00 00 00 01 00 00 00
@112  01 00 00 00 00 00 00 00 00 00 00 00 1b 02 00 00
@128  ff ff 04 00 01 00 62 00 00 00 04 00 01 00 01 00
@144  00 00 28 00 00 00 00 00 00 00 09 00 01 00 00 00
@160  00 00 00 00 00 00 08 00 02 00 39 00 00 00 00 00
@176  00 00 00 00 00 00 04 40 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 41 58 31 44
