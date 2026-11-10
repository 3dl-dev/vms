# Oracle: OpenVMS Alpha V8.4 SYSGEN PQL_DPGFLQUOTA / PQL_MPGFLQUOTA

**rd vms-c746. Clean-room (Rule 8): observed behaviour of a real OpenVMS Alpha system, the
ground truth for the values OVMX's SYSGEN table and executive use for a `$CREPRC` subprocess's
default and minimum page-file quota (rd vms-89dd8).**

- **Captured:** 2026-11-03, lab pod `alphalab-0`, node `alpha1` (AlphaServer ES40 console
  V7.3-1 on AXPbox), stock **OpenVMS Alpha V8.4** from the lab golden system disk.
  Conversational boot (`boot -fl 0,1 dqa0`), `SYSBOOT> SHOW` of each parameter, then
  `CONTINUE`; the version banner that follows confirms the release.
- **Artifact:** `alpha84-sysboot-pql.console.txt` — the console transcript, verbatim
  (NULs and CRs stripped, the copyright byte re-encoded as UTF-8).

## Values

| Parameter | Current | Default | Min. | Max. | Unit | Dynamic |
|---|---|---|---|---|---|---|
| `PQL_DPGFLQUOTA` | 131072 | 131072 | 4096 | -1 (none) | pagelets | yes |
| `PQL_MPGFLQUOTA` | 4096 | 4096 | 4096 | -1 (none) | pagelets | yes |

The internal values are 8192 / 256 pages: an 8 KB Alpha page is 16 pagelets of 512 bytes.

`Current` equals `Default` on this disk, so the shipped default and this system's setting
agree. A `$CREPRC` quota list's PGFLQUOTA below `PQL_MPGFLQUOTA` is raised to it; an absent
PGFLQUOTA takes `PQL_DPGFLQUOTA` (OpenVMS System Services Reference, `$CREPRC`, quota list).
