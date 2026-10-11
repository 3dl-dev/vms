# vms-b20: a real VAX serving MSCP -- the READ/WRITE block-transfer opcodes

Pod `mscp-b20` (image `ovmx-vaxlab:7`, `ASYM_MSCP=1`), 2026-10-11: VAX1 (OpenVMS
VAX V7.3) owns a private RA81 on rq2, `$2$DUA2:` (`INITIALIZE ... DATA2`); VAX2
has no rq2 and reaches it only through VAX1's MSCP server (SHOW DEVICE on VAX2:
`$2$DUA2: (VAX1) ... (remote mount)`). No OVMX node on the bridge.

Steps on VAX2 (05:48-05:51 UTC): `MOUNT/SYSTEM $2$DUA2: DATA2`; `COPY` a
596-block file from it twice (READ), `COPY` a 36-block file onto it (WRITE).
`mscp-read-write.pcap` is every 0x6007 frame of 05:48:30-05:51:10.

Measured (opcode at abs 55; vms_cluster_codec_blk.h sec 5):

- READ, server -> host: `0x14` stream frames (28-byte header + 1426-byte chunk,
  +8 counting down) then `0x15` = the MSCP END message (an ordinary SCS message,
  abs 58 == 0x0004, inner length 46) with the final chunk piggybacked past its
  inner length as a 28-byte header + tail. 108 x 0x14 and 23 x 0x15; every
  0x15's trailer +8 equals its tail. One +4/+6 tag per transfer on every frame,
  trailer included; +0 is the server's own Con.ID (30/30). No 0x0f follows.
- WRITE, server pulls host data: `0x08` request, host answers `0x0c` (non-final)
  ... `0x0d` (final), as for the cluster-manager transfers.

The tcpdump in a 1-CPU pod dropped frames during the first 596-block READ
(05:49:00); the second READ (05:51:02) is complete.
