#!/usr/bin/env python3
"""negctl_shard.py -- assign per-facility negctl defects to CI shards by cost.

usage:
  negctl_shard.py pick --index I --total N [--costs FILE] < defect-names
      print the defects shard I of N runs (one per line)
  negctl_shard.py costs-from-logs LOG... > tools/ci/negctl_costs.tsv
      measure each defect's wall time from per-facility negctl job logs
      (`gh api repos/<o>/<r>/actions/jobs/<id>/logs`) and print the cost table

The partition is a PURE FUNCTION of (defect list, N, cost table): every shard
computes the same assignment, each defect lands on exactly one shard, and the
union is the whole input -- coverage is unchanged, only spread. It is greedy
longest-processing-time (LPT): defects in descending cost order (ties by name),
each onto the currently least-loaded shard (ties by lowest index).

The cost table is a BALANCE HINT, not a record: a defect missing from it costs
the table's median, so a new or renamed defect still runs (somewhere), and a
stale table only makes the shards less even -- it can never drop a defect.
Refresh it from a nightly run's logs when the shard walls drift apart.

Why: the old `NR % N` round-robin put the slow whole-VM defects (6-12 min
each, against a 2 min median) together by name order; measured on merge-group
run 42122832068, 40 round-robin shards finished between 18 and 41 min and the
41 min shard was the merge queue's critical path. LPT over the same costs
gives ~26 min at 40 shards and ~20 min at 64.
"""
import argparse
import datetime
import re
import statistics
import sys


def load_costs(path):
    costs = {}
    if not path:
        return costs
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, secs = line.split("\t")[:2]
        costs[name] = float(secs)
    return costs


def assign(names, total, costs):
    """[[names of shard 0], ..., [names of shard total-1]]"""
    if total < 1:
        raise ValueError("total must be >= 1")
    names = sorted(set(n for n in names if n))
    default = statistics.median(costs.values()) if costs else 1.0
    cost = {n: costs.get(n, default) for n in names}
    load = [0.0] * total
    shards = [[] for _ in range(total)]
    for n in sorted(names, key=lambda n: (-cost[n], n)):
        i = min(range(total), key=lambda k: (load[k], k))
        load[i] += cost[n]
        shards[i].append(n)
    return [sorted(s) for s in shards]


def costs_from_logs(paths):
    mark = re.compile(r"--- negative control: (\S+) ---")
    out = {}
    for p in paths:
        lines = []
        for l in open(p, errors="replace"):
            try:
                t = datetime.datetime.fromisoformat(l[:26])
            except ValueError:
                continue
            lines.append((t, l))
        if not lines:
            continue
        marks = [(t, m.group(1)) for t, l in lines for m in [mark.search(l)] if m]
        end = lines[-1][0]
        for i, (t, n) in enumerate(marks):
            nxt = marks[i + 1][0] if i + 1 < len(marks) else end
            out[n] = (nxt - t).total_seconds()
    return out


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pick")
    p.add_argument("--index", type=int, required=True)
    p.add_argument("--total", type=int, required=True)
    p.add_argument("--costs")
    c = sub.add_parser("costs-from-logs")
    c.add_argument("logs", nargs="+")
    a = ap.parse_args()
    if a.cmd == "pick":
        if not 0 <= a.index < a.total:
            sys.exit("negctl_shard: index %d out of range for %d shards" % (a.index, a.total))
        shards = assign(sys.stdin.read().split(), a.total, load_costs(a.costs))
        for n in shards[a.index]:
            print(n)
    else:
        costs = costs_from_logs(a.logs)
        print("# defect\tseconds -- negctl shard balance hint (tools/ci/negctl_shard.py);")
        print("# regenerate: negctl_shard.py costs-from-logs <per-facility negctl job logs>")
        for n in sorted(costs):
            print("%s\t%d" % (n, round(costs[n])))


if __name__ == "__main__":
    main()
