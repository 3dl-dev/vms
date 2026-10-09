#!/usr/bin/env python3
"""refhash.py -- the DLM resource-name hash, in Python, for lab tooling only.

THE C IMPLEMENTATION IS THE PRODUCT (src/kernel-core/vms_dlm_hash.c, rd
vms-66fe). This module exists because the lab side of rd vms-c6e has to
generate predictions and score a pcap on a workstation, where the executive is
not in the loop; it is NOT a second implementation of a product behaviour and
nothing in src/ may call it.

Both constants below are DATA determined black-box from (name, value) pairs
real OpenVMS VAX nodes put on the cluster wire in the clear -- Baron's ruling
on rd vms-dc2, 2026-10-08. Never from a binary, a disassembly, a listing or
source. Method: docs/design-dlm-name-hash.md.

This module is pinned to real VMS wire values by
tools/cluster/dlm_hash/check_heldout_run.py's anchor set, so it cannot drift
from the C without a red.
"""
MASK32 = 0xFFFFFFFF
ROT = 9
MULT = 0xA53F19B7
NAME_MAX = 31


def rotl32(v, n):
    n &= 31
    if n == 0:
        return v & MASK32
    return ((v << n) | (v >> (32 - n))) & MASK32


def ident_longword(group, mode, name_len):
    """body[44:48] read little-endian: group word, mode byte, length byte."""
    return (group & 0xFFFF) | ((mode & 0xFF) << 16) | ((name_len & 0xFF) << 24)


def name_longwords(name):
    """The name's little-endian longwords, zero-padded to a 4-byte boundary."""
    padded = name + b"\x00" * (-len(name) % 4)
    return [int.from_bytes(padded[i:i + 4], "little")
            for i in range(0, len(padded), 4)]


def name_hash(name, mode, group):
    """The 32-bit directory hash of a ROOT resource identity."""
    if not 1 <= len(name) <= NAME_MAX:
        raise ValueError("name length %d outside 1..%d" % (len(name), NAME_MAX))
    acc = rotl32(ident_longword(group, mode, len(name)), ROT)
    for w in name_longwords(name):
        acc = rotl32(acc ^ w, ROT)
    return (acc * MULT) & MASK32
