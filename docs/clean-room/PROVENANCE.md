# OVMX VMScluster Interop — Clean-Room Provenance & Attestation

**Purpose.** OVMX implements the VMScluster wire protocols (SCS / NISCA / MSCP / DLM)
so that an OVMX (Linux) node can join a real OpenVMS VMScluster. This document is the
auditable record that this work is **clean-room reverse engineering for
interoperability** — derived **only** from (a) observed network behavior of a
legitimately-run OpenVMS reference system and (b) public documentation — and that **no
VSI / HPE / DEC binary was ever disassembled or decompiled and no VMS source code was
ever consulted or copied.**

Interoperability reverse engineering of this kind is legally protected — in the U.S.
under **DMCA § 1201(f)** (reverse engineering to achieve interoperability of an
independently created program) and in the EU under **Software Directive (2009/24/EC)
Article 6** (decompilation for interoperability). That protection depends on the work
actually being clean: this file, and the retained evidence it indexes, exist so the
cleanliness can be **proven**, not merely asserted.

This governs the whole project per **CLAUDE.md Project-Specific Rule 8 (Clean-room VMS
RE — HARD INVARIANT)**.

---

## 1. Sources used (the ONLY permitted inputs)

1. **Wire captures of a legitimately-run OpenVMS VAX 7.3 reference lab.** Real OpenVMS
   VAX nodes running under the **SIMH** emulator, bridged over a host Linux bridge, and
   observed **passively** with `tcpdump`. What is captured is the **externally-visible
   network interface** — the Ethernet frames the nodes emit on ethertype `0x6007`. This
   is *behavioral observation of a running system's public interface*, not reverse
   engineering of its binaries. The frames on the wire ARE the interface being
   interoperated with; the OpenVMS executive image is never inspected.
   - Reference captures inventory + SHA-256: [`reference-captures.sha256`](reference-captures.sha256)
   - Lab description: `~/vax/cluster/README-lab.md`, `~/vax/clean-cluster/FORMATION-NOTES.md`
2. **Public OpenVMS documentation.** VSI/HP/DEC *published* manuals and headers:
   the *VMScluster Systems for OpenVMS* manual, the *Internals and Data Structures
   Manual (IDSM)* terminology, published `$SSDEF` / `$LCKDEF` status/lock codes, the
   *Linker Utility Manual*, and the porting guides. Public reference only.
3. **Publicly documented industry protocols.** MSCP (Mass Storage Control Protocol),
   SCS, and NISCA are described in **public** DEC/VSI literature; their opcodes and
   message structure are used from that public knowledge, cross-checked against the wire.

## 2. Inputs that were NEVER used (the hard prohibition)

- **No disassembly or decompilation** of any VSI / HPE / DEC binary (no `SYS.EXE`, no
  `PEdriver`, no `CLUSTER*.EXE`, no executive image, no `.EXE`/`.STB` inspection).
- **No VMS source code** — not licensed source, not leaked source, not "reference"
  source. None was obtained, opened, or copied.
- **No copying of VSI code or data structures** verbatim. Where public docs do NOT
  publish a byte-level layout, OVMX **defines its own representation** and labels it an
  **OVMX design choice** — explicitly *not* presented as VMS-authentic (e.g. OVMX's
  opaque Con.ID values, which the peer only echoes; see the in-source comments and
  `docs/design-link-native-toolchain.md`).

## 3. The derivation model (wire → fact → code, every step traceable)

Every reverse-engineered protocol value is traceable through an unbroken chain:

```
  packet capture (source)  ->  decoder script  ->  documented fact  ->  code
  e.g. af2-firsttimer-        e.g. af2choreo.py    e.g. docs/design-    e.g. scsd.c/scs_dir.c
  established .pcap           / mscp_ground.py     cluster-join-*.md    with in-line citation
                                                                        "af2 143.7586 op=1"
```

- **Decoder scripts** (methodology, retained): [`tools/`](tools/) holds the exact Python
  used to decode frames from the captures — `pcap.py` (the raw pcap reader),
  `af2*.py` (the SCS/dir/MSCP choreography decoders), `mscp_*.py` (the MSCP command
  decoders). They read *only* the pcap bytes; anyone can re-run them against the
  captures and reproduce every claimed value.
- **In-source citations.** Reversed values in the code carry a comment naming the
  capture frame + timestamp they were observed at (e.g. `GROUNDED from the pcap`,
  `af2 143.759`, `clean-ref idx35`). The code is not "known" — it is *observed*.
- **Design records.** `docs/design-cluster-join-choreography.md`,
  `docs/cluster-protocol-spec.md`, and `docs/design-cluster-node.md` hold the
  byte-level derivations with their capture provenance.

## 4. Retained evidence (chain of custody)

The full evidence set is archived durably and hashed for tamper-evidence. The SHA-256
manifests are committed **here in git**, so the hashes are timestamped and immutable in
the repository history — any later alteration of an artifact is detectable.

| Evidence | What it proves | Location | Manifest |
|----------|----------------|----------|----------|
| Reference wire captures (31) | The legitimate observational source | `~/vax/{cluster,clean-cluster}/captures/` | [`reference-captures.sha256`](reference-captures.sha256) |
| Derivation scripts (52) | *How* every fact was extracted from the wire | archive `derivation-scripts/` + repo [`tools/`](tools/) | [`archive-manifest.sha256`](archive-manifest.sha256) |
| OVMX test captures (27) | What OVMX itself emitted + the lab's responses | archive `ovmx-test-captures/` | [`archive-manifest.sha256`](archive-manifest.sha256) |
| Session transcripts (17) | The full reasoning/derivation trail, step by step | archive `session-transcripts/` | [`archive-manifest.sha256`](archive-manifest.sha256) |
| SCSD run logs | OVMX-side record of frames emitted per run | archive `scsd-logs/` | [`archive-manifest.sha256`](archive-manifest.sha256) |
| Git commit history | Timestamped, ordered record of the implementation | this repo | commit hashes |

**Durable archive root:** `~/vax/clean-room-archive/2026-07-30-session-f0b8efb2/`
(37 MB; 129 files manifested).

The **session transcripts** are the strongest methodology evidence: they record, in
order, every capture decoded, every byte read, and the reasoning from observation to
implementation — showing the derivation path was wire-and-docs, never a binary.

## 5. Reproducibility

Any auditor can independently verify the chain:
1. Confirm artifact integrity: `sha256sum -c reference-captures.sha256` (from `~/vax/`)
   and `sha256sum -c archive-manifest.sha256` (from the archive root).
2. Re-run a decoder against a capture, e.g.
   `python3 docs/clean-room/tools/af2choreo.py ~/vax/cluster/captures/af2-firsttimer-established-20260728.pcap`
   and confirm the values match the design docs and the in-source citations.
3. Cross-check the citations in `src/vmsscs/*.c` against the frames they name.

## 6. Ongoing retention (standing procedure)

Clean-room evidence is retained continuously, not reconstructed after the fact:
- Wire captures are kept in the lab captures directories and never pruned.
- At the end of each RE session, run [`retain.sh`](retain.sh) to snapshot that session's
  transcripts, derivation scripts, and test captures into the dated archive and refresh
  the SHA-256 manifest.
- The manifests in this directory are re-committed whenever the evidence set grows, so
  git carries the running, timestamped chain of custody.

---

*Maintained under CLAUDE.md Rule 8. If any artifact named here is ever found to derive
from a VSI/HPE binary or from VMS source, the affected work must be quarantined and
re-derived clean, and this attestation corrected.*
