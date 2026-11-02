# vms-3106 live re-check: a rebooted OVMX's first SET HOST to a real VAX (2026-11-02)

rd vms-3106, fix PR #1958 (merged as c9fe1db15). This is the live re-check of the side
finding in `../vms-05d-nodec-decnet-20261021/README.md`. When an OVMX node is killed in the
middle of `SET HOST` and rebooted, its first new CTERM Connect Initiate reuses the NSP source
link address of the dead link. The real VAX still holds that link, so it drops the CI.

## Bench

This is the vms-05d bench, run locally on workshop. No lab pod VMS node was involved.

- **VAXC**: real OpenVMS VAX V7.3 under SIMH (MicroVAX 3900, `ovmx-vaxlab:5` image, local
  container). The disk is a scratch copy of the published Node C DECnet disk, sha256 `974af57e…` (`.gz`).
  It runs DECnet 1.965 with OVMXA defined at 1.963. At run time the console registers and loads
  DVNETRTG, VAX-VMS and VAXCLUSTER from the lab PAKs. The console log is paused while the PAK
  lines are typed, so the PAKs are redacted entirely. See `bench/lla_reboot.py`, `Vax.license`.
- **OVMXA**: x86 OVMX under KVM, DECnet 1.963. The databases are put in place by
  `tools/cluster-web-demo/inject-decnet-config.sh --scsnode OVMXA`. Each variant then gets its own
  `SYS$SYSTEM:DECNETD.EXE;2`, a static musl build with `-DOVMX_DATALINK_VIA_EXECUTIVE=ON`, which
  `bench/ods2put.c` writes onto the ODS-2 volume. Each boot console shows the `;2` image with
  `DIRECTORY/SIZE`. STARTNET runs it as NETACP, so SET HOST goes through the NETACP link pool.
- **Wire**: SIMH `udp:` Ethernet is bridged to qemu's socket-mcast segment by `bench/udp2mcast.py`.
  The frames are taken from OVMXA's NIC by qemu `filter-dump`, one pcap per boot, and decoded by
  `bench/decode_nsp.py` into `*-wire.txt`. Passwords in NSP data show as `XX`. The pcaps are not
  committed because they hold the passwords in clear.
- **Procedure** (`bench/lla_reboot.py`): boot OVMXA, `SET HOST VAXC`, and log in. Read VAXC's
  `NCP SHOW KNOWN LINKS`, then SIGKILL qemu mid-session. Boot the same disk again at once,
  read VAXC's links again, and `SET HOST VAXC`.

| variant | image | DECNETD.EXE;2 |
|---|---|---|
| baseline-main | built from origin/main 5c42db530 (`distro/Dockerfile.bootable`) | origin/main 5c42db530 |
| baseline-v0.7-14 | V0.7-14 release `ovmx-distrib.img` / `vmlinuz` / initramfs | the release's own (`;1`, no `;2`) |
| fix | the same origin/main 5c42db530 image | PR #1958 head c1e4d7e0a. Its `src/vmsdecnet` is identical to merged c9fe1db15 |

## Result

| variant | boot1 link (OVMXA srcaddr) | VAXC still holds after reboot | first CI after reboot | outcome |
|---|---|---|---|---|
| baseline-main | 0x2100 (8448) | link 8193, remote link 8448 | srcaddr **0x2100 (8448)**, the dead link's address | VAXC never answers the CI. It is sent 9 times, and OVMX gets `%SYSTEM-F-TIMEOUT` after 44.7 s |
| baseline-v0.7-14 | 0x2100 (8448) | link 8193, remote link 8448 | srcaddr **0x2100 (8448)** | the same: no answer, `%SYSTEM-F-TIMEOUT` after 44.2 s |
| fix | 0x44a9 (17577) | link 8193, remote link 17577 | srcaddr **0x8897 (34967)** | VAXC sends a CC in 4 ms on its new link 8194. `Username:` appears, the login completes, `WHO=VAXC TT=_VAXC$RTA2:`, and `%REM-S-END` returns control |

In both baselines, the only frames VAXC sends to OVMXA during the retry window are NSP
link-service messages on its stale link (dst 0x2100, src 0x2001). VAXC drops the CI silently.
It sends no Disconnect Initiate, so this bench did not see a reason-38 DI. The stale link was
still in VAXC's `SHOW KNOWN LINKS` more than 3 minutes after the kill (baseline-v0.7-14 events).

With the fix, VAXC holds the dead link (8193 ↔ 17577) and the new one (8194 ↔ 34967) side by
side. `fix-vaxc-console.log` shows both in `NCP SHOW KNOWN LINKS`.

## Files

- `*-events.log`: the driver's timeline, including VAXC's `SHOW KNOWN LINKS` at each step.
- `*-vaxc-console.log`: the VAXC console. PAK entry is replaced by a redaction note.
- `*-boot1-ovmxa-console.log` / `*-boot2-ovmxa-console.log`: the OVMXA consoles. Passwords are masked as XX.
- `*-wire.txt`: the decoded 0x6003 frames of both boots. Hellos are counted, not listed, and
  filter-dump's duplicate copies of transmitted frames are dropped.
- `bench/`: `lla_reboot.py`, `node.py`, `udp2mcast.py`, `decode_nsp.py`, `ods2put.c`, `vaxc.ini`.
