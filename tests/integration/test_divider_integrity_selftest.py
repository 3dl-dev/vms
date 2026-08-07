#!/usr/bin/env python3
"""test_divider_integrity_selftest.py -- proves tools/check_divider_integrity.py
(rd vms-6d7) actually catches the corruption class it exists for, and does not
false-positive on the shapes it must leave alone.

BACKGROUND: vms-371's rebase (2e30da9, squash-merged as 2dcd237) silently
corrupted 3 comment-divider blocks in tools/cluster/scs_join_capability_measure.py
-- a '# ===...=' box-divider line lost its trailing newline and fused onto the
next comment line, and the equals-run itself was truncated (68 vs 75 chars).
It sat on main ~30 minutes: py_compile only checks syntax, and this repo's
wire/data tests validate logic via string search on a markdown spec file, not
comment formatting. THAT INSTANCE IS ALREADY FIXED (landed via vms-beb's
squash-merge, a21df8a / 155a820) -- this test is not about re-finding it, it
is about proving a mechanical detector would have.

WHAT THIS PROVES, not merely asserts:

  1. A SYNTHETIC fixture built to the bug's exact reported shape (68-char
     fused run immediately followed by the next comment's text, no
     intervening newline) is flagged as fused-divider.
  2. A SYNTHETIC fixture with a shorter-but-still-separate-line divider
     (truncated, not fused) is flagged as truncated-divider.
  3. Clean, well-formed divider blocks -- including ones this repo actually
     ships, and edge shapes (single/lone dividers, C block-comment '*/'
     closers, '//' dividers) -- produce ZERO findings. Without this, (1) and
     (2) would be satisfied by a detector that flags everything.
  4. The checker run over the REAL, already-fixed file
     (tools/cluster/scs_join_capability_measure.py) is clean -- confirming
     the fix that already landed is intact AND that the detector does not
     false-positive on this repo's actual divider style.
  5. The checker's own CLI (main()) round-trips: exit 0 on a clean temp file,
     exit 1 on a corrupted one, with the finding printed to stdout.

Runs on every host. No captures, no lab, no rd, no network.
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools"))

import check_divider_integrity as C  # noqa: E402


def divider(width, char="="):
    return char * width


class FusionDetection(unittest.TestCase):
    """Pass 1: the fused-line shape, built to the bug's own reported numbers."""

    def test_flags_the_exact_reported_shape(self):
        # 75-char divider, a title line, then a divider that lost its
        # trailing newline and fused onto the next comment's text, with the
        # run itself truncated to 68 chars -- the two numbers vms-6d7 reports.
        text = (
            "# " + divider(75) + "\n"
            "# vms-578: THE SECOND BRACKET -- does the INTEGRATED tree join?\n"
            "# " + divider(68) + "# vms-70e2's bracket above measured that "
            "work/vms-187-closure CANNOT complete a\n"
            "x = 1\n"
        )
        findings = C.check_text("synthetic.py", text)
        kinds = {f.kind for f in findings}
        self.assertIn("fused-divider", kinds,
                       "did not flag the fused divider: %r" % (findings,))
        fused = [f for f in findings if f.kind == "fused-divider"]
        self.assertEqual(fused[0].lineno, 3)

    def test_does_not_flag_a_clean_box_divider(self):
        text = (
            "# " + divider(75) + "\n"
            "# a normal title line\n"
            "# " + divider(75) + "\n"
        )
        self.assertEqual(C.check_text("clean.py", text), [])

    def test_does_not_flag_lone_lengthening_divider(self):
        # A single divider with nothing to compare it to in this file must
        # not be treated as evidence of anything -- it is the sole sample.
        text = "# " + divider(40) + "\n# only one divider here\n"
        self.assertEqual(C.check_text("lone.py", text), [])

    def test_does_not_flag_c_block_comment_closer(self):
        # '===*/' is a legitimate divider immediately followed by a comment
        # closer, not fused text -- must not be mistaken for corruption.
        text = (
            "/*\n"
            " * " + divider(70) + "\n"
            " * title\n"
            " * " + divider(70) + " */\n"
            " */\n"
        )
        findings = C.check_text("cstyle.c", text)
        self.assertEqual(findings, [], "false positive on */ closer: %r" % findings)

    def test_does_not_flag_slash_slash_divider(self):
        text = (
            "// " + divider(60) + "\n"
            "// C++-style divider\n"
            "// " + divider(60) + "\n"
        )
        self.assertEqual(C.check_text("cpp.cc", text), [])

    def test_does_not_flag_markdown_table_separator_row(self):
        # vms-e90: a comment-embedded markdown table's separator row
        # ("|------|------|...") is a legitimate divider shape, not a fused
        # divider run followed by prose -- every character after the run is
        # '|' or a divider char, never letters/words from a following line.
        text = (
            " *   header offset | size | field\n"
            " *   --------------|------|------------------------------------------------\n"
            " *   +0            | 4    | destination connection ID\n"
        )
        findings = C.check_text("table.h", text)
        self.assertEqual(findings, [], "false positive on table row: %r" % findings)

    def test_still_flags_fusion_immediately_after_a_table_style_run(self):
        # A table-separator-shaped run that is IMMEDIATELY followed by real
        # prose (not just '|' and divider chars) must still be caught --
        # the table-row exemption must not swallow genuine fusion.
        text = (
            " *   --------------|------| this text should not be here\n"
        )
        findings = C.check_text("fused_table.h", text)
        kinds = {f.kind for f in findings}
        self.assertIn("fused-divider", kinds, findings)


class TruncationDetection(unittest.TestCase):
    """Pass 2: a shorter divider on its OWN line, sharing a file with
    matching full-length siblings -- truncation without fusion."""

    def test_flags_truncated_divider_among_matching_siblings(self):
        text = (
            "# " + divider(75) + "\n"
            "# box one\n"
            "# " + divider(75) + "\n"
            "\n"
            "# " + divider(68) + "\n"          # truncated -- shorter than siblings
            "# box two\n"
            "# " + divider(75) + "\n"
        )
        findings = C.check_text("trunc.py", text)
        trunc = [f for f in findings if f.kind == "truncated-divider"]
        self.assertEqual(len(trunc), 1, findings)
        self.assertEqual(trunc[0].lineno, 5)

    def test_does_not_flag_a_longer_divider(self):
        # Widening is not this bug's corruption shape; only shortening is.
        text = (
            "# " + divider(75) + "\n"
            "# box one\n"
            "# " + divider(90) + "\n"          # longer, not shorter
            "# box two\n"
            "# " + divider(75) + "\n"
        )
        findings = C.check_text("wider.py", text)
        self.assertEqual([f for f in findings if f.kind == "truncated-divider"], [])

    def test_does_not_flag_uniform_dividers_of_any_common_width(self):
        for width in (3, 10, 75, 100):
            text = "# " + divider(width) + "\n# t\n# " + divider(width) + "\n"
            self.assertEqual(C.check_text("w%d.py" % width, text), [])


class RealRepoFile(unittest.TestCase):
    """The instance this item describes is already fixed on main -- confirm
    it stays that way, and that the detector does not choke on this repo's
    actual style."""

    def test_scs_join_capability_measure_is_clean(self):
        path = os.path.join(REPO, "tools", "cluster",
                             "scs_join_capability_measure.py")
        self.assertTrue(os.path.isfile(path), "fixture file moved/renamed: %s" % path)
        findings = C.check_file(path)
        self.assertEqual(findings, [],
                          "the already-fixed file is flagged again: %r" % findings)

    def test_repo_source_tree_has_no_flagged_dividers(self):
        # A light repo-wide sweep -- this IS the actual gate this item wires
        # into CI. Restricted to .py/.c/.h (the item's specified scope) under
        # src/, tools/, tests/.
        exts = (".py", ".c", ".h")
        roots = ("src", "tools", "tests")
        all_findings = []
        for root in roots:
            base = os.path.join(REPO, root)
            if not os.path.isdir(base):
                continue
            for dirpath, dirnames, filenames in os.walk(base):
                dirnames[:] = [d for d in dirnames if d not in (
                    ".git", "__pycache__",
                )]
                for fn in filenames:
                    if fn.endswith(exts):
                        all_findings.extend(
                            C.check_file(os.path.join(dirpath, fn)))
        self.assertEqual(all_findings, [],
                          "divider corruption found in tracked source: %r"
                          % (all_findings,))


class CliRoundTrip(unittest.TestCase):
    """main() itself: exit code and stdout, run as a subprocess so this
    proves the actual CLI entry point, not just the library function."""

    def _run(self, path):
        return subprocess.run(
            [sys.executable,
             os.path.join(REPO, "tools", "check_divider_integrity.py"), path],
            capture_output=True, text=True,
        )

    def test_cli_exit_0_on_clean_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
            f.write("# " + divider(75) + "\n# t\n# " + divider(75) + "\n")
            path = f.name
        try:
            result = self._run(path)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            os.unlink(path)

    def test_cli_exit_1_on_corrupted_file(self):
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
            f.write(
                "# " + divider(75) + "\n"
                "# t\n"
                "# " + divider(68) + "# fused next-line text right here\n"
            )
            path = f.name
        try:
            result = self._run(path)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("fused-divider", result.stdout)
        finally:
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
