#!/usr/bin/env python3
"""Render the OVMX Compatibility Surface Register.

Reads the structured source of truth under docs/compat/ (domains.yaml +
facilities/*.yaml) and emits:

  * docs/compatibility-surface.md   -- the internal comprehensive register
  * build/compat-surface.json       -- machine export (website + corpus lookup)

Usage:
    python3 tools/compat/render_compat.py            # render both outputs
    python3 tools/compat/render_compat.py --check    # validate only (CI gate)

The source YAML is authoritative; the generated files are never hand-edited.
See docs/design-compat-surface-register.md for the data model and vocab.
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import date, datetime
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("PyYAML required: this generator runs under the build/test tooling, "
             "not on the host (Rule: no host installs).")

REPO = Path(__file__).resolve().parents[2]
COMPAT = REPO / "docs" / "compat"
FACILITIES = COMPAT / "facilities"
OUT_MD = REPO / "docs" / "compatibility-surface.md"
OUT_JSON = REPO / "build" / "compat-surface.json"

# Coverage weights -- a rough index, labelled as such in the output. Not a score
# to optimise; a way to sort and to show the shape of the gap.
COVERAGE_WEIGHT = {
    "absent": 0.0, "designed": 0.1, "stub": 0.2,
    "partial": 0.5, "implemented": 0.85, "verified": 1.0,
}
STATUS_ORDER = ["verified", "implemented", "partial", "stub", "designed", "absent"]
STATUS_MARK = {
    "verified": "✅", "implemented": "🟢", "partial": "🟡",
    "stub": "🟠", "designed": "🔵", "absent": "⬜",
}
AUTH_MARK = {"real": "", "advisory": "≈", "facade-risk": "⚠", "n/a": ""}
STALE_DAYS = 120


class Ctx:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []


def load_yaml(path: Path):
    with path.open() as fh:
        return yaml.safe_load(fh)


def load_all(ctx: Ctx):
    if not (COMPAT / "domains.yaml").exists():
        ctx.errors.append("docs/compat/domains.yaml missing")
        return None, []
    domains = load_yaml(COMPAT / "domains.yaml")
    facilities = []
    for fp in sorted(FACILITIES.glob("*.yaml")) if FACILITIES.exists() else []:
        data = load_yaml(fp)
        if not data:
            ctx.warnings.append(f"{fp.name}: empty")
            continue
        data["_file"] = fp.name
        facilities.append(data)
    return domains, facilities


def validate(ctx: Ctx, domains, facilities):
    vocab_status = set(domains["status"])
    vocab_auth = set(domains["authenticity"])
    vocab_scope = set(domains["scope_1_0"])
    vocab_kind = set(domains["kind"])
    domain_ids = {d["id"] for d in domains["domains"]}
    # facility token -> domain from the registry
    registry = {}
    for dom_id, toks in domains.get("facilities", {}).items():
        for tok in toks or []:
            registry[tok] = dom_id

    seen = set()
    for f in facilities:
        fac = f.get("facility")
        where = f.get("_file", fac)
        if not fac:
            ctx.errors.append(f"{where}: missing 'facility'")
            continue
        seen.add(fac)
        if f.get("domain") not in domain_ids:
            ctx.errors.append(f"{fac}: domain '{f.get('domain')}' not in domains.yaml")
        if fac not in registry:
            ctx.warnings.append(f"{fac}: not in domains.yaml facility registry")
        elif registry[fac] != f.get("domain"):
            ctx.errors.append(f"{fac}: domain '{f.get('domain')}' != registry "
                              f"'{registry[fac]}'")
        for it in f.get("items", []) or []:
            iid = it.get("id", "<no-id>")
            if it.get("status") not in vocab_status:
                ctx.errors.append(f"{fac}/{iid}: bad status '{it.get('status')}'")
            if it.get("authenticity") not in vocab_auth:
                ctx.errors.append(f"{fac}/{iid}: bad authenticity "
                                  f"'{it.get('authenticity')}'")
            if it.get("kind") not in vocab_kind:
                ctx.errors.append(f"{fac}/{iid}: bad kind '{it.get('kind')}'")
            scope = it.get("scope_1_0", f.get("scope_1_0"))
            if scope not in vocab_scope:
                ctx.errors.append(f"{fac}/{iid}: bad scope_1_0 '{scope}'")
            if it.get("status") == "verified" and not it.get("verified_against"):
                ctx.errors.append(f"{fac}/{iid}: status=verified needs verified_against")
            # drift signal: implemented/verified item whose evidence file is gone
            ev = it.get("evidence")
            if ev and it.get("status") in ("implemented", "verified"):
                p = (REPO / ev.split(":")[0])
                if not p.exists():
                    ctx.warnings.append(f"{fac}/{iid}: evidence '{ev}' not found on "
                                        f"current tree (drift?)")
        lr = f.get("last_reviewed")
        if isinstance(lr, (date, datetime)):
            age = (date.today() - (lr.date() if isinstance(lr, datetime) else lr)).days
            if age > STALE_DAYS:
                ctx.warnings.append(f"{fac}: last_reviewed {lr} is {age}d old (>{STALE_DAYS})")

    # uncovered facilities: registered but no file
    uncovered = {tok: dom for tok, dom in registry.items() if tok not in seen}
    return uncovered


def item_scope(f, it):
    return it.get("scope_1_0", f.get("scope_1_0", "undecided"))


def rollup(items):
    by_status = {s: 0 for s in STATUS_ORDER}
    by_auth = {}
    weight = 0.0
    for it in items:
        by_status[it["status"]] = by_status.get(it["status"], 0) + 1
        by_auth[it["authenticity"]] = by_auth.get(it["authenticity"], 0) + 1
        weight += COVERAGE_WEIGHT.get(it["status"], 0.0)
    n = len(items)
    return {
        "count": n,
        "by_status": by_status,
        "by_authenticity": by_auth,
        "coverage_index": round(100 * weight / n, 1) if n else 0.0,
        "facade_risk": by_auth.get("facade-risk", 0),
    }


def bar(by_status, width=24):
    total = sum(by_status.values()) or 1
    cells = []
    for s in STATUS_ORDER:
        cells.append(STATUS_MARK[s] * round(width * by_status[s] / total))
    return "".join(cells)


def flat_items(facilities):
    rows = []
    for f in facilities:
        for it in f.get("items", []) or []:
            rows.append({
                "domain": f["domain"],
                "facility": f["facility"],
                "facility_name": f.get("name", f["facility"]),
                "id": it.get("id"),
                "kind": it.get("kind"),
                "vms": it.get("vms", ""),
                "status": it.get("status"),
                "authenticity": it.get("authenticity"),
                "scope_1_0": item_scope(f, it),
                "evidence": it.get("evidence", ""),
                "verified_against": it.get("verified_against"),
                "notes": it.get("notes", ""),
                "plan_ref": it.get("plan_ref", f.get("plan_ref")),
            })
    return rows


def render_md(domains, facilities, uncovered, ctx):
    rows = flat_items(facilities)
    overall = rollup(rows)
    today = date.today().isoformat()
    L = []
    L.append("<!-- GENERATED by tools/compat/render_compat.py from docs/compat/. "
             "Do not hand-edit. -->")
    L.append("# OVMX Compatibility Surface Register\n")
    L.append(f"> Generated {today} from `docs/compat/` against `origin/main`. "
             "Source of truth is the YAML; this file is regenerated. "
             "See `docs/design-compat-surface-register.md`.\n")
    # dashboard
    rows_in = [r for r in rows if r["scope_1_0"] == "in"]
    overall_in = rollup(rows_in) if rows_in else {"count": 0, "coverage_index": 0.0}
    L.append("## Coverage dashboard\n")
    L.append(f"**{overall['count']} surfaces catalogued** across "
             f"{len(domains['domains'])} domains; **{overall_in['count']} in 1.0 scope**. "
             f"**1.0-scope coverage index {overall_in['coverage_index']}%** "
             f"(all-catalogued {overall['coverage_index']}%; rough weighted index).\n")
    L.append("> ⚠ **What this number is not.** It is coverage of the surfaces this register "
             "*catalogues and marks in-scope* — not a percentage of all of VMS. VMS is vast; "
             "whole areas are out of scope or awaiting a scope call — most **languages** "
             "(Fortran/COBOL/BASIC/Pascal/… — OVMX has only C), DECwindows, and layered "
             "products among them. A high index means \"far along on what we committed to for "
             "1.0,\" never \"nearly all of VMS.\" See the scope line below and the per-domain "
             "figures; `undecided`/`out` surfaces are excluded from the in-scope index but stay "
             "catalogued so the frontier is visible, not hidden.\n")
    L.append("| Status | Count | | Authenticity | Count |")
    L.append("|---|---|---|---|---|")
    auth_rows = list(overall["by_authenticity"].items())
    for i, s in enumerate(STATUS_ORDER):
        a = f"{auth_rows[i][0]}" if i < len(auth_rows) else ""
        an = f"{auth_rows[i][1]}" if i < len(auth_rows) else ""
        L.append(f"| {STATUS_MARK[s]} {s} | {overall['by_status'][s]} | | {a} | {an} |")
    L.append("")
    L.append(f"Legend: {STATUS_MARK['verified']} verified · {STATUS_MARK['implemented']} "
             f"implemented · {STATUS_MARK['partial']} partial · {STATUS_MARK['stub']} stub "
             f"· {STATUS_MARK['designed']} designed · {STATUS_MARK['absent']} absent · "
             "⚠ facade-risk (INV-6/Draper) · ≈ advisory.\n")

    # scope breakdown
    scope_counts = {}
    for r in rows:
        scope_counts[r["scope_1_0"]] = scope_counts.get(r["scope_1_0"], 0) + 1
    L.append("**1.0 scope:** " + " · ".join(
        f"{k}={v}" for k, v in sorted(scope_counts.items())) + "\n")

    # per-domain
    fac_by_domain = {}
    for f in facilities:
        fac_by_domain.setdefault(f["domain"], []).append(f)
    for d in domains["domains"]:
        dfacs = fac_by_domain.get(d["id"], [])
        ditems = [r for r in rows if r["domain"] == d["id"]]
        ditems_in = [r for r in ditems if r["scope_1_0"] == "in"]
        dr = rollup(ditems) if ditems else None
        dr_in = rollup(ditems_in) if ditems_in else None
        L.append(f"## {d['name']}\n")
        L.append(f"_{d['blurb']}_\n")
        if dr:
            in_txt = (f"1.0-scope coverage {dr_in['coverage_index']}% "
                      f"({dr_in['count']} in-scope)" if dr_in
                      else "nothing in 1.0 scope")
            L.append(f"`{bar(dr['by_status'])}`  —  {dr['count']} surfaces catalogued, "
                     f"{in_txt}"
                     + (f", ⚠ {dr['facade_risk']} facade-risk" if dr['facade_risk'] else "")
                     + "\n")
        for f in dfacs:
            items = f.get("items", []) or []
            fr = rollup([{
                "status": it["status"], "authenticity": it["authenticity"]
            } for it in items]) if items else None
            head = f"### {f['facility']} — {f.get('name', '')}"
            L.append(head)
            meta = []
            if f.get("scope_1_0"):
                meta.append(f"scope: {f['scope_1_0']}")
            if f.get("tier"):
                meta.append(f"tier {f['tier']}")
            if f.get("plan_ref"):
                meta.append(f"plan: {f['plan_ref']}")
            if f.get("vms_ref"):
                meta.append(f"ref: {f['vms_ref']}")
            if f.get("last_reviewed"):
                meta.append(f"reviewed {f['last_reviewed']}")
            if meta:
                L.append("<sub>" + " · ".join(str(m) for m in meta) + "</sub>\n")
            if f.get("summary"):
                L.append(f"{f['summary']}\n")
            if fr:
                L.append(f"<sub>{fr['count']} items · coverage {fr['coverage_index']}%"
                         + (f" · ⚠ {fr['facade_risk']} facade-risk" if fr['facade_risk'] else "")
                         + "</sub>\n")
            if not items:
                L.append("_No items catalogued yet._\n")
                continue
            L.append("| | Surface | Kind | VMS | Status | Auth | Scope | Evidence / notes |")
            L.append("|---|---|---|---|---|---|---|---|")
            for it in items:
                mark = STATUS_MARK[it["status"]]
                am = AUTH_MARK.get(it["authenticity"], "")
                ev = it.get("evidence", "")
                note = it.get("notes", "")
                ev_note = " — ".join(x for x in [f"`{ev}`" if ev else "", note] if x)
                L.append(f"| {mark}{am} | `{it.get('id')}` | {it.get('kind')} | "
                         f"{it.get('vms','')} | {it['status']} | {it['authenticity']} | "
                         f"{item_scope(f, it)} | {ev_note} |")
            L.append("")

    # facade-risk index (join to Draper register)
    facades = [r for r in rows if r["authenticity"] == "facade-risk"]
    L.append("## ⚠ Facade-risk index (INV-6 / Draper join)\n")
    if facades:
        L.append("Surfaces that report success without doing the real work, or fake shared "
                 "state per-process. These are bugs; cross-reference "
                 "`docs/draper-faithfulness-register.md`.\n")
        L.append("| Surface | Facility | Status | Evidence / notes |")
        L.append("|---|---|---|---|")
        for r in facades:
            ev_note = " — ".join(x for x in [f"`{r['evidence']}`" if r['evidence'] else "",
                                             r['notes']] if x)
            L.append(f"| `{r['id']}` | {r['facility']} | {r['status']} | {ev_note} |")
    else:
        L.append("_None catalogued._")
    L.append("")

    # uncovered facilities
    L.append("## Not yet catalogued\n")
    if uncovered:
        L.append("Registered facilities with no source file yet (coverage TODO):\n")
        by_dom = {}
        for tok, dom in uncovered.items():
            by_dom.setdefault(dom, []).append(tok)
        for dom, toks in by_dom.items():
            L.append(f"- **{dom}**: {', '.join(sorted(toks))}")
    else:
        L.append("_Every registered facility has a source file._")
    L.append("")
    return "\n".join(L)


def build_json(domains, facilities):
    rows = flat_items(facilities)
    return {
        "meta": {
            "generated": date.today().isoformat(),
            "source": "docs/compat/",
            "note": "Machine export of the OVMX Compatibility Surface Register.",
        },
        "vocab": {
            "status": domains["status"],
            "authenticity": domains["authenticity"],
            "scope_1_0": domains["scope_1_0"],
        },
        "domains": domains["domains"],
        "overall": rollup(rows),
        "overall_in_scope": rollup([r for r in rows if r["scope_1_0"] == "in"]),
        "facilities": [
            {
                "facility": f["facility"],
                "name": f.get("name"),
                "domain": f["domain"],
                "scope_1_0": f.get("scope_1_0"),
                "tier": f.get("tier"),
                "plan_ref": f.get("plan_ref"),
                "vms_ref": f.get("vms_ref"),
                "last_reviewed": str(f.get("last_reviewed", "")),
                "rollup": rollup([{"status": it["status"],
                                   "authenticity": it["authenticity"]}
                                  for it in (f.get("items", []) or [])]),
            }
            for f in facilities
        ],
        "items": rows,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="validate only; non-zero exit on error")
    args = ap.parse_args()

    ctx = Ctx()
    domains, facilities = load_all(ctx)
    if domains is None:
        for e in ctx.errors:
            print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
    uncovered = validate(ctx, domains, facilities)

    for w in ctx.warnings:
        print(f"warning: {w}", file=sys.stderr)
    for e in ctx.errors:
        print(f"ERROR: {e}", file=sys.stderr)
    if ctx.errors:
        sys.exit(2)

    if args.check:
        print(f"OK: {len(facilities)} facilities, "
              f"{sum(len(f.get('items', []) or []) for f in facilities)} items, "
              f"{len(uncovered)} uncovered, {len(ctx.warnings)} warnings.")
        return

    OUT_MD.write_text(render_md(domains, facilities, uncovered, ctx))
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    OUT_JSON.write_text(json.dumps(build_json(domains, facilities), indent=2))
    print(f"wrote {OUT_MD.relative_to(REPO)} and {OUT_JSON.relative_to(REPO)} "
          f"({len(facilities)} facilities, "
          f"{sum(len(f.get('items', []) or []) for f in facilities)} items).")


if __name__ == "__main__":
    main()
