# Oracle: real VAX↔VAX DECnet SET HOST (CTERM) — credential semantics + the NETACP→object-42→RTAn:→LOGINOUT sequence

**rd vms-558 (P0). Clean-room (Rule 8): this records REAL OpenVMS behavior observed on the lab,
the ground truth OVMX's DECnet CTERM / NETACP / RTAn: implementation (P4 vms-f40, P5 vms-9ab)
must match — not invented from OVMX's own guess.**

- **Captured:** 2026-09-09, lab pod `vaxlab-1`. `$ SET HOST VAX2` issued from **VAX1 (DECnet node
  1.1)** to **VAX2 (node 1.2)**, both stock **OpenVMS VAX V7.3**, over the QNA-0 (DELQA) circuit on
  the pod's `br0` bridge. Node MACs are the algorithmic Phase-IV form `AA-00-04-00-<area.node LE>`:
  1.1 = `aa:00:04:00:01:04`, 1.2 = `aa:00:04:00:02:04`.
- **Artifacts (this directory):**
  - `vax-sethost-cterm.pcap` — the DECnet wire trace (ethertype 0x6003), full NSP logical-link life.
  - `vax-sethost-cterm.console.txt` — the initiator-side console transcript (VAX1), incl. the remote
    login, `SHOW TERMINAL`, `SHOW PROCESS/ALL`, and `LOGOUT`.
  - `vax-sethost-cterm.wire.txt` — `tcpdump -nne` decode of the pcap.

---

## 1. The A4/A9 answer (the question P4 cannot be coded without)

**Q: does inbound CTERM carry a username the remote login CONSUMES (auto-login), and does it still
prompt for a password — or not?**

**A: The CTERM connect CARRIES the source `node::user` identity, but the remote LOGINOUT does NOT
consume it as a login credential. It prompts a FRESH `Username:` AND `Password:`. The carried
identity is SOURCE/PROXY (accounting) info, surfaced as "Remote Port Info", never a credential.**

Two independent proofs:

**Wire** — the NSP connect-initiate (`1.1 > 1.2 ... conn-initiate 8193>0 ver 4.1`) session-control
connect data carries the destination **object number `0x2a` = 42 (CTERM)** and the source string
**`53 59 53 54 45 4d` = "SYSTEM"**:
```
conn-initiate 8193>0 ver 4.1 segsize 1459
  0x0020:  0500 2a02 001a 0220 2006 5359 5354 454d   ....*...... SYSTEM
                  ^^ object 42 (CTERM)          ^^^^^^^^^^^^ "SYSTEM" (source id)
```

**Behavior** — despite "SYSTEM" being on the wire, the remote presents a clean prompt and
authenticates fresh (both Username and Password were supplied by the operator; neither was skipped):
```
$ SET HOST VAX2

VAX/VMS V7.3      node VAX2

Username: SYSTEM
Password:
 Welcome to OpenVMS (TM) VAX Operating System, Version V7.3 on node VAX2
```
and the carried identity shows up only as proxy/accounting metadata on the virtual terminal:
```
$ SHOW TERMINAL
Terminal: _RTA1:      Device_Type: VT100         Owner: _RTA1:
                                              Username: SYSTEM
Remote Port Info: 1025::SYSTEM          <-- 1025 = DECnet 1.1 (VAX1); source node::user
```

**OVMX consequence (P4):** the CTERM server MUST route the inbound connection through
`$CREPRC PRC$M_INTER|PRC$M_LOGINOUT` → LOGINOUT on an RTAn:, and LOGINOUT MUST prompt fresh. It MUST
NOT auto-login from the connect-carried username. (The decnetd `--cterm-server` strawman that spawned
DCL directly with no auth — the original LARP — is exactly wrong; the carried username is proxy info,
not a bypass.)

---

## 2. The NETACP → object-42 → RTTDRIVER(RTAn:) → LOGINOUT sequence

Observed end to end:

1. **Connect.** VAX1 sends an NSP `conn-initiate` to VAX2 addressed to **object 42 (CTERM)**, carrying
   ver 4.1, segsize 1459, and the source `node::user` (`1025::SYSTEM`). (`vax-sethost-cterm.wire.txt`.)
2. **Accept + virtual-terminal creation.** VAX2's NETACP accepts (`conn-confirm 8193>8193`), and the
   remote terminal driver (RTTDRIVER) mints a **virtual terminal `_RTA1:`** for the session. Its
   characteristics (from `SHOW TERMINAL`, the shape OVMX's RTAn: must present):
   - `Device_Type: VT100` (negotiated — VAX1's terminal type was conveyed; with VAX1's OPA0: left
     `UNKTERM` the connect still succeeds but the VT is untyped, so set a real type on the source),
     `Owner: _RTA1:`, `Username: SYSTEM`, `Remote Port Info: 1025::SYSTEM`.
   - Interactive, Echo, Type_ahead, TTsync, Wrap, Scope, Fulldup, Line Editing, Insert editing,
     ANSI_CRT, Advanced_video, DEC_CRT, VMS Style Input; Width 80, Page 24, Input/Output 300.
3. **LOGINOUT on the RTAn:.** LOGINOUT runs on `_RTA1:` and authenticates fresh (§1). The session
   process runs ON the virtual terminal — `SHOW PROCESS/ALL`: *"There is 1 process in this job:
   `_RTA1:` (*)"*, with **process rights `INTERACTIVE` + `REMOTE`** and system right `SYS$NODE_VAX2`.
4. **Data.** The CTERM terminal protocol rides the NSP logical link as `data 8193>8193` segments both
   ways (keystrokes ↔ screen) — the byte transport is the logical link, not a raw device.
5. **Teardown.** `$ LOGOUT` →
   ```
   SYSTEM       logged out at  9-SEP-2026 13:05:05.87
   %REM-S-END, control returned to node VAX1::
   ```
   on the wire an NSP `disconn-initiate 8193>8193 object rejected connect` → `disconn-confirm
   ... disconnect complete`. Control returns to the VAX1 `$` prompt.

**OVMX mapping (already have the pieces):** NETACP (P5) accepts object-42 connects; the RTAn: device
is `vms_devtab_add_terminal` (landed #1070, cross-process `$GETDVI`-proven); LOGINOUT-on-RTAn: via
`$CREPRC PRC$M_INTER|PRC$M_LOGINOUT` is landed (#1072). P4 wires the CTERM server to that path
(fresh-auth), P5 makes NETACP the object-dispatching ACP.

---

## 3. Provenance / method

- Lab: `kubectl -n ovmx-lab exec vaxlab-1` driving the two SIMH VAX consoles via the nodedrv FIFO;
  `tcpdump -i br0 'ether proto 0x6003 or 0x6002'` for the wire.
- VAX2's DECnet had to be provisioned first (rd vms-b3a): a running-system NETCONFIG+STARTNET kept
  hitting `%NCP-W-INVPVA` on the QNA-0 line (the LAN was already claimed by TCP/IP post-boot). The
  fix was a **clean pod-restart** so boot-time STARTNET brings DECnet up BEFORE TCP/IP claims the
  NIC — after the reboot VAX2's console logged `DECnet event 4.10, circuit up ... node 1.2 ...
  Circuit QNA-0`, and the SET HOST worked. (Boot-order, not a config defect — recorded on vms-b3a.)
- INV-6: everything above is observed, not inferred; the pcap + console files are the raw evidence.
