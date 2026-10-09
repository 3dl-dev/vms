# Live bracket: OVMX `$ SET HOST` to a real VAX (rd vms-b19, 2026-10-08)

The OVMX node is 1.44, booted in pod dnlab-1. The host is VAX1 at 1.1, running OpenVMS VAX V7.3. Each capture is `tcpdump` on the lab bridge, filtered to OVMX's station address AA-00-04-00-2C-04.

| file | what it shows |
|---|---|
| `sethost-stall.pcap` | The run before the fix. The CTERM session binds and VAX1 sends the `VAX/VMS V7.3 node VAX1` banner. VAX1 then sends its `0f 00` read-characteristics solicit with handle `a7 59 01 00`. OVMX answers with `a7 59 00 00` because it echoed only two bytes. VAX1 ignores that reply and never sends `Username:`. |
| `sethost-login.pcap`, `ovmx-console-login.txt` | The run after the fix, where the handle is echoed as four bytes. These steps all complete over the link: the banner, then logging in as SYSTEM (the lab account in `tests/lab/README.md`), then `SHOW TIME` and `F$GETSYI("NODENAME")` (which returns `VAX1`), then `LOGOUT`. Finally VAX1 prints `%REM-S-END, control returned to node OVMX::`. |

The older oracle (`docs/oracle/vax-sethost-cterm.*`, VAX to VAX) has handles whose high bytes are all zero, so the two-byte echo matched it byte for byte.

There is also a separate observation, filed as its own rd item. A rebooted OVMX opens its first link from NSP address 8193 again. VAX1 still holds the link from before the reboot, so it disconnects the new Connect Initiate with "no response from object" until that old link times out.
