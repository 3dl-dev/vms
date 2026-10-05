#!/usr/bin/env python3
"""render_scoreboard.py - the corpus scoreboard document (vms-44a, roadmap R2).

Single source: the committed measurements and ledgers, never a hand-kept copy:

  tests/conformance/corpus_baseline.json          host column (run_corpus.sh)
  tests/conformance/corpus_runtime_baseline.json  runtime column (guest, live /dev/vms)
  tests/corpus/expected_exit.txt                  designed non-zero exits
  tests/corpus/unreachable.txt                    written reasons, tier-1
  tests/corpus/tiers.txt                          tier-2/3/4/6 status + reasons

Output: docs/corpus-scoreboard.md (generated; never hand-edit).

  render_scoreboard.py            write the document
  render_scoreboard.py --check    exit 1 if the committed document is not what the
                                  sources render, or if a program that is not
                                  running carries no written reason

A tier-1 program is RUNNING when it is run-pass in the runtime column, or (if it
is not in the runtime list) run-pass on the host. A program that fails to
compile or link carries the harness's own diagnostic; every other non-running
program must have a line in unreachable.txt.
"""
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "docs", "corpus-scoreboard.md")


def load_lines(path):
    rows = []
    with open(path) as f:
        for ln in f:
            ln = ln.rstrip("\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            rows.append(ln)
    return rows


def main(argv):
    check = "--check" in argv
    root = ROOT
    for a in argv:
        if a.startswith("--root="):
            root = a.split("=", 1)[1]
    out = os.path.join(root, "docs", "corpus-scoreboard.md")
    host = json.load(open(os.path.join(root, "tests/conformance/corpus_baseline.json")))
    rt = json.load(open(os.path.join(root, "tests/conformance/corpus_runtime_baseline.json")))
    expected = {}
    for ln in load_lines(os.path.join(root, "tests/corpus/expected_exit.txt")):
        p = ln.split(None, 2)
        expected[p[0]] = (p[1], p[2].lstrip("# ").strip() if len(p) > 2 else "")
    reasons = {}
    for ln in load_lines(os.path.join(root, "tests/corpus/unreachable.txt")):
        p = ln.split(None, 2)
        reasons[p[0]] = (p[1], p[2] if len(p) > 2 else "")
    tiers = []
    for ln in load_lines(os.path.join(root, "tests/corpus/tiers.txt")):
        p = ln.split(None, 2)
        tiers.append((p[0], p[1], p[2] if len(p) > 2 else ""))

    errors = []
    tdirs = sorted(d for d in os.listdir(os.path.join(root, "tests/corpus"))
                   if d.startswith("tier") and d != "tier1-examples"
                   and os.path.isdir(os.path.join(root, "tests/corpus", d)))
    listed = {t[0] for t in tiers}
    for d in tdirs:
        if d not in listed:
            errors.append("tests/corpus/%s has no line in tests/corpus/tiers.txt" % d)
    for t in tiers:
        if t[0] not in tdirs:
            errors.append("tests/corpus/tiers.txt names %s, which is not a corpus directory" % t[0])
        if t[1] not in ("running", "not-running"):
            errors.append("tiers.txt: %s status must be running|not-running" % t[0])
        if t[1] == "not-running" and len(t[2].split()) < 4:
            errors.append("tiers.txt: %s is not-running with no written reason" % t[0])

    hp = {p["name"]: p for p in host["programs"]}
    rp = {p["name"]: p for p in rt["programs"]}
    names = sorted(hp)
    rows = []
    n_run = 0
    for n in names:
        h = hp[n]["status"]
        r = rp[n]["status"] if n in rp else "-"
        running = (r == "run-pass") if n in rp else (h == "run-pass")
        if running:
            n_run += 1
            continue
        if n in reasons:
            reason = "%s: %s" % reasons[n]
        elif h in ("compile-fail", "link-fail"):
            if hp[n].get("missing_symbols"):
                syms = []
                for s in hp[n]["missing_symbols"]:
                    s = s.strip("'")
                    if s not in syms:
                        syms.append(s)
                reason = "link: undefined " + ", ".join(syms)
            elif hp[n].get("missing_headers"):
                reason = "compile: missing header " + ", ".join(hp[n]["missing_headers"])
            else:
                errs = [e for e in hp[n].get("errors", []) if "error" in e]
                first = re.sub(r"^\S+?(tier1-examples/)?[^ ]*?:\d+(:\d+)?: ", "", errs[0]) if errs else "compile error"
                reason = "compile: " + first.strip()[:140]
        else:
            reason = ""
            errors.append("tier-1 program %s is %s (host) / %s (runtime) and has no line in tests/corpus/unreachable.txt" % (n, h, r))
        rows.append((n, h, r, reason.replace("|", "/")))

    tot = len(names)
    hs = host["summary"]
    rs = rt["summary"]
    L = []
    L.append("# Corpus scoreboard\n")
    L.append("<!-- GENERATED by tools/corpus/render_scoreboard.py from the committed baselines and")
    L.append("     tests/corpus/{expected_exit,unreachable,tiers}.txt. Do not hand-edit; regenerate. -->\n")
    L.append("Roadmap R2: *it builds and runs every VMS app we can find.* This page is the published scoreboard,")
    L.append("including the programs that do not run and why.\n")
    L.append("## Tier 1 (%d Eight-Cubed example programs)\n" % tot)
    L.append("| column | run-pass | of | measured by |")
    L.append("|---|---:|---:|---|")
    L.append("| host (gcc container, no executive) | %d | %d | `tests/conformance/run_corpus.sh` |" % (hs["run-pass"], host["total"]))
    L.append("| **runtime (guest, live /dev/vms)** | %d | %d (programs that link) | `OVMX_CORPUS_RT=1 tests/qemu/run_tests.sh` + `tests/qemu/corpus_runtime_report.sh` |" % (rs["run-pass"], rt["total"]))
    L.append("")
    L.append("**Running** (run-pass in the runtime column, or on the host for programs not in the runtime list): **%d of %d**." % (n_run, tot))
    L.append("")
    L.append("Host column detail: compile-fail %d, link-fail %d, run-fail %d, run-crash %d; %d host passes printed an unhandled %%E/%%F condition (`run-pass-signaled`)." %
             (hs["compile-fail"], hs["link-fail"], hs["run-fail"], hs["run-crash"], hs.get("run-pass-signaled", 0)))
    L.append("Runtime column detail: run-fail %d, run-crash %d, vm-crash %d, not-run %d." % (rs["run-fail"], rs["run-crash"], rs["vm-crash"], rs["not-run"]))
    L.append("")
    L.append("### Designed non-zero exits\n")
    L.append("These demonstrations exist to end an image with a failing status; they are run-pass when they exit with exactly the documented code (`tests/corpus/expected_exit.txt`).\n")
    L.append("| program | exit | why |")
    L.append("|---|---:|---|")
    for n in sorted(expected):
        L.append("| `%s` | %s | %s |" % (n, expected[n][0], expected[n][1].replace("|", "/")))
    L.append("")
    L.append("### Not running (%d)\n" % len(rows))
    L.append("| program | host | runtime | reason |")
    L.append("|---|---|---|---|")
    for n, h, r, reason in rows:
        L.append("| `%s` | %s | %s | %s |" % (n, h, r, reason))
    L.append("")
    L.append("## Other tiers\n")
    L.append("| directory | status | note |")
    L.append("|---|---|---|")
    for d, st, txt in tiers:
        L.append("| `%s` | %s | %s |" % (d, st, txt.replace("|", "/")))
    L.append("")
    doc = "\n".join(L)

    if errors:
        for e in errors:
            print("render_scoreboard: " + e, file=sys.stderr)
        return 1
    if check:
        cur = open(out).read() if os.path.exists(out) else ""
        if cur != doc:
            print("render_scoreboard: docs/corpus-scoreboard.md is stale -- run tools/corpus/render_scoreboard.py", file=sys.stderr)
            return 1
        print("corpus scoreboard: current (%d running of %d; every non-running program carries a reason)" % (n_run, tot))
        return 0
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write(doc)
    print("wrote %s (%d running of %d, %d with reasons)" % (os.path.relpath(out, root), n_run, tot, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
