# rd vms-c6e leg 2 — the DRIVEN held-out run

**The predictions in `predicted.tsv` are committed and pushed BEFORE the VAX is
asked to lock anything. The commit timestamp is the proof.** Nothing in this
directory is a capture yet.

Leg 1 of rd vms-c6e is done (253/253 on rows frozen before the derivation) and
so is an unplanned forward prediction (533/533 on `m3-soledir`, a capture
written 16 minutes after the function was frozen — `docs/design-dlm-name-hash.md`
§4a). Both are **VMS's own traffic**, so both only ever exercise the names VMS
happens to lock. This run exists to drive the cases VMS never does.

## What is being driven, and what each part proves

61 names, 81 pre-registered `(name, mode, group)` triples.

| block | names | flavour | what it closes |
|---|---|---|---|
| length sweep | 31 | user + `LCK$M_SYSTEM` | **every** length 1..31. The corpus has no 1, 2, 4, 6, 9, 19, 20, 23, 28, 29 or 31 at all, and length is the one input that also feeds the descriptor longword (`name_len << 24`) |
| byte-value stress | 15 | user + `LCK$M_SYSTEM` | digits, `$` runs, underscores, two punctuation runs, lower case, trailing spaces, embedded NULs, control bytes `01 02 1F 7F`, high-bit bytes `80 81 FE FF`, 31×`FF` (every name bit set), 31×`NUL` (every name bit clear), a leading `A2` as the real `LMF$` names carry, alternating `55 AA`, and one high bit alone in a longword |
| mode | 5 | **exec** + `LCK$M_SYSTEM` | the mode byte at 1 instead of 3, driven rather than observed |
| group | 5 + 5 | user + group, **exec** + group | the group word off 0, under three UIC groups — 1, 300 and 16382 — which between them exercise group bits 0..13. The whole corpus only ever showed group 0 and group 1 |

**51 of the 81 triples sit at group 0 and do not depend on any model of how
VMS qualifies a group lock.** They are the headline. The other 30 pre-register
the *recipe*: if the wire shows a group the recipe did not ask for, the scorer
reports it as `UNREGISTERED` and prints what the function says for it — a
finding about group semantics, never scored as a win.

## Files

| file | what |
|---|---|
| `names.tsv` | the 61 names, each with its lock flavour and the mode/group that flavour is expected to produce |
| `predicted.tsv` | the 81 pre-registered triples and the value **this tree predicts** for each |
| `C6EDRV.MAR` | the MACRO-32 driver, 305 lines, with the name table assembled **in** |
| `C6EDRV.COM` | `MACRO` + `LINK` + `RUN` |

All four are **generated** by `tools/cluster/dlm_hash/gen_heldout_run.py`; the
ctest `dlm_hash_heldout_run_predictions` fails if any of them has drifted, so
the names the VAX locks cannot diverge from the names that were predicted.
`cluster_host_test_dlm_hash` separately checks all 81 predictions against the
**C** implementation (`src/kernel-core/vms_dlm_hash.c`), so the number the lab
is scored against is the product's own function.

The table is assembled into the `.MAR` rather than read from a data file on
purpose: the set contains NUL, control and high-bit bytes, and a data file
would have to survive FTP, an RMS record format and a record terminator before
the VAX ever locked anything. A `.BYTE` list is plain ASCII all the way to the
assembler.

## Run recipe

Wire setup (lab side): `VAX1`/`VAX2` `LOCKDIRWT 0`, `OVMXE` `LOCKDIRWT 1`, so
every VAX root lookup goes to OVMX and is captured. Start the capture before
pass 1 and leave it running through pass 3.

### Transfer

FTP `C6EDRV.MAR` and `C6EDRV.COM` to `VAX1` in **ASCII** mode (both are plain
7-bit text — every non-ASCII name byte is a decimal `.BYTE` literal, so nothing
depends on an 8-bit-clean path). Then check they arrived intact:

```
$ DIRECTORY/SIZE=ALL C6EDRV.*
$ SEARCH C6EDRV.MAR "NREC ="          ! must print:  NREC = 61
$ SEARCH C6EDRV.MAR ".END"            ! must print:  .END C6EDRV
```

| file | lines | bytes | sha256 |
|---|---|---|---|
| `C6EDRV.MAR` | 305 | 10458 | `900de5efcc29b0828a3135367c731fced9e825d3f86e335d70e7e2c6d19b2658` |
| `C6EDRV.COM` | 16 | 558 | `5c6923af23a4d70ba4e418e00d24964da6fb8a8a4d29c7fed79d908dad738751` |

### Pass 1 — SYSTEM, all flavours (46 user+SYSTEM, 5 exec+SYSTEM, and the
group names under group 1)

From `SYSTEM` (UIC `[1,4]`), which has **SYSLCK** (needed by `LCK$M_SYSTEM`)
and **CMEXEC** (needed by the exec flavours):

```
$ @C6EDRV
```

It assembles, links, shows its privileges and UIC for the record, locks all 61
names, holds them 10 seconds, then releases them. While it holds, from another
terminal:

```
$ ANALYZE/SYSTEM
SDA> SHOW RESOURCE/NAME=C6E03
SDA> SHOW RESOURCE/NAME=C6EX01
```

### Pass 2 — second UIC group (300)

```
$ MCR AUTHORIZE
UAF> ADD C6E300 /PASSWORD=C6E300PW /UIC=[300,1] /PRIV=(CMEXEC,TMPMBX,NETMBX) -
UAF> /DEFPRIV=(CMEXEC,TMPMBX,NETMBX) /DEVICE=SYS$SYSDEVICE /DIRECTORY=[C6E300]
UAF> EXIT
$ CREATE/DIRECTORY SYS$SYSDEVICE:[C6E300]/OWNER=[300,1]
$ COPY C6EDRV.MAR,C6EDRV.COM SYS$SYSDEVICE:[C6E300]
$ SET FILE/OWNER=[300,1] SYS$SYSDEVICE:[C6E300]C6EDRV.*
```

Then log in as `C6E300` (a real login, so the process UIC really is `[300,1]`)
and run the **group-only** pass — it skips the two `LCK$M_SYSTEM` flavours, so
no SYSLCK is needed:

```
$ @C6EDRV GRP
```

### Pass 3 — third UIC group (16382), optional but it is the one that moves
the high group bits

Same as pass 2 with `/UIC=[16382,1]` and account `C6E16382`. `16382` is the
top of the VMS UIC group range, so between groups 1, 300 and 16382 the group
word is exercised at bits 0..13 instead of only bit 0.

If you skip pass 3, its 10 triples come back `ABSENT`, which the scorer reports
plainly — it does not turn into a pass.

### Score it

```
tools/cluster/dlm_hash/check_heldout_run.py \
    --rundir tests/lab/captures/vms-c6e-heldout-20261008 \
    --pcap   <the capture>
```

It re-verifies the static half first (artifacts undrifted, predictions are the
reference hash, reference hash still reproduces 8 real captured values), then
sorts every cat-0x02 op-0x01/op-0x0d ROOT request a **non-OVMX** sender made
into four buckets:

| bucket | meaning |
|---|---|
| `MATCH` | pre-registered triple, wire value is the predicted one |
| **`MISMATCH`** | pre-registered triple, wire value is **not** the predicted one — **the only failure, and the one that would retire the function** |
| `UNREGISTERED` | the wire carried a triple the recipe did not pre-register (so the flavour's mode/group model was wrong); printed with what the function says for it, never scored |
| `ABSENT` | a pre-registered triple never reached the wire — wrong privilege, wrong pass, or the lookup never left the node |

Exit status is nonzero only on `MISMATCH`.

## Privileges, and what fails without them

| flavour | needs | if missing |
|---|---|---|
| user + `LCK$M_SYSTEM` | `SYSLCK` | `$ENQ` returns `SS$_NOPRIV`, the name never reaches the wire, scorer says `ABSENT` |
| exec + anything | `CMEXEC` | `$CMEXEC` returns `SS$_NOPRIV`, same |
| user/exec + group | none beyond `CMEXEC` for the exec one | — |

The driver counts only the locks it actually got (`NGOT`) and releases exactly
those, so a privilege failure degrades to `ABSENT` rows rather than to a wrong
answer.

## What this run still does not cover

* **Supervisor mode (mode 2).** There is no `$CMSUPER`-style path a user image
  can take to issue an `$ENQ` in supervisor mode, so mode 2 stays unobserved.
  Modes 0 (kernel, 913 corpus rows), 1 and 3 are covered.
* **Group bits 14 and 15.** The VMS UIC group range tops out at `37776` octal
  (16382), so a 16-bit group word's top two bits cannot be reached through a
  UIC at all. Whether anything else can set them is unknown.
* **Sub-resources.** Out of scope by construction: a sub-resource's value is a
  property of its parent too (rd vms-4fb finding 3).

## The run (2026-10-09, evacuation lane, pod vaxlab-3)

Real VAX1 (1025) + VAX2 (1026), V7.3, group 1, both LOCKDIRWT 0; OVMXE (main
5b61777e boot artifacts) LOCKDIRWT 1, so every VAX root lookup and every op-0x0d
registration went to OVMXE and is on `run-20261009-vaxlab3.pcap` (all 0x6007
frames on the pod bridge, capture started before pass 1). Predictions were
committed at 28b06f50f before the run.

* Pass 1 as SYSTEM `[1,4]`; pass 2 as C6E300 `[454,1]`; pass 3 as C6E16382
  `[37776,1]`. VMS UIC numbers are OCTAL: 454 octal = group 300 and 37776 octal
  = group 16382, the decimal groups this run pre-registered. All three passes
  ended `C6EDRV status: %X00000001`.
* Score (`check_heldout_run.py`, `run-20261009-score.txt`): pre-registered 81 ->
  **MATCH 55, MISMATCH 0**, ABSENT 26 (18 of them the mode-3 group-0 flavour).
  Unregistered root values seen on the same wire: **541, every one matching the
  function**.
* Open: why the 26 ABSENT triples never reached the wire (the driver's own
  NGOT count was not printed on the console) -- the next run should print it.
