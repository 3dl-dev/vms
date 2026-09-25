# vms-e18e — ⭐ CN=3 **in the browser**: OVMX/x86, OVMX/VAX, and a real OpenVMS VAX V7.3

**2026-09-25, a temporary `ovmx-ci/vms-e18e-cn3b` pod on `k3s-worker`, deleted
after use. Nothing was served from vax.3dl.network; the lab's `vaxlab-*` pods
were never touched.**

One page, one in-page L2 hub, three emulators, no backend:

| node | what it is | identity | image |
|---|---|---|---|
| **OVMXA** | OVMX/x86_64 under **qemu-wasm** | `OVMXA`, 1987, VOTES 1, EXPECTED_VOTES 3 | `build-boot-artifacts` at `184a8206`, injected by `build-cluster-demo` |
| **OVMXB** | **OVMX/VAX** under **pcjs KA655** (NetBSD/vax substrate) | `OVMXB`, 1988, VOTES 1, EXPECTED_VOTES 3 | rebuilt at `184a8206`, sha256 `93580efa…63bee1` |
| **VAXC** | a **real** OpenVMS VAX **V7.3** under pcjs | `VAXC`, 1989, VOTES 1, EXPECTED_VOTES 1 | pinned rd vms-2570 volume, sha256 `45355fd2…4fe15` |

Cluster group **257**, `VAXCLUSTER=2`. `EXPECTED_VOTES=3` on both OVMX nodes is
deliberate and comes from the demo SSOT (`mk_democonfig.py`, updated by #1302):
neither can satisfy quorum on its own vote, so neither can found a cluster, and
reaching MEMBER can only be a real admission by the real VAX.

This is the browser counterpart of rd vms-1ac's lab CN=3 (#1306), and it is not
the same cluster: the lab's third node was a second **OVMX/x86** guest under
QEMU/KVM. Here the third architecture is real — **OVMX/VAX**, a NetBSD/vax
substrate executive, in a JavaScript VAX emulator, in a tab.

## The result — both arrival orders pass (`cn3-pass-CBA/`, `cn3-pass-CAB/`)

`cn3-result.json`:

```json
{"pass": true, "hold_s": 767, "elapsed_s": 2458,
 "announced": {"OVMXA": true, "OVMXB": true},
 "oracle":    {"OVMXA": true, "OVMXB": true},
 "bugchecks": {}, "losses": {}}
```

**Each OVMX node's own `SHOW CLUSTER`, typed at its own console in the page:**

```
View of Cluster from system ID 1987 node: OVMXA    25-SEP-2026 08:18:04
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXA  | 00010003 | VMX V0.7        | MEMBER           |
| 1988   | 00010002 |                 | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |

View of Cluster from system ID 1988 node: OVMXB
| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXB  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
| 1987   | 00010003 |                 | MEMBER           |
```

**The oracle — the real VAX's own CNXMAN and OPCOM** (`VAXC.console.log`), for
BOTH admissions, which is VMS reporting on itself:

```
%CNXMAN,  received VAXcluster membership request from system OVMXB
%CNXMAN,  proposing addition of system OVMXB
          Node VAXC (csid 00010001) proposed addition of node OVMXB
%CNXMAN,  completing VAXcluster state transition

%CNXMAN,  received VAXcluster membership request from system OVMXA
          Node VAXC (csid 00010001) proposed addition of node OVMXA
%CNXMAN,  completing VAXcluster state transition
```

**Stability, 12 m 47 s of it** — three consecutive polls, each one a fresh
`SHOW CLUSTER` on both OVMX consoles, never a cached table:

```
t=1353s members={"OVMXA":3,"OVMXB":3} vaxcTx= 7624 alive=true
t=1736s members={"OVMXA":3,"OVMXB":3} vaxcTx=10201 alive=true
t=2120s members={"OVMXA":3,"OVMXB":3} vaxcTx=12769 alive=true
```

`vaxcTx` is the real VAX's own NIC transmit counter. It matters: in this rig
before #1303, a third cluster participant stopped the VAX transmitting dead
(`tests/lab/captures/vms-e18e-cn3-20260924/`, 1679 frames at t=420 s and 1679
at t=1486 s). Here it climbs by ~2,600 frames per poll interval to the end.

Zero bugchecks; no console logged `lost connection`, `quorum lost` or `was
removed from the cluster`.

## Two things had to be fixed to get a console to talk to at all

Both are in the harness/page layer, not the executive, and both are recorded
because each one reads exactly like a dead guest.

**1. The pcjs page's login-gate marker never fires for an OVMX guest.**
`machines/dec/vax/browser/ovmx-cluster.html` blocks at VMS's "press RETURN to
log in" gate and sends one RETURN when it sees `GATE_READY`, which is
`/Welcome to VAX\/VMS/i` — ground-truthed against the **V5.5** volume. An
OVMX/VAX guest never prints that string, so Node B's console stopped forever at

```
%RUN-S-PROC_ID, identification of created process is 0000006B
```

and never reached `Username:` — in *any* browser run, this session or the
previous one. **A visitor clicking Node B on the live page gets the same dead
console.** The fix is one line, and it is not invented: it is the marker
openvmx-site's own `demo/cluster/node.html` already uses for an OVMX guest
(`LOGIN_READY = /%RUN-S-PROC_ID|JOB_CONTROL/`), added as alternatives:

```js
const GATE_READY = /Welcome to VAX\/VMS|%RUN-S-PROC_ID|JOB_CONTROL/i;
```

Proven here: with that one change to the served copy, Node B reached
`Username:` and logged in on the next run. The edit belongs to the **pcjs**
repo (`baron-3dl/pcjs`) and is NOT made in this repo — it is a prerequisite for
the redeploy, listed with it.

**2. A clustered node's operator lines break LOGINOUT's read.**
While the cluster is up, the executive keeps writing to the same console
LOGINOUT is reading — `%DLM, refusing a lock message from a system that has not
proved it runs this implementation` every time the real VAX sends a lock
message. A login attempt that lands while one of those is being written is
abandoned: the console shows the username echoed, then the login banner and a
fresh `Username:`, and **no `Password:` prompt ever appears**. Real VMS
redisplays the prompt after a broadcast rather than dropping the read; the same
asymmetry cost this harness the VAXC login yesterday, there via OPCOM
(`%LOGIN-F-CMDINPUT`). The grader works around it by re-offering the line on a
short beat until the prompt it should produce appears, and records the cost:

```
OVMXA: reached Password: (after 2 offers)
```

This is a real fidelity gap, filed separately; it is a workaround here, not a
fix.

## Honest scope — what is NOT claimed

* **The join is intermittent in the browser.** Four of six attempts this
  session reached CN=3; two stalled, both with the identical signature, on the
  OVMX/VAX node roughly one second after the peer opened its VMS$VAXcluster
  connection (`cn3-intermittent/`):

  ```
  [  88.69] %CNXMAN, the cluster opened the VMS$VAXcluster connection to this node
  [  89.54] %CNXMAN, lost connection to a cluster member, reconnecting
  [  89.55] %CNXMAN, lost the VMS$VAXcluster connection before this node was admitted
  [  90.56] %CNXMAN, adopting the VMS$VAXcluster connection the executive holds for this member
  [  90.91] %CNXMAN, a cluster member refused this node's reconnect: not asking it again until ...
  [ 110.04] %CNXMAN, reconnect interval expired, proposing removal
  [ 110.06] %CNXMAN, this node has no cluster system id; it cannot coordinate a state transition
  ```

  and then a membership-request loop that never recovers, for the rest of the
  window, while the other two nodes stay a healthy cluster. Every line is
  honest — nothing is fabricated, the node says plainly that it is **not** a
  member — but the recovery never happens. Filed as its own item; **not** fixed
  here.
* **No SDA/`SHOW CLUSTER` read-back on VAXC.** Its `OPA0:` login did not come
  up inside the window (the OPCOM/broadcast problem above, on the V7.3 side
  where this harness has no workaround). The VMS-side evidence is VMS's own
  CNXMAN/OPCOM transcript instead, which is the same oracle rd vms-b34 and rd
  vms-1ac used.
* The `SOFTWARE` column is blank for the two remote nodes and `VMX V0.7` for
  self; that is how OVMX's `SHOW CLUSTER` renders today and is unchanged here.
* `OVMXB`'s table header carries `01-JAN-2010` — the NetBSD/vax guest's TOD
  clock is not set (`WARNING: preposterous TOD clock time` at boot). Cosmetic,
  and not introduced by this run.

## Files

* `cn3-pass-CBA/` — the passing run (arrival order C, B, A): all three
  consoles, the harness transcript, `cn3-result.json`, final screenshot.
* `cn3-pass-CAB/` — the other arrival order (C, A, B), which **also passes**:
  `{"pass": true, "hold_s": 767, "oracle": {"OVMXA": true, "OVMXB": true},
  "bugchecks": {}, "losses": {}}`, with

  ```
  t=1353s members={"OVMXA":3,"OVMXB":3} vaxcTx= 6861 alive=true
  t=1736s members={"OVMXA":3,"OVMXB":3} vaxcTx= 9351 alive=true
  t=2120s members={"OVMXA":3,"OVMXB":3} vaxcTx=11843 alive=true
      OVMXA: ["OVMXA=00010002","1989=00010001","1988=00010003"]
      OVMXB: ["OVMXB=00010003","1987=00010002","1989=00010001"]
  ```

  The CSIDs differ from the other order because the coordinator hands out the
  round-robin CSV slot in arrival order (p. 7-25): A took slot 2 and B slot 3
  here, the reverse of the C,B,A run. Same cluster, same three systems.
* `cn3-intermittent/` — a stalled attempt, kept whole, for the signature above.
* `cn3-browser-grade.js` — the grader. A throwaway probe built on openvmx-site's
  committed `demo/cluster/e2e/e2e-boot.js` page-driving code (the precedent of
  rd vms-2570's `probe-run3` and rd vms-b34's `cn2-grade.js`), kept with the
  capture so the run is reproducible.

## Reproducing

```
# in a temporary k3s-worker pod (playwright image), never vax.3dl.network:
node coi-server.js  <demo bundle dir> 8110 &    # openvmx-site demo/cluster/e2e/coi-server.js
node pcjs-server.js <minimal pcjs tree> 8301 &  # a plain static server, CORP: cross-origin

NODE_B="http://localhost:8301/machines/dec/vax/browser/ovmx-cluster.html?rom=ka655x.bin&diskgz=ovmx-vax-nodeB.img.gz" \
NODE_C="http://localhost:8301/machines/dec/vax/browser/ovmx-cluster.html?rom=ka655x.bin&diskgz=vms73-nodeC-cluster.dsk.gz" \
BOOT_ORDER="C,B,A" STAGGER_MS=300000 HOLD_MS=660000 node cn3-browser-grade.js
```

Two things about the pcjs tree that each cost an hour to find:

* it needs **both** `machines/dec/vax/modules/v2/` and `machines/modules/v2/` —
  `vaxworker.js` imports `../modules/v2/*`, and those import
  `/machines/modules/v2/*`. With only one, the page loads, the screen stays
  blank and the machine reports `Worker error: undefined`;
* `ovmx-cluster.html` needs the `GATE_READY` alternation above, or the OVMX/VAX
  node never reaches a prompt.
