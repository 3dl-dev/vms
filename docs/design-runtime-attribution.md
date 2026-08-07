# Per-assertion runtime attribution

Tracked on **vms-38c**, **vms-d33**, **vms-2b2**. This document is the handoff
for the next adversarial round: what was built, what was measured (with the
commands), and what is **not** closed.

**Nothing here closes an item.** Three items independently named this
instrument as their direction; this is a first working version of the
instrument plus its measured controls, not the fix to any of the three.

---

## 1. The problem, as the three items left it

All three converged, over several measured rounds each, on the same sentence:

> the only instrument that can tell a real dependency from a compiled-but-dead
> or ignored one is EXECUTION.

and on the same trap:

> a wrapper that merely records "was called" is bought by one more ignored call
> inside the assertion. Coverage must be attributed to what the assertion
> ASSERTS, not to what it touches.

The sharpest recorded buy is vms-38c's, re-measured on main at run-6:

| edit | what it is |
|---|---|
| 1 | flip `sys$wflor`'s `OVMX-PARTIAL` + `OVMX-LOCAL` block to `OVMX-EXECUTIVE proof=tests/qemu/test_syssvc_ef_mproc.c` |
| 2 | one `(void)sys$wflor(0u, 0u);` in that proof — **placed after `return`**, statically unreachable dead code |

Two edits, register `rc=0`, EXECUTIVE 7→8. Check 4 ("the proof CALLS the
service") is satisfied by code the compiler is free to delete.

---

## 2. The mechanism: the defect is the probe

The instrument is deliberately **not** a call tracer, a coverage counter or a
"was reached" marker — every one of those is bought by an ignored call.

> An assertion goes RED when a function is mutated **iff** the value that
> assertion checks depends on that function's behaviour.

That is not an approximation of "asserts on it". It *is* "asserts on it",
measured. An ignored call cannot redden an assertion; nor can dead code, a
comment, a re-worded assertion text, a manifest field, a declaration, an added
file, or a `CMakeLists` line. Every buy measured across the vms-d89 / vms-ecf /
vms-e2b / vms-c13 rounds bought a **static property**, and is therefore inert
against this instrument.

The attribution is a join of two halves. **Neither half is a hand-written
field**, and that is the whole point — `targets` in `facility_defects.sh` is
hand-written, and both check 5 and check 6 read it.

### The SITE half — re-derived every run, never committed

`facility_attribution.sh sites` runs the **real** `facility_defects.sh apply`
against a throwaway copy of the tree, diffs the result against the pristine
copy, and reports the **enclosing C function** of every line the `sed`
actually changed.

To move a site you must move a `sed` anchor; an anchor that does not land is
already a `BROKEN FIXTURE` (`cmd_apply`). A fabricated site is not reachable by
editing a field.

### The RED half — `tests/qemu/facility_negctl_observed.tsv`

Which assertion actually went red, in QEMU, against a real `/dev/vms`, emitted
by `run_facility_negctl.sh` and floored by that driver's row-for-row comparison
of the committed record against what it just observed.

### The join

```
ATTR <suite> <assertion> <file> <function> <defect>
```

> "mutating `<function>` was OBSERVED to change the verdict of `<assertion>` in
> `<suite>`, in QEMU, against a real /dev/vms."

---

## 3. What was built

| file | what |
|---|---|
| `tests/qemu/facility_attribution.sh` | the instrument. `sites` / `attribute` / `depends` / `functions` / `handlers` / `caveats` / `selftest` |
| `tests/qemu/facility_attribution_negctl.sh` | controls A–G below; **constructs the recorded 2-edit buy** and runs both gates against it |
| `tests/qemu/CMakeLists.txt` | both registered as ctests, label `harness`, no container needed |
| `tests/integration/test_userspace_service_register.sh` | keeps the **handler name** in the answer-path derivation (it was being collapsed to a filename), and prints a MEASURED / UNMEASURED column per claim |

### The one-line change that mattered most in the register

The four-hop derivation already knew which *function* answers each ioctl —
`hs[hi]` — and threw it away, keeping only `deffile[hs[hi]]`. That collapse is
why **one mutation of `kernel/vms_eflag.c` pays for all seven event-flag
services at once**. The handler name is now emitted to `$WORK/answerfn` beside
the file list.

---

## 4. What was measured

All commands run from the repo root on the branch.

### 4.1 The buy still works; the measured instrument refuses it

```
$ sh tests/qemu/facility_attribution_negctl.sh
--- A. the recorded 2-edit buy STILL WORKS against the register ---
  ok: edit 1 alone reds with 'EXECUTIVE DECLARATION WHOSE PROOF NEVER CALLS THE SERVICE' (as recorded)
  ok: THE BUY LANDS: the register exits 0 with sys$wflor now a full exemption (11 claim(s))
        |       sys$wflor              tests/qemu/test_syssvc_ef_mproc.c
        |           paid by eflag-clref-noop                   (edits kernel/vms_eflag.c )
        |           paid by eflag-waitfr-eintr-normal          (edits kernel/vms_eflag.c )

--- B. the MEASURED instrument refuses the same buy ---
      answer path (handler granularity): vms_ioctl_wflor
  ok: NO measured dependence. The bought OVMX-EXECUTIVE claim is UNPAID by execution,
        where the register's check 4 accepted it from dead code.

--- C. the measured instrument is NOT vacuous ---
      MEASURED-PAID  : sys$clref sys$waitfr
      UNMEASURED     : sys$setef sys$readef sys$ascefc
  ok: the instrument pays real claims: sys$clref sys$waitfr

--- D. an IGNORED CALL ON A LIVE PATH changes zero attribution rows ---
      attribution rows: pristine=254  with-ignored-call=254
  ok: byte-identical: an executed-but-ignored call bought exactly nothing

--- E. DEAD CODE AFTER `return` changes zero attribution rows ---
      attribution rows: pristine=254  with-dead-code-and-flipped-declaration=254
  ok: byte-identical

--- G. the OFFLINE site derivation equals what the REAL container injection does ---
      host: 43 site row(s)   container: 43 site row(s)
  ok: IDENTICAL, all 43 rows: the host derivation IS the container injection,
        not a model of it

 Attribution negative controls: 9 passed, 0 failed
```

**C is the load-bearing control.** B passes trivially for a check that refuses
everything; C is what makes B a discrimination rather than a blanket no.

**G matters more than it looks.** The site half is only execution-sourced if
the `apply` run on the host is the `apply` `inject_and_run.sh` runs in the
container. That was an argument until it was measured; it is now a diff of 43
rows produced in both places.

### 4.2 End-to-end against real hardware, one defect

```
$ CONTAINER_ENGINE=docker sh tests/qemu/run_facility_negctl.sh eflag-clref-noop
  ok: pristine image: all 27 suites rc=0, and ZERO failing assertions, against a real /dev/vms
  inject:   injected 'eflag-clref-noop' into /src/repo/src/kernel/vms_eflag.c
  ok: vms.ko still loaded and /dev/vms present
  ok: the red set is EXACTLY the 5 assertion(s) the manifest names (observed 5)
  PASS: 'eflag-clref-noop' turns the harness red
 Facility negative controls: 3 passed, 0 failed
```

Chained with the site derivation, this is the full claim executed once:
injection lands at `kernel/vms_eflag.c:240`, inside `vms_ioctl_clref()`; QEMU
boots with it and exactly five assertions change verdict; those five assertions
are attributed to `vms_ioctl_clref`.

### 4.3 The selftest has teeth

The scanner was broken deliberately (`enclosing_functions` forced to return
`(file-scope)`) in a scratch copy:

```
$ sh tests/qemu/facility_attribution.sh selftest      # scanner broken
  FAIL: fewer than half the sites resolved to a function (0/43)
  FAIL: eflag-clref-noop resolved to file scope
  ok:   no assertion is attributed to vms_ioctl_wflor        <-- passes VACUOUSLY
  FAIL: zero attribution rows; 'depends' would refuse everything
  FAIL: the fabricated row produced NO rows at all, so this check judged nothing
SELFTEST FAILED
```

Note check 3 passing vacuously under a broken scanner. That is exactly why
checks 4 and 5 exist, and both caught it.

Check 6 exists because of an **untested branch found by looking, not by it
failing**: `cmd_attribute` admits only RED rows whose defect has a RUN row with
verdict `pass`, and all 42 RUN rows in the committed record are `pass` — so that
filter never fires in normal use. Check 6 flips one verdict in a scratch copy:

```
--- 6. a defect the driver REJECTED contributes no attribution ---
    eflag-clref-noop attribution rows: verdict=pass -> 5, verdict=fail -> 0
  ok: all 5 row(s) withdrawn when the driver's verdict is 'fail'
```

### 4.4 vms-2b2, answered by execution instead of by line overlap

```
$ sh tests/qemu/facility_attribution.sh handlers
      9 MEASURED
     24 UNPROBED
      0 PROBED
```

The 24 `UNPROBED` handlers are **the identical set** vms-2b2 lists, arrived at
by a different route (real injection + enclosing function, rather than
line-hunk overlap). Two consequences, and the second is the more useful:

1. vms-2b2's cardinality is corroborated by a second, behavioural instrument.
2. vms-2b2's own "softener 2" — that the line-level measure understates,
   because a mutation of shared code reddens handlers without the hunk sitting
   literally inside them — **is not borne out here**, and `PROBED = 0` says so.
   See residual R2: that is a limit of *this* instrument, not a refutation of
   the softener.

### 4.5 No regressions

```
$ sh tests/integration/test_userspace_service_register.sh        rc=0  (10 claims annotated)
$ sh tests/integration/test_userspace_service_register_negctl.sh rc=0  (64 passed, 0 failed)
$ sh tests/qemu/facility_defects.sh selftest .                   rc=0
$ sh tests/qemu/facility_record_negctl.sh .                      rc=0  (25 passed, 0 failed)
$ sh tests/qemu/facility_attribution.sh selftest                 rc=0  (6 checks)
$ sh tests/qemu/facility_attribution_negctl.sh                   rc=0  (9 passed, 0 failed)
```

The register's own 64 negative controls are the load-bearing regression check
here: every one of them runs the whole gate on a sandbox tree, so the
MEASURED/UNMEASURED annotation is exercised 64 times against deliberately
broken trees, not just once against a healthy one.

### 4.6 What it costs

The register runs once per sandbox in its own negative-control suite, so the
annotation's cost is paid ~25 times there. Measured on this host by hiding
`facility_attribution.sh` and re-running:

```
register WITHOUT annotation:  9.28 s
register WITH    annotation: 10.83 s      (+1.55 s)
facility_attribution.sh sites: 1.27 s     (42 real injections + diffs)
```

`FA_ATTR_CACHE` is what makes that +1.55 s rather than +15 s: without it the
register re-ran all 42 injections once per OVMX-EXECUTIVE claim. Correctness
does not depend on the cache — an absent or empty one is recomputed — but it is
the difference between a gate that runs in seconds and one that runs in
minutes.

---

## 5. What the register now prints

```
      sys$clref              tests/qemu/test_syssvc_ef_mproc.c
          paid by eflag-clref-noop                   (edits kernel/vms_eflag.c )
          MEASURED: assertion(s) in test_syssvc_ef_mproc.c were observed to change verdict when
                    vms_ioctl_clref() was mutated -- this claim is paid by EXECUTION
      sys$ascefc             tests/qemu/test_syssvc_ef_mproc.c
          paid by eflag-clref-noop                   (edits kernel/vms_eflag.c )
          UNMEASURED: no observed red is attributed to any of [vms_ioctl_ascefc].
                      NOT a refutation -- no defect probes those handlers (rd vms-2b2).
                      The static join above is the only thing paying this claim.
```

**2 of the 10 standing OVMX-EXECUTIVE claims are paid by execution**
(`sys$clref`, `sys$waitfr`). Eight are UNMEASURED.

---

## 6. Reported, NOT enforced — and the reason is a number

The measured check is **not** wired into the register's exit code. That is a
disclosed decision, not timidity:

- enforcing it today reds a **pristine** tree for 8 of 10 claims;
- those 8 are UNMEASURED because **only 9 of 33 `vms_ioctl_*` handlers have any
  defect landing in them at all** (vms-2b2);
- so enforcement would report a **coverage gap as a lie**. UNMEASURED means
  *nothing probed it*, never *the claim is false* — the same asymmetry check 6
  carries, and it must not be collapsed by a reader.

**The path to enforcement is therefore not "turn this on".** It is: write a
defect for each unprobed handler (vms-2b2), watch the MEASURED column fill in,
and flip the check when it reaches the standing claims. That ordering is
vms-2b2 blocking vms-38c, which is not how the two are currently wired.

---

## 7. Residuals — what is NOT closed

**R1 — the instrument is ASYMMETRIC, and this is inherent.**
It can prove an assertion depends on a function. It can never prove it does
not. A function nobody wrote a defect for produces no `ATTR` row because
nothing probed it. Every consumer must print that distinction; collapsing it is
how "9 of 33 handlers" gets misread as "24 untested handlers", which vms-2b2
warns against in terms.

**R2 — attribution does NOT propagate along the kernel call graph.**
A defect mutating a shared helper (`try_grant_waiters`, `devinfo_fill`,
`proc_fill_info`, `vms_proc_may_read`, `vms_proc_find_or_err`) attributes to
that helper and to no handler. Propagating would be a *static inference*
re-entering a measured claim, and `vms_proc_find_or_err` is called by nearly
every handler — propagation would attribute half the tree to one defect. The
honest fix is a second, clearly-labelled `MEASURED-VIA` column, not silent
propagation. **This is why vms-2b2's real question — "which wired handlers have
NO mutation that changes what they RETURN" — is still open.** `PROBED = 0` is a
property of direct-site attribution, not a finding about the executive.

**R3 — the RED half is still a committed snapshot.**
A hand-written `RED` row reads like an observed one to this file. It inherits
the driver's row-for-row floor and establishes nothing new. What control 5 of
the selftest *does* add: a fabricated `RED` row still needs a `SITE` row for
the same defect, so **the record can lie about which assertion, never about
which function**.

**R4 — `require_fail` vs `knock_on_fail` is not carried into the record.**
`RED` rows do not distinguish "this assertion checks the mutated behaviour"
from "this assertion broke because a shared prerequisite broke". So neither
does the attribution. A mutation breaking a shared prerequisite produces `ATTR`
rows a careful reader would not call a dependency of what the assertion
asserts. Closing this means adding the class to the record, which needs a full
42-defect driver run to regenerate.

**R5 — the instrument is exactly as fine as the 42 defects.**
It is a probe set, not a coverage tool.

**R6 — only ONE defect was executed in QEMU on this branch.**
`eflag-clref-noop`. The committed record's other 41 rows are inherited from the
last full CI run, unchanged by this work. The full 42-defect driver run was not
performed here.

**R7 — vms-d33's census is untouched.**
The census still asks "could this be called from a root". The instrument built
here is the one vms-d33 names, and `functions` already reports which
`vms_kif_*` entry points have measured dependence (`vms_kif_setmode`,
`kif_bind`) — but nothing was wired into `test_kif_caller_census.sh`. That
wiring has the same "would red pristine" problem as the register, for the same
reason, and needs the same vms-2b2 groundwork first.

**2026-08-07 addendum, disclosed not claimed-closed:** the sentence above is
still true of THIS instrument — the census does not consult
`facility_attribution.sh` and the "reachable-but-never-executed" gap it names
is unchanged, still gated on vms-2b2. Separately, vms-d33 closed one narrower,
purely-static loophole in the census's OWN root rule: rule 2 (`test_kif_
caller_census.sh` section 2') granted a root to "every product function
prototyped in a header the build compiles" without checking that the
definition actually carried external linkage, so a `static` function whose
declaration AND body both lived in a compiled header (e.g.
`src/vmsdcl/include/dcl/dcl_cmd.h`) fell through the reader's (origin-file,
name) statics tagging — which only fires for an origin that is itself one of
the compiled `.c` translation units — and landed on the same bare-name node an
actually-exported symbol gets. That bought a root for two edits with rc=0 at
44/32/12 (negative control 48 in `test_kif_caller_census_negctl.sh` pins it).
Fixed by tracking header-resident `static` definitions independent of the
existing per-TU tagging and excluding them from rule 2. This is a linkage
question the census can answer statically; it is not, and does not claim to
be, an answer to R7's actual question, which remains: EXECUTION is still the
only way to tell a reachable-but-dead call from a live one, and that still
needs the vms-2b2 probe set before this instrument's MEASURED column can be
enforced anywhere, including here.

**R9 — a multi-handler answer path is an OR, and that is proof-shoppable.**
The measured check pays a claim when **any** handler in the derived answer path
has a measured dependence in the cited proof. `sys$enq` and `sys$enqw` both
derive `[vms_ioctl_convert, vms_ioctl_enq, vms_ioctl_setef]` — the transitive
call-graph closure reaches `VMS_IOCTL_SETEF`, which is not what answers a lock
request. Had any defect probed `vms_ioctl_setef` and reddened an assertion in
`test_syssvc_lock_status.c`, both lock claims would print MEASURED on evidence
about the event-flag handler. Nothing on this tree currently exploits that
(`vms_ioctl_setef` is UNPROBED), so it is a hole in the shape rather than a
measured buy — but it is the *cheapest* place to attack the measured check once
vms-2b2's probes exist, and it is the same over-reach the register's header
already records for the userspace remainder. The fix is presumably to require
the dependence on the handler for the ioctl the service *itself* issues, not
anything in its closure; that was not attempted here.

**R8 — the enclosing-function scanner is a heuristic.**
Brace-depth over comment/string-stripped C. 37 of 43 sites resolve to a named
function; the other 6 are genuine file-scope data (`lock-compat-*` tables,
`kstat-*` header macros, `executive-not-pinned`). Its failure direction is safe
— a mis-parse yields `(file-scope)`, which matches no handler and can only
**withhold** attribution, never grant it — but "safe direction" is an argument
about the parser, not a measurement of it, and it is only pinned by selftest
checks 1 and 2.

---

## 8. For the adversary picking this up

The cheapest attacks worth measuring first:

1. **Buy an `ATTR` row without changing behaviour.** The instrument's claim is
   that this requires making a real assertion's verdict really depend on the
   function. Is there a shape that produces a red without that?
2. **R2 is the widest hole.** A service whose handler is a thin wrapper over a
   probed helper is UNMEASURED under direct attribution while being genuinely
   covered. Conversely, a `MEASURED-VIA` column would be buyable by adding a
   call to a probed helper.
3. **R4.** Can a knock-on red be arranged to attribute a function to an
   assertion that does not check it?
4. **Control G is the load-bearing one for the site half.** If the container
   and host injections could be made to differ, the site half stops being
   execution-sourced.
5. **The register's report is not a gate.** Nothing yet fails on UNMEASURED, so
   nothing here raises the price of the 2-edit buy in CI. It raises the price
   of the buy *going unnoticed by a reader*, which is a different and weaker
   claim.
