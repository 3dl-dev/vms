# A real OpenVMS VAX COPY into and out of OVMX's FAL server (2026-10-04/05)

This is the inbound direction for the DECnet lane, captured on its own lab pod `dnlab-1`. VAX1 (1.1) is a stock OpenVMS VAX V7.3 node. Its `COPY` was pointed at node 1.43.

At 1.43, `dapprobe.py serve` carried the NSP link. Every DAP segment was handed to `faldrv serve`, which runs OVMX's **compiled** `dnet_fal_server_run` (`src/vmsdecnet/fal/dnet_fal.c` + `src/vmsdecnet/dap/dnet_dap.c`). In this lab harness the file side is stubbed onto POSIX. The shipped server does it with RMS over the ACP, as an authenticated `FAL.EXE` server process (rd vms-d85).

To make 1.43 reachable from an endnode with no router, the probe sent router hellos, and VAX1 picked it as its designated router. The access-control passwords are test values the stub never checks.

| VAX command | Result |
|---|---|
| `COPY T1.TXT 1.43"SYSTEM xxxxxx"::INB_PUT.TXT` | silent success; the server stored `Hello line one` / `line two` verbatim |
| `COPY 1.43"SYSTEM x"::GREET.TXT INB_GET.TXT` | silent success; `TYPE INB_GET.TXT` on VAX1 prints the server's two records |

## What a real VMS COPY needs from a FAL server

Each item below was found by a refusal on the wire:

1. **Directory list (SYSCAP bit 25).** A COPY that reads a remote file `$SEARCH`es it first. Without bit 25 it refuses with `FAL-F-ACCFUNC, unsupported RMS service call`. With it, VMS opens one link for `ACCESS(DIRLIST)`, which gets NAME(directory), NAME(file) and then ACCESS COMPLETE. It then opens a **second** link to OPEN the resultant name.
2. **NAME before ACK.** If the server advertises NAME (bit 40), VMS asks for it in `ACCESS.DISPLAY`. It then rejects a reply that has no NAME before the ACK with `RMS-F-BUG_DAP 0001A006` (MAC 10 sync, MIC = ACK). So a CREATE must `$CREATE` the file at ACCESS time, so that the NAME it returns is the real resultant spec.
3. **Accesses per link.** The server keeps serving on a link until the peer disconnects. VMS ends each link after its access.
4. **Transfer modes.** VMS used sequential file transfer (RAC 3) in both directions, with `ROP` bits set. Records went OVMX→VMS one DATA per record, and VMS→OVMX as LEN256 blocked DATA.

`fal-inbound.pcap` / `.wire.txt` hold the wire. The replay test `tests/vmsdecnet/test_dnet_fal_server.c` pins these exact segments in CI.
