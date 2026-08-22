#!/usr/bin/env python3
"""Roadmap reconcile + publish — rd (source of truth) -> roadmap doc + site data.

WHAT THIS IS
    A REPEATABLE, IDEMPOTENT checkpoint tool. rd is the single source of truth for
    release status (milestones are encoded as labels rel-0.5..rel-1.0; the 1.0-gate
    epics are named below). This script snapshots rd, computes per-milestone and
    per-gate-epic status DETERMINISTICALLY, and regenerates:

      1. the GENERATED block inside docs/release-roadmap-to-1.0.md  (in-repo, detailed,
         carries item IDs + the labeling-gap report)
      2. build/roadmap.json                                          (canonical machine
         export, with IDs — the internal artifact)
      3. <site>/data/roadmap.json                                    (curated + trademark
         -scrubbed, MILESTONE-LEVEL, no internal IDs — the public site view)

    The site's roadmap/ and status/ pages fetch data/roadmap.json (same pattern as
    /compat/ -> data/compat-surface.json). The page HTML is stable; only the DATA is
    regenerated each checkpoint — so re-running with unchanged rd is byte-identical.

IDEMPOTENCY
    - Every collection is sorted by a stable key (item id / milestone order).
    - There are NO wall-clock timestamps in the output. The only date is the --as-of
      stamp (a DATE, not a time), which defaults to today (UTC) and can be pinned so a
      CI check is reproducible within the day. Pass the SAME --as-of to get byte-
      identical output.
    - --check exits non-zero if regenerating WOULD change any tracked file (drift gate).

CONTINUATION-IDENTITY NOTE (see ~/.claude/CLAUDE.md)
    Milestone THEMES / workstream summaries / release notes below are EDITORIAL naming,
    not derived status — they are stable curation. Everything with the word "status",
    "progress", "done", "open", "blocked" is RE-DERIVED from the live rd snapshot on
    every run and is never frozen here.

USAGE
    python3 tools/roadmap/reconcile.py                 # regenerate (calls `rd list --all --json`)
    python3 tools/roadmap/reconcile.py --site-dir ../openvmx-site
    python3 tools/roadmap/reconcile.py --rd-json snap.json --as-of 2026-08-14 --check
    python3 tools/roadmap/reconcile.py --print-gaps    # just report rd-labeling gaps
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys
from collections import Counter

# --------------------------------------------------------------------------- #
#  EDITORIAL CONFIG (stable curation — NOT derived status)                     #
# --------------------------------------------------------------------------- #

# Ordered milestone ladder. `band` classifies unshipped milestones for the public
# status badge when no tag exists yet. `theme` is the public one-line.
MILESTONES = [
    ("0.3", "shipped",
     "A real system — the command language, file system, system services, and kernel "
     "executive stand on their own."),
    ("0.4", "shipped",
     "Installs and boots faithfully — the product installs to a target disk and reboots "
     "into a login."),
    ("0.5", "shipped",
     "The authenticity flip — RMS reads and writes genuine Files-11 ODS-2 over the "
     "executive ACP (the /vms passthrough retired on the runtime path), binary SYSUAF "
     "and Purdy login, and a userland that builds itself in-guest."),
    ("0.6", "active",
     "Cluster correctness — quorum and reconfiguration, a real distributed lock manager, "
     "and cluster membership resident in the executive."),
    ("0.7", "planned",
     "Cluster wire fidelity — the SCS/MSCP connection manager answers a real VAX "
     "byte-for-byte."),
    ("0.8", "planned",
     "Rejoin and satellite boot — a removed node rejoins under its own identity; "
     "diskless satellites boot from a served disk."),
    ("0.9", "planned",
     "Feature-complete — a voting member joins, serves genuine ODS-2 storage, holds "
     "locks, and evacuates a live node; TCP/IP, DECnet, and the self-hosting toolchain "
     "reach done. The last features land here."),
    ("1.0", "goal",
     "Hardened and proven — feature-frozen on 0.9: authenticity enforced by the "
     "executive, not by convention, and the whole system proven on real hardware and in "
     "extended cluster interop against a real VAX. The release you trust; fixes only."),
]
MILESTONE_ORDER = [m[0] for m in MILESTONES]

# The 1.0-gate epics ("workstreams" on the public site). Order is the presentation order.
#   id, public name, public one-line summary, milestone band it lands by
GATE_EPICS = [
    ("vms-6b8", "Executive substrate",
     "Shared system state owned by the kernel executive: logical names, processes, "
     "locks, event flags, and devices survive across processes.", "0.5"),
    ("vms-8ad", "Command-surface parity",
     "Real breadth and depth across the DCL command set and system utilities — no facades.",
     "continuous"),
    ("vms-678", "Self-hosting toolchain",
     "OVMX builds OVMX from within: compiler, librarian, and linker run as native images "
     "with no host tools in the build path.", "0.5-0.9"),
    ("vms-098", "Cluster configuration",
     "Provision a node into a cluster the VMS way: SYSGEN parameters, AUTOGEN, "
     "CLUSTER_AUTHORIZE, and CLUSTER_CONFIG.", "0.5-0.9"),
    ("vms-67f", "TCP/IP networking",
     "A VMS-faithful IP layered product: the network device, sockets, the resolver, "
     "and the client tools.", "0.5-0.9"),
    ("vms-30e", "DECnet Phase IV",
     "Clean-room DECnet: SET HOST and file transfer to and from a lab node.", "0.9"),
    ("vms-19e", "Kernel substrate",
     "SYSKRNL: the OVMX executive layered over the Linux and NetBSD kernels — not a "
     "kernel of its own. Built from pinned upstream source with the VMS modules "
     "in-tree, curated per architecture.", "0.5"),
    ("vms-8e8", "VAX as a first-class platform",
     "OVMX runs natively on VAX through a NetBSD system kernel: the executive, ODS-2 "
     "storage, and DCL boot on real VAX emulation, co-released across every architecture.",
     "0.5-0.9"),
]
GATE_IDS = [g[0] for g in GATE_EPICS]

# Editorial release notes, tag -> one line. Tags without an entry get a generic note.
RELEASE_NOTES = {
    "V0.5-1": "Completes the authenticity flip on all THREE substrates: the NetBSD/VAX executive Files-11 ODS-2 ACP flip (vms-d9c green), Alpha authentic binary-SYSUAF login with a standing green-by-SHA CI gate, and C++ first-light — a real C++ program (constructors, std::string/iostream, throw/catch) runs to exit-0 as an OVMX image, proven across x86_64, Alpha LP64, and VAX ILP32.",
    "V0.5": "The authenticity flip: RMS reads and writes genuine Files-11 ODS-2 over the executive ACP (the /vms passthrough retired on the runtime path); binary $UAFDEF SYSUAF + Purdy login proven on x86_64 and Alpha LP64; and the shipped MMK.EXE self-hosts the userland in-guest (TCC->LIBRARIAN->LINK->activate, zero bash, byte-identical). The NetBSD/VAX substrate flip is done, proven over SIMH, and lands as V0.5-1.",
    "V0.4-6": "Real OpenSSH key exchange over the executive network path, genuine ODS-2 read/write/INITIALIZE foundations, cluster rejoin proof, VAX co-release, and a sharded kernel-executive gate.",
    "V0.4-5": "Feature pack marching toward 0.5.",
    "V0.4-4": "Feature pack marching toward 0.5.",
    "V0.4-3": "Feature pack marching toward 0.5.",
    "V0.4-2": "Feature pack marching toward 0.5.",
    "V0.4-1": "Dense feature pack toward 0.5.",
    "V0.4": "Installs to a target disk and reboots into a login.",
    "V0.3-9": "Executive-backed DCL, cluster rejoin, OPCOM messages.",
    "0.3-4": "SET ACCOUNTING / SET VOLUME, clean cluster leave.",
    "0.3-3": "Conversational boot, DCL parser fidelity.",
    "0.3-2": "DCL fidelity, phases 0 and 1.",
    "0.3-1": "Rebrand, single QEMU runtime.",
    "0.3": "A real system: DCL, RMS, system services, and a kernel executive.",
    "0.2": "Install, log in, and run a real RMS indexed-file application.",
}

# How many recent releases the public site surfaces.
PUBLIC_RELEASE_LIMIT = 8

GEN_BEGIN = ("<!-- GENERATED:BEGIN roadmap-reconcile — do not edit by hand, "
             "run tools/roadmap/reconcile.py -->")
GEN_END = "<!-- GENERATED:END roadmap-reconcile -->"
# On first run (no markers yet) the block is inserted immediately before this anchor.
DOC_ANCHOR = "## 2. The 1.0 objective and its pillars"

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ROADMAP_DOC = os.path.join(REPO_ROOT, "docs", "release-roadmap-to-1.0.md")
BUILD_JSON = os.path.join(REPO_ROOT, "build", "roadmap.json")


# --------------------------------------------------------------------------- #
#  rd snapshot + rollups                                                        #
# --------------------------------------------------------------------------- #

TERMINAL = {"done", "cancelled", "failed"}


def load_snapshot(rd_json_path: str | None) -> list[dict]:
    if rd_json_path:
        with open(rd_json_path) as fh:
            return json.load(fh)
    out = subprocess.run(
        ["rd", "list", "--all", "--json"],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(out)


def index(items: list[dict]) -> dict[str, dict]:
    return {x["id"]: x for x in items}


def child_map(items: list[dict]) -> dict[str, list[str]]:
    kids: dict[str, list[str]] = {}
    for x in items:
        p = x.get("parent_id")
        if p:
            kids.setdefault(p, []).append(x["id"])
    return kids


def descendants(root: str, kids: dict[str, list[str]]) -> list[str]:
    """Transitive children of root (excludes root), deduped + sorted (idempotent)."""
    seen: set[str] = set()
    stack = list(kids.get(root, []))
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        stack += kids.get(i, [])
    return sorted(seen)


def rollup(ids: list[str], by: dict[str, dict]) -> dict:
    """Status rollup for a set of item ids. Deterministic."""
    sc = Counter(by[i]["status"] for i in ids if i in by)
    total = sum(sc.values())
    cancelled = sc.get("cancelled", 0) + sc.get("failed", 0)
    done = sc.get("done", 0)
    blocked = sc.get("blocked", 0) + sc.get("waiting", 0)
    denom = total - cancelled
    open_ = denom - done
    pct = round(100 * done / denom) if denom else 0
    return {
        "total": total, "done": done, "open": open_,
        "blocked": blocked, "cancelled": cancelled, "pct": pct,
        "by_status": dict(sorted(sc.items())),
    }


def milestone_items(label: str, items: list[dict]) -> list[str]:
    return sorted(x["id"] for x in items if label in (x.get("labels") or []))


def compute(items: list[dict]) -> dict:
    by = index(items)
    kids = child_map(items)

    # ---- milestones (explicit rel-* label signal) ----
    milestones = []
    for ver, band, theme in MILESTONES:
        label = f"rel-{ver}"
        ids = milestone_items(label, items)
        r = rollup(ids, by)
        # public status badge: a shipped band stays shipped; otherwise reflect signal
        if band == "shipped":
            status = "shipped"
        elif band == "goal":
            status = "goal"
        elif r["done"] and r["done"] < (r["total"] - r["cancelled"]):
            status = "in_progress"
        elif r["done"] and r["done"] == (r["total"] - r["cancelled"]) and r["total"]:
            status = "complete"
        else:
            status = "planned"
        milestones.append({
            "version": ver, "band": band, "theme": theme,
            "status": status, "label": label,
            "rollup": r,
            "items": [{"id": i, "status": by[i]["status"], "title": by[i]["title"]}
                      for i in ids],
        })

    # ---- gate-epic workstreams (parent_id descendant rollup) ----
    workstreams = []
    for gid, name, summary, band in GATE_EPICS:
        node = by.get(gid, {})
        desc = descendants(gid, kids)
        r = rollup(desc, by)
        self_status = node.get("status", "missing")
        # workstream status from its tree
        if not desc:
            wstatus = self_status
        elif r["done"] == 0:
            wstatus = "planned"
        elif r["done"] == (r["total"] - r["cancelled"]) and r["total"]:
            wstatus = "complete"
        else:
            wstatus = "in_progress"
        workstreams.append({
            "id": gid, "name": name, "summary": summary, "band": band,
            "self_status": self_status,
            "status": wstatus,
            "rollup": r,
            "labels": sorted(node.get("labels") or []),
            "descendant_count": len(desc),
        })

    # ---- releases (git tags) ----
    releases = git_releases()

    # ---- rd-labeling gaps ----
    gaps = labeling_gaps(items, by, kids)

    return {
        "milestones": milestones,
        "workstreams": workstreams,
        "releases": releases,
        "gaps": gaps,
    }


def git_releases() -> list[dict]:
    """Release-like tags, newest first, deterministic order."""
    try:
        raw = subprocess.run(
            ["git", "-C", REPO_ROOT, "tag"],
            capture_output=True, text=True, check=True,
        ).stdout.split()
    except Exception:
        return []

    def key(tag: str):
        t = tag.lstrip("Vv")
        # split "0.4-1" -> (0,4,1); "0.3" -> (0,3,-1); ignore non release-ish
        main, _, pt = t.partition("-")
        parts = main.split(".")
        try:
            nums = [int(p) for p in parts]
        except ValueError:
            return None
        pt_n = int(pt) if pt.isdigit() else -1
        while len(nums) < 3:
            nums.append(0)
        return (nums[0], nums[1], nums[2], pt_n)

    scored = [(key(t), t) for t in raw]
    scored = [(k, t) for k, t in scored if k is not None]
    # newest first; tie-break on the tag string for determinism
    scored.sort(key=lambda kt: (kt[0], kt[1]), reverse=True)
    out = []
    for _, tag in scored:
        out.append({"tag": tag, "note": RELEASE_NOTES.get(tag, "Point release.")})
    return out


def labeling_gaps(items: list[dict], by: dict[str, dict],
                  kids: dict[str, list[str]]) -> dict:
    """Where rd labeling is incomplete, so the generated view can't lie silently."""
    gate_missing_rel = []
    for gid, name, _summary, band in GATE_EPICS:
        node = by.get(gid, {})
        labels = node.get("labels") or []
        rels = [l for l in labels if l.startswith("rel-")]
        if not rels:
            gate_missing_rel.append({
                "id": gid, "name": name, "expected_band": band,
                "level_epic": node.get("level") == "epic",
            })
    # rel items whose parent gate epic (if any) carries no rel label
    orphan_band = []
    gate_set = set(GATE_IDS)
    for x in items:
        rels = [l for l in (x.get("labels") or []) if l.startswith("rel-")]
        p = x.get("parent_id")
        if not rels and p in gate_set:
            gnode = by.get(p, {})
            if not any(l.startswith("rel-") for l in (gnode.get("labels") or [])):
                orphan_band.append({"id": x["id"], "parent": p})
    return {
        "gate_epics_missing_rel_label": gate_missing_rel,
        "unbanded_children_of_unlabeled_gates_count": len(orphan_band),
    }


# --------------------------------------------------------------------------- #
#  render: in-repo roadmap doc GENERATED block                                  #
# --------------------------------------------------------------------------- #

BADGE = {
    "shipped": "SHIPPED", "complete": "COMPLETE", "in_progress": "in progress",
    "planned": "planned", "goal": "1.0 goal", "active": "in progress",
    "inbox": "planned", "blocked": "blocked", "missing": "MISSING", "waiting": "waiting",
}


def render_doc_block(data: dict, as_of: str) -> str:
    L = []
    L.append(GEN_BEGIN)
    L.append("")
    L.append("## Live status — generated")
    L.append("")
    L.append(f"> Reconciled from rd (source of truth) **as of {as_of}** by "
             "`tools/roadmap/reconcile.py`. Milestones are the `rel-*` labels; "
             "workstreams are the 1.0-gate epics rolled up over their child items. "
             "Re-derive any line from `rd show <id>` before acting on it.")
    L.append("")

    # milestone ladder
    L.append("### Milestone ladder")
    L.append("")
    L.append("| Milestone | Theme | Status | Done | Open | Blocked | Signal |")
    L.append("|---|---|---|---:|---:|---:|---:|")
    for m in data["milestones"]:
        r = m["rollup"]
        sig = f"{r['pct']}%" if r["total"] else "—"
        L.append(f"| **{m['version']}** | {m['theme']} | {BADGE.get(m['status'], m['status'])} "
                 f"| {r['done']} | {r['open']} | {r['blocked']} | {sig} |")
    L.append("")

    # workstreams
    L.append("### 1.0-gate workstreams (epic rollups)")
    L.append("")
    L.append("| Workstream | Epic | Lands by | Status | Done/Total | Blocked |")
    L.append("|---|---|---|---|---:|---:|")
    for w in data["workstreams"]:
        r = w["rollup"]
        band = w["band"].replace("continuous", "continuous").replace("-", "→")
        L.append(f"| {w['name']} | `{w['id']}` | {band} | {BADGE.get(w['status'], w['status'])} "
                 f"| {r['done']}/{r['total'] - r['cancelled']} | {r['blocked']} |")
    L.append("")

    # per-milestone item detail
    L.append("### Per-milestone items (rd)")
    L.append("")
    for m in data["milestones"]:
        if not m["items"]:
            continue
        L.append(f"**{m['version']}** — {m['label']} "
                 f"({m['rollup']['done']}/{m['rollup']['total'] - m['rollup']['cancelled']} done)")
        L.append("")
        for it in m["items"]:
            L.append(f"- `{it['id']}` [{it['status']}] {it['title']}")
        L.append("")

    # recent releases
    L.append("### Shipped releases (git tags)")
    L.append("")
    for rel in data["releases"][:12]:
        L.append(f"- **{rel['tag']}** — {rel['note']}")
    L.append("")

    # labeling gaps
    L.append("### rd-labeling gaps (fix these to keep the source accurate)")
    L.append("")
    gaps = data["gaps"]
    gm = gaps["gate_epics_missing_rel_label"]
    if gm:
        L.append("Gate epics carrying **no `rel-*` label** — the generated milestone view "
                 "cannot place their milestone from rd alone; the band below is editorial "
                 "(`GATE_EPICS` in the script), not derived:")
        L.append("")
        for g in gm:
            lvl = "" if g["level_epic"] else " · also not `level=epic`"
            L.append(f"- `{g['id']}` — {g['name']} (editorial band: {g['expected_band']}){lvl}")
        L.append("")
    else:
        L.append("- All gate epics carry a `rel-*` label. ✓")
        L.append("")
    n = gaps["unbanded_children_of_unlabeled_gates_count"]
    L.append(f"- Children of unlabeled gate epics with no `rel-*` of their own: **{n}** "
             "(they inherit the editorial band; label them to make rd authoritative).")
    L.append("")
    L.append(GEN_END)
    return "\n".join(L)


def splice_doc(doc_text: str, block: str) -> str:
    if GEN_BEGIN in doc_text and GEN_END in doc_text:
        pre = doc_text.split(GEN_BEGIN)[0]
        post = doc_text.split(GEN_END, 1)[1]
        return pre + block + post
    # first run: insert before the anchor section
    if DOC_ANCHOR in doc_text:
        pre, _, post = doc_text.partition(DOC_ANCHOR)
        return pre + block + "\n\n---\n\n" + DOC_ANCHOR + post
    # fallback: append
    return doc_text.rstrip() + "\n\n---\n\n" + block + "\n"


# --------------------------------------------------------------------------- #
#  render: canonical + public JSON                                              #
# --------------------------------------------------------------------------- #

def canonical_json(data: dict, as_of: str) -> str:
    obj = {
        "meta": {
            "generated": as_of,
            "source": "rd (rel-* labels + 1.0-gate epics)",
            "note": "Canonical machine export. Internal — carries item IDs.",
            "tool": "tools/roadmap/reconcile.py",
        },
        "milestones": data["milestones"],
        "workstreams": data["workstreams"],
        "releases": data["releases"],
        "gaps": data["gaps"],
    }
    return json.dumps(obj, indent=1, sort_keys=False, ensure_ascii=False) + "\n"


def _scrub(s: str) -> str:
    # same trademark contract as the compat export (data/REFRESH.md)
    s = s.replace("VSI OpenVMS", "VMS").replace("OpenVMS", "VMS")
    import re
    s = re.sub(r"\bVSI\b", "vendor", s)
    return s


def public_json(data: dict, as_of: str) -> str:
    """MILESTONE-LEVEL, curated, trademark-scrubbed. NO internal item IDs."""
    ladder = []
    for m in data["milestones"]:
        r = m["rollup"]
        ladder.append({
            "version": m["version"],
            "theme": _scrub(m["theme"]),
            "status": m["status"],
            "progress": {"done": r["done"], "open": r["open"],
                         "blocked": r["blocked"], "total": r["total"] - r["cancelled"],
                         "pct": r["pct"]},
        })
    streams = []
    for w in data["workstreams"]:
        r = w["rollup"]
        streams.append({
            "name": _scrub(w["name"]),
            "summary": _scrub(w["summary"]),
            "lands_by": w["band"],
            "status": w["status"],
            "progress": {"done": r["done"], "total": r["total"] - r["cancelled"],
                         "pct": r["pct"]},
        })
    releases = [{"tag": rel["tag"], "note": _scrub(rel["note"])}
                for rel in data["releases"][:PUBLIC_RELEASE_LIMIT]]
    obj = {
        "meta": {
            "generated": as_of,
            "source": "OVMX release board (rd)",
            "note": "Derived, milestone-level public view. Regenerated each checkpoint; "
                    "do not hand-edit.",
            "scrubbed": "trademark scrub applied on render (public site)",
        },
        "vocab": {
            "status": {
                "shipped": "Released as a tag.",
                "in_progress": "Actively landing.",
                "complete": "All tracked work done; not yet cut.",
                "planned": "Scoped, not started.",
                "goal": "The 1.0 objective.",
            }
        },
        "milestones": ladder,
        "workstreams": streams,
        "releases": releases,
    }
    return json.dumps(obj, indent=1, sort_keys=False, ensure_ascii=False) + "\n"


# --------------------------------------------------------------------------- #
#  write helpers (idempotent)                                                   #
# --------------------------------------------------------------------------- #

def write_if_changed(path: str, content: str, changed: list, check: bool) -> None:
    old = None
    if os.path.exists(path):
        with open(path) as fh:
            old = fh.read()
    if old == content:
        return
    changed.append(path)
    if check:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as fh:
        fh.write(content)


def print_gaps(data: dict) -> None:
    gaps = data["gaps"]
    print("rd-labeling gaps:")
    gm = gaps["gate_epics_missing_rel_label"]
    if not gm:
        print("  - all gate epics carry a rel-* label")
    for g in gm:
        lvl = "" if g["level_epic"] else "  (also not level=epic)"
        print(f"  - {g['id']} {g['name']}: NO rel-* label; editorial band "
              f"{g['expected_band']}{lvl}")
    print(f"  - unbanded children of unlabeled gates: "
          f"{gaps['unbanded_children_of_unlabeled_gates_count']}")


# --------------------------------------------------------------------------- #

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rd-json", help="read snapshot from file instead of `rd list --all --json`")
    ap.add_argument("--as-of", help="date stamp YYYY-MM-DD (default: today UTC)")
    ap.add_argument("--site-dir", help="openvmx-site checkout: also write data/roadmap.json")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if regeneration would change any file (drift gate)")
    ap.add_argument("--print-gaps", action="store_true",
                    help="print the rd-labeling gap report and exit")
    args = ap.parse_args()

    as_of = args.as_of or _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%d")

    items = load_snapshot(args.rd_json)
    data = compute(items)

    if args.print_gaps:
        print_gaps(data)
        return 0

    changed: list[str] = []

    # 1. in-repo roadmap doc GENERATED block
    with open(ROADMAP_DOC) as fh:
        doc = fh.read()
    block = render_doc_block(data, as_of)
    write_if_changed(ROADMAP_DOC, splice_doc(doc, block), changed, args.check)

    # 2. canonical machine export
    write_if_changed(BUILD_JSON, canonical_json(data, as_of), changed, args.check)

    # 3. public site data
    if args.site_dir:
        site_data = os.path.join(args.site_dir, "data", "roadmap.json")
        write_if_changed(site_data, public_json(data, as_of), changed, args.check)

    if args.check:
        if changed:
            print("DRIFT — regeneration would change:")
            for p in changed:
                print(f"  {p}")
            return 1
        print("clean — generated artifacts are up to date")
        return 0

    if changed:
        print(f"wrote (as of {as_of}):")
        for p in changed:
            print(f"  {p}")
    else:
        print(f"no changes (as of {as_of})")
    print_gaps(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
