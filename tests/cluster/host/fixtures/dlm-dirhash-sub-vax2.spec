%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-dirhash-sub-vax2
class:     scs-msg
origin:    capture
capture:   dirhash-L1.pcap
spec:      vms_cluster_codec_dlm.h "THE DIRECTORY HASH -- body[128:132]" (rd vms-4fb)
wire-len:  204
sha256:    ce0a016631caa5c9ebd82e19696cdd4254efe1ffd2b308fb1e8ae093d53c6750
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-4fb-dir-hash-20261004/dirhash-L1.pcap: a SUB-resource request (F11B$s<fid>, parent span body[36:44] nonzero) from VAX2 to VAX1.
; Private three-node OpenVMS VAX V7.3 cluster, scenario L1 (rd vms-4fb README).
; body[8:10] = 02 01; body[36:44] parent span = 0400000171030036;
; body[46] = 00; body[128:132] = 9fbf2042 (the directory hash value).
@0    aa 00 04 00 01 04 08 00 2b 92 0b 09 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   31 01 89 00 01 00 12 00 31 01 00 00 89 00 00 00
@48   31 01 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 52 2f 09 00 db cb 6c 00 14 01 01 00 fa ba
@80   02 01 00 00 01 00 01 00 00 00 00 00 02 02 40 20
@96   06 00 00 01 39 02 04 00 02 00 01 00 04 00 00 01
@112  71 03 00 36 00 00 00 0a 46 31 31 42 24 73 04 00
@128  00 00 4b 31 20 20 20 20 20 00 00 00 00 00 00 00
@144  01 00 01 00 02 02 40 20 00 00 00 00 0d 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 9f bf 20 42
