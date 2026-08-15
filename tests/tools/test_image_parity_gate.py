#!/usr/bin/env python3
"""test_image_parity_gate.py - self-test for tools/parity/image_parity.py
(rd vms-e1d, the cross-arch image-parity accountability gate).

Two jobs:
  1. Prove the gate has TEETH: a synthetic unallowlisted gap must fail it,
     and the identical gap once allowlisted must pass -- both for the
     compute_diff() core and for the CMakeLists.txt/_OVMX_IMAGES_DEPS
     extraction specifically (a target dropped from the aggregate must
     disappear from the extracted vax set).
  2. Prove the REAL extraction (Dockerfile.bootable / CMakeLists.txt /
     cut-release-vax.sh parsing) still finds the images we know are there,
     on the actual repo tree -- so a future edit to any of them that breaks
     the text-scan (renames a stage, re-flows the RUN block, restructures
     _OVMX_IMAGES_DEPS) reds THIS test instead of silently making the live
     gate blind.

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


_FAKE_CUT_RELEASE_VAX_SH = (
    "printf '%s\\n' vms.kmod.o vmsfs.kmod vmsfs_mount\n"
    'printf \'%s\\n\' "${IMAGE_NAMES[@]}"\n'
)


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
    (repo / "CMakeLists.txt").write_text(
        "set(_OVMX_IMAGES_DEPS\n"
        "    ovmx_init            # STARTUP.EXE\n"
        ")\n"
    )
    (repo / "tools" / "cut-release-vax.sh").write_text(_FAKE_CUT_RELEASE_VAX_SH)
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
    (repo / "CMakeLists.txt").write_text(
        "set(_OVMX_IMAGES_DEPS\n"
        "    ovmx_init            # STARTUP.EXE\n"
        "    vmslibrarian         # LIBRARIAN.EXE\n"
        ")\n"
    )
    (repo / "tools" / "cut-release-vax.sh").write_text(_FAKE_CUT_RELEASE_VAX_SH)
    (repo / "tools" / "parity" / "image-parity-allowlist.json").write_text(
        '{"entries": ['
        '{"names": ["LIBRARIAN.EXE", "vms.kmod.o", "vmsfs.kmod", "vmsfs_mount"], '
        '"missing_on": "x86_64", "reason": "test fixture"}'
        "]}"
    )

    exit_code = ip.main(["--repo-root", str(repo)])

    assert exit_code == 0
    out = capsys.readouterr().out
    assert "OK: no unallowlisted image-set drift" in out


def test_extract_vax_images_regression_when_target_dropped():
    """TEETH on the extraction itself (not just compute_diff): removing a
    target from _OVMX_IMAGES_DEPS must remove its shipped name from the
    extracted vax set -- the exact regression this gate exists to catch
    (e.g. someone reverting the vms-838 SCSD.EXE cross-build)."""
    full_cmakelists = (
        "set(_OVMX_IMAGES_DEPS\n"
        "    ovmx_init            # STARTUP.EXE\n"
        ")\n"
        "list(APPEND _OVMX_IMAGES_DEPS scsd_exe)  # SCSD.EXE\n"
    )
    regressed_cmakelists = (
        "set(_OVMX_IMAGES_DEPS\n"
        "    ovmx_init            # STARTUP.EXE\n"
        ")\n"
        "# scsd_exe dropped -- simulates reverting the vax SCSD.EXE cross-build\n"
    )

    full = ip.extract_vax_images(full_cmakelists, _FAKE_CUT_RELEASE_VAX_SH)
    regressed = ip.extract_vax_images(regressed_cmakelists, _FAKE_CUT_RELEASE_VAX_SH)

    assert "SCSD.EXE" in full
    assert "SCSD.EXE" not in regressed

    # And end-to-end: with x86_64 shipping SCSD.EXE, the regressed vax set
    # must surface it as real (unallowlisted) drift.
    x86_64_images = {"STARTUP.EXE", "SCSD.EXE"}
    result = ip.compute_diff(x86_64_images, regressed, [])
    assert "SCSD.EXE" in result.x86_64_only
    assert result.has_real_drift


def test_extract_vax_images_excludes_vmslink_under_netbsd_guard():
    """vmslink (LINK.EXE) is appended to _OVMX_IMAGES_DEPS only `if(NOT
    CMAKE_SYSTEM_NAME STREQUAL "NetBSD")` -- it must NOT appear in the vax
    extraction."""
    cmakelists = (
        "set(_OVMX_IMAGES_DEPS\n"
        "    ovmx_init            # STARTUP.EXE\n"
        ")\n"
        "list(APPEND _OVMX_IMAGES_DEPS scsd_exe)  # SCSD.EXE\n"
        'if(NOT CMAKE_SYSTEM_NAME STREQUAL "NetBSD")\n'
        "    list(APPEND _OVMX_IMAGES_DEPS vmslink)  # LINK.EXE\n"
        "endif()\n"
    )
    images = ip.extract_vax_images(cmakelists, _FAKE_CUT_RELEASE_VAX_SH)
    assert "LINK.EXE" not in images
    assert "SCSD.EXE" in images
    assert "STARTUP.EXE" in images


# ---------------------------------------------------------------------------
# Real extraction, against the actual repo tree
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def dockerfile_text() -> str:
    return (REPO_ROOT / ip.DOCKERFILE_BOOTABLE).read_text()


@pytest.fixture(scope="module")
def cmakelists_text() -> str:
    return (REPO_ROOT / ip.CMAKELISTS_TXT).read_text()


@pytest.fixture(scope="module")
def cut_release_vax_text() -> str:
    return (REPO_ROOT / ip.CUT_RELEASE_VAX_SH).read_text()


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


def test_real_vax_extraction_finds_boot_chain_images(cmakelists_text, cut_release_vax_text):
    images = ip.extract_vax_images(cmakelists_text, cut_release_vax_text)
    for expected in (
        "STARTUP.EXE", "DCL.EXE", "LOGINOUT.EXE", "PROVISION.EXE", "JOB_CONTROL.EXE",
        "LIBRARIAN.EXE", "SCSD.EXE",
    ):
        assert expected in images, f"{expected} missing from vax extraction -- CMakeLists.txt _OVMX_IMAGES_DEPS parsing regressed"

    for expected in ("vms.kmod.o", "vmsfs.kmod", "vmsfs_mount"):
        assert expected in images


def test_real_vax_extraction_excludes_vmslink(cmakelists_text, cut_release_vax_text):
    """LINK.EXE has no vax role by design (Decision A) -- it must not appear
    in the real extraction, matching the guard around vmslink in
    CMakeLists.txt's ovmx-images aggregate."""
    images = ip.extract_vax_images(cmakelists_text, cut_release_vax_text)
    assert "LINK.EXE" not in images


def test_allowlist_loads_and_every_entry_is_well_formed(allowlist):
    assert allowlist, "allowlist should not be empty -- Decision A differences are real"
    for entry in allowlist:
        assert entry.names
        assert entry.missing_on in (ip.X86_64, ip.VAX)
        assert entry.reason and len(entry.reason) > 20


def test_gate_run_against_real_repo_reports_known_findings():
    """Documents the CURRENT known state of the gate against this repo: fully
    set-equal (drift 0) in BOTH directions.

    x86_64-has-vax-lacks (the operator's explicit "no release with
    x86_64-not-vax" bar) is CLOSED: vms-838 (SCSD.EXE) + vms-88c (the vax
    release cut shipping the full ovmx-images aggregate) closed all ten images
    this gate originally found drifting that direction.

    vax-has-x86_64-lacks is now ALSO closed: vms-3bc (#595) added the two
    images the aggregate builds for every substrate but the x86_64 image had
    not staged -- LIBRARIAN.EXE (backs the LIBRARY command; %LIB-F-NOIMG
    without it) and OVMXDUMP (the object/image dumper) -- to
    distro/Dockerfile.bootable's /system-stage SYSEXE list. The extractor also
    learned OVMXDUMP has no .EXE/.ko suffix (_EXTENSIONLESS_IMAGE_BASENAMES).

    This test now asserts the fully-closed, set-equal state -- the state that
    lets `Cross-Arch Image Parity Gate (vms-e1d)` become a REQUIRED,
    bidirectional, CI-enforced invariant. If either assertion below fails, a
    genuine cross-arch gap reopened: fix the gap (build/ship the image for the
    missing arch), or -- only for a documented Decision-A difference -- add a
    reviewed allowlist entry. Do NOT loosen or delete an assertion to silence
    a real gap.
    """
    result = ip.run(REPO_ROOT)

    # The x86_64-has-vax-lacks direction is fully closed: nothing shipped on
    # x86_64 real drifts un-covered on vax.
    assert result.x86_64_only == set(), (
        f"x86_64-only images reappeared (real regression on the closed direction): {result.x86_64_only}"
    )

    # SCSD.EXE in particular must never be allowlisted -- it now builds+ships
    # for vax (vms-838), so its Decision-A-free presence on both sides must
    # show up as neither drift nor an allowlist entry.
    assert "SCSD.EXE" not in result.x86_64_only
    assert "SCSD.EXE" not in result.allowlisted_x86_64_only

    # The vax-has-x86_64-lacks direction is now ALSO closed: vms-3bc (#595)
    # added LIBRARIAN.EXE + OVMXDUMP to distro/Dockerfile.bootable's x86_64
    # SYSEXE staging (LIBRARIAN.EXE backs the LIBRARY command; OVMXDUMP is the
    # object/image dumper -- VMS ships both in SYS$SYSTEM on every install), so
    # neither is vax-only drift anymore. Per this test's own standing note, the
    # expectations are updated to the resolved state -- NOT loosened to hide a
    # gap: both images genuinely ship on both arches now.
    assert "LIBRARIAN.EXE" not in result.vax_only
    assert "OVMXDUMP" not in result.vax_only
    assert result.vax_only == set(), (
        f"vax-only images reappeared (real regression on the now-closed "
        f"reverse direction): {result.vax_only}"
    )

    # And the Decision-A diffs must NOT show up as unallowlisted drift.
    for name in ip.LINK_NATIVE_SHAREABLES:
        assert name not in result.x86_64_only
    assert "IMGACT.EXE" not in result.x86_64_only

    # Both directions are now closed: the gate is set-equal (drift 0), which is
    # exactly the state that lets it become a REQUIRED bidirectional invariant.
    assert not result.has_real_drift, (
        "the gate reports real, unallowlisted drift -- see rd vms-e1d. This test "
        "asserts the fully-closed (set-equal) state; if it fails, a genuine "
        "cross-arch gap reopened. Fix the gap (build/ship the image for the "
        "missing arch) or, only for a documented Decision-A difference, add a "
        "reviewed allowlist entry -- do NOT delete this assertion."
    )


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
