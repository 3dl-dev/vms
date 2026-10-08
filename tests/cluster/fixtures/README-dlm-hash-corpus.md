# DLM resource-name hash capture corpus (rd vms-9c95)

Every row is a 32-bit directory-hash **value a real OpenVMS VAX put on the
cluster wire itself**, next to the resource name it was looking up. Nothing
here was read out of a VMS binary, a listing, a disassembly or source: Baron's
ruling on rd vms-dc2 (2026-10-08) extends Rule 8's scope to the black-box
determination of this function **from (name, value) pairs VMS broadcasts in
the clear, and from nothing else.**

## Where a value comes from

A cat-0x02 op-0x01 (ENQ) **request** for a **ROOT** resource, per
`tests/lab/captures/vms-4fb-dir-hash-20261004/README.md` and
`src/kernel-core/vms_cluster_codec_dlm.h` (`struct vms_dlm_res_ident`):

| field | SYSAP-body offset | note |
|---|---|---|
| category / opcode | `body[8]` / `body[9]` | `0x02` / `0x01`; a `0x82` answer is NOT used |
| parent span | `body[36:44]` | all-zero = ROOT. A sub-resource's value depends on its parent too, so it teaches nothing about a name (vms-4fb finding 3) and is excluded |
| group | `body[44:46]` | LE u16 |
| access mode | `body[46]` | the mode qualifying the name (vms-4fb finding 5) |
| name length | `body[47]` | 1..31 |
| name | `body[48:48+len]` | raw bytes, not necessarily printable |
| **value** | `body[128:132]` | **LE u32 — the hash.** The directory node is `vector[(value >> 16) mod n]` (vms-4fb finding 4) |

## Sender filter (INV-6)

Only frames whose sender this run could **positively identify as a non-OVMX
node** are kept. A sender's SCSNODE is read out of the multicast discovery
(HELLO) frames it sent in the same input set; a sender that never identified
itself is excluded and counted, never assumed to be a VAX. Learning OVMX's own
arithmetic back would be circular, so `OVMX*` senders are excluded by name.

Contributing senders: `VAX1`, `VAX2`, `VAX3` (OpenVMS VAX **V7.3**, the
`ovmx-lab/dlmlab` three-node cluster) and `VAXC` (an unmodified OpenVMS VAX
**V5.5-2H4** system disk, the `run-dlm2` rig) — two independent VMS versions.

## Files

| file | rows | what |
|---|---|---|
| `dlm_hash_corpus.tsv` | 1216 | every unique `(name, mode, group) -> value`, with the pcap + frame it was first seen in |
| `dlm_hash_derivation.tsv` | 963 | the only rows the derivation (rd vms-66fe) may look at |
| `dlm_hash_heldout.tsv` | 253 | **do not look at these while deriving.** The generalisation test (rd vms-c6e leg 1) |
| `dlm_hash_prestudy_names.tsv` | 25 | keys printed on screen during the pre-split reconnaissance; forced into the derivation split |
| `dlm_hash_predicted_m3soledir.tsv` | 533 | **not part of the split.** A FORWARD PREDICTION set: cut from `run-evac/runs/m3-soledir/wire.pcap`, a capture written at 2026-10-08T20:57:15Z — *sixteen minutes after* the function was committed (`99ce6ad6e`, 20:41:38Z) — on a different rig and a different cluster group, from op-0x01 lookups **and** op-0x0d directory registrations. 84 of its keys appear in no other file here; all 533 reproduce. `sha256 af9437c0c5a21e51827ab9af08b03a776dac9ab6e3316511bc7386d31770c165` |

16102 op-0x01 ROOT requests from identified real-VAX senders collapsed to 1216
unique keys with **zero conflicts**: every `(name, mode, group)` carries exactly
one value, from every sender, in every capture, across both VMS versions.

## Reproducing

The lab pcaps live on the shared lab PVC (read-only; mounted in pod
`ovmx-lab/dlmlab`), not in this repo — they are 15 MB+ of VAXcluster traffic.
`sha256` of every capture that supplied a row:

```
2d87fadf6ef53d177aa373ea1c146e16d0dc25e9fc6e286c187bd1735589781f  dlmlab/CF1/CF1.pcap
7a0a99fc90565437e71f7198259ea5d0ef032c2b4dadb58859054603a5733718  dlmlab/L1/L1.pcap
2d95966f9a3de06e4149655804ffa5836a1e106cff526a064802b456e147d70a  dlmlab/L2/L2.pcap
dd91c85e2d354a95124bc67122c89efbebc2b7a8f4f2f26dfa35e6335abe8152  run-dlm2/runs/K-7/s8.pcap.gz
ed5e2fe2e702a50f5868683fb9718785e129e3c5e3d5c8eacce6fec5273cf1a7  run-dlm2/runs/PK-1/s8.pcap.gz
708743b10fa56be6b81a6670568a522edb211c0b0e117438d0eacbb137bbbc30  tests/lab/captures/vms-b36-cnxmgrerr-20260925/oracle/oracle-3node-clean.pcap.gz
abbb488e98edb361cd416d24b84e728cfbc43f2fc9e492a84b680ab94fc168f0  tests/lab/captures/vms-1ac-cn3-achieved-20260925/cn3.pcap
3b991432c0e12422ca234014a8885919c790bc5120ff8443616ca2fd31fde112  tests/lab/captures/vms-20c-rebuild-1789489597/rejoin-rebuild.pcap
e7b4bfc23c5878f05cf70d5bd75d0aae55b66d8e8e599eed6eba28f1184b7ff2  tests/lab/captures/vms-727-lvb-20260911/c1-baseline.pcap
cdc2989eb935d994f214921ee8a971203d2c6bbab88e1e4bb7a08c284b99934b  tests/lab/captures/vms-727-lvb-20260911/c3-mode.pcap
f220eb6c39f8c5084d0cbac2855f4a768f4a65d73c96b4fb5acbee5fdba1cbca  tests/lab/captures/vms-727-lvb-20260911/c4-namelen.pcap
dc1689591ad18e29a07d05c3eb5f218e46fd2c3086022ab0686f8680c1a2139f  tests/lab/captures/vms-727-lvb-20260911/c5-persistent-multiwrite.pcap
a84e557bebdf256b13f2718008fd86ae1c28936e392527b88aa537c68338b58c  tests/lab/captures/vms-727-lvb-20260911/c6-demote-ex-to-cr.pcap
f5d0f3a2e417670bc12ab7bf046636836a6b726a308a960ffbbf5eff4d7a09e7  tests/lab/captures/vms-727-lvb-20260911/c7-upconvert-cr-to-ex.pcap
229110df13d91c79646ee63c9bde24b51ce892db5e5f6e6e9f2f65a7a601e331  tests/lab/captures/vms-b34-group-on-wire-20260924/br1-run2-fixed.pcap
751babb1fd6df7f933b071803489b73001c10bb6ae8b001c42eef963ab297bba  tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-blk2-20260911.pcap
e2e2be59f93d23233fbfa7a9c2bf6f936f3313c23c16054ead85c494b5be72eb  tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-deq-20260911.pcap
6ac0de0512f7b6111da4397dae6a8ee516781164a65db53d24d3b0fa89861c0c  tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-lvb3-20260911.pcap
```

```sh
# the full input set that was scanned (order matters only for which capture is
# credited with a key's FIRST sighting)
tools/cluster/dlm_hash/extract.py --dedupe \
    /lab/k8s-labs/dlmlab/*/*.pcap \
    $(find tests/lab/captures -name '*.pcap' -o -name '*.pcap.gz' | sort) \
    $(find /lab/run-dlm2/runs -name 's8.pcap.gz' | sort) \
  > tests/cluster/fixtures/dlm_hash_corpus.tsv

tools/cluster/dlm_hash/split.py tests/cluster/fixtures/dlm_hash_corpus.tsv \
    --derivation tests/cluster/fixtures/dlm_hash_derivation.tsv \
    --held-out   tests/cluster/fixtures/dlm_hash_heldout.tsv \
    --prestudy   tests/cluster/fixtures/dlm_hash_prestudy_names.tsv
```

`--dedupe` exits 2 if any `(name, mode, group)` ever carried two different
values. It exited 0.

The forward-prediction fixture adds `--ops 01,0d`, which also trusts the
op-0x0d directory-registration record's identity block (grounded by 479/479
reproductions in that run). The default stays `--ops 01`, so the corpus above
re-extracts byte-for-byte:

```sh
tools/cluster/dlm_hash/extract.py --dedupe --ops 01,0d \
    /lab/run-evac/runs/m3-soledir/wire.pcap \
  > tests/cluster/fixtures/dlm_hash_predicted_m3soledir.tsv
```

**Only op-0x01 and op-0x0d are trusted as name carriers**, and that is measured,
not assumed: in the same capture an op-0x06 body and two op-0x07 bodies carry a
visibly half-overwritten name span next to a hash field that is a correct hash
of a *different, real* resource — the "a wire field that looks like data and is
not" hazard `src/kernel-core/vms_cluster_codec_dlm.h` warns about.
See `docs/design-dlm-name-hash.md` SS4a.

## The split, and why it is frozen here

`split.py` is deterministic and seeded (`"vms-9c95:"`): a key is held out iff
`sha256("vms-9c95:" + name_hex + ":" + mode + ":" + group)[0:4]` big-endian
`% 5 == 0`, unless it is one of the 25 pre-study keys. The split was written
**before** any derivation work started (same commit as the corpus), so
"it matches the held-out rows too" is a real prediction and not a fit.
