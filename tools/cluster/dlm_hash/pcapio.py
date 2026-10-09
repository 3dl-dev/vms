#!/usr/bin/env python3
"""Minimal classic-pcap reader + the LAVC frame slicing the DLM corpus needs.

Shared by extract.py and the held-out checker.  Deliberately dependency-free
(no scapy): the lab pcaps are classic little-endian DLT_EN10MB and the only
layers that matter here are Ethernet + the SCA content a VAXcluster puts in a
protocol-0x6007 frame.

Offsets used here are the ones GROUNDED in
tests/lab/captures/vms-4fb-dir-hash-20261004/README.md and encoded in
src/kernel-core/vms_cluster_codec_dlm.h (struct vms_dlm_res_ident); nothing is
re-derived.
"""
import gzip
import struct
import sys

PCAP_MAGIC_US = 0xA1B2C3D4
PCAP_MAGIC_NS = 0xA1B23C4D

ETH_HDR_LEN = 14
SCA_ETHERTYPE = b"\x60\x07"
# AB-00-04-01-gg-gg: the VAXcluster discovery multicast; the low two
# bytes carry the cluster GROUP, so only the 4-byte prefix is fixed.
CLUSTER_MULTICAST_PREFIX = bytes.fromhex("ab000401")

# SCA-content-relative (content = frame[14:]) -- the SCS message-class word and
# the start of the 132-byte SYSAP body (abs 60 / abs 72 frame-relative).
SCA_OFF_MSGCLASS = 46
SCA_OFF_SYSAP_BODY = 58
SYSAP_BODY_LEN = 132
MSGCLASS_SCS_MSG = 10

# SYSAP-body-relative DLM fields (vms_cluster_codec_dlm.h).
OFB_CAT = 8
OFB_OP = 9
OFB_PARENT = 36          # [36:44] all zero => ROOT resource
OFB_GROUP = 44           # [44:46] LE u16
OFB_MODE = 46            # access mode qualifying the name
OFB_NAMELEN = 47         # 1..31
OFB_NAME = 48
OFB_HASH = 128           # [128:132] LE u32 -- the directory hash value
NAME_MAX = 31

DLM_CAT_REQUEST = 0x02
DLM_WIREOP_ENQ = 0x01
DLM_WIREOP_REBUILD = 0x0d

# Discovery-family node name (vms_cluster_codec_hello.h), frame-absolute.
OFF_DISC_NAMELEN = 40
OFF_DISC_NAME = 41
DISC_NAME_MAX = 6


def _open(path):
    return gzip.open(path, "rb") if path.endswith(".gz") else open(path, "rb")


def frames(path):
    """Yield (frame_index, frame_bytes) for a classic pcap (optionally .gz)."""
    with _open(path) as fh:
        data = fh.read()
    if len(data) < 24:
        return
    magic = struct.unpack("<I", data[:4])[0]
    if magic not in (PCAP_MAGIC_US, PCAP_MAGIC_NS):
        raise ValueError("%s: not a little-endian classic pcap (magic %08x)"
                         % (path, magic))
    off, idx = 24, 0
    while off + 16 <= len(data):
        _, _, caplen, _ = struct.unpack("<IIII", data[off:off + 16])
        off += 16
        if off + caplen > len(data):
            break
        yield idx, data[off:off + caplen]
        off += caplen
        idx += 1


def is_sca(frame):
    return len(frame) > ETH_HDR_LEN and frame[12:14] == SCA_ETHERTYPE


def src_mac(frame):
    return frame[6:12].hex()


def sysap_body(frame):
    """The 132-byte SCS SYSAP body of a message-class-10 frame, else None."""
    content = frame[ETH_HDR_LEN:]
    need = SCA_OFF_SYSAP_BODY + SYSAP_BODY_LEN
    if len(content) < need:
        return None
    cls = struct.unpack("<H", content[SCA_OFF_MSGCLASS:SCA_OFF_MSGCLASS + 2])[0]
    if cls != MSGCLASS_SCS_MSG:
        return None
    return content[SCA_OFF_SYSAP_BODY:need]


def disc_node_name(frame):
    """SCSNODE a multicast discovery frame names itself with, else None."""
    if frame[0:4] != CLUSTER_MULTICAST_PREFIX:
        return None
    if len(frame) < OFF_DISC_NAME + DISC_NAME_MAX:
        return None
    nlen = frame[OFF_DISC_NAMELEN]
    if not 1 <= nlen <= DISC_NAME_MAX:
        return None
    name = frame[OFF_DISC_NAME:OFF_DISC_NAME + nlen]
    if not all(32 <= c < 127 for c in name):
        return None
    return name.decode("ascii").strip()


def root_named_request(body, ops=(DLM_WIREOP_ENQ,)):
    """(name, mode, group, value) for a cat-02 ROOT request, else None.

    `ops` selects which opcodes are trusted to carry a COHERENT identity block.
    Only two are: op-0x01 (the ENQ/lookup, grounded rd vms-4fb) and op-0x0d
    (the rebuild/registration record, grounded spec SS4(p) and accepted by
    vms_dlm_res_ident_parse_body). An op-0x06/op-0x07 body also has readable
    bytes at body[48] and they are NOT the resource -- see the
    vms_cluster_codec_dlm.h warning about a wire field that looks like data and
    is not, and the measurement in docs/design-dlm-name-hash.md SS6.

    A sub-resource (nonzero parent span) is rejected: its value is a property
    of the parent too, so it teaches nothing about the name (vms-4fb finding 3).
    """
    if body[OFB_CAT] != DLM_CAT_REQUEST or body[OFB_OP] not in ops:
        return None
    if body[OFB_PARENT:OFB_PARENT + 8] != b"\0" * 8:
        return None
    nlen = body[OFB_NAMELEN]
    if not 1 <= nlen <= NAME_MAX:
        return None
    name = bytes(body[OFB_NAME:OFB_NAME + nlen])
    if len(name) != nlen:
        return None
    group = struct.unpack("<H", body[OFB_GROUP:OFB_GROUP + 2])[0]
    value = struct.unpack("<I", body[OFB_HASH:OFB_HASH + 4])[0]
    return name, body[OFB_MODE], group, value
