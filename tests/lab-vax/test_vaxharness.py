#!/usr/bin/env python3
"""test_vaxharness.py - self-test for vaxharness.py (rd vms-cf5).

Proves the fragility class documented in vaxharness.py's module docstring is
DEAD: a fake pexpect child that raises pexpect.TIMEOUT, one that raises
pexpect.EOF, one whose `.before` is None (or raises on access), and one that
raises the exact anita AttributeError bug -- none of them can make
`safe_expect()` raise, and `.before` on the result is always a `str`. Also
proves the negctl-contract inversion table, in both the Python and bash
mirrors, and that `Proof`/`StepResult` reduce a run to one honest JSON line.

Run: pytest tests/lab-vax/test_vaxharness.py -v
     (or: python3 -m pytest tests/lab-vax/test_vaxharness.py -v)
No SIMH, no anita, no container needed -- only `pexpect` (for its TIMEOUT/EOF
exception classes) and `pytest`.
"""

import json
import os
import subprocess
import sys

import pexpect
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import vaxharness as vh


# --------------------------------------------------------------------------
# Fake pexpect children -- deliberately NOT real pexpect, so these tests need
# no SIMH/anita/tty at all.
# --------------------------------------------------------------------------
class _RaisingChild:
    """A fake child whose .expect() always raises a given exception."""

    def __init__(self, exc, before=None):
        self._exc = exc
        self.before = before
        self.after = None

    def expect(self, patterns, **kwargs):
        raise self._exc


class _BeforeRaisesChild:
    """A fake child that matches successfully but whose `.before` ATTRIBUTE
    ACCESS ITSELF raises (not just holds None) -- the harder case."""

    def __init__(self):
        self.after = "tail"

    def expect(self, patterns, **kwargs):
        return 0

    @property
    def before(self):
        raise RuntimeError("simulated broken .before property")


class _MatchingChild:
    """A fake child that matches pattern index `index` and returns
    (before, after) verbatim."""

    def __init__(self, index=0, before="console output", after="MATCHED"):
        self._index = index
        self.before = before
        self.after = after

    def expect(self, patterns, **kwargs):
        return self._index


class _AnitaBugChild:
    """Simulates anita's own broken expect() wrapper: even though
    pexpect.TIMEOUT/EOF are never in our pattern list (safe_expect never adds
    them), this stands in for the case where SOMETHING still raises the exact
    AttributeError anita's `self.match.group(0)` post-hook produces, proving
    safe_expect() catches it as an ordinary exception rather than propagating
    it."""

    def __init__(self):
        self.before = "partial output before the bug fired"
        self.after = None

    def expect(self, patterns, **kwargs):
        raise AttributeError("'TIMEOUT' object has no attribute 'group'")


# --------------------------------------------------------------------------
# Bug #1 + #2: safe_expect() must never raise, and .before must never be None
# --------------------------------------------------------------------------
class TestSafeExpectNeverCrashes:
    def test_timeout_is_caught_not_raised(self):
        child = _RaisingChild(pexpect.TIMEOUT("simulated timeout"), before=None)
        result = vh.safe_expect(child, [r"never matches"], timeout=5)
        assert result.kind == vh.ExpectKind.TIMEOUT
        assert result.ok is False
        assert result.index is None

    def test_eof_is_caught_not_raised(self):
        child = _RaisingChild(pexpect.EOF("simulated eof"), before=None)
        result = vh.safe_expect(child, [r"never matches"], timeout=5)
        assert result.kind == vh.ExpectKind.EOF
        assert result.ok is False
        assert result.index is None

    def test_before_is_none_after_timeout_is_coerced_to_str(self):
        """The exact class #2 bug: .before is None after a failed expect();
        a bare `child.before or ""` looked safe but a genuinely angry mock
        (or anita's own broken sentinel handling) could still surface
        something that crashes .count()/.strip(). safe_expect's result must
        carry a real str no matter what."""
        child = _RaisingChild(pexpect.TIMEOUT("t"), before=None)
        result = vh.safe_expect(child, [r"x"], timeout=1)
        assert isinstance(result.before, str)
        assert result.before == ""
        # and it must be usable exactly like the crashing code tried to use it
        assert result.before.count("x") == 0
        assert result.before.strip() == ""

    def test_before_attribute_access_itself_raising_is_swallowed(self):
        """Harder than .before being None: the ATTRIBUTE ACCESS raises. Still
        must never propagate, and .before on the result is still a str."""
        child = _BeforeRaisesChild()
        result = vh.safe_expect(child, [r"anything"], timeout=5)
        assert result.kind == vh.ExpectKind.MATCH
        assert result.index == 0
        assert isinstance(result.before, str)
        assert result.before == ""  # best-effort: access failed, so ""

    def test_anita_attributeerror_bug_is_caught_not_raised(self):
        """Even if something upstream still raises the EXACT anita
        AttributeError this module exists to design around, safe_expect must
        not propagate it -- it becomes an honest ERROR result instead of a
        crash."""
        child = _AnitaBugChild()
        result = vh.safe_expect(child, [r"whatever"], timeout=5)
        assert result.kind == vh.ExpectKind.ERROR
        assert result.ok is False
        assert "AttributeError" in result.error
        assert isinstance(result.before, str)
        assert result.before == "partial output before the bug fired"

    def test_arbitrary_other_exception_is_caught_not_raised(self):
        child = _RaisingChild(ValueError("something else entirely"), before="bef")
        result = vh.safe_expect(child, [r"x"], timeout=5)
        assert result.kind == vh.ExpectKind.ERROR
        assert "ValueError" in result.error
        assert result.before == "bef"

    def test_successful_match_reports_index_and_text(self):
        child = _MatchingChild(index=2, before="stuff before", after="THE MARKER")
        result = vh.safe_expect(child, [r"a", r"b", r"c"], timeout=5)
        assert result.kind == vh.ExpectKind.MATCH
        assert result.ok is True
        assert result.index == 2
        assert result.before == "stuff before"
        assert result.after == "THE MARKER"

    def test_bytes_before_is_decoded_to_str(self):
        child = _MatchingChild(index=0, before=b"raw \xff bytes", after=b"AFTER")
        result = vh.safe_expect(child, [r"x"], timeout=5)
        assert isinstance(result.before, str)
        assert isinstance(result.after, str)
        assert "raw" in result.before

    def test_empty_pattern_list_raises_valueerror_not_silently_ignored(self):
        # A programming error in the CALLER (forgot to pass patterns) must
        # surface loudly -- this is not part of the timeout/EOF/None class.
        with pytest.raises(ValueError):
            vh.safe_expect(_MatchingChild(), [], timeout=5)

    def test_never_raises_across_the_whole_matrix(self):
        """Belt-and-suspenders: sweep every fake child class through
        safe_expect() and assert NONE of them ever raise out of the call."""
        children = [
            _RaisingChild(pexpect.TIMEOUT("t")),
            _RaisingChild(pexpect.EOF("e")),
            _RaisingChild(RuntimeError("r")),
            _BeforeRaisesChild(),
            _AnitaBugChild(),
            _MatchingChild(),
        ]
        for child in children:
            try:
                result = vh.safe_expect(child, [r"pat"], timeout=1)
            except Exception as e:  # pragma: no cover - the failure this proves impossible
                pytest.fail("safe_expect() raised for %r: %r" % (child, e))
            assert isinstance(result, vh.SafeExpectResult)
            assert isinstance(result.before, str)


# --------------------------------------------------------------------------
# Proof / StepResult: the target-adapter-style structured result contract
# --------------------------------------------------------------------------
class TestProofStructuredResult:
    def test_step_from_expect_match_is_ok(self):
        proof = vh.Proof("demo")
        child = _MatchingChild(index=0)
        result = vh.safe_expect(child, [r"MILESTONE"], timeout=5)
        step = proof.step_from_expect("milestone", result, expected_index=0)
        assert step.ok is True
        assert proof.ok is True

    def test_step_from_expect_timeout_is_not_ok_with_honest_detail(self):
        proof = vh.Proof("demo")
        child = _RaisingChild(pexpect.TIMEOUT("t"))
        result = vh.safe_expect(child, [r"MILESTONE"], timeout=5)
        step = proof.step_from_expect("milestone", result, expected_index=0)
        assert step.ok is False
        assert "timeout" in step.detail.lower()
        assert proof.ok is False

    def test_step_from_expect_eof_is_not_ok(self):
        proof = vh.Proof("demo")
        child = _RaisingChild(pexpect.EOF("e"))
        result = vh.safe_expect(child, [r"MILESTONE"], timeout=5)
        step = proof.step_from_expect("milestone", result, expected_index=0)
        assert step.ok is False
        assert "eof" in step.detail.lower() or "exited" in step.detail.lower()

    def test_step_from_expect_wrong_index_is_not_ok(self):
        proof = vh.Proof("demo")
        child = _MatchingChild(index=1)  # matched a DIFFERENT pattern
        result = vh.safe_expect(child, [r"WANT", r"OTHER"], timeout=5)
        step = proof.step_from_expect("milestone", result, expected_index=0)
        assert step.ok is False

    def test_proof_with_no_steps_is_not_ok(self):
        proof = vh.Proof("empty")
        assert proof.ok is False
        assert proof.exit_code() == vh.PROOF_FAILED

    def test_proof_all_steps_ok_yields_zero_exit(self):
        proof = vh.Proof("demo")
        proof.record("a", True)
        proof.record("b", True)
        assert proof.ok is True
        assert proof.exit_code() == 0

    def test_proof_one_bad_step_yields_nonzero_exit(self):
        proof = vh.Proof("demo")
        proof.record("a", True)
        proof.record("b", False, detail="facility absent")
        assert proof.ok is False
        assert proof.exit_code() == vh.PROOF_FAILED

    def test_exit_code_never_returns_anything_but_zero_or_proof_failed(self):
        """rd vms-cf5's retrofit ask: Proof.exit_code() must be able to
        return EXACTLY one of two values -- never a driver-invented ad hoc
        nonzero code."""
        ok = vh.Proof("demo"); ok.record("a", True)
        bad = vh.Proof("demo"); bad.record("a", False)
        assert ok.exit_code() in (0, vh.PROOF_FAILED)
        assert ok.exit_code() == 0
        assert bad.exit_code() in (0, vh.PROOF_FAILED)
        assert bad.exit_code() == vh.PROOF_FAILED
        assert vh.PROOF_FAILED != 0
        assert vh.PROOF_FAILED != vh.HARNESS_ERROR

    def test_emit_result_line_is_exactly_one_parseable_json_line(self):
        import io

        proof = vh.Proof("demo")
        proof.record("a", True, marker_seen="0")
        proof.record("b", False, detail="nope")
        buf = io.StringIO()
        line = proof.emit_result_line(stream=buf)
        printed = buf.getvalue()
        # exactly one line was written, and it round-trips as the same JSON
        # emit_result_line() returned
        assert printed.strip().count("\n") == 0
        parsed = json.loads(printed.strip())
        assert parsed == json.loads(line)
        assert parsed["proof"] == "demo"
        assert parsed["ok"] is False
        assert len(parsed["steps"]) == 2
        assert parsed["steps"][0]["step"] == "a"
        assert parsed["steps"][1]["ok"] is False


# --------------------------------------------------------------------------
# Bug #3: the negctl inversion contract, Python side
# --------------------------------------------------------------------------
class TestNegctlGatePython:
    @pytest.mark.parametrize(
        "driver_exit_code,negctl,expected",
        [
            (0, False, True),   # positive mode, driver passed  -> gate satisfied
            (1, False, False),  # positive mode, driver failed  -> gate NOT satisfied
            (7, False, False),  # positive mode, any nonzero    -> gate NOT satisfied
            (0, True, False),   # negctl mode, driver PASSED (no teeth) -> gate FAILS
            (1, True, True),    # negctl mode, driver failed (has teeth) -> gate satisfied
            (7, True, True),    # negctl mode, any nonzero              -> gate satisfied
        ],
    )
    def test_negctl_gate_table(self, driver_exit_code, negctl, expected):
        assert vh.negctl_gate(driver_exit_code, negctl) is expected

    def test_negctl_wrapper_exit_code_mirrors_the_gate(self):
        assert vh.negctl_wrapper_exit_code(0, False) == 0
        assert vh.negctl_wrapper_exit_code(1, False) == 1
        assert vh.negctl_wrapper_exit_code(0, True) == 1  # no teeth -> wrapper FAILS
        assert vh.negctl_wrapper_exit_code(1, True) == 0  # teeth confirmed -> wrapper PASSES

    def test_the_exact_regression_this_item_documents(self):
        """rd vms-cf5's own example: run-eflag.sh's negctl-load mode expects
        NONZERO from the driver to mean 'negctl satisfied'; a driver variant
        that exited 0 logging 'negctl ok' must be read by the gate as FAILURE
        (no teeth), not success."""
        driver_exit_code_from_buggy_variant = 0  # "negctl ok" -- WRONG per contract
        assert vh.negctl_gate(driver_exit_code_from_buggy_variant, negctl=True) is False

    # --------------------------------------------------------------------------
    # rd vms-cf5's retrofit tail: the PROOF_FAILED/HARNESS_ERROR carve-out.
    # --------------------------------------------------------------------------
    def test_proof_failed_code_inverts_under_negctl(self):
        assert vh.negctl_gate(vh.PROOF_FAILED, negctl=True) is True

    def test_proof_failed_code_is_not_satisfied_in_positive_mode(self):
        assert vh.negctl_gate(vh.PROOF_FAILED, negctl=False) is False

    def test_harness_error_code_stays_red_under_negctl(self):
        """The one behavior change this retrofit makes: a genuine
        harness-level break must NEVER look like 'negctl teeth confirmed',
        even though it is a nonzero exit code -- the old 'any nonzero
        inverts' rule would have wrongly satisfied this."""
        assert vh.negctl_gate(vh.HARNESS_ERROR, negctl=True) is False

    def test_harness_error_code_stays_red_in_positive_mode_too(self):
        assert vh.negctl_gate(vh.HARNESS_ERROR, negctl=False) is False

    def test_harness_error_never_satisfies_the_wrapper_either(self):
        assert vh.negctl_wrapper_exit_code(vh.HARNESS_ERROR, True) == 1
        assert vh.negctl_wrapper_exit_code(vh.HARNESS_ERROR, False) == 1

    def test_legacy_nonzero_codes_still_invert_unchanged(self):
        """A not-yet-migrated call site returning some other ad hoc nonzero
        value keeps behaving exactly as it always did -- this retrofit is
        additive, not a wholesale semantics change."""
        assert vh.negctl_gate(60, negctl=True) is True
        assert vh.negctl_gate(16, negctl=True) is True


# --------------------------------------------------------------------------
# Bug #3, bash mirror: negctl_gate.sh must implement the IDENTICAL table
# --------------------------------------------------------------------------
NEGCTL_GATE_SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "negctl_gate.sh")


def _bash_negctl_gate(driver_exit_code, negctl_flag):
    """Run negctl_gate.sh's vaxharness_negctl_gate() in bash and return
    whether IT considers the gate satisfied (its own $? -- 0 means satisfied,
    matching the Python function's True)."""
    script = (
        "set -e; source %s; "
        "vaxharness_negctl_gate %s %s"
        % (
            NEGCTL_GATE_SH,
            driver_exit_code,
            "1" if negctl_flag else "0",
        )
    )
    proc = subprocess.run(["bash", "-c", script])
    return proc.returncode == 0


@pytest.mark.skipif(
    subprocess.run(["bash", "-c", "true"], check=False).returncode != 0,
    reason="bash not available",
)
class TestNegctlGateBashMirror:
    @pytest.mark.parametrize(
        "driver_exit_code,negctl,expected",
        [
            (0, False, True),
            (1, False, False),
            (7, False, False),
            (0, True, False),
            (1, True, True),
            (7, True, True),
        ],
    )
    def test_bash_mirror_matches_python_table(self, driver_exit_code, negctl, expected):
        assert _bash_negctl_gate(driver_exit_code, negctl) is expected
        # and it must agree with the Python implementation for every case,
        # not just the ones enumerated in this table -- these two "cannot
        # disagree" per the module contract.
        assert _bash_negctl_gate(driver_exit_code, negctl) is vh.negctl_gate(driver_exit_code, negctl)

    @pytest.mark.parametrize(
        "driver_exit_code,negctl,expected",
        [
            (vh.PROOF_FAILED, True, True),
            (vh.PROOF_FAILED, False, False),
            (vh.HARNESS_ERROR, True, False),
            (vh.HARNESS_ERROR, False, False),
        ],
    )
    def test_bash_mirror_matches_the_new_carve_out(self, driver_exit_code, negctl, expected):
        assert _bash_negctl_gate(driver_exit_code, negctl) is expected
        assert _bash_negctl_gate(driver_exit_code, negctl) is vh.negctl_gate(driver_exit_code, negctl)


# --------------------------------------------------------------------------
# Bug #4: the shared guest-session run helper.
# --------------------------------------------------------------------------
class TestRunCaptured:
    def test_returns_the_guest_rc_and_text_without_raising(self):
        """A nonzero guest rc (a clean proof-fail) must come back as an
        ordinary (rc, text) tuple -- run_captured() itself must never raise
        or abort just because the underlying command failed."""
        calls = []

        def fake_run_fn(cmd, timeout):
            calls.append((cmd, timeout))
            return 7, "some captured output\n"

        rc, out = vh.run_captured(fake_run_fn, "false", "/tmp/x.out", 30)
        assert rc == 7
        assert out == "some captured output\n"
        assert len(calls) == 1
        sent_cmd, sent_timeout = calls[0]
        assert sent_timeout == 30
        assert "/tmp/x.out" in sent_cmd
        assert "cat /tmp/x.out" in sent_cmd

    def test_default_cleanup_prefixes_rm(self):
        calls = []
        vh.run_captured(lambda cmd, timeout: calls.append(cmd) or (0, ""),
                         "echo hi", "/tmp/y.out", 10)
        assert calls[0].startswith("rm -f /tmp/y.out; ")

    def test_cleanup_false_appends_instead_of_truncating(self):
        calls = []
        vh.run_captured(lambda cmd, timeout: calls.append(cmd) or (0, ""),
                         "echo hi", "/tmp/y.out", 10, cleanup=False)
        assert "rm -f" not in calls[0]
        assert ">>/tmp/y.out" in calls[0]

    def test_the_guest_command_itself_is_embedded_unmodified(self):
        calls = []
        vh.run_captured(lambda cmd, timeout: calls.append(cmd) or (0, ""),
                         "vmsprobe selftest", "/tmp/z.out", 10)
        assert "vmsprobe selftest" in calls[0]

    def test_zero_rc_success_path_also_passes_through_cleanly(self):
        rc, out = vh.run_captured(
            lambda cmd, timeout: (0, "PASS\n"), "true", "/tmp/ok.out", 5)
        assert rc == 0
        assert out == "PASS\n"


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
