# DECnet SET HOST → live remote DCL, OVMX → real OpenVMS VAX V7.3 (2026-09-11)

Wire-proof for the **CTERM SET HOST rung** (rd vms-a70 direction A / vms-62a): a complete
live interactive terminal session over DECnet Phase IV, from an OVMX node to a **real
OpenVMS VAX V7.3** node, captured on the isolated `vaxlab-3` lab (2 real VMS VAX nodes
under SIMH on br0). OVMX = node 1.42 (`OVMXR3`), the target = real VAX1 = node 1.1.

`DECNETD.EXE --set-host 1.1` drove the entire exchange, wire-verified end to end:

```
CI → CC → NSP link-service (credit grant) → CTERM foundation negotiation (seg 1..N,
byte-exact vs the real oracle) → LOGINOUT spawns _RTA1: on VAX1 →
  Username: → SYSTEM
  Password: → system
  Welcome to OpenVMS (TM) VAX Operating System, Version V7.3 on node VAX1
  $ WRITE SYS$OUTPUT F$GETSYI("NODENAME")   → VAX1
  $ SHOW SYSTEM   → VAX1's live process table, INCLUDING "20200228 _RTA1: CUR"
                    (the session's own process, in its own output — proof of a
                     genuine interactive DCL, not a canned reply)
  $ LOGOUT   → SYSTEM logged out at 11-SEP-2026 12:59:45.30
→ NSP disconnect-initiate / disconnect-confirm (clean teardown)
```

- `sethost-live-dcl.pcap` — the 85-frame capture (ethertype 0x6003, br0).
- `sethost-live-dcl.txt` — `tcpdump -nn -A` decode (the ASCII session is legible in it).

## What this proves (and what it does not)

Proven, against real VMS hardware: DECnet Phase IV **datalink + routing + NSP + CTERM
foundation + terminal I/O**, all the way to an authenticated, interactive DCL session and
a clean logout. The `_RTA1:` process appearing in its own `SHOW SYSTEM` output is the
anti-LARP tell — a real LOGINOUT-created interactive process on the real VAX.

This is **direction A** (OVMX SET HOST → real VAX). Direction B (real VAX SET HOST → OVMX)
needs the full booted OVMX executive (`$CREPRC` LOGINOUT on an OVMX-minted RTAn:) and is a
separate rung.

## How the CTERM layer was made real

OVMX's CTERM protocol was previously an *invented* wire format (a facade that a real VAX
rejected). It was re-grounded byte-for-byte on captured real-VMS oracles (clean-room: the
wire bytes are the uncopyrightable protocol fact; the C is OVMX's own): the foundation
short-TLV + `09 00 [len]` msgtype-9 envelope codec (#1146), the host-speaks-first
negotiation FSM, the NSP link-service (MSGFLG 0x10) credit grant after CC, and prompt-gated
terminal input (one Read Data per host read-solicit). Each step was root-caused from a lab
capture and validated on real hardware.
