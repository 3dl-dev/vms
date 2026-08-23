"""Self-tests for the roadmap reconcile collapse-guard.

Regression cover for the 2026-08-15 all-0% incident: reconcile ran against an
empty/stale rd snapshot and published a public roadmap.json where every
milestone was total=0 and every workstream 'missing' — the live status page
showed all 0%. The guard (tools/roadmap/reconcile.py::sanity_gate) must turn
that into a hard, loud abort that writes nothing.

These drive reconcile.py through synthetic --rd-json snapshots only — no live
rd, no network — so they run in plain per-PR CI.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RECONCILE = os.path.join(REPO, "tools", "roadmap", "reconcile.py")

# The gate epics the tool rolls up (must match GATE_EPICS in reconcile.py).
GATE_IDS = ["vms-6b8", "vms-8ad", "vms-678", "vms-098", "vms-67f", "vms-30e", "vms-19e"]


def _run(snapshot, tmp_path, *extra):
    """Run reconcile.py against a synthetic snapshot; return CompletedProcess."""
    snap = tmp_path / "snap.json"
    snap.write_text(json.dumps(snapshot))
    site = tmp_path / "site"
    (site / "data").mkdir(parents=True)
    # Seed a good published file so we can assert the guard does NOT clobber it.
    good = site / "data" / "roadmap.json"
    good.write_text('{"sentinel": "pre-existing good data"}\n')
    cmd = [sys.executable, RECONCILE, "--rd-json", str(snap),
           "--as-of", "2026-08-20", "--site-dir", str(site), *extra]
    return subprocess.run(cmd, capture_output=True, text=True), good


def _healthy_snapshot():
    """A minimally-healthy board: >min_items, gate epics present, rel-* labels."""
    items = [{"id": gid, "title": f"gate {gid}", "status": "in_progress",
              "level": "epic", "labels": ["rel-0.5"]} for gid in GATE_IDS]
    # pad past the 200-item floor, half labelled into a milestone
    for i in range(300):
        items.append({
            "id": f"vms-pad{i:04d}", "title": f"pad {i}",
            "status": "done" if i % 2 else "open",
            "parent_id": GATE_IDS[i % len(GATE_IDS)],
            "labels": ["rel-0.5"] if i % 3 == 0 else [],
        })
    return items


def test_empty_snapshot_aborts_without_writing(tmp_path):
    proc, good = _run([], tmp_path)
    assert proc.returncode == 2, proc.stderr
    assert "REFUSING TO PUBLISH" in proc.stderr
    # the pre-existing good published file must be untouched
    assert json.loads(good.read_text())["sentinel"] == "pre-existing good data"


def test_truncated_snapshot_trips_the_floor(tmp_path):
    # 7 gate epics present but far below the 200 floor -> still refused.
    small = [{"id": gid, "title": gid, "status": "open", "labels": ["rel-0.5"]}
             for gid in GATE_IDS]
    proc, good = _run(small, tmp_path)
    assert proc.returncode == 2, proc.stderr
    assert "floor" in proc.stderr
    assert json.loads(good.read_text())["sentinel"] == "pre-existing good data"


def test_healthy_snapshot_publishes(tmp_path):
    proc, good = _run(_healthy_snapshot(), tmp_path)
    assert proc.returncode == 0, proc.stderr
    published = json.loads(good.read_text())
    # the sentinel is gone -> the tool actually wrote real output
    assert "sentinel" not in published
    assert published["milestones"], "expected milestones in published output"
    # and it is NOT a collapse: at least one milestone has real progress
    assert any(m["progress"]["total"] > 0 for m in published["milestones"])


def test_allow_empty_overrides_the_guard(tmp_path):
    proc, _good = _run([], tmp_path, "--allow-empty")
    assert proc.returncode == 0, proc.stderr


if __name__ == "__main__":
    sys.exit(subprocess.call([sys.executable, "-m", "pytest", __file__, "-v"]))
