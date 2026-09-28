# The 3-node browser demo as a VISITOR sees it (rd vms-1a1, 2026-09-28)

Field report, 2026-09-27, from the live page: **"vax vms never joined, booted to Username:"** — while
the headless verification of the same deploy (#1324) had passed. So the difference between the harness
and a real visitor *was* the bug. Two separate causes were measured here, and a third condition is
handed to the cluster lane.

Everything below was produced against the fixed bundle served locally in the pod `vms-1a1-verify`
(k3s `ovmx-ci`, 6 CPU / 20 GiB, host load average 5–7), headless Chromium, **no query parameters, cold
cache per run, and nothing typed into any console** unless a file says otherwise.

## 1. `split-brain/` — what the visitor actually hit (the old roster)

`run.log` + the three consoles from a visitor-order run on the shipped configuration. Both OVMX nodes
reach `this node is now a VAXcluster member` naming each other (`0x7c3`, `0x7c4`); the real VAX admits
nobody, and node A never names a third system. The OVMX pair, each with a vote, satisfied quorum
between themselves and founded a cluster of their own — the two of them boot far faster than a KA655
does under pcjs, so clicking the page top-to-bottom hands them the cluster. The old grader always
clicked Node C first and waited minutes, which handed it to the real VAX instead, and so never saw it.

Fixed in `tools/cluster-web-demo/mk_democonfig.py`: **both OVMX nodes are non-voting**, the ordinary
VMS satellite configuration, so only the real VAX can found. `cnxman_quorum_could_found()` refuses a
zero-vote system on its own terms, so the pair *cannot* form a cluster no matter who boots first.

## 2. `probes/` — why the VAX looked like it never joined even when it had

`probe-c5.log` is the decisive one, and it involves **no click and no keystroke**:

| time | Node C's screen | Node C's panel | node A's CNXMAN |
|------|-----------------|----------------|-----------------|
| t=20s…180s | frozen, 211 chars, mid power-on self-test | `top=668` in a 720-high viewport — 52 px above the fold | already naming other systems |
| `iframe.scrollIntoView()` | — | `top=195` | — |
| +10s | **6864 chars**: the whole VMS boot, `Node VAXC (csid 00010001) completed VAXcluster state transition`, `Username:` | in view | — |

The guest was running and clustering the entire time; the **panel was not repainting** while it was
(mostly) outside the viewport — Chromium throttles rendering in an offscreen cross-origin iframe and
the pcjs terminal writes its text on the render tick. `probe-c3.log` shows the same freeze broken by a
single RETURN (238 → 7513 chars), which is why a harness that logs in never saw it.

That is the field report exactly: scroll down to the VAX panel and it shows whatever it repaints on
arrival, with the join history having never been painted. Filed as **rd vms-0bc** — the fix belongs in
the embed (flush the terminal text on a timer, which still runs in a throttled frame), not in the
grader. The grader now reveals a panel before sampling it and counts the times that is what made the
console advance.

## 3. `gate6/` — the six-case visitor matrix on the fixed roster + fixed gate

`summary.json`, `run.log`, and the consoles of the two runs worth keeping.

| case | order | gap | cpu | verdict |
|------|-------|-----|-----|---------|
| page-order | A,B,C | 5 s | ×1 | CN=3 |
| all-at-once | A,B,C | 0.5 s | ×1 | **NOT CN=3** — see below |
| page-order-throttled4 | A,B,C | 5 s | ×4 | CN=3 |
| b-first | B,A,C | 3 s | ×1 | CN=3 |
| c-first-legacy | C,A,B | 5 s | ×1 | CN=3 |
| all-at-once-throttled2 | A,B,C | 0.5 s | ×2 | CN=3 |

`ovmx_founded` is empty in every run: no OVMX node founded anything, in any order, at any speed.
`05-c-first-legacy/` is the case that FAILED before the repaint fix and passes now — the previous
"SPLIT BRAIN" verdict was a throttled screen, not a cluster fact.

### The remaining failure belongs to the cluster lane

`02-all-at-once/VAXC.console.log` carries the real VAX's own words:

```
%SYSINIT, waiting to form or join a VMScluster system
%VAXcluster-I-LOADSECDB, loading the cluster security database
%MSCPLOAD-I-LOADMSCP, loading the MSCP disk server

KA655-B V5.3, VMB 2.7            <-- it restarted here
...
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAXC
%CNXMAN,  completing VAXcluster state transition

KA655-B V5.3, VMB 2.7            <-- and again
```

Three power-on banners in one run: the real VAX restarted **twice**, the second time after it had
already formed the cluster. Node B restarted four times in the same run. This is the all-at-once case —
three emulators starting within a second on a loaded host — i.e. CPU starvation, which is the
visitor-on-a-laptop condition, and every inter-node frame in this demo crosses four main threads. No
bugcheck text survives in the transcript, and it would not be expected to: under the repaint stall
above, whatever the VAX printed between two samples is replaced rather than appended. It is consistent
with the CNXMGRERR bugcheck-and-reboot seen live, and it is **rd vms-8c54 / vms-b36 (cluster lane)** —
not a page defect and not fixed here.

The grader reproduces it on demand: `CASE=all-at-once node visitor-gate.mjs` against the bundle.
Repeated four times on this host (once in the matrix above, three times back to back): **2 CN=3, 2
failures**, both failures carrying guest restarts (`OVMXB restarted 5x; VAXC restarted 2x`) and the real
VAX's own `lost connection to system OVMXB`. The other five cases did not fail once.
