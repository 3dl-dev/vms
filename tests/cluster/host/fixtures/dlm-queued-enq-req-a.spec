%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-queued-enq-req-a
class:     scs-msg
origin:    capture
capture:   order1-vax-first.pcap
frame:     1039
spec:      vms_cluster_codec_dlm.h "THE SAME RECORD WHEN THE MASTER QUEUED THE REQUEST INSTEAD" (rd vms-85f)
wire-len:  204
sha256:    bde071af025443e5c37b509ee0ce492db7f7dd976ea46920e7a9f80d94b693fc
%bytes
; REAL captured frame 1039 of tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap.
; A real OpenVMS VAX V7.3 node (08:00:2b:22:85:c2) asks VAX1 (aa:00:04:00:01:04, the
; tree's MASTER) for EX on the ROOT resource 'UCX$INETACP_...': cat-0x02 op-0x01,
; body[30] = 0x05, body[24:28] = 0e0002b2 (its own handle), body[36:44] all zero --
; no parent, so this is a ROOT request. Its answer is dlm-queued-enq-ans-a.
@0    aa 00 04 00 01 04 08 00 2b 22 85 c2 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   c0 14 0d 1e 01 00 12 00 c0 14 00 00 0d 1e 00 00
@48   c0 14 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 90 0f 09 00 90 0f 95 1d 47 14 03 00 1e 06
@80   02 01 49 53 01 00 02 00 00 00 00 00 13 02 40 20
@96   b2 02 00 0e 10 02 05 00 13 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 03 10 55 43 58 24 49 4e 45 54
@128  41 43 50 5f 0a 02 01 64 4b 31 20 20 20 20 20 00
@144  00 00 f9 7f 70 00 00 00 00 00 00 00 00 00 00 00
@160  00 41 00 00 20 00 81 02 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff cf e5 2f 39
