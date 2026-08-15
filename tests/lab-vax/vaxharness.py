"""vaxharness.py - shared crash-proof console/proof helper for the vax SIMH
lab (rd vms-cf5, parent vms-8e8).

WHY THIS EXISTS. The vax PORT code lands clean (event flags passed first try,
LNM was one real bug); the SIMH+anita TEST HARNESS is the time-sink -- it
re-bites the SAME fragility class once per rung, and three separate agents
have each hand-rolled a fix for a variant of it inside drive_boot_vax.py /
drive_eflag_vax.py (see e.g. drive_boot_vax.py's `_console_text()` and its
scattered `except (pexpect.TIMEOUT, pexpect.EOF, Exception)` call sites). This
module kills the class ONCE, mined from ~/projects/pcjs-vax's Target Adapter
Protocol (docs/reference/target-adapter-protocol.md, tools/ehkaa-gate/gate.py):
that gate never scrapes a console -- it runs one adapter command and parses
ONE line of JSON from stdout, target-agnostic. This module adapts that shape
to a SIMH/pexpect console: a driver's pass/fail decision becomes DATA (a
Proof of StepResults, emitted as one JSON line), not scattered
`if seen.get(...)` checks, and the fragile pexpect boundary is behind ONE
crash-proof call instead of N ad hoc try/excepts.

THE THREE BUGS THIS KILLS (all observed on origin/main in tests/lab-vax/):

  1. ANITA'S expect() CRASHES ON ITS OWN TIMEOUT/EOF SENTINEL. anita vendors a
     pexpect subclass whose expect() wrapper unconditionally calls
     `self.match.group(0)` after every call -- including when the match IS the
     pexpect.TIMEOUT or pexpect.EOF sentinel object, which has no `.group()`
     (`AttributeError: 'TIMEOUT' object has no attribute 'group'`). The ONLY
     way to avoid this from caller code is to never put TIMEOUT/EOF in the
     pattern list passed to expect() -- pexpect then raises them as
     EXCEPTIONS instead of returning them as matches, and anita's buggy
     post-match hook never runs. `safe_expect()` below enforces this
     unconditionally: it never appends a sentinel to the caller's patterns,
     and it catches pexpect.TIMEOUT / pexpect.EOF / anything else as
     exceptions, never as matches.

  2. `child.before` CAN BE None AFTER A FAILED expect(). Code that then calls
     `.count()` / `.strip()` / string methods on it crashes
     (`AttributeError: 'NoneType' object has no attribute ...`). Every path
     through `safe_expect()` reads `.before` through `_safe_text()`, which
     NEVER returns anything but a `str` (`""` on None, on a decode error, or
     on the attribute access itself raising).

  3. NEGATIVE-CONTROL EXIT-CODE CONTRADICTIONS. `run-eflag.sh`'s negctl-load
     mode expects the driver session to exit NONZERO for "negative control
     satisfied" (see run-eflag.sh's `if run_session prove skip; then die
     "NO TEETH"; fi`); a since-fixed driver variant instead exited 0 and
     logged "negctl ok" -- the driver and its wrapper silently disagreed
     about what "negctl satisfied" means. `negctl_gate()` below is the ONE
     inversion rule, used the same way by any Python caller and mirrored
     byte-for-byte in `negctl_gate.sh` for bash wrappers, so a driver's raw
     exit code and a wrapper's interpretation of it can never diverge again.

This is OVMX's own code, written against the public pexpect API; it copies no
third-party source (CLAUDE.md Rule 8). It has NO SIMH/anita import at module
scope -- `pexpect` is imported lazily inside `safe_expect()` so this module
(and its test suite) can be imported and unit-tested with a fake `child`
object and no pexpect/anita/SIMH installed at all.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field, asdict
from typing import Any, List, Optional, Sequence


# --------------------------------------------------------------------------
# Bug #2: child.before (and .after) can be None, bytes, or something an
# angry mock throws on. This is the ONE place that gets normalized.
# --------------------------------------------------------------------------
def _safe_text(value: Any) -> str:
    """Best-effort coercion of a pexpect `.before`/`.after` value to `str`.
    NEVER raises, NEVER returns anything but a `str`.

    Handles: None (-> ""), bytes (-> decoded, replacing undecodable bytes),
    str (passthrough), anything else (-> str(value), swallowing failures).
    """
    try:
        if value is None:
            return ""
        if isinstance(value, bytes):
            return value.decode("utf-8", "replace")
        if isinstance(value, str):
            return value
        return str(value)
    except Exception:
        return ""


def _safe_attr_text(obj: Any, name: str) -> str:
    """Read `obj.<name>` and normalize it via `_safe_text()`. If the
    ATTRIBUTE ACCESS ITSELF raises (an angry mock, a property that throws),
    that is swallowed too -- this function never raises."""
    try:
        raw = getattr(obj, name, None)
    except Exception:
        return ""
    return _safe_text(raw)


# --------------------------------------------------------------------------
# Bug #1: the crash-proof expect() wrapper.
# --------------------------------------------------------------------------
class ExpectKind:
    """The four outcomes `safe_expect()` can report. Never a fifth: any
    exception not otherwise recognized is folded into ERROR."""

    MATCH = "match"
    TIMEOUT = "timeout"
    EOF = "eof"
    ERROR = "error"


@dataclass
class SafeExpectResult:
    """The structured, always-safe result of one `safe_expect()` call.

    `before`/`after` are ALWAYS `str`, NEVER `None` -- regardless of outcome.
    `index` is the matched pattern's position in the CALLER'S pattern list
    (only meaningful when kind == ExpectKind.MATCH). `error` carries a human
    string for kind == ExpectKind.ERROR; None otherwise.
    """

    kind: str
    index: Optional[int]
    before: str
    after: str
    error: Optional[str] = None

    @property
    def ok(self) -> bool:
        """True iff a real pattern matched (kind == MATCH). TIMEOUT/EOF/ERROR
        are all "not ok" -- callers that need to distinguish them read `kind`."""
        return self.kind == ExpectKind.MATCH


def safe_expect(child: Any, patterns: Sequence[Any], timeout: Optional[int] = None) -> SafeExpectResult:
    """Crash-proof replacement for `child.expect(patterns, timeout=...)`.

    GUARANTEES (this is the entire point of the module):
      - `patterns` is passed to pexpect EXACTLY as given -- this function
        deliberately never appends pexpect.TIMEOUT/pexpect.EOF to it. Doing
        so would make a timeout/EOF a MATCH, and anita's expect() wrapper
        unconditionally calls `.group(0)` on whatever matched, which raises
        AttributeError for the TIMEOUT/EOF sentinel objects (bug #1's exact
        mechanism). By never adding the sentinels, an unmatched
        timeout/EOF surfaces as a pexpect.TIMEOUT/pexpect.EOF EXCEPTION
        instead, which this function catches directly -- anita's buggy
        post-match hook never runs.
      - This function NEVER raises. pexpect.TIMEOUT, pexpect.EOF, and any
        other exception (including a still-surfacing anita AttributeError,
        or a completely unrelated bug in a fake/mock child under test) are
        all caught and turned into a structured `SafeExpectResult`.
      - `.before`/`.after` on the returned result are ALWAYS `str` (bug #2).

    `patterns` must be non-empty (a single pattern is fine, but pass it as
    e.g. `[r"foo"]` -- always a sequence, matching pexpect's list-form
    `expect()` and this module's `index`-based result contract).
    """
    if not patterns:
        raise ValueError("safe_expect requires at least one pattern")

    import pexpect  # lazy: keeps this module importable with no pexpect installed

    kwargs = {}
    if timeout is not None:
        kwargs["timeout"] = timeout

    try:
        idx = child.expect(list(patterns), **kwargs)
        return SafeExpectResult(
            kind=ExpectKind.MATCH,
            index=idx,
            before=_safe_attr_text(child, "before"),
            after=_safe_attr_text(child, "after"),
        )
    except pexpect.TIMEOUT:
        return SafeExpectResult(
            kind=ExpectKind.TIMEOUT,
            index=None,
            before=_safe_attr_text(child, "before"),
            after="",
        )
    except pexpect.EOF:
        return SafeExpectResult(
            kind=ExpectKind.EOF,
            index=None,
            before=_safe_attr_text(child, "before"),
            after="",
        )
    except Exception as e:  # anita's own AttributeError bug, or anything else
        return SafeExpectResult(
            kind=ExpectKind.ERROR,
            index=None,
            before=_safe_attr_text(child, "before"),
            after="",
            error="%s: %s" % (type(e).__name__, e),
        )


# --------------------------------------------------------------------------
# The target-adapter-style result contract: one proof step -> one structured
# result. A driver's pass/fail decision is DATA (a list of these, reduced to
# one JSON line), never scattered `if seen.get(...)` checks.
# --------------------------------------------------------------------------
@dataclass
class StepResult:
    """One proof step's outcome -- the {step, ok, marker_seen, detail} shape
    rd vms-cf5 asks for. `marker_seen` is the matched pattern/marker text (or
    index) when relevant, else None."""

    step: str
    ok: bool
    marker_seen: Optional[str] = None
    detail: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


class Proof:
    """Accumulates a driver's StepResults and reduces them to ONE JSON line
    on stdout at the end (mirrors the Target Adapter Protocol's "exactly one
    JSON object on stdout" contract, doc §"Output contract") -- a wrapper
    script can key on that structured line instead of guessing from log
    text or trusting a bare exit code alone.
    """

    def __init__(self, name: str):
        self.name = name
        self.steps: List[StepResult] = []

    def record(self, step: str, ok: bool, marker_seen: Optional[str] = None, detail: str = "") -> StepResult:
        r = StepResult(step=step, ok=ok, marker_seen=marker_seen, detail=detail)
        self.steps.append(r)
        return r

    def step_from_expect(
        self,
        step: str,
        result: SafeExpectResult,
        expected_index: int = 0,
        detail: str = "",
    ) -> StepResult:
        """Convenience: turn one `safe_expect()` result directly into a
        StepResult. Matching `expected_index` is success; a TIMEOUT, EOF,
        ERROR, or a match at the WRONG index are all failure, each with an
        honest `detail` explaining which (never raises, never crashes --
        this is the whole point of routing every expect() through
        `safe_expect()` first).
        """
        if result.kind == ExpectKind.MATCH and result.index == expected_index:
            return self.record(step, True, marker_seen=str(expected_index), detail=detail)

        reasons = {
            ExpectKind.TIMEOUT: "timeout waiting for expected marker",
            ExpectKind.EOF: "child exited (EOF) before the expected marker appeared",
            ExpectKind.ERROR: "expect() error: %s" % (result.error or "unknown"),
            ExpectKind.MATCH: "matched pattern index %r, expected %r" % (result.index, expected_index),
        }
        reason = reasons[result.kind]
        full_detail = "%s -- %s" % (reason, detail) if detail else reason
        return self.record(step, False, marker_seen=None, detail=full_detail)

    @property
    def ok(self) -> bool:
        """True iff every recorded step was ok. A Proof with NO steps is
        deliberately NOT ok -- a driver that forgot to record anything must
        not look like a passing proof (mirrors gate.py's AdapterError-vs-FAIL
        distinction: "no gradeable result" must never look like PASS)."""
        return bool(self.steps) and all(s.ok for s in self.steps)

    def to_dict(self) -> dict:
        return {"proof": self.name, "ok": self.ok, "steps": [s.to_dict() for s in self.steps]}

    def emit_result_line(self, stream=None) -> str:
        """Print exactly ONE JSON line (the structured result contract) and
        return it. Human `log()` lines (the existing `[drive_x]` convention)
        may precede this on stdout; this is always the LAST line, so a
        wrapper can take the last stdout line the way gate.py's
        `_parse_adapter_json()` does, letting the two conventions compose."""
        stream = stream if stream is not None else sys.stdout
        line = json.dumps(self.to_dict())
        print(line, file=stream, flush=True)
        return line

    def exit_code(self) -> int:
        """The RAW process exit code a driver's main() should return: 0 iff
        every step held. This is deliberately NOT mode-aware (see
        `negctl_gate()` below) -- a driver never special-cases negctl into an
        early exit; it always reports its own honest proof-of-work result."""
        return 0 if self.ok else 1


# --------------------------------------------------------------------------
# Bug #3: the uniform negative-control contract.
# --------------------------------------------------------------------------
def negctl_gate(driver_exit_code: int, negctl: bool) -> bool:
    """The ONE inversion rule a run-*.sh wrapper applies to a driver's raw
    process exit code, so a driver and its wrapper can never again disagree
    about what "negative control satisfied" means (rd vms-cf5).

    CONTRACT:
      - A driver's own exit code is NEVER mode-aware. It always means:
        0 = every one of the proof's positive assertions held; nonzero = at
        least one failed -- REGARDLESS of whether the driver is running in
        negctl mode. A driver must NOT special-case negctl into an early
        "negctl ok, exit 0": that is exactly the bug this item documents
        (run-eflag.sh expected nonzero for "negctl satisfied" while a driver
        variant exited 0 and logged "negctl ok" -- the two sides silently
        disagreed about the contract).
      - It is the WRAPPER, applying THIS function to that raw exit code,
        that inverts the meaning for negctl mode:

          positive mode (negctl=False): gate satisfied <=> exit code == 0
          negctl mode   (negctl=True):  gate satisfied <=> exit code != 0

        In negctl mode the facility under test is deliberately absent, so
        the driver runs the SAME positive-assertion proof and it must fail
        FOR REAL (nonzero exit); a negctl run that exits 0 (the proof
        somehow passed despite the facility being missing) has NO TEETH and
        MUST be reported as gate FAILURE.

    See `negctl_gate.sh` in this directory for the byte-for-byte bash
    mirror of this same table, for use directly in a run-*.sh wrapper.
    """
    if negctl:
        return driver_exit_code != 0
    return driver_exit_code == 0


def negctl_wrapper_exit_code(driver_exit_code: int, negctl: bool) -> int:
    """Convenience: the process exit code a WRAPPER (not a driver) should
    itself exit with, given a driver's raw exit code and whether it ran in
    negctl mode. 0 = gate satisfied, 1 = gate failed. Equivalent to
    `0 if negctl_gate(...) else 1`, provided so a wrapper never has to
    re-derive that inversion either."""
    return 0 if negctl_gate(driver_exit_code, negctl) else 1
