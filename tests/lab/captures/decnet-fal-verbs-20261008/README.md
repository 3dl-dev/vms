# A real OpenVMS VAX FAL answering DIRECTORY/FULL, TYPE, DELETE and RENAME (2026-10-08)

**Provenance.** Two stock OpenVMS VAX V7.3 nodes (SIMH MicroVAX 3900) on the DECnet lane's own lab: VAX1 is DECnet 1.1 and VAX2 is 1.2. VAX1 ran each DCL command below against VAX2's own FAL (object 17). Nothing OVMX was on the wire. Clean-room (AGENTS.md Rule 8): this is observed wire behaviour only, and no VSI/HPE software was disassembled or read.

| file | what |
|---|---|
| `vax1-console.txt` | Each command VAX1 ran, then what its console printed. |
| `falverbs-wire.txt` | Every NSP connect, link-service and DAP data segment of those sessions, in wire order, decoded by the lab's `nspdump` (the instrument in `../decnet-sethost-inbound-20261005/nspdump.py`). |

The access-control passwords in the connect frames are replaced with `x` bytes of the same length. VMS itself prints `password` in place of the password on the console.

## Commands → links

VMS opens one NSP link per access. A command often uses two or three links: a DIRECTORY LIST of its argument, then the operation on a link of its own.

| links (RCI) | user | command | VAX FAL's DAP answer |
|---|---|---|---|
| 02 | SYSTEM | `DIRECTORY/FULL DELME.TXT` | DIRLIST (DISPLAY 0x839) → NAME volume / directory / file, ATTRIBUTES, SUMMARY (empty), DATE AND TIME, PROTECTION, type-18, ACK, ACCESS COMPLETE |
| 03 | SYSTEM | `TYPE NOSUCH.TXT` | DIRLIST → NAME `SYS$COMMON:` / `[SYSMGR]` / `NOSUCH.TXT;`, STATUS `4032` STV `0910` |
| 04 | *(default account)* | `TYPE PROT.TXT` | Disconnect reason 34 before any DAP (`INVLOGIN`) |
| 05, 06 | SYSTEM | `RENAME RENME.TXT RENAMED.TXT` | DIRLIST; then ACCESS RENAME + NAME(new) → NAME(old), ACK, NAME(new), ACK, ACCESS COMPLETE |
| 07, 08, 09 | SYSTEM | `DELETE DELME.TXT;1` | two DIRLISTs; then ACCESS ERASE → NAME, ACK, ACCESS COMPLETE |
| 0a, 0b | SYSTEM | `DELETE NOSUCH.TXT;1` | two DIRLISTs, each STATUS `4032` STV `0910` |
| 0c, 0d | DNTEST | `TYPE PROT.TXT` (SYSTEM-only file) | DIRLIST; then ATTRIBUTES + ACCESS OPEN → STATUS `4032` STV `0910`. The VAX answers **FNF**, not PRV. |
| 0e, 0f, 10 | DNTEST | `DELETE PROT.TXT;1` | two DIRLISTs; then ACCESS ERASE → STATUS `4055` (MIC 0125 PRV) STV `24` (SS$_NOPRIV) |
| 11, 12 | DNTEST | `RENAME PROT.TXT STOLEN.TXT` | DIRLIST; then ACCESS RENAME + NAME → STATUS `405f` (MIC 0137 RMV), no STV. The console prints `-NONAME-W-NOMSG, Message number 00000000`. |

## Reading the bytes

- Both VAXes speak DAP 7.2. Every data segment after the CONFIGURATION ends with 2 bytes that the DAP 5.6 spec does not define. With OVMX advertising DAP 5.6, a VAX sends none (`../decnet-fal-dap-20261004/README.md`, item 4). The replay test strips them.
- A reply is one segment. Every message carries FLAGS.LENGTH except the last, which runs to the end of the segment.
- DAP 7-only content that OVMX does not speak: the type-18 message (`File ID: None` on the console), ATTRIBUTES menu bit 21, and binary dates in DATE AND TIME menu bits 7 and 8.

`tests/vmsdecnet/test_dnet_fal_server.c` reads `falverbs-wire.txt` at run time and replays every link's client segments through OVMX's `dnet_fal_server_run`. It then compares the replies with the VAX FAL's, segment by segment (rd vms-277a).
