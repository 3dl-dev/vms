# vms-6a4: DIRECTORY, TYPE and COPY node::spec between OVMX and a real VAX (2026-11-02)

rd vms-6a4, PR #1962 (head 64b24dc6d). OVMX DCL `DIRECTORY` and `TYPE` of `NODE"user pw"::spec`
go through FAL (object 17) on a real OpenVMS VAX V7.3, and `COPY` works in both directions.

## Bench

This is the bench from `../vms-3106-lla-reboot-20261102/`, run locally on workshop. No lab pod
VMS node was used.

- **VAXC**: real OpenVMS VAX V7.3 under SIMH (`ovmx-vaxlab:5`). The disk is a scratch copy of the
  Node C DECnet disk, sha256 `974af57e…` (`.gz`), and it runs DECnet 1.965. At run time the console
  loads DVNETRTG, VAX-VMS and VAXCLUSTER from the lab PAKs. The console log is paused while they
  are typed, so the PAKs are redacted entirely.
- **OVMXA**: x86 OVMX under KVM. The whole image is built from 64b24dc6d with
  `distro/Dockerfile.bootable`. DECnet 1.963 is injected by
  `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA`.
- **Fixtures on VAXC** (`bench/fal_verbs.py`): they reproduce the oracle's layout
  (`../decnet-fal-verbs-20261008/vax-to-vax-sys-login/`):
  - the bench creates an unprivileged account DNTEST [200,201] with SYS$LOGIN = SYS$SYSDEVICE:[SYSMGR];
  - BRK1.TXT and BRK2.TXT are owned by DNTEST with protection (S:RWED,O:RWED,G:RE,W);
  - BRKP.TXT is owned by [1,4] with protection (S:RWED,O:RWED,G,W).

  The SYSTEM account runs the steps the job asks for. DNTEST runs the protection and oracle cases,
  because SYSTEM's privileges read every file.
- **Wire**: the frames are taken from OVMXA's NIC by qemu `filter-dump` and decoded by
  `bench/decode_nsp.py` into `wire.txt`. Passwords show as XX. The pcap is not committed.

## Results

| # | command | result |
|---|---|---|
| 1a | OVMXA `DIRECTORY VAXC"SYSTEM pw"::SYS$LOGIN:*.COM` | 36 files from `SYS$COMMON:[SYSMGR]`, the same list VAXC's own console shows. `$STATUS` %X00000001 |
| 1b | `DIRECTORY VAXC"SYSTEM pw"::SYS$LOGIN:NOSUCH*.XYZ` | `%DIRECT-W-NOFILES, no files found` |
| 1c/1d | `DIRECTORY …BRK*.TXT` as SYSTEM and as DNTEST | BRK1, BRK2, BRKP, `Total of 3 files.` |
| 1e | `DIRECTORY VAXC"DNTEST pw"::SYS$LOGIN:NOSUCH.TXT` | `%DIRECT-W-NOFILES` |
| 1f | `DIRECTORY/FULL VAXC"DNTEST pw"::…` | **`%SYSTEM-F-UNSUPPORTED`**. The PR serves only the plain form and refuses any qualifier honestly |
| 2a | `TYPE VAXC"SYSTEM pw"::…BRK1.TXT` | both records |
| 2b | `TYPE VAXC"SYSTEM pw"::…BRK*.TXT` | three files, each with its header. SYSTEM can read BRKP |
| 2c/2d | `TYPE VAXC"DNTEST pw"::SYS$LOGIN:BRK*.TXT` / `BRKP.TXT` | BRK1 and BRK2 are typed. BRKP gives `%TYPE-W-OPENIN … -RMS-E-PRV` |
| 3 | `COPY VAXC"SYSTEM pw"::…BRK1.TXT SYS$LOGIN:` then `COPY SYS$LOGIN:BRK1.TXT VAXC"SYSTEM pw"::…BRK1RT.TXT` | On VAXC, `DIFFERENCES` finds 0 difference sections. `DUMP/RECORDS` matches record for record, `CHECKSUM` is 742093723 for both, and `DIRECTORY/FULL` gives the same record format (VAR, CR) and protection |
| 4 | VAXC `COPY V2O.TXT OVMXA"SYSTEM pw"::V2O.TXT`, `COPY OVMXA"SYSTEM pw"::V2O.TXT V2O_BACK.TXT`, `DIFFERENCES`, `DIRECTORY OVMXA"SYSTEM pw"::` | Both copies succeed and `DIFFERENCES` finds 0 sections. `DIRECTORY` lists OVMXA's `SYS$SYSROOT:[SYSMGR]`, 17 files, V2O.TXT among them. OVMXA's own `TYPE SYS$LOGIN:V2O.TXT` shows the two lines |

## Against the VAX-to-VAX oracle (`oracle-diff.txt`)

`bench/oracle_diff.py` normalises the node string `VAXC"DNTEST password"::` to the oracle's `VAX2::`,
because the oracle used no access-control string. It then diffs the console output:

- `DIRECTORY …BRK*.TXT`: **identical**, byte for byte, blank lines included.
- `DIRECTORY …NOSUCH.TXT`: **identical**.
- `TYPE …BRK*.TXT`, including the BRKP `%TYPE-W-OPENIN` / `-RMS-E-PRV` refusal: **identical**.
- `DIRECTORY/FULL`: **different**. VMS prints the full attribute listing and, for BRKP, a
  per-file `insufficient privilege` line. OVMX prints `%SYSTEM-F-UNSUPPORTED`. This is the one
  difference found.

Other observations, not differences from the oracle:
- With an access string, OVMX prints the node as `VAXC"SYSTEM password"::`, with the literal word
  `password`. The oracle has no access-string case to compare this against.
- The fixture files were made with `CREATE` from the terminal, so they are VAR/CR. The oracle's
  files were VFC/print. Only `/FULL` would show that, and it is refused.

## Files

`ovmxa-console.log`, `vaxc-console.log`, `events.log` (passwords XX), `wire.txt`, `oracle-diff.txt`,
and `bench/` (`fal_verbs.py`, `oracle_diff.py`, `decode_nsp.py`, `node.py`, `udp2mcast.py`, `vaxc.ini`).
