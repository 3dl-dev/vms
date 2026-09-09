# Oracle: real VAX↔VAX DECnet file COPY (FAL/DAP) — object-17 access-control credential semantics + the NSP/DAP message sequence

**rd vms-cd3 (P0 for FAL, vms-8c2). Clean-room (Rule 8): this records REAL OpenVMS behaviour
observed on the lab — the ground truth OVMX's DECnet FAL server (object 17) + DAP codec + the
`COPY NODE"user pw"::` client (rung vms-8c2, north-star vms-e4dc) must match, not invent.**

- **Captured:** 2026-09-09, lab pod `vaxlab-1`. `$ COPY OVMXDAP.TXT 1.2"SYSTEM xxxxxx"::OVMXDAP_R.TXT`
  issued from **VAX1 (DECnet node 1.1)** to **VAX2 (node 1.2)**, both stock **OpenVMS VAX V7.3**, over
  the QNA-0 circuit on the pod's `br0` bridge. Algorithmic Phase-IV MACs `AA-00-04-00-<area.node LE>`:
  1.1 = `aa:00:04:00:01:04`, 1.2 = `aa:00:04:00:02:04`. The transfer **succeeded** — `OVMXDAP_R.TXT;1`
  landed in `SYS$SYSROOT:[SYSMGR]` on VAX2 (verified by `DIR` on the remote).
- **Artifacts (this directory):**
  - `vax-copy-fal-dap.pcap` — the DECnet wire trace (ethertype 0x6003), full NSP logical-link life.
  - `vax-copy-fal-dap.wire.txt` — the `tcpdump -nne` packet sequence (hellos stripped).
  - `vax-copy-fal-dap.hex.txt` — the `tcpdump -nnX` hex of the connect + every DAP data segment.
  - `vax-copy-fal-dap.console.txt` — the VAX1 initiator console transcript.

> **Redaction (INV-0):** the lab account's cleartext password value has been scrubbed from every
> artifact — replaced by a same-length placeholder `xxxxxx` in `.md`/`.hex.txt` and in the `.pcap`
> packet payload (so the pcap's checksum is intentionally stale; an oracle does not need a valid one).
> The oracle's value is the *structure* (tag `0x27`, length prefix, position) and the *carried-cleartext
> semantic*, not the password value — all of that is preserved. The username `SYSTEM` (uppercase) is
> retained; only the password value is redacted.

---

## 1. THE credential answer (the question FAL cannot be coded without) — and it is the OPPOSITE of CTERM

**Q: does an inbound file-access connect (COPY) carry the username/password the remote CONSUMES, or
does it authenticate some other way?**

**A: The FAL connect CARRIES the username AND password, IN CLEARTEXT, in the DECnet Session-Control
access-control fields of the conn-initiate. FAL authenticates the session FROM those connect-time
credentials — it does NOT prompt and does NOT re-challenge. This is the OPPOSITE of CTERM/SET HOST
(rd vms-558), where the access-control fields were EMPTY and the remote LOGINOUT prompted fresh.**

The conn-initiate (`1.1 > 1.2 ... conn-initiate 8195>0 ver 4.1`) session-control connect data
(`vax-copy-fal-dap.hex.txt` lines 25-26):
```
0x0020:  0500 1102 001a 0220 2006 5359 5354 454d   ..........SYSTEM
0x0030:  2706 5359 5354 454d 0678 7878 7878 7800   '.SYSTEM.xxxxxx.
              ^^ object 0x11 = 17 (FAL)
         ...  20 06 "SYSTEM"        = access-control RQSTRID / account  (tag 0x20, len 6)
         ...  27 06 "SYSTEM" 06 "xxxxxx"  = username "SYSTEM" + PASSWORD "xxxxxx"  (tag 0x27)
```
Contrast rd vms-558 CTERM (object 0x2a=42): there the connect carried only a proxy source id and the
access-control credential fields were EMPTY, so LOGINOUT prompted a fresh Username/Password. **FAL is
different: the `NODE"user password"::` access string goes ON THE WIRE (cleartext) and the FAL server
validates it at connect time.** (Proxy access — an empty access-control connect matched against a
`NETNODE_REMOTE`/proxy DB — is the other real VMS path; not exercised in this capture, which used an
explicit access string.)

**OVMX consequence (vms-8c2):**
- The FAL server (object 17) MUST decode the access-control username+password from the conn-initiate
  and authenticate them through the ONE faithful authenticator (the SYSUAF/Purdy path LOGINOUT uses —
  the same executive auth, NOT a second credential check), refusing the connect (NSP disconnect with a
  reject reason) on bad credentials. It MUST NOT run a session with no auth, and MUST NOT trust the
  carried username without the password check.
- The COPY **client** MUST place the access-control username+password from the `NODE"user pw"::`
  filespec into the conn-initiate access-control fields (tags 0x20/0x27), exactly here — it does not
  prompt interactively for a file copy.
- Security note (INV-6 / never-crash-a-peer): these fields are attacker-controlled on an inbound
  connect; the FAL server's decoder must be fully bounded (tag/len checked, never over-read), like the
  CTERM decoder (vms-f40).

---

## 2. The NETACP → object-17 (FAL) → RMS → DAP sequence

Observed end to end (`vax-copy-fal-dap.wire.txt`), whole COPY completed in ~0.33 s:

1. **Connect.** VAX1 → VAX2 NSP `conn-initiate 8195>0 ver 4.1 segsize 1459`, **object 17 (FAL)**,
   carrying the access-control username+password (§1).
2. **Accept.** VAX2's NETACP accepts → `conn-confirm 8195>8195`; NSP flow-control (`link-service` /
   `ils-ack`) opens the link. FAL is now the session's image on VAX2.
3. **DAP negotiation + transfer** over `data 8195>8195` segments both ways (payloads in
   `vax-copy-fal-dap.hex.txt`). Directly observed content in the DAP messages:
   - the target **file name** `OVMXDAP_R.TXT;` then, from VAX2, the resolved full spec
     `SYS$SYSROOT:[SYSMGR]OVMXDAP_R.TXT;1` and the owner **UIC `[000001,000004]`** — the DAP
     CONFIGURATION/ATTRIBUTES/NAME exchange (file attributes negotiated before data);
   - the **record data** verbatim — `Hello from VAX1 node 1.1 - DAP/FAL oracle capture line one` … —
     carried in DAP DATA message(s) (`hex.txt`, the `1.1 > 1.2 ... seg 5` segment);
   - a closing status/access-complete exchange.
   The DAP message layer rides the NSP logical link as the byte transport (not a raw device), exactly
   as CTERM's terminal protocol did.
4. **Teardown.** `disconn-initiate 8195>8195` → `disconn-confirm ... disconnect complete`. The COPY
   returns silently to the `$` prompt (VMS COPY is silent on success); the file is present on VAX2.

**OVMX mapping (vms-8c2, on the P5 base already on main):** NETACP (vms-9ab, merged) accepts the
object-17 connect and dispatches it to the FAL server image; FAL authenticates the access-control
creds (§1), then speaks DAP over the NSP link (dnet_nsp/dnet_link on main) to an RMS file on the ODS-2
volume via the executive ACP. The `COPY NODE::` client places the access string on the connect and
drives the DAP client side. The DAP codec is coded against the captured bytes here + the public DAP
specification (clean-room), NOT invented.

---

## 3. DAP message-layer decoding (raw bytes are ground truth; field decode references the public DAP spec)

`vax-copy-fal-dap.hex.txt` carries every logical-link packet's payload. Each NSP `data` segment's
payload (after the NSP data header) is one or more DAP messages; each DAP message begins with an
OPERATOR (type) byte + FLAGS, then typed fields. The message TYPES and field semantics are the public
DEC **DAP (Data Access Protocol) specification** (Rule 8 — public spec, not VSI disassembly): the
CONFIGURATION, ATTRIBUTES, ACCESS, CONTROL, ACKNOWLEDGE, DATA, STATUS, ACCESS-COMPLETE, NAME message
set. The byte-level field decode of each captured message (the exact tag/length framing) is the FAL
implementer's task against this hex + the spec — this oracle fixes the *ground truth bytes* and the
*credential/object/sequence semantics* the implementation is forbidden to invent; it does not
hand-transcribe every DAP sub-field. Directly-confirmed anchors for that decode: object 17; the
cleartext access-control creds (§1); the filename/full-spec/UIC attributes and the verbatim record
data (§2) are all locatable in the hex by their ASCII.

---

## 4. Provenance / method

- Lab: `kubectl -n ovmx-lab exec vaxlab-1` driving the two SIMH VAX consoles via the nodedrv FIFO
  (`/lab/k8s-labs/vaxlab-1/logs/vax{1,2}.log[.in]`, base64 to survive `$`/`"`); `tcpdump -i br0
  'ether proto 0x6003'` for the wire. DECnet was already up (this is the same pod that produced the
  vms-558 CTERM oracle; both nodes 1.1/1.2 circuit-up).
- A real `OVMXDAP.TXT` was CREATEd on VAX1, COPYed to VAX2 with an explicit `1.2"SYSTEM xxxxxx"::`
  access string, and confirmed present on VAX2 by `DIR` — a genuine successful FAL/DAP transfer, not a
  probe.
- INV-6: everything above is observed, not inferred; the pcap + hex + console files are the raw
  evidence. Where a statement is spec-derived rather than directly observed (the DAP per-field
  framing), §3 says so explicitly.
