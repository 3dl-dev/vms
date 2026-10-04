# vms-e18e — CN=3 on the LIVE public page at V0.7-2

**2026-09-26. `https://openvmx.3dl.dev/demo/cluster/` in headless Chromium with
NO query overrides**, so what is graded is the deployed defaults a visitor gets.
Node B and Node C were fetched from `vax.3dl.network`; nothing was served
locally.

V0.7-2 (`4511c8a7`) is the patch release whose subject is precisely this demo's
failure mode — a real OpenVMS VAX peer bugchecking CNXMGRERR under LAN jitter
(#1316 / #1320 / #1321).

## Result

```json
{"pass": true, "hold_s": 999, "elapsed_s": 2938,
 "announced": {"OVMXA": true, "OVMXB": true},
 "oracle":    {"OVMXA": true, "OVMXB": true},
 "bugchecks": {}, "losses": {}}
```

Each OVMX node's own `SHOW CLUSTER`, typed at its own console in the page:

```
View of Cluster from system ID 1987 node: OVMXA    26-SEP-2026 08:06:46
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

The oracle — the real VAX's own CNXMAN/OPCOM, for BOTH admissions:

```
%CNXMAN,  received VAXcluster membership request from system OVMXB
%CNXMAN,  proposing addition of system OVMXB
          Node VAXC (csid 00010001) proposed addition of node OVMXB
%CNXMAN,  completing VAXcluster state transition
%CNXMAN,  received VAXcluster membership request from system OVMXA
          Node VAXC (csid 00010001) proposed addition of node OVMXA
%CNXMAN,  completing VAXcluster state transition
```

**16 m 39 s sustained**, three consecutive polls, each a fresh `SHOW CLUSTER` on
both OVMX consoles, with the real VAX's own NIC transmit counter climbing:

```
t=1484s members={"OVMXA":3,"OVMXB":3} vaxcTx= 8623 alive=true
t=1984s members={"OVMXA":3,"OVMXB":3} vaxcTx=11945 alive=true
t=2483s members={"OVMXA":3,"OVMXB":3} vaxcTx=15273 alive=true
```

Zero bugchecks; no `lost connection` / `quorum lost` / `was removed from the
cluster` on any console.

## Two harness lessons from #1310's (correct) login deadline

#1310 gave LOGINOUT a real VMS read deadline — ~20 s, announced
("Error reading command input" / "Timeout period expired"). That is faithful, and
it broke this harness twice:

* A slow re-offer beat **straddles** the deadline and splits the line: the
  console showed `Username: SYSTE` … `M`. Offers must sit well inside the window
  (2 s here).
* Once an offer lands, `Password:` never appears again — so a loop that only
  watches for `Password:` keeps typing the username **at the `$` prompt**.
  Measured: 187 offers and a console full of
  `%DCL-E-IVVERB, unrecognized command verb` while the node had been logged in
  the whole time. The login must **ask whether it is already at DCL first**.

## An observation for the cluster lane (not a claim about correctness)

Restarting the harness mid-cluster (killing the browser while the three nodes are
members) left the next run's Node B refused by the new incarnation logic:

```
%CNXMAN, the cluster rejected this node's connection (version id …)
%CNXMAN, this node has already re-incarnated and the cluster still …
```

That is #1320 doing what it says. Whether the joiner should recover on its own
after the reconnect interval, rather than staying out for the rest of the
window, is worth a look — but it only reproduced after an unclean teardown, and
a first run after a fresh page load has joined every time.

## Honest scope

* Node C's `OPA0:` login did not come up inside the window, so the VMS-side
  evidence is VMS's own CNXMAN/OPCOM rather than an SDA `SHOW CLUSTER` — the same
  oracle rd vms-b34 and rd vms-1ac used.
* `OVMXB`'s table header reads `01-JAN-2010`: the NetBSD/vax guest's TOD clock is
  unset (`WARNING: preposterous TOD clock time` at boot). Cosmetic, pre-existing.
* One passing run at this tag.
