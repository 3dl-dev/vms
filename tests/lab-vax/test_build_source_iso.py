#!/usr/bin/env python3
"""test_build_source_iso.py - host unit test for drive_boot_vax.build_source_iso's
extra_files parameter (rd vms-cee).

WHY THIS EXISTS: the Node-B (OVMX/VAX) browser-demo group-257 rebuild needs to
drop a caller-authored CLUSTER_AUTHORIZE.DAT onto the boot-artifact CD from
OUTSIDE artifacts_dir, because artifacts_dir is a READ-ONLY mount in the real
run-boot.sh pipeline (`-v ARTIFACTS_DIR:/artifacts:ro`) -- so it cannot simply
be dropped into that directory. This proves:
  1. extra_files=None (the default) is BYTE-IDENTICAL to the pre-existing
     behavior: genisoimage is invoked directly on artifacts_dir, no staging
     copy is made -- zero regression for every existing caller (install-boot,
     install-kernel, assemble-single without cluster injection).
  2. extra_files={"name": path} stages a TEMP directory containing every file
     already in artifacts_dir PLUS the extra file under its given name, runs
     genisoimage against THAT staging directory, and cleans it up afterward
     (no leftover temp dirs).
  3. `required` is satisfied by a name present ONLY in extra_files (not
     physically in artifacts_dir) -- the whole point of the parameter.
  4. A `required` name absent from BOTH artifacts_dir and extra_files still
     raises (no silent skip -- INV-6).

No anita / genisoimage / pexpect / SIMH needed: `anita` and `netbsd_console`
(drive_boot_vax.py's only non-stdlib imports) are stubbed via sys.modules
before import, and genisoimage's subprocess.check_call is mocked so this runs
anywhere python3 does.

Run: pytest tests/lab-vax/test_build_source_iso.py -v
"""

import os
import sys
import tempfile
import types
from unittest import mock

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# --- stub the two non-stdlib imports drive_boot_vax.py needs at module load
# time (anita, netbsd_console) so this test needs neither installed. Staged
# under the system tmp dir, never inside the repo tree. ---------------------
if "anita" not in sys.modules:
    sys.modules["anita"] = types.ModuleType("anita")

_netbsd_stub_dir = tempfile.mkdtemp(prefix="ovmx-test-netbsd-stub-")
with open(os.path.join(_netbsd_stub_dir, "netbsd_console.py"), "w") as _fp:
    _fp.write("# stub for test_build_source_iso.py (rd vms-cee)\n")
os.environ.setdefault("OVMX_NETBSD_DIR", _netbsd_stub_dir)

import drive_boot_vax as dbv  # noqa: E402


@pytest.fixture
def artifacts_dir(tmp_path):
    d = tmp_path / "artifacts"
    d.mkdir()
    (d / "vms.kmod").write_bytes(b"fake-kmod-bytes")
    (d / "STARTUP.EXE").write_bytes(b"fake-startup-bytes")
    return str(d)


def _fake_check_call_capturing(calls):
    def _fake(cmd):
        calls.append(cmd)
        # genisoimage's real effect (an output file existing) is asserted by
        # the caller via os.path.getsize -- create an empty one so that call
        # does not raise.
        out_idx = cmd.index("-o") + 1
        with open(cmd[out_idx], "wb") as fp:
            fp.write(b"\0" * 2048)
    return _fake


class TestNoRegressionWithoutExtraFiles:
    def test_genisoimage_runs_directly_on_artifacts_dir(self, artifacts_dir, tmp_path):
        out_iso = str(tmp_path / "out.iso")
        calls = []
        with mock.patch.object(dbv.subprocess, "check_call",
                                side_effect=_fake_check_call_capturing(calls)):
            dbv.build_source_iso(artifacts_dir, out_iso, ("vms.kmod",))
        assert len(calls) == 1
        # last argument is the source directory genisoimage was pointed at
        assert calls[0][-1] == artifacts_dir, (
            "extra_files=None must build the ISO directly from artifacts_dir "
            "(no staging copy) -- byte-identical to the pre-vms-cee behavior")

    def test_no_temp_staging_dir_left_behind(self, artifacts_dir, tmp_path):
        out_iso = str(tmp_path / "out.iso")
        before = set(os.listdir(tmp_path))
        with mock.patch.object(dbv.subprocess, "check_call",
                                side_effect=_fake_check_call_capturing([])):
            dbv.build_source_iso(artifacts_dir, out_iso, ("vms.kmod",))
        after = set(os.listdir(tmp_path)) - {"out.iso"}
        assert after == before - {"out.iso"} or after == set(), (
            "no extra_files -> no staging directory should be created at all")


class TestExtraFiles:
    def test_extra_file_is_staged_alongside_artifacts(self, artifacts_dir, tmp_path):
        extra_src = tmp_path / "cluster_authorize.dat"
        extra_src.write_bytes(b"CAUT" + b"\0" * 40)
        out_iso = str(tmp_path / "out.iso")
        calls = []
        staged_dir_snapshot = {}

        def _fake(cmd):
            calls.append(cmd)
            src_dir = cmd[-1]
            staged_dir_snapshot["dir"] = src_dir
            staged_dir_snapshot["files"] = sorted(os.listdir(src_dir))
            with open(cmd[cmd.index("-o") + 1], "wb") as fp:
                fp.write(b"\0" * 2048)

        with mock.patch.object(dbv.subprocess, "check_call", side_effect=_fake):
            dbv.build_source_iso(artifacts_dir, out_iso, ("vms.kmod",),
                                 extra_files={"cluster_authorize.dat": str(extra_src)})

        assert len(calls) == 1
        assert staged_dir_snapshot["dir"] != artifacts_dir, (
            "extra_files given -> a SEPARATE staging dir must be used, never "
            "a write into the (real-world read-only) artifacts_dir")
        assert staged_dir_snapshot["files"] == sorted(
            ["vms.kmod", "STARTUP.EXE", "cluster_authorize.dat"]), (
            "the staging dir must carry every artifacts_dir file PLUS the extra")

    def test_staging_dir_is_cleaned_up_after(self, artifacts_dir, tmp_path):
        extra_src = tmp_path / "extra.dat"
        extra_src.write_bytes(b"x")
        out_iso = str(tmp_path / "out.iso")
        captured = {}

        def _fake(cmd):
            captured["dir"] = cmd[-1]
            assert os.path.isdir(captured["dir"])
            with open(cmd[cmd.index("-o") + 1], "wb") as fp:
                fp.write(b"\0" * 2048)

        with mock.patch.object(dbv.subprocess, "check_call", side_effect=_fake):
            dbv.build_source_iso(artifacts_dir, out_iso, ("vms.kmod",),
                                 extra_files={"extra.dat": str(extra_src)})

        assert not os.path.isdir(captured["dir"]), (
            "the temp staging directory must be removed once the ISO is built")

    def test_required_name_satisfied_only_by_extra_files(self, artifacts_dir, tmp_path):
        # "cluster_authorize.dat" is required but exists ONLY in extra_files,
        # never physically in artifacts_dir -- must NOT raise.
        extra_src = tmp_path / "cluster_authorize.dat"
        extra_src.write_bytes(b"CAUT")
        out_iso = str(tmp_path / "out.iso")
        with mock.patch.object(dbv.subprocess, "check_call",
                                side_effect=_fake_check_call_capturing([])):
            dbv.build_source_iso(artifacts_dir, out_iso,
                                 ("vms.kmod", "cluster_authorize.dat"),
                                 extra_files={"cluster_authorize.dat": str(extra_src)})

    def test_required_name_missing_everywhere_still_raises(self, artifacts_dir, tmp_path):
        out_iso = str(tmp_path / "out.iso")
        with mock.patch.object(dbv.subprocess, "check_call",
                                side_effect=_fake_check_call_capturing([])):
            with pytest.raises(RuntimeError):
                dbv.build_source_iso(artifacts_dir, out_iso,
                                     ("vms.kmod", "nonexistent.dat"),
                                     extra_files={"cluster_authorize.dat": "/dev/null"})


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
