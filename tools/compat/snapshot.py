#!/usr/bin/env python3
"""Per-release coverage snapshot + delta for the Compatibility Surface Register.

Ties the register into the release-engineering train (pillar vms-a84). At cut
time this:

  1. regenerates the register (so the snapshot reflects the cut tree),
  2. writes a tracked per-version snapshot: docs/compat/snapshots/<version>.json
     (per-item status, for coverage-over-time and the website trend),
  3. optionally drops a machine copy into the release bundle (--out-dir),
  4. prints a Markdown "Compatibility coverage" block, diffed against the
     previous snapshot, for tools/gen_release_notes.py to embed.

Usage:
    python3 tools/compat/snapshot.py                     # snapshot HEAD's version, print delta
    python3 tools/compat/snapshot.py --version 0.4       # override the version label
    python3 tools/compat/snapshot.py --out-dir dist/release-0.4   # also drop compat-coverage.json in the bundle
    python3 tools/compat/snapshot.py --prev docs/compat/snapshots/0.3-9.json
    python3 tools/compat/snapshot.py --notes-only        # just the Markdown block (no files written)

The snapshot is the source for the website's coverage trend; the printed block
is the source for the release notes. Neither is hand-maintained.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import render_compat as rc  # noqa: E402

REPO = rc.REPO
SNAP_DIR = REPO / "docs" / "compat" / "snapshots"
IDENTITY_H = REPO / "src" / "libvms" / "include" / "ovmx_identity.h"


def product_version() -> str:
    """Read OVMX_PRODUCT_VERSION — the single source of truth (INV-1)."""
    try:
        txt = IDENTITY_H.read_text()
        m = re.search(r'OVMX_PRODUCT_VERSION\s+"([^"]+)"', txt)
        if m:
            return m.group(1)
    except OSError:
        pass
    return "dev"


def build_snapshot(version: str):
    ctx = rc.Ctx()
    domains, facilities = rc.load_all(ctx)
    if domains is None or ctx.errors:
        for e in ctx.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
    rows = rc.flat_items(facilities)
    overall = rc.rollup(rows)
    by_domain = {}
    for d in domains["domains"]:
        ditems = [r for r in rows if r["domain"] == d["id"]]
        if ditems:
            by_domain[d["id"]] = rc.rollup(ditems)
    items = {
        f"{r['facility']}${r['id']}": {
            "status": r["status"],
            "authenticity": r["authenticity"],
            "scope_1_0": r["scope_1_0"],
        }
        for r in rows
    }
    return {
        "version": version,
        "overall": overall,
        "overall_in_scope": rc.rollup([r for r in rows if r["scope_1_0"] == "in"]),
        "by_domain": by_domain,
        "items": items,
    }


def find_prev(version: str, explicit: str | None):
    if explicit:
        return json.loads(Path(explicit).read_text())
    if not SNAP_DIR.exists():
        return None
    cands = [p for p in SNAP_DIR.glob("*.json") if p.stem != version]
    if not cands:
        return None
    # newest by mtime is good enough; snapshots are written in release order
    prev = max(cands, key=lambda p: p.stat().st_mtime)
    return json.loads(prev.read_text())


def status_rank(s):
    return rc.STATUS_ORDER[::-1].index(s)  # absent=0 … verified=5


def delta_block(snap, prev) -> str:
    o = snap["overall"]
    inn = snap.get("overall_in_scope", {})
    L = []
    L.append("### Compatibility surface")
    L.append("")
    # Counts, never a percentage of the surface (no known denominator).
    line = (f"**{o['count']} surfaces catalogued** — "
            f"{o['by_status'].get('verified',0)} verified, "
            f"{o['by_status'].get('implemented',0)} implemented, "
            f"{o['by_status'].get('partial',0)} partial, "
            f"{o['by_status'].get('absent',0)} absent, "
            f"{o['facade_risk']} facade-risk.")
    if inn:
        line += (f" V1: {inn.get('met',0)} of {inn.get('count',0)} committed "
                 f"surfaces met.")
    if prev:
        pm = prev.get("overall_in_scope", {}).get("met")
        if pm is not None and inn:
            dm = inn.get("met", 0) - pm
            line += f" ({dm:+} V1 met vs {prev['version']}.)"
    L.append(line)
    L.append("")
    if not prev:
        L.append("_Baseline snapshot; no prior release to diff against._")
        return "\n".join(L)

    pi, ci = prev["items"], snap["items"]
    improved, regressed, new_surfaces, resolved_facades, new_facades = [], [], [], [], []
    for k, cur in ci.items():
        old = pi.get(k)
        if old is None:
            new_surfaces.append(k)
            continue
        if status_rank(cur["status"]) > status_rank(old["status"]):
            improved.append((k, old["status"], cur["status"]))
        elif status_rank(cur["status"]) < status_rank(old["status"]):
            regressed.append((k, old["status"], cur["status"]))
        if old["authenticity"] == "facade-risk" and cur["authenticity"] != "facade-risk":
            resolved_facades.append(k)
        if old["authenticity"] != "facade-risk" and cur["authenticity"] == "facade-risk":
            new_facades.append(k)

    def bullets(title, xs, fmt):
        if not xs:
            return []
        out = [f"- **{title} ({len(xs)}):** "]
        out[0] += ", ".join(fmt(x) for x in xs[:12])
        if len(xs) > 12:
            out[0] += f", … (+{len(xs)-12} more)"
        return out

    L += bullets("Surfaces advanced", improved,
                 lambda x: f"`{x[0]}` {x[1]}→{x[2]}")
    L += bullets("Facades resolved", resolved_facades, lambda k: f"`{k}`")
    L += bullets("Newly catalogued", new_surfaces, lambda k: f"`{k}`")
    if regressed:
        L += bullets("⚠ Regressed", regressed, lambda x: f"`{x[0]}` {x[1]}→{x[2]}")
    if new_facades:
        L += bullets("⚠ New facade-risk", new_facades, lambda k: f"`{k}`")
    if len(L) <= 4:
        L.append("_No status changes since the previous release._")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None, help="version label (default: OVMX_PRODUCT_VERSION)")
    ap.add_argument("--prev", default=None, help="previous snapshot json to diff against")
    ap.add_argument("--out-dir", default=None, help="also write compat-coverage.json here (release bundle)")
    ap.add_argument("--notes-only", action="store_true", help="print the Markdown block only; write nothing")
    args = ap.parse_args()

    version = args.version or product_version()
    # build_snapshot reads docs/compat/*.yaml directly, so it always reflects
    # the current tree (no stale generated file in the loop).
    snap = build_snapshot(version)
    prev = find_prev(version, args.prev)
    block = delta_block(snap, prev)

    if args.notes_only:
        print(block)
        return

    SNAP_DIR.mkdir(parents=True, exist_ok=True)
    snap_path = SNAP_DIR / f"{version}.json"
    snap_path.write_text(json.dumps(snap, indent=2))
    if args.out_dir:
        outp = Path(args.out_dir) / "compat-coverage.json"
        outp.parent.mkdir(parents=True, exist_ok=True)
        outp.write_text(json.dumps(snap, indent=2))
    print(f"wrote {snap_path.relative_to(REPO)}"
          + (f" and {args.out_dir}/compat-coverage.json" if args.out_dir else ""),
          file=sys.stderr)
    print(block)


if __name__ == "__main__":
    main()
