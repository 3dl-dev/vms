# Message-ident authenticity audit (vms-916)

**Item:** vms-916 — "real VMS message idents, no invented ones" (authenticity Tier-0).
**Requires:** operator sign-off (D3) to close.
**Method (clean-room Rule 8):** every ident is marked `VERIFIED` (grounded to public
VSI/HP OpenVMS documentation, cited) or `OVMX-design` (labelled as OVMX's own, under
facility `OVMX`, because OpenVMS has no authentic equivalent for the exact condition).
No ident is presented as VMS-authentic without a citation.

The clearly-invented idents lived in the DCL queue/submit command handlers
(`src/vmsdcl/dcl_cmd_process.c`, `src/vmsdcl/dcl_cmd_file.c`) as inline
`dcl_error(facility, sev, ident, text)` string literals — NOT in the
`known_codes[]` / `msg_db[]` catalogs, so `sys$getmsg`/`sys$putmsg` never emitted them.

---

## 1. Replacement table — invented → real VMS (VERIFIED)

| Site(s) | Invented (before) | Real VMS (after) | Provenance |
|---|---|---|---|
| SUBMIT / PRINT / SHOW QUEUE / SHOW ENTRY / SET ENTRY / SET QUEUE / DELETE-ENTRY queue-manager-unavailable path | `%<CMD>-E-QMANERR, queue manager initialization failed` | `%JBC-E-JOBQUEDIS, system job queue manager is not running` | JBC facility. VSI/HP OpenVMS *System Messages and Recovery Procedures Reference Manual* (JBC); corroborated by the queue-manager troubleshooting entries in the *OpenVMS System Manager's Manual* (Ch. "Managing the Queue Manager"), and field transcripts on comp.os.vms / HPE OpenVMS forum showing `%JBC-E-JOBQUEDIS, system job queue manager is not running`. This is the message a real system returns when a queue operation is attempted and the queue manager is not running — the exact user-visible condition OVMX's `ensure_queue_init()` failure stands for. |
| SUBMIT failure (`vmsq_submit` → `SS$_ITEMNOTFOUND`) | `%SUBMIT-E-SUBMITERR, failed to submit job to queue <q>` | `%JBC-E-NOSUCHQUE, no such queue - <q>` | JBC facility. Dominant failure of `vmsq_submit` is "target queue does not exist" (`find_queue_slot() < 0`). `%JBC-E-NOSUCHQUE, no such queue` is documented in the *OpenVMS System Manager's Manual* queue chapter and DCL Dictionary examples. |
| PRINT failure (same path) | `%PRINT-E-PRINTERR, failed to queue file to <q>` | `%JBC-E-NOSUCHQUE, no such queue - <q>` | As above. |
| SHOW QUEUE / SET QUEUE nonexistent-queue check | `%SHOW-E-NOSUCHQUE` / `%SET-E-NOSUCHQUE, no such queue - <q>` | facility corrected → `%JBC-E-NOSUCHQUE, no such queue - <q>` | The ident/text were already real; only the FACILITY was wrong. On VMS this condition is raised by the queue service and rendered under JBC, not the DCL command verb. |
| SET ENTRY / SHOW ENTRY entry-not-found | `%<CMD>-E-ENTNOTFND, entry <n> not found` | `%JBC-E-NOSUCHENT, no such entry` | JBC facility. See DELETE/ENTRY citation below. Emitted as a standalone primary line here; on VMS `JBC$_NOSUCHENT` renders identically (`F$MESSAGE`), the `%`-vs-`-` prefix being a primary/continuation positional detail, not part of the ident text. |
| DELETE/ENTRY entry-not-found | `%DELETE-E-ENTNOTFND, entry <n> not found` | two-line faithful chain: `%DELETE-W-SEARCHFAIL, error searching for <n>` + `-JBC-E-NOSUCHENT, no such entry` | Verbatim from the VSI/HP *OpenVMS DCL Dictionary*, DELETE/ENTRY entry (digiater.nl / HPE `9996pro_53.html`; VSI OpenVMS wiki DELETE/ENTRY): a DELETE/ENTRY of a completed/absent entry prints `%DELETE-W-SEARCHFAIL, error searching for 203` chained with `-JBC-E-NOSUCHENT, no such entry`. Severity of SEARCHFAIL is `W`. |
| DELETE/ENTRY & SET ENTRY & SET QUEUE missing required value | `%DCL-E-NOENTRY, missing entry number...` / `%SET-E-NOQUNAM, missing queue name` | `%DCL-W-INSFPRM, missing command parameters - supply all required parameters` | Real DCL parser message for an absent required parameter. VSI/HP *OpenVMS System Messages* (DCL facility): `%DCL-W-INSFPRM, missing command parameters - supply all required parameters`. Severity `W`. (On a real terminal DCL would prompt for the value; OVMX does not reach the CLD prompt on these paths, so it emits the batch-mode message.) |

**Facility note:** `JBC` (Job Controller) is the OpenVMS facility that owns queue/batch/print
condition values (`$JBCMSGDEF`). Routing these through `JBC` — rather than the DCL command
verb — is what a real system does: the verb's handler calls `$SNDJBC`/`$GETQUI`, which return
JBC condition values, and the handler `PUTMSG`s them.

## 2. OVMX-design idents (LABELLED — never presented as VMS)

These conditions have no VMS-authentic ident for the exact OVMX code path, so they are
emitted under facility `OVMX` (OVMX's customer-facility convention — see
`src/libvms/include/ovmx_status.h` and `sys_msg.c` `facility_name()`), so no reader can
mistake them for a SYSTEM/JBC condition.

| Site | Ident (after) | Why OVMX-design |
|---|---|---|
| SET ENTRY / SHOW ENTRY / DELETE-ENTRY, non-numeric entry value | `%OVMX-E-IVENTNUM, invalid entry number - <s>` | On real VMS a non-numeric `/ENTRY=` value is rejected by the CLD command parser (a `%CLI-`/`%DCL-` parse-time diagnostic), a path OVMX's hand-rolled handler does not reproduce. Rather than borrow a real ident whose exact wording/facility for this case is not documented, the OVMX validation error is labelled as OVMX's own. |
| SET QUEUE /STOP,/START,/PAUSE state-change write fault | `%OVMX-E-QUESETERR, failed to <op> queue <q>` | Reached only after the queue is confirmed to exist, i.e. an internal queue-database write/lock fault with no VMS-authentic analogue (VMS's queue state is held by the JOB_CONTROL process, not a flat file). Labelled OVMX-design. |

## 3. Success-message idents — FLAGGED for a follow-up (out of scope here)

The queue commands' SUCCESS lines use `%<CMD>-S-<ident>` idents that diverge from real
VMS, which prints plain, unprefixed text (e.g. `Job MYJOB (queue SYS$BATCH, entry 4)
started on SYS$BATCH` — VSI/HP OpenVMS DCL Dictionary, SUBMIT). Affected:
`%SUBMIT-S-SUBMITTED`, `%PRINT-S-QUEUED`, `%SET-S-MODIFIED`, `%SET-S-QUEMOD`,
`%DELETE-S-DELETED`.

These were **not changed in this PR** because (a) the item's Tier-0 target and anchors are
the ERROR idents, and (b) two currently-green gated tests key on this success FORMAT
(`tests/dcl/test_queue.sh`, `tests/dcl/test_print_submit_coverage.sh`), so changing the
success-line shape is a coordinated UX change that must update those tests in the same
commit. **Recommendation:** file a follow-up item to render queue success lines in the
plain VMS form and migrate the two tests together. This is a divergence in success-line
FORMAT, not a wrong error ident — a lower-severity, separable concern.

## 4. Broader catalog — already provenance-complete (no change needed)

- `src/libvms/status.c` `known_codes[]` (read by `sys$getmsg`/`sys$putmsg`): every row is
  a real SS$_/RMS$_ condition; several carry `ORACLE-PINNED` provenance comments
  (`vms-6a7`, `vms-68c`, `vms-9fc`, `vms-2a8`, DUPLNAM, ILLIOFUNC, …). The only non-VMS
  rows are the four `OVMX$_` conditions, already carrying the customer-defined bit and
  rendered under facility `OVMX`.
- `src/vmsdcl/dcl_messages.c` `msg_db[]`: DCL/COPY/DELETE/DIRECT/RMS/SYSTEM/SET/… idents
  matching documented VMS message idents.
No invented ident presented as VMS-authentic remains in these catalogs.

## 5. Declaration repointing & citation apparatus (task item #3)

- **`sys_msg.c` OVMX-USERSPACE declarations already cite the live owner `vms-916`**
  (`sys$getmsg (vms-916)`, `sys$putmsg (vms-916)`) — not the closed `vms-5b4`. The
  `rd vms-5b4` text in the file header is the shared *register-header* line present in
  every `src/libvms/syssvc/sys_*.c` and `src/vmsrms/rms_*.c` file; it names the item that
  BUILT the register/gate, not a per-service citation. No repointing was required.
- **`tracking/rd-citations.tsv` + `tools/gen_rd_citations.py` no longer exist** — the
  citation-ledger apparatus was torn down by operator ruling `vms-dc7` (2026-08-06; see
  the note in top-level `CMakeLists.txt` where `rd_citations_fresh`/`gen_rd_citations.py`
  are recorded as removed). There is nothing to regenerate; recreating it would revert an
  operator ruling.

## 6. Test evidence

`tests/dcl/test_queue_messages.sh` (added) drives the three real failure paths and asserts
the REAL VMS text is emitted and the invented idents are GONE:
- queue manager unavailable → `%JBC-E-JOBQUEDIS`
- SUBMIT/PRINT to a nonexistent queue → `%JBC-E-NOSUCHQUE`
- DELETE/ENTRY of a nonexistent entry → `%DELETE-W-SEARCHFAIL` + `-JBC-E-NOSUCHENT`
- `EXPECT_NOT` guards for `QMANERR`, `SUBMITERR`, `PRINTERR`, `ENTNOTFND`.

## Sources (public OpenVMS documentation)

- VSI OpenVMS *System Messages and Recovery Procedures Reference Manual* (M–Z), docs.vmssoftware.com.
- VSI/HP OpenVMS *DCL Dictionary* — DELETE/ENTRY and SUBMIT entries (digiater.nl mirror `9996pro_53.html`; VSI OpenVMS wiki DELETE/ENTRY).
- *OpenVMS System Manager's Manual* — Managing the Queue Manager (sysworks/mi.infn.it mirrors).
- Field transcripts confirming `%JBC-E-JOBQUEDIS, system job queue manager is not running` (HPE OpenVMS community; comp.os.vms).
