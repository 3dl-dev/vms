%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dir-redirect-vax1
class:     scs-msg
origin:    capture
capture:   dirhash-L1.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY ROLE" (rd vms-8219)
wire-len:  204
sha256:    7c3ecead7716aae62c19e79951bba1dc00b83eb614c20efa64dff9690109f2fd
%bytes
; REAL captured frame 6 of tests/lab/captures/vms-4fb-dir-hash-20261004/dirhash-L1.pcap: directory node VAX1's 0xf8 REDIRECT answer to VAX3's DLMTA lookup: the master is VAX2, CSID 00010002 at body[28:32].
; body[8:10]=82 01; body[28:40]=020001000354f84954452e00; body[44:48]=00000305;
; name=b'DLMTA'; body[128:132]=e36f3300.
@0    08 00 2b 81 28 a1 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 03 04 01 00 aa 00 04 00 01 04 4b 13
@32   2e 34 91 1d 01 00 12 00 2e 34 00 00 91 1d 00 00
@48   2e 34 00 00 01 00 00 02 92 00 04 00 0a 00 03 00
@64   0a 00 1e e9 0d 00 52 2f c6 1c 64 33 01 00 c5 e4
@80   82 01 c8 00 01 00 01 00 00 00 00 00 16 02 60 20
@96   f6 02 00 79 02 00 01 00 03 54 f8 49 54 45 2e 00
@112  00 00 00 00 00 00 03 05 44 4c 4d 54 41 00 00 00
@128  53 44 53 4b 31 20 20 20 20 20 9b 16 00 00 00 00
@144  00 00 25 20 20 20 20 28 00 00 00 00 20 6e 00 00
@160  65 20 56 41 58 33 20 20 20 61 74 20 20 34 2d 4f
@176  43 54 2d 32 30 32 36 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff e3 6f 33 00
