#!/usr/bin/env python3
"""dissect_sca.py -- clean-room dissector for the OpenVMS VMScluster wire
protocol (DEC SCA/LAVC, ethertype 0x6007).

CLEAN-ROOM PROVENANCE (read this before trusting a label below)
-----------------------------------------------------------------
Every field this script labels is derived ONLY from:
  (a) observing the raw bytes on the wire in the pcaps captured off our own
      2-node SIMH OpenVMS VAX 7.3 reference cluster, and
  (b) public OpenVMS documentation / documented tool output (SDA "ANALYZE/
      SYSTEM", SYSGEN SHOW/CLUSTER, SYSMAN CONFIGURATION SHOW
      CLUSTER_AUTHORIZATION) captured in
      ~/vax/cluster/captures/sda-scs-extract-vax1.txt and cross-referenced in
      ~/vax/cluster/captures/RE-specimens-2026-07-26.md.
No VSI/HPE VMS source or binary was ever disassembled, decompiled, or
consulted. Fields that cannot be grounded this way are printed as
`unknown[off..off]` (or noted "inferred" in the spec) rather than guessed.
See docs/cluster-protocol-spec.md for the full field-by-field writeup and
citations, and CLAUDE.md rule 8 for the standing clean-room invariant.

Dependency-free: stdlib only (struct). No tshark/scapy required.

Usage:
    dissect_sca.py <pcap> [--limit N] [--frame N] [--summary]

    --summary   print only a histogram of observed SCA message classes
    --frame N   dump only frame index N (0-based, counting ALL pcap records)
    --limit N   stop after decoding N SCA (0x6007) frames (default: all)
"""
import struct
import sys
import argparse

ETHERTYPE_SCA = b"\x60\x07"

# --- Node identity map (grounded: SDA `SHOW CLUSTER` CSBs + SYSMAN
# CONFIGURATION SHOW CLUSTER_AUTHORIZATION, see sda-scs-extract-vax1.txt) ---
MAC_NAMES = {
    "aa:00:04:00:01:04": "VAX1 (logical LAVC addr, CSID 00010001, SCSSYSTEMID 1025)",
    "aa:00:04:00:02:04": "VAX2 (logical LAVC addr, CSID 00010002, SCSSYSTEMID 1026)",
    "aa:00:04:00:03:04": "VAX3 (logical LAVC addr, satellite, SCSSYSTEMID 1027)",
    "08:00:2b:78:56:b9": "VAX2 (real HW MAC, used before DECnet reprograms the adapter)",
    "ab:00:04:01:01:01": "cluster group 1 multicast (AB-00-04-01-<group16>, SYSMAN CONFIGURATION SHOW CLUSTER_AUTHORIZATION)",
}

# --- SCS Connection ID -> SYSAP map (grounded: SDA `SHOW CONNECTIONS` CDTs,
# sda-scs-extract-vax1.txt lines 92-316) ---
CONID_NAMES = {
    0x62C50000: "VAX1:SCS$DIRECTORY (listen)",
    0x62C50001: "VAX1:MSCP$TAPE (listen)",
    0x62C50002: "VAX1:VMS$SDA (listen)",
    0x62C50003: "VAX1:VMS$VAXcluster (listen)",
    0x62C50004: "VAX1:SCA$TRANSPORT (listen)",
    0x62C60005: "VAX1:VMS$DISK_CL_DRVR (open, local)",
    0x62C60006: "VAX1:MSCP$DISK (listen)",
    0x62C60007: "VAX1:VMS$TAPE_CL_DRVR (open, local)",
    0x63080008: "VAX1:SCA$TRANSPORT <-> VAX2",
    0x62C50009: "VAX1:VMS$VAXcluster <-> VAX2  (Send/Recv credit 10/8)",
    0x62C6000A: "VAX1:MSCP$DISK <-> VAX2:VMS$DISK_CL_DRVR (credit 10/8)",
    0x62D4000B: "VAX1:VMS$DISK_CL_DRVR <-> VAX2:MSCP$DISK (credit 7/10)",
    0x33580008: "VAX2:VMS$VAXcluster <-> VAX1",
    0x33590009: "VAX2:VMS$DISK_CL_DRVR <-> VAX1:MSCP$DISK",
    0x3359000A: "VAX2:SCA$TRANSPORT <-> VAX1",
    0x3367000B: "VAX2:MSCP$DISK <-> VAX1:VMS$DISK_CL_DRVR",
}

# Tunables cross-validated against SYSGEN SHOW/CLUSTER (see decoder ring).
NISCS_LAN_OVRHD = 18
CLUSTER_CREDITS = 10
MSCP_CREDITS = 8


def read_pcap(path):
    """Pure-stdlib pcap reader. Returns list of (ts_sec, ts_usec, orig_len,
    frame_bytes). Supports classic (not pcapng) libpcap files, big or little
    endian, sec or nsec timestamp resolution (resolution doesn't matter here,
    we only use it for relative ordering)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise ValueError(f"{path}: too short to be a pcap file")
    magic = data[0:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        raise ValueError(f"{path}: unrecognized pcap magic {magic!r} (pcapng not supported)")
    off = 24
    frames = []
    n = len(data)
    while off + 16 <= n:
        ts_sec, ts_usec, incl_len, _orig_len = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        frame = data[off:off + incl_len]
        off += incl_len
        frames.append((ts_sec, ts_usec, _orig_len, frame))
    return frames


def mac_str(b):
    return ":".join("%02x" % x for x in b)


def mac_label(b):
    s = mac_str(b)
    name = MAC_NAMES.get(s)
    return f"{s} ({name})" if name else s


def conid_label(v):
    name = CONID_NAMES.get(v)
    return f"0x{v:08X} ({name})" if name else f"0x{v:08X} (unknown/inferred: not in decoder ring)"


def printable(bs):
    return "".join(chr(b) if 32 <= b < 127 else "." for b in bs)


class Field:
    """One labeled field in the field-by-field breakdown."""
    __slots__ = ("off", "size", "name", "value")

    def __init__(self, off, size, name, value):
        self.off = off
        self.size = size
        self.name = name
        self.value = value

    def __str__(self):
        end = self.off + self.size - 1
        rng = f"[{self.off:3d}..{end:3d}]" if self.size > 1 else f"[{self.off:3d}]     "
        return f"  {rng}  {self.name}: {self.value}"


def decode_ethernet(fr):
    """Returns (fields, ethertype_bytes, payload_bytes) using FULL-FRAME byte
    offsets (0 = first byte of the Ethernet destination address), matching
    what tcpdump -xx / -e shows. This is the offset convention used
    throughout this dissector and docs/cluster-protocol-spec.md."""
    if len(fr) < 14:
        return None
    dst, src, et = fr[0:6], fr[6:12], fr[12:14]
    fields = [
        Field(0, 6, "Ethernet dst", mac_label(dst)),
        Field(6, 6, "Ethernet src", mac_label(src)),
        Field(12, 2, "Ethertype", "0x%s (DEC SCA/LAVC)" % et.hex()),
    ]
    return fields, et, fr[14:]


def decode_sca_discovery_header(pl, base):
    """Fields [0..57], common to BOTH HELLO and SOLICIT (verified byte-exact
    against scs-idle-baseline.pcap frames 1-3 and
    satellite-niscs-boot-solicit.pcap frame 1100). This is the shared 'SCA
    discovery header' template; HELLO and SOLICIT diverge only after
    offset 57. See spec 4(b)/(c)."""
    fields = []
    lenword = struct.unpack("<H", pl[0:2])[0]
    fields.append(Field(base + 0, 2, "SCA length field (LE u16, = remaining bytes - 2)",
                         f"0x{lenword:04X} ({lenword} -> total SCA content {lenword+2} bytes)"))
    fields.append(Field(base + 2, 6, "Dest/group logical LAVC addr (multicast group for multicast HELLO/SOLICIT; peer's logical addr for directed HELLO)",
                         mac_label(pl[2:8])))
    fields.append(Field(base + 8, 2, "Connect flag (constant 0x0001 observed)", "0x%s" % pl[8:10].hex()))
    fields.append(Field(base + 10, 6, "Src logical LAVC addr (sender's own)", mac_label(pl[10:16])))
    fields.append(Field(base + 16, 2, "unknown/inferred: per-frame word (varies by sender/direction: a000 mcast/b300,b400 directed on VAX1-VAX2; b600 on VAX3 SOLICIT)",
                         "0x%s" % pl[16:18].hex()))
    subtype = pl[22]
    subtype_note = ""
    if subtype == 0x05:
        subtype_note = " -- observed 0x05 on all HELLO frames"
    elif subtype == 0x02:
        subtype_note = " -- observed 0x02 on all SOLICIT frames (GROUNDED: consistently distinguishes SOLICIT from HELLO)"
    fields.append(Field(base + 18, 4, "unknown/inferred: constant prefix (0800 0080)", "0x%s" % pl[18:22].hex()))
    fields.append(Field(base + 22, 1, "message-class byte (inferred: 0x05=HELLO, 0x02=SOLICIT, cross-checked across all captures)",
                         "0x%02x" % subtype + subtype_note))
    fields.append(Field(base + 23, 3, "unknown/inferred: constant suffix (01 00 00)", "0x%s" % pl[23:26].hex()))
    namelen = pl[26]
    name = pl[27:27 + namelen] if 27 + namelen <= len(pl) else b""
    fields.append(Field(base + 26, 1, "Node name length prefix", str(namelen)))
    fields.append(Field(base + 27, namelen, "Node name (ASCII, space-padded)", repr(printable(name))))
    fields.append(Field(base + 33, 17, "unknown/inferred: constant capability/version-ish span (differs slightly HELLO vs SOLICIT)",
                         "0x%s" % pl[33:50].hex()))
    fields.append(Field(base + 50, 1, "unknown/inferred: constant 0x03", "0x%s" % pl[50:51].hex()))
    fields.append(Field(base + 51, 3, "unknown/inferred: zero", "0x%s" % pl[51:54].hex()))
    nonce = pl[54:58]
    nonce_note = " -- GROUNDED: matches the cluster-wide join nonce documented in RE-specimens-2026-07-26.md (zero on multicast HELLO, non-zero shared token e.g. ee05395b on directed HELLO/SOLICIT)"
    fields.append(Field(base + 54, 4, "Connect/join nonce", "0x%s" % nonce.hex() + nonce_note))
    return fields


def decode_hello(pl, base):
    """Decode a HELLO/keepalive frame (empirically: SCA length field value
    118 => total SCA content 120 bytes). Grounded in scs-idle-baseline.pcap
    frames 1-37 and formation-ci1-joinwindow.pcap. Offsets [0..57] are the
    shared discovery header (see decode_sca_discovery_header); [58..119] are
    HELLO-specific. See spec 4(b)."""
    fields = decode_sca_discovery_header(pl, base)
    fields.append(Field(base + 58, 20, "unknown/inferred: zero padding", "0x%s" % pl[58:78].hex()))
    flag = pl[78:80]
    flag_note = " -- GROUNDED: 0x0000 on multicast HELLO, 0x0001 on directed/unicast HELLO"
    fields.append(Field(base + 78, 2, "directed-HELLO flag", "0x%s" % flag.hex() + flag_note))
    fields.append(Field(base + 80, 2, "constant trailer word (0x9205)", "0x%s" % pl[80:82].hex()))
    fields.append(Field(base + 82, 4, "unknown/inferred: changing 4-byte value (timer/tick, increases within a capture)",
                         "0x%s" % pl[82:86].hex()))
    fields.append(Field(base + 86, 12, "constant tail (0x9900bc0003585141000000) across all HELLOs",
                         "0x%s" % pl[86:98].hex()))
    fields.append(Field(base + 98, 8, "unknown/inferred: zero padding", "0x%s" % pl[98:106].hex()))
    hwmac = pl[106:112]
    fields.append(Field(base + 106, 6, "sender's real hardware LAN MAC (GROUNDED: matches the actual src MAC seen on the wire before DECnet reprograms the adapter to its logical addr)",
                         mac_label(hwmac)))
    fields.append(Field(base + 112, 2, "constant trailer (0x2600)", "0x%s" % pl[112:114].hex()))
    poll = pl[114:116]
    poll_note = ""
    if poll == b"\x1f\x00":
        poll_note = " -- GROUNDED: 0x1f=31 matches SDA SHOW PORTS PEA0 'Poller Sweep 31'"
    fields.append(Field(base + 114, 2, "poller-sweep/directed marker (0x0000 multicast / 0x001f directed)",
                         "0x%s" % poll.hex() + poll_note))
    fields.append(Field(base + 116, 2, "unknown/inferred: constant 0x0064 (100 decimal)", "0x%s" % pl[116:118].hex()))
    fields.append(Field(base + 118, 2, "unknown/inferred: trailer, always 0x0000 in captures", "0x%s" % pl[118:120].hex()))
    return fields


def decode_solicit(pl, base):
    """Boot-time NISCS disk-server SOLICIT_SERVICE (satellite -> multicast).
    Grounded in satellite-niscs-boot-solicit.pcap frame 1100 (VAX3); matches
    RE-specimens doc 'Decoded boot SOLICIT' section. SCA length field value
    76 => 78 bytes. Shares the [0..57] discovery header with HELLO
    (see decode_sca_discovery_header); [58..77] is SOLICIT-specific.
    See spec 4(c)."""
    fields = decode_sca_discovery_header(pl, base)
    fields.append(Field(base + 58, 4, "unknown/inferred: zero", "0x%s" % pl[58:62].hex()))
    if len(pl) > 62:
        tgtlen = pl[62]
        tgt = pl[63:63 + tgtlen]
        fields.append(Field(base + 62, 1, "Target device spec length prefix (GROUNDED)", str(tgtlen)))
        fields.append(Field(base + 63, tgtlen, "Target device spec (ASCII, GROUNDED: e.g. '_$2$DUA0:' = 'serve me this system disk')",
                             repr(printable(tgt))))
        tail_off = 63 + tgtlen
        if tail_off < len(pl):
            fields.append(Field(base + tail_off, len(pl) - tail_off, "unknown/inferred: trailing zero pad",
                                 "0x%s" % pl[tail_off:].hex()))
    return fields


def decode_scs_envelope(pl, base):
    """Generic directed SCS-bearing frame (connect/directory-lookup,
    sequenced-message, or bulk block-transfer). Grounded in
    scs-idle-baseline.pcap frames 9-14 cross-referenced against SDA `SHOW
    CONNECTIONS` (Local/Remote Con.ID, credits) -- see spec 4(d).

    IMPORTANT (empirically verified, formation-ci1.pcap, 18541 SCA frames):
    the Local/Remote Connection ID pair at offset [50:58] is ONLY reliably
    present for the fixed 190-byte total-length message class -- EVERY one
    of 17,557 such frames in that capture decodes to a Con.ID present in the
    SDA decoder ring. Every OTHER length class (58/62/66/70/94/106/110/
    206/270/.../1500 bytes) matches the decoder ring 0-73% of the time at
    that same offset, i.e. it is not a reliable field location for those
    classes -- their header layout is NOT grounded and is left as raw hex.
    """
    fields = []
    lenword = struct.unpack("<H", pl[0:2])[0]
    total = lenword + 2
    fields.append(Field(base + 0, 2, "SCA length field (LE u16, = remaining bytes - 2)",
                         f"0x{lenword:04X} ({lenword} -> total SCA content {total} bytes)"))
    if len(pl) < total:
        fields.append(Field(base + 2, 0, "WARNING", f"frame shorter ({len(pl)}) than length field claims ({total})"))
    elif len(pl) > total:
        pad = len(pl) - total
        fields.append(Field(base + total, pad, "Ethernet minimum-frame padding (not part of SCA content)",
                             "0x%s" % pl[total:].hex()))
    if len(pl) < 16:
        return fields
    fields.append(Field(base + 2, 6, "Destination logical LAVC addr", mac_label(pl[2:8])))
    fields.append(Field(base + 8, 2, "Connect flag", "0x%s" % pl[8:10].hex()))
    fields.append(Field(base + 10, 6, "Source logical LAVC addr (sender's own)", mac_label(pl[10:16])))
    if len(pl) < 18:
        return fields
    # offset 16 = SCS message-type byte (inferred names, GROUNDED value<->phase
    # partition, 2975/2975 directed envelope frames in the join window -- see
    # spec 4(g)); offset 17 = format/version constant 0x13 (GROUNDED constant).
    SCS_MSGTYPE = {0x41: "START/config", 0x5b: "directory-lookup",
                   0x4b: "sequenced-application (connect/VC/DLM data)",
                   0x48: "credit-return short"}
    mt = pl[16]
    mtname = SCS_MSGTYPE.get(mt, "unknown message-type")
    fields.append(Field(base + 16, 1, "SCS message-type byte (inferred label; GROUNDED value<->phase partition, spec 4g)",
                         "0x%02x (%s)" % (mt, mtname)))
    b17note = " (GROUNDED constant 0x13 across all directed SCS-envelope frames)" if pl[17] == 0x13 else ""
    fields.append(Field(base + 17, 1, "SCS format/version constant", "0x%02x%s" % (pl[17], b17note)))
    # ONLY the fixed 190-byte message class has a validated Con.ID location
    # at offset [50:58] (see docstring). Everything else is presented as
    # unlabeled body.
    if total == 190 and len(pl) >= 58:
        fields.append(Field(base + 18, 32, "SCS sequence-number region (3x repeats of two 16-bit counters, zero-padded to 32 bits; NISCS_LAN_OVRHD=18 constant at [24:26], GROUNDED)",
                             "0x%s" % pl[18:50].hex()))
        remote = struct.unpack("<I", pl[50:54])[0]
        local = struct.unpack("<I", pl[54:58])[0]
        fields.append(Field(base + 50, 4, "Remote Connection ID (GROUNDED for the 190-byte message class: byte-exact match to SDA SHOW CONNECTIONS 'Remote Con. ID' in 17557/17557 sample frames)",
                             conid_label(remote)))
        fields.append(Field(base + 54, 4, "Local Connection ID (GROUNDED for the 190-byte message class: byte-exact match to SDA SHOW CONNECTIONS 'Local Con. ID' in 17557/17557 sample frames)",
                             conid_label(local)))
        body = pl[58:total] if total <= len(pl) else pl[58:]
        strs = extract_strings(body)
        note = f"; printable strings observed: {strs}" if strs else ""
        fields.append(Field(base + 58, len(body), "SYSAP-specific message body (DLM/MSCP/directory payload -- opcode-level fields NOT grounded, unknown)",
                             f"{len(body)} bytes{note}"))
    else:
        body = pl[18:total] if total <= len(pl) else pl[18:]
        strs = extract_strings(body)
        note = f"; printable strings observed: {strs}" if strs else ""
        fields.append(Field(base + 18, len(body), "unknown/inferred: message body for this length class -- header layout NOT grounded beyond the common dst/flag/src preamble (see docstring: Con.ID offset only validated for the 190-byte class)",
                             f"{len(body)} bytes{note}"))
    return fields


def extract_strings(bs, minlen=4):
    out = []
    cur = []
    for b in bs:
        if 32 <= b < 127:
            cur.append(chr(b))
        else:
            if len(cur) >= minlen:
                out.append("".join(cur).rstrip())
            cur = []
    if len(cur) >= minlen:
        out.append("".join(cur).rstrip())
    return [s for s in out if s.strip()]


def classify(pl):
    lenword = struct.unpack("<H", pl[0:2])[0]
    total = lenword + 2
    if total == 120:
        return "HELLO"
    if total in (78, 92):
        return "SOLICIT"
    if total >= 58:
        return "SCS-ENVELOPE"
    return "SCS-SHORT"


def decode_frame(idx, ts, fr):
    out = []
    eth = decode_ethernet(fr)
    if eth is None:
        return None, None
    eth_fields, et, pl = eth
    if et != ETHERTYPE_SCA:
        return None, None
    if len(pl) < 2:
        return None, None
    cls = classify(pl)
    out.extend(eth_fields)
    if cls == "HELLO":
        out.extend(decode_hello(pl, 14))
    elif cls == "SOLICIT":
        out.extend(decode_solicit(pl, 14))
    else:
        out.extend(decode_scs_envelope(pl, 14))
    out.sort(key=lambda f: f.off)
    return cls, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--frame", type=int, default=None)
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    frames = read_pcap(args.pcap)

    if args.summary:
        import collections
        hist = collections.Counter()
        for i, (ts, us, ol, fr) in enumerate(frames):
            if len(fr) < 14 or fr[12:14] != ETHERTYPE_SCA:
                continue
            pl = fr[14:]
            if len(pl) < 2:
                continue
            lenword = struct.unpack("<H", pl[0:2])[0]
            cls = classify(pl)
            hist[(cls, lenword + 2)] += 1
        total_sca = sum(hist.values())
        total_frames = len(frames)
        print(f"{args.pcap}: {total_frames} pcap records, {total_sca} SCA (0x6007) frames")
        for (cls, ln), cnt in sorted(hist.items(), key=lambda kv: -kv[1]):
            print(f"  {cnt:6d}  class={cls:14s} sca_len={ln}")
        return

    decoded = 0
    for i, (ts, us, ol, fr) in enumerate(frames):
        if args.frame is not None and i != args.frame:
            continue
        cls, fields = decode_frame(i, ts, fr)
        if cls is None:
            continue
        print(f"=== frame {i}  t={ts}.{us:06d}  orig_len={ol}  class={cls} ===")
        for f in fields:
            print(f)
        print()
        decoded += 1
        if args.limit is not None and decoded >= args.limit:
            break

    if decoded == 0:
        print("no 0x6007 (SCA) frames decoded", file=sys.stderr)


if __name__ == "__main__":
    main()
