# The VMS DLM resource-name hash, determined black-box from the wire

**rd vms-9c95 (corpus) → vms-66fe (derivation) → vms-c6e (held-out proof).**
Authorised by Baron's ruling on **rd vms-dc2**, 2026-10-08.

## 1. What was allowed, and what was used

Every `$ENQ` for a root resource in a VMScluster must reach that resource's
**directory node**, `vector[(value >> 16) mod n]`. The vector is published
(Davis, *VAXcluster Principles*, pp. 6-31..6-33; implemented in
`src/kernel-core/vms_dlm_ldwv.c`). The hash **value** is not: Davis describes
it only abstractly (p. 6-49). So until this work OVMX never computed it — it
learned the value off the wire for names a VMS node had already looked up, and
for a name OVMX touched *first* it had no value and mastered locally, which is
the data-integrity hole rd vms-dc2 exists to close.

Baron's ruling extends Rule 8's scope exactly far enough:

> OVMX may determine the DLM resource-name hash **black-box from (name, value)
> pairs VMS itself broadcasts in the clear on the cluster wire**. NEVER from
> binaries, disassembly or source.

**The only input used was that corpus.** No VSI/HPE binary, listing,
disassembly or source was read; no web search for VMS source or listings of
this routine was made. The inputs are:

| input | what |
|---|---|
| `tests/cluster/fixtures/dlm_hash_corpus.tsv` | 1216 unique `(name, mode, group) -> value` pairs, each with the pcap + frame it was first seen in |
| `tests/cluster/fixtures/dlm_hash_derivation.tsv` | the 963 rows the derivation was allowed to look at |
| `tests/cluster/fixtures/dlm_hash_heldout.tsv` | the 253 rows it was not, chosen by a seeded rule and **frozen in the corpus commit before derivation began** |
| `tests/lab/captures/vms-4fb-dir-hash-20261004/README.md` | where the value rides (`body[128:132]` of a cat-0x02 op-0x01 ROOT request) and that the directory index is its high 16 bits mod n |
| `src/kernel-core/vms_cluster_codec_dlm.h` | the already-grounded field map of the identity block (`struct vms_dlm_res_ident`) |

Provenance and sender filtering (only positively-identified non-OVMX senders;
OVMX values would be circular) are in
`tests/cluster/fixtures/README-dlm-hash-corpus.md`.

One pre-split disclosure, recorded for audit: 25 keys were printed on screen
while locating the value in `L1.pcap`, *before* the split rule was written.
They are listed in `dlm_hash_prestudy_names.tsv` and `split.py` forces them
into the derivation split, so no held-out row had ever been looked at.

## 2. The answer

```c
acc = 0;
acc = ROTL32(acc ^ (group | (mode << 16) | (name_len << 24)), 9);
for (each little-endian longword w of the name, zero-padded to a multiple of 4)
        acc = ROTL32(acc ^ w, 9);
value = (uint32_t)(acc * 0xA53F19B7);
```

The hashed object is the resource-identity block **exactly as it rides the
wire**: `body[44:46]` group, `body[46]` access mode, `body[47]` name length,
`body[48:48+len]` name — i.e. longword 0 is `body[44:48]` read
little-endian and the rest are the name's longwords. Implementation:
`src/kernel-core/vms_dlm_hash.c`.

Two constants are data: the rotate **9** and the multiplier **0xA53F19B7**.

## 3. How it was found — every hypothesis, and how it was eliminated

The method was **structural probing on one-variable differences**, never a
search over candidate algorithms. All numbers below are from the derivation
split alone.

### 3.1 Is the value a function of the name at all?

18736 pairs of corpus names differing in exactly one byte were enumerated. For
each `(length, position)` the union of changed output bits was computed. Result:
no `(name, mode, group)` key ever carried two values (across two VMS versions,
four senders, 18 captures, 16102 requests) → the value is a pure function of
the identity block. **Kept.**

### 3.2 Is it GF(2)-linear (a CRC / XOR-shift family)?

Eliminated. In the family `CACHE$cmSYSDSK1␣␣␣␣␣<b>\x01\x00\x00` the single-bit
difference `0x02 ^ 0x03` changes the value by `0x82CFB200`, while
`0x04 ^ 0x05` — the *same* input bit — changes it by `0x825DF200`. A
GF(2)-affine function of the byte must give the same XOR for both.
**Carries are present; no CRC variant can fit.**

### 3.3 Is it additive in the byte (`h = h*m + b`, Adler/Fletcher, polynomial)?

Eliminated. Per-bit weights were extracted from families with up to 86 distinct
values of one byte. For `F11B$aSYSDSK1␣␣␣␣␣<b>\x00\x00\x00` they are

```
bit0 94fc66dc  bit1 29f8cdb8  bit2 ac0e6490  bit3 a7e336e0
bit4 b0399240  bit5 9f8cdb80  bit6 c0e64900  bit7 81cc9200
```

exactly `s_i · 2^i · 0x94fc66dc` with **mixed signs** `+ + − + − + − −`. A
function affine in `b` cannot have per-bit weights of opposite sign. The sign
pattern is, however, exactly what `U·(b ⊕ c)` produces: flipping a bit of `b`
*adds* `U·2^i` where `c_i = 0` and *subtracts* it where `c_i = 1`. Fitting
`value = A + U·(b ⊕ c)` reproduced **every** value in each family (80, 86, 72,
71, 36, 35 values; 0 residuals). **So the byte is XORed into state, and what
follows is affine over Z/2³².**

### 3.4 Is the state a multiply-chain (`h = (h ⊕ b) · m`)?

Eliminated. Such a chain forces `U_p = ±m^d` (`d` = steps after position `p`),
so `v2(U_p)` must be a multiple of `v2(m)·d`. Measured `U` had
`v2 ∈ {1, 2, 9, 10, 17, 18, 19, 22, 23, 25, 26}` at `d ∈ {0,1,2,3,5,6,12,21,22}`
— e.g. `d = 0` gave `v2 = 9` for one length and `v2 = 1` for another, and
`d = 3` gave both 2 and 9. No constant `m` fits. The same arithmetic kills
`h = h·m ⊕ b` and the reverse-order variants.

### 3.5 Is `U` a shift of one master constant?

**Kept — this was the break.** All eleven independently measured `U` values are
`± 2^e · V` for the single odd constant

```
V = 0x253f19b7            (the low 31 bits of the multiplier)
U(8,7)   = V·2^1    U(22,18) = V·2^2    U(5,4)  = V·2^9    U(24,20) = V·2^9
U(22,19) = V·2^10   U(15,13) = V·2^17   U(24,21)= V·2^17   U(15,8)  = V·2^18
U(13,7)  = V·2^19   U(26,4)  = V·2^22   U(13,9) = V·2^26
```

A single-bit input change therefore moves the value by exactly `±(V << e)`.
That is the signature of **one final 32-bit multiply** over an accumulator in
which each name bit occupies exactly one bit position: `e` is that position.

Re-fitting every family under the constraint `U ∈ {±(V<<e)}` made **all 18**
`(length, position)` groups determine `e` uniquely, with no ambiguity left.

### 3.6 Where does each name bit land? (`e` as a function of length and position)

`e` is not affine in the position — within length 22 it steps `+8` per byte
(`p18 → 2`, `p19 → 10`) but within length 30 it steps `−1`
(`p7 → 23`, `p8 → 22`). Splitting `p` into a **byte lane** `p mod 4` and a
**longword index** `p div 4` resolved it: every one of the 18 measurements
satisfies

```
e(L, p) = [ 8·(p mod 4) + 9·(ceil(L/4) − (p div 4)) ] mod 32
```

The `8·(p mod 4)` term says the name is consumed as **little-endian 32-bit
longwords**; the `9·(…)` term says the accumulator is **rotated left 9 bits
once per longword**, with one extra rotation beyond the name's longwords — i.e.
the loop rotates *after* XORing (`acc = ROTL(acc ^ w, 9)`), and there is one
longword ahead of the name. **Kept.**

### 3.7 Verifying the accumulator really is a bit permutation

In the two-byte-varying family `CACHE$cmSYSDSK1␣␣␣␣␣<b20><b21>\x00\x00` (320
cells), every one of the **561** closable rectangles satisfied
`acc(x₁,y₁) ⊕ acc(x₁,y₂) ⊕ acc(x₂,y₁) ⊕ acc(x₂,y₂) = 0`, where
`acc = value · V⁻¹`. The accumulator is exactly GF(2)-affine in the name bits.
**Kept.**

### 3.8 The 32nd bit of the multiplier, and the extra longword

With `acc = value · V⁻¹` and `V = 0x253f19b7`, the residual
`acc ⊕ fold(name)` was constant per `(length, mode, group)` **except for bit
31**, on a varying subset of names. The explanation is structural, not
statistical: bit 31 of `V` is **invisible** to every `U` measurement, because
`U = V<<e` loses it for every `e ≥ 1` and no family had `e = 0`. Retesting with
`V = V + 2^31 = 0xA53F19B7` made the residual **exactly constant for all 963
derivation rows in all 28 `(length, mode, group)` groups.** **Kept.**

The residuals then decoded the longword ahead of the name. With
`descriptor = ROR(residual, 9·(J+1))`, `J = ceil(L/4)`:

| row | residual | decoded descriptor |
|---|---|---|
| L=16, mode 0, group 0 | `00000200` | `10000000` = len 16 |
| L=22, mode 0, group 0 | `0b000000` | `16000000` = len 22 |
| L=24, mode 0, group 0 | `0c000000` | `18000000` = len 24 |
| L=30, mode 3, group 0 | `00003c06` | `1E030000` = len 30, mode 3 |
| L=30, mode 3, group 1 | `00023c06` | `1E030001` = len 30, mode 3, group 1 |

i.e. `descriptor = group | (mode << 16) | (name_len << 24)` — which is
precisely `body[44:48]`, the identity block's own first longword, read
little-endian. The hash is computed over the identity block as it stands, from
`body[44]`, nothing prepended and nothing invented.

Zero padding of the final partial longword is **measured, not assumed**:
lengths 3, 5, 7, 10, 11, 13, 14, 15, 17, 18, 21, 22, 25, 26, 27 and 30 — every
residue of `L mod 4` — reproduce exactly with zeros.

## 4. Results

| set | rows | reproduced |
|---|---|---|
| derivation split (the only evidence used) | 963 | **963 (100%)** |
| **held-out split** (frozen before derivation, unseen until the function was final) | 253 | **253 (100%)** |
| whole corpus | 1216 | **1216 (100%)** |
| **forward prediction** — `m3-soledir`, a capture that did not exist when the function was frozen | 533 | **533 (100%)** |

The corpus spans **two independent OpenVMS VAX versions** — `VAX1`/`VAX2`/`VAX3`
at **V7.3** and `VAXC` at **V5.5-2H4** — and all 242 `VAXC` rows reproduce, so
the function is not version-specific within that range.

Test: `ctest -R cluster_host_test_dlm_hash` (`tests/cluster/host/test_dlm_hash.c`).
The split's integrity is itself gated: `ctest -R dlm_hash_corpus_split_frozen`
(`tools/cluster/dlm_hash/check_corpus.py`) fails if the corpus is re-split,
de-duplicated differently, or an OVMX-sourced row appears in it.

## 4a. The forward prediction, and what it independently confirms

The function was committed at **2026-10-08T20:41:38Z** (`99ce6ad6e`). The lab
rig then wrote `/lab/run-evac/runs/m3-soledir/wire.pcap` at **20:57:15Z** —
sixteen minutes later, on a different rig, a different cluster group, with
`VAX1`/`VAX2` at `LOCKDIRWT 0` and the OVMX node (`OVMXE`) at `LOCKDIRWT 1`, so
the VAX masters registered their roots *to* OVMX. Nothing was re-fitted; the
committed constants were simply run against it.

```
sha256 af9437c0c5a21e51827ab9af08b03a776dac9ab6e3316511bc7386d31770c165
       run-evac/runs/m3-soledir/wire.pcap
```

**533 unique keys, 533 reproduced, zero conflicts — and 84 of those keys appear
in no other fixture here.** Fixture: `dlm_hash_predicted_m3soledir.tsv`.

Two things this run establishes that the corpus could not:

* **op-0x0d carries the same identity block and the same value.** 479 of 479
  VAX-originated op-0x0d directory-registration records reproduce exactly, so
  the `body[44:48]` descriptor + name + `body[128:132]` reading is grounded on
  the registration shape as well as the lookup shape. `extract.py --ops 01,0d`
  is how the fixture was cut; the default stays `01` so the original corpus
  re-extracts byte-for-byte.
* **Where the identity block is NOT coherent, measured rather than assumed.**
  Taken over *every* cat-0x02 root-named VAX frame in that capture, the
  predictions are: op-0x01 261/261, op-0x03 1/1, op-0x04 25/25, op-0x0d
  479/479, op-0x0e 4/4, op-0x0f 5/5, op-0x10 4/4, op-0x12 1/1 — and op-0x07
  6/8, op-0x06 0/1. The three misses are not the function failing; they are the
  hazard `vms_cluster_codec_dlm.h` warns about. Their name spans are visibly
  half-overwritten (`F11B\x00\x00…1␣␣␣␣␣*\x00\x00\x00`), while the hash field
  is a correct hash of a REAL resource: frame 5514's `0x48501b6d` is exactly the
  value of `F11B$aSYSDSK1␣␣␣␣␣*\x00\x00\x00`, and frame 8258's `0xfccdecd3` is
  exactly `DMT$_$2$DUA1:` — both already in the corpus from other captures. So
  an op-0x06/op-0x07 body's `body[48]` is stale buffer, exactly as the codec
  header says, and only op-0x01 and op-0x0d are trusted as name carriers.

## 5. What is still NOT proven, and what does not change yet

* **The EDGE CASES are still unexercised.** The `m3-soledir` run above is a
  genuine forward prediction, but it is VMS's own traffic, so it exercises only
  the names VMS happens to lock. rd vms-c6e leg 2 still owes a *driven* run:
  predicted values written down for brand-new names (1 character, 31
  characters, non-alpha, each access mode, nonzero group), timestamped, then
  `$ENQ`ed by a real V7.3 VAX with `LOCKDIRWT` set so the lookups cross the
  wire. That is what would exercise the mode and group bits below.
* **Coverage limits of the corpus, stated honestly.** Observed access modes are
  0, 1 and 3 (bits 0-1 of the mode byte); observed groups are 0 and 1 (bit 0 of
  the group word). Lengths 1, 2, 4, 6, 9, 19, 20, 23, 28, 29 and 31 do not
  appear. The field placement of mode and group is grounded by the decoded
  descriptor above, but the high bits of each are extrapolation from the
  layout, not measurement — which is exactly what the fresh-lab edge-case run
  is for.
* **IT ROUTES NOW (rd vms-b5b0, 2026-10-09), and the coverage above is what
  bounds it.** `vms_lock.c` `dir_resolve` computes the value for a root
  resource this node is the first to touch and resolves the directory node
  through the weight vector in every LOCKDIRWT configuration — a real VAX
  directory node included. For an identity OUTSIDE the proven coverage
  (supervisor mode; a UIC group with bit 14/15 set; a name of 23 or 29 bytes)
  it builds NO FRAME and consults no vector, and the resource is **mastered on
  that node only** — counted (`vms_lock_dlm_dir_hash_uncovered()`) and
  announced on the console once. It is deliberately **not** refused to the
  caller: that refusal reached a booted node's ACP and killed STARTUP.COM
  (PR #1578's lab run), and Baron's ruling on rd vms-dc2 names refusing
  ("option B") as the one answer never to give.
  `vms_dlm_name_hash_proven()` is the only entry point routing may use
  (enforced by `tools/ci/cluster_dlm_hash_gate.sh`), and
  `vms_dlm_name_hash()` stays ungated solely so tests and capture scorers can
  evaluate identities the wire has not shown. A *wrong* hash on the wire is not
  a benign error — the directory node scans the wrong chain, decides the name
  is unknown and names the sender as master, which is the 35-per-second grant
  storm in operator memory `cluster-promotion-gap.md` — so the executive also
  carries a LIVE FALSIFICATION DETECTOR: a frame contradicting a value this
  node computed makes the wire value win and raises
  `vms_lock_dlm_dir_hash_computed_wrong()`. It must read 0 on every real
  cluster.
* **THE LENGTH GAPS ARE A LAB ERRAND.** Lengths 23 and 29 were pre-registered
  in the driven run and the VAX never put them on the wire, so they are the one
  coverage gap that needs no new reasoning: lock one 23-character and one
  29-character root name on a real VAX, score the capture with
  `tools/cluster/dlm_hash/check_heldout_run.py`, and widen
  `VMS_DLM_HASH_LEN_PROVEN` WITH the capture (the ctest derives the mask from
  the evidence, so it cannot be widened without one).
* **Sub-resources are out of scope.** A sub-resource's value depends on its
  parent too (rd vms-4fb finding 3), so no `(name → value)` pair can be learned
  for one and none was used. This function answers for ROOT resources only.
