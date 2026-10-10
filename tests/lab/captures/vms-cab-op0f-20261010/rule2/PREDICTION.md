# vms-cab op-0x0f answer, revised rule: pre-registered prediction (2026-10-10)

Committed BEFORE the held-out capture it predicts.

## What changed

op 0x0f is one step of VMS lock-tree remastering (rd vms-50d):
- The old master sends 0e/0f/10/12…/13 to the chosen new master.
- It sends 0f + 11 to the other members.

The first rule (PREDICTION.md one level up) was derived and held out on two-node clusters only. In a two-node cluster every 0f recipient is the new master, so the "no lock interest → clear body[28:32]" reading was confounded with "new master". Three-node captures show:
- The new master clears body[28:32]: 1497/1497 pairs.
- Every other recipient keeps body[28:32]: 888/888 pairs.

The recipient can tell which it is from its own state. The new master has already received the 0e for that tree from that sender, and has not yet closed it with its 14.

## RULE (`op0f_role_rule.py`)

The answer to a cat-0x02 op-0x0f is the request echoed with:
- cat 0x82, op 0x15;
- body[32:36] = 01 00 fa 00;
- body[28:32] cleared **iff** the recipient has an open remaster for that tree from that sender (0e received, no 14 sent yet), otherwise kept as in the request.

The comparison is over body[4:132].

## Derivation score

Every real VMS↔VMS pair in the lab library: dlmlab E1–E3, rm-A, op0f-ho1/2, and every older capture.
- **3299/3299**: 2366 clear, 933 keep.
- Pairs are by envelope sequence (response body[2:4] == request body[0:2]) and deduplicated.

## Prediction for the held-out capture

**Setup:** a NEW three-node VAX-only capture (dlmlab volume, VAX1/2/3, private bridge, tcpdump started before the first boot). Arms, one variable each:
- **E4:** LOCKDIRWT 0/1/0, VAX1 departs with REMOVE_NODE.
- **E5:** LOCKDIRWT 0/0/0, VAX2 departs with REMOVE_NODE.

**P1.** Every op-0x0f / 82-0x15 pair matches the RULE. Zero misses.

**P2.** Both roles occur: at least 50 "clear" and at least 50 "keep".

If P1 fails once, the OVMX change does not merge.

## What OVMX will do

OVMX never adopts a tree (no 0e handling yet, rd vms-50d). So it always answers 0f in the keep form, which is what a VAX that is not the new master sends.
