#!/usr/bin/env python3
"""test_image_parity_gate.py - self-test for tools/parity/image_parity.py
(rd vms-e1d, the cross-arch image-parity accountability gate).

Two jobs:
  1. Prove the gate has TEETH: a synthetic unallowlisted gap must fail it,
     and the identical gap once allowlisted must pass.
  2. Prove the REAL extraction (Dockerfile.bootable / cut-release.sh parsing)
     still finds the images we know are there, on the actual repo tree --
     so a future edit to either file that breaks the text-scan (renames a
     stage, re-flows the RUN block, restructures VAX_ARTIFACT_ORDER) reds
     THIS test instead of silently making the live gate blind.

Run: python3 -m pytest tests/tools/test_image_parity_gate.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "parity"))

import image_parity as ip  # noqa: E402


# ---------------------------------------------------------------------------
# Teeth: synthetic gaps
# ---------------------------------------------------------------------------


def test_unallowlisted_gap_fails():
    """An image x86_64 has and vax doesn't, with NO allowlist entry, must be
    reported as real drift and fail the gate."""
    x86_64_images = {"FOO.EXE", "DCL.EXE"}
    vax_images = {"DCL.EXE"}
    allowlist: list[ip.AllowlistEntry] = []

    result = ip.compute_diff(x86_64_images, vax_images, allowlist)

    assert result.has_real_drift
    assert result.x86_64_only == {"FOO.EXE"}
    assert result.vax_only == set()


def test_unallowlisted_gap_vice_versa_fails():
    """Same, in the other direction: vax has an image x86_64 lacks."""
    x86_64_images = {"DCL.EXE"}
    vax_images = {"DCL.EXE", "BAR.EXE"}
    allowlist: list[ip.AllowlistEntry] = []

    result = ip.compute_diff(x86_64_images, vax_images, allowlist)

    assert result.has_real_drift
    assert result.vax_only == {"BAR.EXE"}
    assert result.x86_64_only == set()


def test_allowlisted_gap_passes():
    """The identical gap as test_unallowlisted_gap_fails, but now covered by
    an allowlist entry, must NOT be reported as drift."""
    x86_64_images = {"FOO.EXE", "DCL.EXE"}
    vax_images = {"DCL.EXE"}
    allowlist = [
        ip.AllowlistEntry(names=["FOO.EXE"], missing_on=ip.VAX, reason="test fixture: legitimate diff")
    ]

    result = ip.compute_diff(x86_64_images, vax_images, allowlist)

    assert not result.has_real_drift
    assert result.x86_64_only == set()
    assert result.allowlisted_x86_64_only == {"FOO.EXE"}


def test_allowlist_entry_on_wrong_side_does_not_mask_drift():
    """An allowlist entry for 'FOO.EXE missing on vax' must not accidentally
    suppress a DIFFERENT gap ('FOO.EXE missing on x86_64')."""
    x86_64_images = {"DCL.EXE"}
    vax_images = {"DCL.EXE", "FOO.EXE"}
    allowlist = [
        ip.AllowlistEntry(names=["FOO.EXE"], missing_on=ip.VAX, reason="wrong direction on purpose")
    ]

    result = ip.compute_diff(x86_64_images, vax_images, allowlist)

    assert result.has_real_drift
    assert result.vax_only == {"FOO.EXE"}


def test_identical_sets_pass_with_empty_allowlist():
    result = ip.compute_diff({"DCL.EXE", "LOGINOUT.EXE"}, {"DCL.EXE", "LOGINOUT.EXE"}, [])
    assert not result.has_real_drift


def test_load_allowlist_rejects_bad_missing_on(tmp_path):
    bad = tmp_path / "bad-allowlist.json"
    bad.write_text(
        '{"entries": [{"names": ["FOO.EXE"], "missing_on": "aarch64", "reason": "nope"}]}'
    )
    with pytest.raises(ValueError):
        ip.load_allowlist(bad)


def test_main_cli_exits_nonzero_on_real_drift(tmp_path, capsys):
    """End-to-end through main(): a repo tree with a real gap must produce a
    nonzero exit code, not just an internal DiffResult."""
    repo = tmp_path / "fake-repo"
    (repo / "distro").mkdir(parents=True)
    (repo / "tools" / "parity").mkdir(parents=True)
    (repo / "distro" / "Dockerfile.bootable").write_text(
        "# shareable-image graph (DECC$SHR.EXE, LIBVMSSYS$SHR.EXE, LIBVMSPROCESS$SHR.EXE,\n"
        "# LIBVMSLNM$SHR.EXE, LIBVMSFS$SHR.EXE, LIBVMS$SHR.EXE, LIBVMSRMS$SHR.EXE)\n"
        "RUN cp build-static/bin/STARTUP.EXE /system-stage/init && \\\n"
        "    cp build-static/bin/FOO.EXE /system-stage/vms/SYS0/SYSCOMMON/SYSEXE/\n"
    )
    (repo / "tools" / "cut-release.sh").write_text(
        'VAX_ARTIFACT_ORDER=()\n'
        'if true; then\n'
        '    VAX_ARTIFACT_ORDER=(STARTUP.EXE)\n'
        "fi\n"
    )
    (repo / "tools" / "parity" / "image-parity-allowlist.json").write_text('{"entries": []}')

    exit_code = ip.main(["--repo-root", str(repo)])

    assert exit_code == 1
    out = capsys.readouterr().out
    assert "FOO.EXE" in out


def test_main_cli_exits_zero_when_fully_allowlisted(tmp_path, capsys):
    repo = tmp_path / "fake-repo-clean"
    (repo / "distro").mkdir(parents=True)
    (repo / "tools" / "parity").mkdir(parents=True)
    (repo / "distro" / "Dockerfile.bootable").write_text(
        "# shareable-image graph (DECC$SHR.EXE, LIBVMSSYS$SHR.EXE, LIBVMSPROCESS$SHR.EXE,\n"
        "# LIBVMSLNM$SHR.EXE, LIBVMSFS$SHR.EXE, LIBVMS$SHR.EXE, LIBVMSRMS$SHR.EXE)\n"
        "RUN cp build-static/bin/STARTUP.EXE /system-stage/init\n"
    )
    (repo / "tools" / "cut-release.sh").write_text(
        'VAX_ARTIFACT_ORDER=()\n'
        'if true; then\n'
        '    VAX_ARTIFACT_ORDER=(STARTUP.EXE LIBRARIAN.EXE)\n'
        "fi\n"
    )
    (repo / "tools" / "parity" / "image-parity-allowlist.json").write_text(
        '{"entries": [{"names": ["LIBRARIAN.EXE"], "missing_on": "x86_64", "reason": "test fixture"}]}'
    )

    exit_code = ip.main(["--repo-root", str(repo)])

    assert exit_code == 0
    out = capsys.readouterr().out
    assert "OK: no unallowlisted image-set drift" in out


# ---------------------------------------------------------------------------
# Real extraction, against the actual repo tree
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def dockerfile_text() -> str:
    return (REPO_ROOT / ip.DOCKERFILE_BOOTABLE).read_text()


@pytest.fixture(scope="module")
def cut_release_text() -> str:
    return (REPO_ROOT / ip.CUT_RELEASE_SH).read_text()


@pytest.fixture(scope="module")
def allowlist() -> list[ip.AllowlistEntry]:
    return ip.load_allowlist(REPO_ROOT / ip.ALLOWLIST_JSON)


def test_real_x86_64_extraction_finds_boot_chain_images(dockerfile_text):
    images = ip.extract_x86_64_images(dockerfile_text)
    for expected in ("STARTUP.EXE", "DCL.EXE", "LOGINOUT.EXE", "PROVISION.EXE", "JOB_CONTROL.EXE"):
        assert expected in images, f"{expected} missing from x86_64 extraction -- Dockerfile.bootable parsing regressed"


def test_real_x86_64_extraction_finds_shareables_and_modules(dockerfile_text):
    images = ip.extract_x86_64_images(dockerfile_text)
    for name in ip.LINK_NATIVE_SHAREABLES:
        assert name in images
    assert "vms.ko" in images
    assert "vmsfs.ko" in images


def test_real_x86_64_extraction_excludes_non_images(dockerfile_text):
    images = ip.extract_x86_64_images(dockerfile_text)
    assert "OVMX-OS.KIT" not in images
    assert "PARTS_SETUP.COM" not in images


def test_real_vax_extraction_finds_boot_chain_images(cut_release_text):
    images = ip.extract_vax_images(cut_release_text)
    for expected in ("STARTUP.EXE", "DCL.EXE", "LOGINOUT.EXE", "PROVISION.EXE", "JOB_CONTROL.EXE", "LIBRARIAN.EXE"):
        assert expected in images

    for expected in ("vms.kmod.o", "vmsfs.kmod", "vmsfs_mount"):
        assert expected in images


def test_allowlist_loads_and_every_entry_is_well_formed(allowlist):
    assert allowlist, "allowlist should not be empty -- Decision A differences are real"
    for entry in allowlist:
        assert entry.names
        assert entry.missing_on in (ip.X86_64, ip.VAX)
        assert entry.reason and len(entry.reason) > 20


def test_gate_run_against_real_repo_reports_known_findings():
    """Documents the CURRENT known state of the gate against this repo: real
    (unallowlisted) drift exists today because VAX release-artifact coverage
    (tools/cut-release-vax.sh, R2 scope) does not yet build the full x86_64
    system-utility surface, and x86_64's release image does not stage
    LIBRARIAN.EXE the way the VAX release bundle does. This test PASSES by
    correctly detecting that state -- it is not asserting parity exists, it
    is asserting the gate correctly sees that it doesn't. When VAX coverage
    (or x86_64's LIBRARIAN.EXE staging) changes, update this test's expected
    sets to match -- do NOT loosen it to silence a real gap.
    """
    result = ip.run(REPO_ROOT)

    expected_x86_64_only_at_least = {
        "HELP.EXE",
        "AUTHORIZE.EXE",
        "MAIL.EXE",
        "MONITOR.EXE",
        "INITIALIZE.EXE",
        "INSTALL.EXE",
        "SYSGEN.EXE",
        "SCSD.EXE",
        "PRODUCT.EXE",
        "PARTS.EXE",
    }
    assert expected_x86_64_only_at_least <= result.x86_64_only

    assert "LIBRARIAN.EXE" in result.vax_only

    # And the Decision-A diffs must NOT show up as unallowlisted drift.
    for name in ip.LINK_NATIVE_SHAREABLES:
        assert name not in result.x86_64_only
    assert "IMGACT.EXE" not in result.x86_64_only
    assert result.has_real_drift, (
        "the gate currently has real, reported (not allowlisted) findings -- "
        "see rd vms-e1d. If this now fails, VAX coverage caught up: update "
        "the expectations above rather than deleting them."
    )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
