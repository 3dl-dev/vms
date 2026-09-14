# CodeQL `cpp/cleartext-transmission` on the DECnet FAL password — by design

**Status:** decision record for Baron (security/confidentiality posture + CI-gate
override are Baron-reserved). Staged, NOT merged. rd vms-ea8 / PR #1230.

## The finding

CodeQL alert (default code-scanning setup) on
`src/vmsdecnet/engine/decnetd.c` — the `--password-fd` read in `run_copy_loop`
whose value is carried to the remote FAL in the object-17 Session Control
CONNECT (`copy_client_run` → `dnet_cterm_sc_connect_build` → the datalink send):

> `cpp/cleartext-transmission` — Cleartext transmission of sensitive information.

## Why it is by design (protocol-faithful true positive, not a new exposure)

- **DECnet Phase IV FAL access control puts the credentials in the connect.** The
  oracle (`docs/oracle/vax-copy-fal-dap.*`, §1) shows a real VAX FAL connect
  carrying the username **and password** — the OPPOSITE of CTERM, whose connect
  creds are empty and which authenticates fresh. To authenticate to a remote FAL
  at all, the password must be in the connect.
- **The protocol has no transport encryption.** DECnet Phase IV predates it. OVMX
  is a **clean-room faithful** reproduction (Rule 8); it cannot encrypt what the
  wire protocol defines as cleartext without ceasing to interoperate.
- **Not a new exposure.** The already-shipped **inbound** FAL server
  (`decnet$fal`, proven by `DECNETD.EXE --fal-accept-test` in the acceptance
  battery) has the identical property — it decodes the same connect-carried
  password. This PR's outbound client is the same posture, not a new one.
- **OVMX's orthogonal hardening is present and real:** the password is handed to
  the client over an **inherited pipe fd, never argv** (so it is not
  world-readable in `/proc/<pid>/cmdline`), is length-bounded, and is **wiped**
  immediately after the connect is built (`run_copy_loop`, and the DCL side in
  `dcl_cmd_file.c copy_dnet_activate`).

The finding is therefore a **true positive about the protocol** that is
**won't-fix by design** — the same call any faithful DECnet FAL client makes.

## Mechanisms to clear the gate (Baron's choice)

The inline `// codeql[cpp/cleartext-transmission]` comment already committed at the
source line is **inert under the repo's DEFAULT code-scanning setup** (default
setup ignores inline suppression comments). The two mechanisms that actually work:

### (1) API / UI dismiss-as-by-design — out-of-band, works today
```
gh api -X PATCH repos/3dl-dev/vms/code-scanning/alerts/172 \
  -f state=dismissed -f dismissed_reason="won't fix" \
  -f dismissed_comment="DECnet Phase IV FAL carries creds in the connect (oracle §1); no transport encryption; same posture as the shipped inbound FAL server; password never on argv, bounded, wiped after use. rd vms-ea8."
```
Reversible; not greppable in-repo (lives in the code-scanning UI/audit log). The
harness flags this as a **CI-bypass**, so it is Baron's action (it was blocked for
the agent, correctly).

### (2) In-repo, auditable — requires switching to ADVANCED code-scanning setup
There is **no narrow committed config that scopes to a single alert**: CodeQL
`query-filters` exclude a query **repo-wide**, which would silence *all*
`cpp/cleartext-transmission` findings everywhere and weaken scanning for a real
bug class — **not recommended** (a posture weakening, not a targeted suppression).

The genuinely narrow, in-repo, greppable mechanism is the **line-scoped inline
suppression already committed** at the FAL password read — but it only takes
effect once the repo is switched from **default** to **advanced** code-scanning
setup (an advanced `.github/workflows/codeql.yml` honors inline suppressions).
That is a repo-wide CI-infrastructure change (and a GitHub repo-setting flip from
default→advanced) — larger than the one FP warrants unless advanced setup is
wanted for other reasons. Not staged blind here to avoid shipping unverified CI.

## Recommendation

For a single protocol-faithful FP, **mechanism (1) dismiss-as-by-design is
proportionate**. Adopt mechanism (2) only if advanced code-scanning setup is
wanted repo-wide independently. Either way, everything else on PR #1230 is proven
green (Build & Test, Analyze(c-cpp), and the DCL/SHOW booted-acceptance leg where
`--copy-accept-test` transfers a file both directions byte-verified on a real
executive) — it reaps the instant the gate clears.
