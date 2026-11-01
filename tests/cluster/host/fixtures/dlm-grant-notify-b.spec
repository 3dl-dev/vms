%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-grant-notify-b
class:     scs-msg
origin:    capture
capture:   r3-ovmx-first.pcap
frame:     7812
spec:      vms_cluster_codec_dlm.h "THE DEFERRED GRANT (op 0x02)" (rd vms-a4d7)
wire-len:  204
sha256:    ffba15895d83d8835a281cdeef673ec236aeae758b5f7b590530faca5abb6650
%bytes
; REAL captured frame 7812 of tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap
; -- a SECOND arrangement: a DIFFERENT master (08:00:2b:7a:2a:ac), a different
; requester (08:00:2b:03:7e:61), a different resource ('QMAN..EW_JOBCTL') and a
; different mode (EX, not PW). That master had answered the requester's op-0x01 with
; 0xfb QUEUED one frame earlier (frame 7811); this is the deferred grant, with
; body[30] = body[31] = 05 and the same structural cells as specimen A.
@0    08 00 2b 03 7e 61 08 00 2b 7a 2a ac 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   be 10 46 0b 01 00 12 00 be 10 00 00 46 0b 00 00
@48   be 10 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   09 00 52 2f 09 00 52 2f 16 0b 8e 10 00 00 00 00
@80   02 02 00 00 01 00 03 00 28 44 00 80 31 05 00 07
@96   74 02 00 05 14 00 05 05 40 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  f4 8b f8 7f 7e 00 00 00 09 94 f8 7f 83 00 00 00
@144  09 94 f8 7f 7c 00 00 00 09 94 f8 7f 00 00 00 00
@160  00 41 00 00 00 00 64 00 01 00 00 00 0c 00 01 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 00 00 00 00
