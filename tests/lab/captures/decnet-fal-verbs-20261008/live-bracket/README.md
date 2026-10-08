# Live bracket: a real VAX against a booted OVMX FAL (2026-10-08)

The operator's DECnet lab ran VAX1 (OpenVMS VAX V7.3, 1.1) against a booted OVMX node, 1.42. The OVMX image was #1490 plus #1487 at 549c21d2. The console is `falverbs-live-vax1-console.txt` and the NSP/DAP dump is `falverbs-live-wire.txt` (12 links). Passwords are masked as `XX` bytes.

This is the bracket's evidence for the fixes in rd vms-277a:

| VAX symptom | wire cause | fix |
|---|---|---|
| DIRECTORY lists only BRK1, then `RMS-F-BUG_DAP, DAP code = 0001A006` | OVMX sent an ACKNOWLEDGE after each DIRLIST file. DAP 5.6 (spec 5.2.11) has none, and 0001A006 is MAC 10 sync with MIC = ACK. | The per-file ACK is dropped. |
| DIRECTORY/FULL shows empty protection and no dates | OVMX's SYSCAP lacked bits 24, 26 and 27, so the VAX asked for DISPLAY = MAIN only. | The bits are advertised. |
| RENAME gets `RMS-F-SUPPORT` | SYSCAP lacked bit 37 (rename). | The bit is advertised. |
| DELETE `;*` gets `RMS-F-WLD` | SYSCAP lacks bit 38 (wildcard). | Fixed: bit 38 is advertised. The VAX resolves the wildcard itself with a DIRECTORY LIST and erases each file it lists; that needs three links per node (second run, below). |
| DIRECTORY of a missing file prints `Total of 1 file` | OVMX sent NAMEs, then STATUS FNF. | Fixed: the reply follows the VAX-to-VAX SYS$LOGIN capture, and the second run (below) prints NOFILES as the VAX does. |

`tests/vmsdecnet/test_dnet_fal_server.c` (test 5) replays the VAX's DIRLIST from this capture. It checks that the reply now lists both files with no ACK and that the CONFIGURATION advertises the new bits.

## Second run (#1487 at 937b34bf, with #1490): `falverbs-live2-*`

DIRECTORY/FULL, DIRECTORY, TYPE of a wildcard, the GUEST TYPE of SYSUAF.DAT (RMS-E-PRV), and NOFILES now behave as on the VAX. Two failures were left:

| VAX symptom | wire cause | status |
|---|---|---|
| GUEST `DELETE ...SYSUAF.DAT;*` gets `RMS-E-MKD` / `SYSTEM-F-REMRSRC` | VMS holds two DIRLIST links open and then connects a third for the ERASE. NETACP refused the third link immediately with Disconnect reason 1 (resource), because its per-node share was 2. The VAX-to-VAX capture shows three concurrent links. | Fixed outside this PR: Baron's rd vms-9cd decision allows 3 links per node (pool 9), merged as #1522. |
| `RENAME` gets `RMS-F-SUPPORT` | After OVMX's CONFIGURATION, VAX1 disconnects without sending an ACCESS. Some property of the CONFIGURATION (version or SYSCAP) makes the client refuse. | Open. `config_probe_drv.py` lets the lab try candidate CONFIGURATIONs against the same RENAME. |

## RENAME and the DAP version (rd vms-b2f, 2026-10-08)

The peer ran a two-link probe from VAX1 V7.3: `RENAME 1.43"SYSTEM x"::X.TXT 1.43"SYSTEM x"::Y.TXT`.
- Link A was played by `dirlist_drv.py`.
- Link B was played by `config_probe_drv.py`.
- Both ran under `dapprobe_skipci.py` instances.

Results:
- **At DAP 5.6, link B is refused.** If link B answers with DAP 5.6, the VAX disconnects (`RMS-F-SUPPORT`) even with the VAX's full SYSCAP.
- **At DAP 7.2, the VAX sends the rename.** With either SYSCAP it sends ACCESS RENAME. With OVMX's SYSCAP it uses the DAP 5.6 field layout: no DAP 7 ACCESS extension and no segment trailer.
- **Link-A names make no difference.**

OVMX therefore advertises DAP 7.2. The lab driver `faldrv2.c` and `run3.sh` run OVMX's compiled server under three concurrent dapprobe instances, one per link, for a lab replay of whole VMS commands. They were first run on 2026-10-08 (below).

## Third run: DAP 7.2 (rd vms-b2f): `falverbs-live3-*`

`faldrv2` (built from this branch, so it advertises DAP 7.2 and ACKs each DIRLIST file for a DAP 7 client) ran at 1.43 under `run3.sh`. VAX1 V7.3 typed each command; `falverbs-live3-vax1-console.txt` is its console, and `falverbs-live3-<command>-<link>.log` is each link's DAP traffic as `dapprobe_skipci.py` logged it. The serve directory held `X.TXT`, `BRK1.TXT`, `BRK2.TXT` and `PRIVP.TXT`; the driver refuses `PRIV*` files, as the executive refuses a protected one.

| command on VAX1 | result |
|---|---|
| `RENAME ...::X.TXT ...::Y.TXT` (`ren-0`, the earlier lab binary, which sent no per-file ACK, with `CFG_VER` patching its CONFIG to 7.2) | `RMS-F-BUG_DAP, DAP code = 0001A007`: a DAP 7.2 client wants the per-file ACK before ACCESS COMPLETE. |
| `RENAME ...::X.TXT ...::Y.TXT` (`ren2`) | Renamed. Link B carried ACCESS RENAME + NAME; X.TXT became Y.TXT. |
| `DIRECTORY/FULL ...::BRK*.TXT` (`dirf`) | Two full entries, `Total of 2 files`. The dates show `<None specified>` because the driver's file stand-in has none. |
| `TYPE ...::BRK*.TXT` (`type`) | Both files typed, each headed by its name. |
| `DELETE ...::BRK2.TXT;*` (`del`) | Deleted (three links: two DIRLISTs, then the ERASE). |
| `DELETE ...::PRIVP.TXT;*` (`delp`) | `%DELETE-W-FILNOTDEL ... -RMS-E-PRV`, as against a VAX FAL. |
| `RENAME ...::PRIVP.TXT ...::Z.TXT` (`renp`) | `-RMS-F-RMV, ACP remove function failed`, the same lines as the VAX-to-VAX run (`../vax1-console.txt`). |
| `DIRECTORY ...::NOSUCH.TXT` (`nof`) | `%DIRECT-W-NOFILES, no files found`. |

With OVMX's SYSCAP the 7.2 client still uses the DAP 5.6 field layout: no segment trailer, no type-18 File ID. Test 7 of `tests/vmsdecnet/test_dnet_fal_server.c` replays six of these links and requires OVMX's reply to be the bytes VAX1 accepted.
