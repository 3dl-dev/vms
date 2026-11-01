%OVMX-CLUSTER-SPECIMEN-1
name:      ra8-12-root-ans
class:     scs-msg
origin:    capture
capture:   ra8-remaster.pcap
frame:     72
spec:      vms_cluster_codec_dlm.h "LOCK-TREE REMASTERING ON THE WIRE" (rd vms-380)
wire-len:  204
sha256:    9e2d844cc1db0dcc25716d0286e17801a49e6e47270263e57a9e8839c30babef
%bytes
; RA8 (pod labrm8, 2026-10-21, PRE-REGISTERED in PLAN.md before the pod existed).
; THIRD topology: O=VAX3 (1ca8), N=VAX1 (0104), H=VAX2 (3f00) -- the rotation neither RA5
; nor RA6 took. The holder VAX2 hands over its ROOT lock in RMT8 and the new master VAX1
; GRANTS it. REQ [20:24]=20400217 is HOLD2 EPID, [24:28]=150000AB is VAX2 own root lkid
; (SDA before the move), [28]=0x11, [30:32]=01 01 (CR/CR, SDA-matched).
; ANS [20:24]=01000923 is the master copy SDA on VAX2 lists AFTER the move, [24:28] echoed,
; [30:32]=f1 01 and [32:34]=04d3 are the two cells rd vms-380 does not name.
@0    08 00 2b 3e 3f 00 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   13 33 88 25 01 00 12 00 13 33 00 00 88 25 00 00
@48   13 33 00 00 01 00 00 02 92 00 04 00 0a 00 02 00
@64   09 00 52 2f 09 00 52 2f 94 24 20 32 06 00 39 21
@80   82 01 00 00 01 00 03 00 00 00 00 00 23 09 00 01
@96   ab 00 00 15 10 00 f1 01 d3 04 fa 00 52 4d 4c 56
@112  42 2d 30 31 32 33 34 35 36 37 38 39 01 00 00 00
@128  45 4d 24 56 41 58 32 20 20 20 20 20 2a 04 00 00
@144  00 00 09 00 70 00 00 00 00 00 00 00 6c 74 00 00
@160  52 4d 4c 56 42 2d 30 31 32 33 34 35 36 37 38 39
@176  01 00 00 00 00 00 01 04 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff a4 20 bb f8
