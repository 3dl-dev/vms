# vms-05d follow-on: Node C disks re-imaged (2026-10-21)

Baron's rulings, 2026-10-21. Two changes:

1. **SYSTEM password never expires, on both Node C disks.** The old disks printed
   `WARNING - Your password expires on Friday, 23-OCT-2026 18:44` at every login.
2. **DECnet disk peers follow SCSSYSTEMID.** OVMXA is 1.963 (SCSSYSTEMID 1987) and OVMXB is
   1.964 (1988), for the same reason VAXC is 1.965. Real VMS will not start DECnet on a cluster
   member whose address differs from its SCSSYSTEMID (`%DECdtm-W-ADDRNEQ`; see
   `../vms-05d-nodec-decnet-20261021/`).

| disk | build | sha256 (.gz) | raw sha256 |
|---|---|---|---|
| `vms73-nodeC-cluster.dsk.gz` | `NOEXPIRE=1 FROM_GZ=` previous original `45355fd2…4fe15` | `2fc51785040e34e17961511cb0abfd836774a438c8535ecaf21d9b83c45d1980` | `59880a1ee14037f3301fe7592c08d8154fc53983296e6ef36f03de753608a7bc` |
| `vms73-nodeC-decnet-cluster.dsk.gz` | `DECNET=1 FROM_GZ=` the new original above | `974af57e43fa13026cafdb0bff081b548d279a7ba6e1fb23b450438a3f536376` | `4113c11b108329ba55d19930c56e8db135c85b1fa07ad21f4433535a64191d3f` |

Both disks are 1,505,766,912 bytes raw (RA92), published by baron-3dl/pcjs#11.

- `original-noexpire-build-console.log`: the noexpire build of the original disk. It boots,
  shows the old expiry warning, runs `MCR AUTHORIZE MODIFY SYSTEM/PWDLIFETIME=NONE/NOPWDEXPIRED`
  (`%UAF-I-MDFYMSG, user record(s) updated`), then logs out and back in with no warning, proves
  cluster genesis, and shuts down cleanly. `original-noexpire-build.out` holds the script's
  assertions.
- `decnet-rebuild-*.log`: PHASE 3 on the new original. At boot STARTNET still prints
  `%SYSTEM-F-NOLICENSE`. `NCP LIST KNOWN NODES` shows executor 1.965 (VAXC) and remote nodes
  1.963 (OVMXA) and 1.964 (OVMXB). `SHOW LICENSE` says no licenses exist.
- `verify-original-login.log` and `verify-decnet-login.log`: each published image, freshly
  decompressed, booted and logged in as SYSTEM. Neither shows an expiry warning.
  `AUTHORIZE SHOW SYSTEM` gives `Pwdlifetime: (none)` and empty `Flags:`. The DECnet image also
  shows the NCP database. While verifying the original, `MCR AUTHORIZE` was first typed outside
  SYS$SYSTEM and asked to create a new SYSUAF. That was answered NO, on a throwaway copy.
