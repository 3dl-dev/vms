# vms-239: what a disk class driver says when it connects to an MSCP server

`connects.pcap` holds every SCS connection-control frame (CONNECT_REQ/RSP,
ACCEPT_REQ/RSP, REJECT_REQ/RSP, port opcode 0x02, format word 0x0004) of the
MSCP$DISK connect attempts in two lab runs on 2026-10-11, image `ovmx-vaxlab:7`:

- pod `mscp-b20` (VAX1 + VAX2 + VAX3, all OpenVMS VAX V7.3, no OVMX): the
  three VAXes' disk class drivers connect to each other's MSCP servers.
- pod `mscp-safe` (VAX1 serving `$2$DUA2:` with `ASYM_MSCP=1`, VAX2, and
  OVMXE built from main + #1631): OVMXE's class driver connects to both VAXes.

## What every real class driver sends

Census over every pcap in the lab corpus (`tests/lab/captures` plus the lane's
working captures, 58 files): 4,032 `VMS$DISK_CL_DRVR -> MSCP$DISK`
CONNECT_REQs from 19 distinct real VAX stations, all content 110, all with

| abs     | field                         | value                          |
|---------|-------------------------------|--------------------------------|
| 72-73   | SCS$W_MIN_CR (p. 2-44)        | `02 00` (2)                    |
| 74-75   | SCS$W_STATUS                  | `01 00`                        |
| 76-91   | destination SYSAP             | `MSCP$DISK` + 7 blanks         |
| 92-107  | source SYSAP                  | `VMS$DISK_CL_DRVR`             |
| 108-123 | connect data (spec sec 4(N))  | `V5.0` + 10 blanks + `+ `      |

No variation by node, server, cluster or boot.

## What OVMX sent, and the answer

OVMXE (stations `52:54:00:..`, 5,117 connects across the corpus) sent MIN_CR 0
and 16 blanks. In `mscp-safe` both real VAXes answered every one with
REJECT_REQ, SCS$W_STATUS `0x002c` (VAX-to-VAX rejects in the same capture
carry `0x0001`). OVMX's class driver never reached a READ.

## The fix (rd vms-239)

The class driver's SYSAP registration now states MIN_CR 2 and the 16 bytes
above (`vms_cnxman_join_fsm.h`, `CNXMAN_DISK_CL_MIN_CREDITS` and
`cnxman_join_disk_cl_conndata`), and `scs_connect()` reads both off the
registration. They are a property of the class-driver protocol, not of any
node: no node's private value is carried.

The second rung, the READ END itself (port opcode 0x15 = an SCS message with a
block trailer past its inner length), is grounded on
`../vms-b20-mscp-read-20261011/mscp-read-write.pcap`.
