# Live bracket: a real VAX against a booted OVMX FAL (2026-10-08)

The operator's DECnet lab ran VAX1 (OpenVMS VAX V7.3, 1.1) against a booted OVMX node, 1.42. The OVMX image was #1490 plus #1487 at 549c21d2. The console is `falverbs-live-vax1-console.txt` and the NSP/DAP dump is `falverbs-live-wire.txt` (12 links). Passwords are masked as `XX` bytes.

This is the bracket's evidence for the fixes in rd vms-277a:

| VAX symptom | wire cause | fix |
|---|---|---|
| DIRECTORY lists only BRK1, then `RMS-F-BUG_DAP, DAP code = 0001A006` | OVMX sent an ACKNOWLEDGE after each DIRLIST file. DAP 5.6 (spec 5.2.11) has none, and 0001A006 is MAC 10 sync with MIC = ACK. | The per-file ACK is dropped. |
| DIRECTORY/FULL shows empty protection and no dates | OVMX's SYSCAP lacked bits 24, 26 and 27, so the VAX asked for DISPLAY = MAIN only. | The bits are advertised. |
| RENAME gets `RMS-F-SUPPORT` | SYSCAP lacked bit 37 (rename). | The bit is advertised. |
| DELETE `;*` gets `RMS-F-WLD` | SYSCAP lacks bit 38 (wildcard). | Open: wildcard retrieval, delete and rename (spec 5.2.20) must be served first. |
| DIRECTORY of a missing file prints `Total of 1 file` | OVMX sent NAMEs, then STATUS FNF. | Open: needs a VAX-to-VAX capture of the same command. |

`tests/vmsdecnet/test_dnet_fal_server.c` (test 5) replays the VAX's DIRLIST from this capture. It checks that the reply now lists both files with no ACK and that the CONFIGURATION advertises the new bits.

## Second run (#1487 at 937b34bf, with #1490): `falverbs-live2-*`

DIRECTORY/FULL, DIRECTORY, TYPE of a wildcard, the GUEST TYPE of SYSUAF.DAT (RMS-E-PRV), and NOFILES now behave as on the VAX. Two failures were left:

| VAX symptom | wire cause | status |
|---|---|---|
| GUEST `DELETE ...SYSUAF.DAT;*` gets `RMS-E-MKD` / `SYSTEM-F-REMRSRC` | VMS holds two DIRLIST links open and then connects a third for the ERASE. NETACP refused the third link immediately with Disconnect reason 1 (resource), because its per-node share was 2. The VAX-to-VAX capture shows three concurrent links. | The share is raised to 3 and the pool to 9 (decnetd `NETACP_MAX_PER_SOURCE`). |
| `RENAME` gets `RMS-F-SUPPORT` | After OVMX's CONFIGURATION, VAX1 disconnects without sending an ACCESS. Some property of the CONFIGURATION (version or SYSCAP) makes the client refuse. | Open. `config_probe_drv.py` lets the lab try candidate CONFIGURATIONs against the same RENAME. |
