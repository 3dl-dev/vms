%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dirhash-root-vax3
class:     scs-msg
origin:    capture
capture:   dirhash-L1.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY HASH -- body[128:132]" (rd vms-4fb)
wire-len:  204
sha256:    2bb4e6896c24fb317a15c38fc78c637ccd8db64b92fd4b88357d230660db3a9e
%bytes
; REAL captured frame 5 of tests/lab/captures/vms-4fb-dir-hash-20261004/dirhash-L1.pcap: a ROOT request for the SAME name DLMTA from a DIFFERENT sender, VAX3 (1027), to VAX1.
; Private three-node OpenVMS VAX V7.3 cluster, scenario L1 (rd vms-4fb README).
; body[8:10] = 02 01; body[36:44] parent span = 0000000000000000;
; body[46] = 03; body[128:132] = e36f3300 (the directory hash value).
@0    aa 00 04 00 01 04 08 00 2b 81 28 a1 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 03 04 4b 13
@32   90 1d 2e 34 01 00 12 00 90 1d 00 00 2e 34 00 00
@48   90 1d 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0d 00 52 2f 0a 00 1e e9 64 33 c5 1c 01 00 c5 e4
@80   02 01 c8 00 01 00 01 00 00 00 00 00 16 02 60 20
@96   f6 02 00 79 10 00 00 00 16 00 01 00 00 00 00 00
@112  00 00 00 00 00 00 03 05 44 4c 4d 54 41 00 00 00
@128  53 44 53 4b 31 20 20 20 20 20 9b 16 00 00 00 00
@144  00 00 25 20 20 20 20 28 00 00 00 00 20 6e 00 00
@160  65 20 56 41 58 33 20 20 20 61 74 20 20 34 2d 4f
@176  43 54 2d 32 30 32 36 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff e3 6f 33 00
