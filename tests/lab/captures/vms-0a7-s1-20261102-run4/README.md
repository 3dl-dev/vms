# vms-0a7 S1, run 4: DIRECTORY, TYPE and COPY node:: against a real VAX V7.3, with NSP flow control (2026-11-02)

rd vms-985 (NSP flow control) and rd vms-0a7 S1, branch `work/vms-0a7-s1-rms-remote`, PR #1994 (draft).
This repeats run 3 (`../vms-0a7-s1-20261102/`) on the same bench against the same real VAX. Run 3's two
large-file failures are gone: NSP DI reason 1 (VAX→OVMX) and the OVMX→VAX hang.

**Result: every large COPY completes both ways, and every round trip is byte-identical (DIFFERENCES 0,
CHECKSUM equal).** Two outputs still differ from the VAX's (see "Does not match"):
- COPY/LOG of the 290-block BIG.TXT reports `(297 blocks)`. The VAX reports `(290 blocks)`.
- DIRECTORY/FULL prints `Created: <None specified>` and `Revised: <None specified>`. The VAX prints the dates.

## Runs

| run | image (git SHA) | what happened |
|---|---|---|
| 4a | 422c6641d (vms-985 NSP fix + drain-before-DI) | Every COPY completed, with no DI reason 1 and no hang. The BIG and LONG round trips through a local OVMX copy came back with each record's first 2 bytes missing (`cord 0001 …`): DIFFERENCES found 3 records, BIG checksum 258688 against 33952600. The VAX → OVMX FAL → VAX path (7a–7d) was identical. Cause: `rms_remote_get` dropped the VFC fixed control. Fixed in 4bb6a7c6c. Logs are in `run4a-422c6641d/` (no wire decode). |
| **4b** | **4bb6a7c6c** | **These logs.** Every step completed. Bench exit 0, `DONE` at 23:56:48. |

**Image for 4b:**
- Built with `docker build -f distro/Dockerfile.bootable --target runner` from the worktree at 4bb6a7c6c. Docker image id `sha256:3e8503a44120…`.
- DECnet 1.963 was injected with `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA` (PASS).
- The sha256 of the disk, kernel and initramfs are in `artifacts.sha256`.
- The disk image carries vms-985's NETACP (its new log text is present in DECNETD.EXE on the volume).

## Bench

The bench is run 3's, copied to `bench/` and run locally on workshop. It adds two steps:
- `3i`: OVMX `COPY/LOG` of the 290-block BIG.TXT;
- `L3i`: the same command run by the VAX against itself.

`bench/oracle_diff.py` compares them. Everything else is as in run 3:
- **VAXC:** a fresh scratch copy of the Node C DECnet disk (sha256 `974af57e…`), licensed from the lab PAKs. The console log is paused while the PAKs are typed, so they are redacted entirely.
- **Wire:** SIMH is bridged to qemu mcast. The frames are taken from OVMXA's NIC by qemu `filter-dump`.
- **Fixtures:** DNTEST, BRK1/BRK2/BRKP, BIG.TXT (2000 VFC records, 290 blocks) and LONG.TXT (20000 VFC records, 3125 blocks).

**Passwords:** they are replaced by `XX` in every committed file. The pcap is not committed. `wire.txt.gz` is its
decode (82 274 lines), with passwords masked by `decode_nsp.py` (MASK). A grep for the lab passwords in
every committed file finds 0.

## Results (run 4b; $STATUS from `events.log`; VAX side = its own L-step or its DIFFERENCES/CHECKSUM)

| # | case (on OVMXA unless VAXC) | OVMX result | VAX result | $STATUS OVMX / VAX | verdict |
|---|---|---|---|---|---|
| 0a–0b | `DEFINE/PROCESS SYS$SYSTEM SYS$LOGIN:`, then `SET HOST VAXC` (positive control) | `DIRECTORY SYS$SYSTEM:DECNETD.EXE` → NOFILES; SET HOST → `%SET-I-NOTAVAIL` | — | %X00000001 / — | control valid |
| 0c–0f | DIRECTORY / TYPE / COPY BRK2 both ways with DECNETD.EXE unreachable | all work | BRK2RT: DIFFERENCES 0, CHECKSUM 1225656911 = 1225656911 | %X00000001 / %X006C8009 | **PASS**: no DECNETD.EXE activation |
| 0g | `COPY VAXC::BIG.TXT` (2000 rec) with DECNETD.EXE unreachable | completes in 88 s, no DI | — | %X00000001 / — | **PASS** (run 3: DI reason 1) |
| 6a samples | VAXC `SET HOST OVMXA` samples `SHOW SYSTEM` 3× during the 11-min LONG COPY | OPCOM, NETACP, STARTUP, SYSTEM, JOB_CONTROL, _RTA0: in every sample; no DECNETD / FAL process | — | — | **PASS** (the COPY was running) |
| 1a | `DIRECTORY VAXC"SYSTEM pw"::SYS$LOGIN:*.COM` | both search-list directories | — | %X00000001 / — | PASS |
| 1b / 1e | `DIRECTORY …NOSUCH*` | `%DIRECT-W-NOFILES`; identical to L1e | same | %X10018290 / %X10018290 | **PASS** |
| 1c / 1d | `DIRECTORY …BRK*.TXT` (SYSTEM, DNTEST) | the file list | L1d: one more file, BRK1RT, which 3c created before L1d ran | %X00000001 / %X00000001 | PASS |
| 1f | `DIRECTORY/FULL VAXC"DNTEST pw"::…BRK*.TXT` | full listing; sizes, owner, protection, record format and totals match | the same listing, plus dates | %X00000001 / %X00000001 | **DOES NOT MATCH**: Created/Revised `<None specified>` |
| 2a / 2b | `TYPE` one file / a wildcard (SYSTEM) | the records, with per-file headers | — | %X00000001 / — | PASS |
| 2c | `TYPE VAXC"DNTEST pw"::…BRK*.TXT` | the readable files are typed; BRKP gives OPENIN/-RMS-E-PRV | the same, plus BRK1RT (created by 3c before L2c ran) | %X10951098 / %X10951098 | **PASS** |
| 2d | `TYPE …BRKP.TXT` | identical to L2d | same | %X10951098 / %X10951098 | **PASS** |
| 5a | `DIRECTORY VAXC"DNTEST wrong"::…` | identical to L5a; the password is not echoed | same | %X1001C02A / %X1001C02A | **PASS** |
| 5b | `TYPE VAXC"DNTEST wrong"::…` | identical to L5b | same | %X10951238 / %X10951238 | **PASS** |
| 3a–3c | COPY BRK1 VAX→OVMX→VAX as BRK1RT | completes | DIFFERENCES 0; CHECKSUM 742093723 = 742093723 | %X00000001 / %X006C8009 | **PASS** |
| 3d | `COPY/LOG VAXC"SYSTEM pw"::…BRK*.TXT SYS$LOGIN:*.*` | 6 `%COPY-S-COPIED … (1 block)` lines + NEWFILES; identical to L3d | same | %X00000001 / %X00000001 | **PASS** (" (N blocks)" now printed) |
| 3f / 3h | COPY BIG.TXT VAX→OVMX (97 s), then OVMX→VAX as BIGRT (10 s) | both complete | BIGRT 290/297 blocks; DIFFERENCES 0; CHECKSUM 33952600 = 33952600 | %X00000001 / %X006C8009 | **PASS**, byte-identical (run 3: DI reason 1) |
| 3i | `COPY/LOG VAXC::BIG.TXT SYS$LOGIN:BIGL.TXT` | `… copied to …BIGL.TXT;1 (297 blocks)` | L3i: `… (290 blocks)` | %X00000001 / %X00000001 | **DOES NOT MATCH**: block count |
| 6a / 6b | COPY LONG.TXT (20000 rec) VAX→OVMX (10 min 56 s), then OVMX→VAX as LONGRT (1 min 43 s) | both complete | LONGRT 3125/3132 blocks; DIFFERENCES 0; CHECKSUM 2 = 2 | %X00000001 / %X006C8009 | **PASS**, byte-identical (run 3: DI reason 1) |
| 4a–4g | VAXC COPY V2O.TXT into OVMX's FAL and back; DIRECTORY of OVMXA | complete | DIFFERENCES 0; CHECKSUM 2033922319 = 2033922319 | — / %X00000001 | PASS |
| 7a | VAXC `COPY BIG.TXT OVMXA"SYSTEM pw"::BIGV.TXT` (into OVMX's FAL) | 47 s (run 3: 157 s) | — | — / %X00000001 | PASS |
| 7c / 7d | OVMX `COPY SYS$LOGIN:BIGV.TXT VAXC::BIGVRT.TXT` (OVMX→VAX, large) | 10 s | BIGVRT 290/297; DIFFERENCES 0; CHECKSUM 33952600 = 33952600 | %X00000001 / %X006C8009 | **PASS** (run 3: hung) |

The `(A)` section of `oracle-diff.txt` compares against the 2026-10-08 VAX-to-VAX capture. Its fixture files
were VFC, so record-format lines differ; the BRK2RT line comes from this run's 0f. Neither is an OVMX difference.

## On the wire (`wire.txt.gz`)

- **No DI reason 1.** The 53 DIs OVMX sent all carry reason 0 (normal). The VAX's two reason-34 DIs are the
  two wrong-password cases.
- **The receive window holds.** Example, the 0g link (OVMX 0x4b5c ↔ VAX 0x2008, around 12.86 s):
  - when half of OVMX's 32-segment queue is in use, OVMX sends link service `01 80 02 00 01 00`, which says stop sending;
  - the VAX acks it (OTHER-ACK) and stops sending data;
  - OVMX sends `01 80 03 00 02 00`, which says resume, once the reader has drained the queue.
  OVMX sent 468 such stop/resume link services; the VAX sent 30 to OVMX.
- **Link-service form.** Every link service OVMX sends now has the VAX's own 6-byte layout. Run 3's 4-byte
  `00 80 01 00` is gone.
- **Duplicate frames.** As in run 3, the capture shows some OVMXA frames twice in the same millisecond.
  This is a capture artefact on the mcast tap; both copies have the same sequence numbers.

## Does not match (open)

| case | OVMX | VAX V7.3 | cause | follow-up |
|---|---|---|---|---|
| 3i COPY/LOG block count | `(297 blocks)` | `(290 blocks)` | OVMX's local RMS has no RAB$L_RHB. Its `$PUT` of a VFC record writes a zeroed fixed-control area, and `$GET` discards it (`src/vmsrms/rms_seq.c`). With 4bb6a7c6c, a VFC record keeps its print control at the front of the record, as OVMX's FAL already stored it (7a). The local copy therefore holds 2 extra bytes per record: 4000 bytes over 2000 records, which is the 7 blocks. Round trips are byte-identical. A local `TYPE` of such a file would show the 2 control bytes; that is inferred from the code and was not run in this bench. | rd item: local RMS VFC fixed control via RAB$L_RHB (`$GET`/`$PUT`), and COPY passing it. This touches the `struct RAB` layout, which `tests/libvms/test_conformance_layout.c` pins. |
| 1f DIRECTORY/FULL dates | `Created:   <None specified>` / `Revised:   <None specified>` | `Created:    2-NOV-2026 23:40:25.81` … | `rms_remote` takes only the DAP 7 64-bit DATETIME fields (`have_cdt64`). This VAX, talking to OVMX, sent none of them (the host unit test passes on the VAX-to-VAX DAP 7.2 capture). | rd item: DIRECTORY/FULL dates against a live VAX (ASCII DATETIME, or negotiate the 64-bit form) |

**Throughput:** VAX→OVMX LONG.TXT takes 10 min 56 s, against 1 min 43 s OVMX→VAX. The wire shows OVMX's
receive queue filling and the resume going out only once the client has drained it. The OVMX client
process is slower than the wire (the time is in the local write path, not in NSP). This was not
investigated here.

## Files

- **Run 4b:**
  - logs: `ovmxa-console.log`, `vaxc-console.log`, `events.log`
  - comparisons and wire: `oracle-diff.txt`, `wire.txt.gz`
  - image hashes: `artifacts.sha256`
- `run4a-422c6641d/`: run 4a's consoles, events and oracle diff.
- `bench/`: `fal_verbs.py`, `oracle_diff.py`, `decode_nsp.py`, `node.py`, `udp2mcast.py`, `vaxc.ini`.
