#!/usr/bin/env python3
"""scs_type10_measure.py - vms-4eb: re-derive the SCA message-type-10 decode
from the raw lab-1 captures.

Every figure quoted in docs/cluster-protocol-spec.md sec 4(h)(1e) ("the
110-content type-10 class") and in the sec 5 register entries it opens comes
out of this script. It reads pcaps only; the only OVMX code it imports is the
pure-stdlib pcap reader in dissect_sca.py, the shared census refusal in
census_guard.py and the capture manifest in capture_manifest.py.

    tools/cluster/scs_type10_measure.py           # re-measure, PASS/FAIL vs EXPECTED
    tools/cluster/scs_type10_measure.py --print   # just print what the captures say

Requires the lab-1 captures, host-only and NOT in git (CLAUDE.md rule 8):
/data/training/vax/cluster/captures/**/*.pcap. Override with --captures.

`ctest -R scs_type10_figures` does not REQUIRE the captures: it always asserts
every figure in EXPECTED still appears in the spec, and where the packets are
readable it also re-derives EXPECTED from them (vms-371, via rederive()).

===========================================================================
THE QUESTION, AND WHY IT WAS THE LARGEST UNKNOWN ON OUR WIRE
===========================================================================

In the 110-content 0x?B13 class the real-VAX population splits on the SCA
message type at content [46:48] as {0: 1101, 2: 324, 10: 2889} -- type 10 is
the MAJORITY of what real VAXes put on that class, and OVMX's own population
({0: 396, 2: 70}) has never contained one. spec sec 4(h)(1a) already carried
type 10 as "the SCS application-message MTYPE" -- the *taxon*. What it did not
carry is WHAT THE APPLICATION MESSAGE SAYS, and a taxon is not a decode.

What changed: *MSCP Basic Disk Functions Manual* AA-L619A-TK v1.2 (Apr 1982),
a customer-orderable, non-confidential DEC manual (UDA50 Programmer's Doc Kit
QP-905-GZ) -- admissible public documentation under CLAUDE.md rule 8 -- gives
the byte-level MSCP message formats: Table A-1 (opcodes), Table A-7 (end and
attention message offsets), Table B-1/B-2 (status and sub-codes), Appendix C
(media type identifier encoding) and sec 6.12 (GET UNIT STATUS). Decoding this
class stops being reverse engineering and becomes transcription plus a
conformance check against our own captures.

  Provenance note kept here because it is load-bearing: the bitsavers
  `dec/dsa/mscp` v2.4.0 files are stamped DEC CONFIDENTIAL AND PROPRIETARY /
  RESTRICTED DISTRIBUTION and are EXCLUDED under rule 8; so is the bitsavers
  *VAXcluster Disk I/O Internals Manual*. Neither was read. AA-L619A-TK has a
  plain copyright page and no distribution restriction.

===========================================================================
WHAT IT MEASURES, AND WHY EACH PART EXISTS
===========================================================================

(A) THE UNRESTRICTED CENSUS. Every envelope-conformant SCA frame in every
    lab-1 capture, keyed (SCA content length, message type, source
    population). NO length filter of any kind -- the vms-c11 failure (a census
    silently restricted to {62, 66, 110} that published a false absence) is
    what this part exists not to repeat, and census_guard.check_census() is
    called on the ONE place a restriction is applied (part D's 110-content
    focus) with a written reason.

(B) THE COMMAND/END PAIRING -- the part that identifies the class. For each
    110-content type-10 frame, look for a 94-content type-10 frame earlier in
    the same capture, on the MIRRORED Con.ID pair, carrying the SAME 32-bit
    value at body offset 0. That is MSCP's own correlation rule: AA-L619A-TK
    sec 5.1 defines the command reference number as "a 32-bit, unique,
    non-zero number identifying a host command... copied to the end message".
    The pairing is reported as (command opcode -> endcode), so the END-message
    flag (Table A-1: an endcode is `command | OP.END`, OP.END = 0x80) either
    shows up as the difference between the two or the identification is wrong.

(C) THE SYSAP FILTER, which is a category error if it is skipped. Con.IDs are
    bound to SYSAP names by the connect frames (types 0/2) in the same
    capture. body[8] is an MSCP opcode ONLY on an MSCP connection: on the
    SCS$DIRECTORY / SCS$DIR_LOOKUP connections the same offset reads 0x24 and
    0x56, which are not Table A-1 opcodes at all. This part measures both, so
    "we filtered" is a number rather than a claim.

    It also measures WHICH of the two 16-byte SYSAP name fields on a connect
    frame is the sender's -- see the ROLE OF THE NAME FIELDS note below.

(D) THE FIELD DECODE, against Table A-7's GET UNIT STATUS end-message offsets
    and sec 6.12's validity rules. Every documented field is censused over the
    population, the documented reserved fields are checked for zero, and
    sec 6.12's rule ("if unit identifier = 0, virtually no characteristics are
    valid") is checked as a PARTITION with a residual count rather than
    asserted.

(E) THE RESIDUE -- what is NOT decoded, counted rather than waved at: the four
    bytes at body [48:52] that lie past the end of the documented end message,
    and the unit-flags bit Table A-5 does not define. A decode that does not
    say where it stops is not a decode.

===========================================================================
POPULATION SPLIT: OVMX'S OWN FRAMES ARE NOT EVIDENCE ABOUT VMS
===========================================================================

Same rule the rest of the toolchain uses (scs_connect_data_measure.py,
scs_disc_measure.py): a real lab VAX sources from the DEC NIC OUI 08:00:2b or
from a DECnet-assigned aa:00:04 logical address; OVMX sources from a
locally-administered Linux tap MAC. `unclassified_sources` counts every frame
the rule cannot place and the comparison requires it to be ZERO -- a new OVMX
MAC REDS this script instead of silently joining the VAX population.

The rule places `08:00:2b:11:22:33` as a VAX, and that is not an accident of
the OUI: it is vax3's SIMH adapter address, set literally in
`/data/training/vax/cluster/vax3/local.ini` (`set xq mac=08-00-2b-11-22-33`).
The classification is cross-checked, not assumed.

===========================================================================
ROLE OF THE TWO SYSAP NAME FIELDS -- WHY IT HAD TO BE RE-MEASURED HERE
===========================================================================

The decode below binds a Con.ID to a SYSAP, so it depends on which of the two
16-byte name fields on a connect frame is the SENDER's. spec sec 4(h)(2)
already grounds them as (DESTINATION, SOURCE) -- [62:78] is "the CONNECT_REQ's
TARGET SYSAP name" -- by the request/response swap test on the SCS$DIRECTORY
dialogue. This script re-derives that over the WHOLE corpus rather than one
dialogue: on CONNECT_REQ (type 0) the pair is (`MSCP$DISK`,
`VMS$DISK_CL_DRVR`) 809x and (`SCS$DIRECTORY`, `SCS$DIR_LOOKUP`) 201x, and on
the ACCEPT_REQ (type 2) answering those same connections it is exactly those
two strings SWAPPED. `MSCP$DISK` and `SCS$DIRECTORY` are the LISTENERS (SDA
`SHOW CONNECTIONS` prints them "(listen)", see
captures/sda-scs-extract-vax1.txt), so [62:78] on a CONNECT_REQ names the
SYSAP being connected TO.

MSCP supplies an independent, decisive confirmation that costs nothing here:
read the other way round, the Con.ID bound to `MSCP$DISK` would be the one the
CLASS DRIVER holds, and then every one of the 2,889 MSCP END messages would be
travelling from client to controller -- impossible, because an endcode is by
definition what a controller sends (Table A-1). Two independent tests, same
answer.

sec 4(N) labels [62:78] "the local SYSAP name" while keying its connect-data
census on it. Under sec 4(h)(2)'s grounded reading that label is wrong (it is
the DESTINATION), which does not move any of sec 4(N)'s counts -- they are
per-SYSAP either way, and this script re-derives 809/1101/324/2889 identically
-- but it does flip WHOSE version claim the `MSCP$DISK` row's
`"V5.0          + "` is. This script does not touch sec 4(N); the consequence
is written up in the spec sec 5 register and filed as its own rd item.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""
import argparse
import collections
import glob
import os
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

SCA_ETHERTYPE = b"\x60\x07"

# SCA content offsets shared by every envelope-conformant SCS message
# (spec sec 4(h)(1a), scs_rx.h): inner length, format word, message type,
# credit, then the (destination, sender) Con.ID pair.
OFF_INNER_LEN = 42
OFF_FORMAT = 44
OFF_MSGTYPE = 46
OFF_CREDIT = 48
OFF_CONID_DST = 50
OFF_CONID_SRC = 54
OFF_BODY = 58
# The two 16-byte ASCII SYSAP name fields on a connect frame, in the roles the
# docstring's measurement establishes.
OFF_NAME_DST = (62, 78)
OFF_NAME_SRC = (78, 94)

MSGTYPE_APPLICATION = 10
MSGTYPE_CONNECT_REQ = 0
MSGTYPE_ACCEPT_REQ = 2

LEN_MSCP_COMMAND = 94      # SCA content length of the MSCP command class
LEN_MSCP_GUS_END = 110     # ...and of the GET UNIT STATUS end-message class
LEN_MSCP_SCC_END = 86      # ...and of the SET CONTROLLER CHARACTERISTICS one

# --- MSCP numerics, ALL from AA-L619A-TK (public). Nothing here is inferred.
MSCP_OP_END = 0x80            # Table A-1 OP.END; endcode == command | OP.END
MSCP_OP_GTUNT = 0x03          # Table A-1 OP.GUS  GET UNIT STATUS
MSCP_OP_STCON = 0x04          # Table A-1 OP.SCC  SET CONTROLLER CHARACTERISTICS
MSCP_TABLE_A1_OPCODES = frozenset(
    (0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x10, 0x12, 0x14,
     0x20, 0x21, 0x22, 0x40, 0x41, 0x42))
MSCP_ST_MASK = 0x1F           # Table B-1 ST.MSK, 5-bit major status code
MSCP_ST_SUB = 0x20            # Table B-1 ST.SUB, sub-code multiplier
MSCP_ST_NAMES = {0: "Success", 1: "Invalid Command", 2: "Command Aborted",
                 3: "Unit-Offline", 4: "Unit-Available", 5: "Media Format Error",
                 6: "Write Protected", 7: "Compare Error", 8: "Data Error",
                 9: "Host Buffer Access Error", 10: "Controller Error",
                 11: "Drive Error", 31: "Message from an internal diagnostic"}
MSCP_MD_NXU = 0x0001          # Table A-2 MD.NXU, "Next Unit" (GET UNIT STATUS)

# Table A-7, GET UNIT STATUS end message. Offsets are body-relative, i.e. from
# SCA content [58]. (name, offset, size); size 0 marks a reserved run.
GUS_END_FIELDS = (
    ("cmd_ref", 0, 4),          # P.CRF  MSCP$L_CMD_REF   generic
    ("unit", 4, 2),             # P.UNIT MSCP$W_UNIT      generic
    ("reserved_6", 6, 2),       # generic reserved
    ("endcode", 8, 1),          # P.OPCD MSCP$B_OPCODE
    ("end_flags", 9, 1),        # P.FLGS MSCP$B_FLAGS   Table A-3
    ("status", 10, 2),          # P.STS  MSCP$W_STATUS  Table B-1/B-2
    ("multi_unit", 12, 2),      # P.MLUN MSCP$W_MULT_UNT
    ("unit_flags", 14, 2),      # P.UNFL MSCP$W_UNT_FLGS Table A-5
    ("reserved_16", 16, 4),     # GUS reserved
    ("unit_id", 20, 8),         # P.UNTI MSCP$Q_UNIT_ID
    ("media_id", 28, 4),        # P.MEDI MSCP$L_MEDIA_ID Appendix C
    ("shadow_unit", 32, 2),     # P.SHUN MSCP$W_SHDW_UNT
    ("track_size", 36, 2),      # P.TRCK MSCP$W_TRACK
    ("group_size", 38, 2),      # P.GRP  MSCP$W_GROUP
    ("cylinder_size", 40, 2),   # P.CYL  MSCP$W_CYLINDER
    ("reserved_42", 42, 2),     # GUS reserved
    ("rct_size", 44, 2),        # P.RCTS MSCP$W_RCT_SIZE
    ("rbns", 46, 1),            # P.RBNS MSCP$W_RBNS
    ("rct_copies", 47, 1),      # P.RCTC MSCP$B_RCT_CPYS
)
# The documented message ends at body[48]; Table A-7 defines nothing beyond it.
GUS_END_DOCUMENTED_LEN = 48
# The reserved runs a conforming sender must zero (AA-L619A-TK sec 5.2).
GUS_END_RESERVED = ((6, 2), (16, 4), (42, 2))

VAX_SOURCE_OUIS = ("08002b", "aa0004")
OVMX_HW_MACS = frozenset(("b6168adc3a53", "e684efb14fee"))
VAX, OVMX = "vax", "ovmx"

# vms-69c: the ONE restriction this script applies, and its written reason.
TYPE10_RESTRICT_REASON = (
    "part (D) decodes ONE class -- the 110-content MSCP GET UNIT STATUS end "
    "message. Parts (A)/(B)/(C) run with no length filter at all and report "
    "every envelope-conformant class, so the restriction is a focus of the "
    "decode, not of the census: the excluded classes are counted and printed "
    "by this same script (see 'unrestricted census' in --print output)."
)


def lab1_only(paths):
    """Reject any capture not DECLARED lab-1 in tools/cluster/capture_manifest.py.

    Same fence every lab-1-grounded tool carries (vms-beb): lab-2 replicas
    reuse lab-1's SCSSYSTEMIDs and node MACs by design, so a lab-2 capture in
    this directory silently moves every census here.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import capture_manifest
    return capture_manifest.check_paths(paths, capture_manifest.LAB1)


def _read_pcap():
    """Imported LAZILY so the figures gate can read EXPECTED out of this module
    without dissect_sca.py on the path, and so the mutation harness can copy
    this file into a scratch tree beside it."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    from dissect_sca import read_pcap
    return read_pcap


def _census_guard():
    """Imported lazily for the same reason."""
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    import census_guard
    return census_guard


def u16(b, o):
    return b[o] | (b[o + 1] << 8)


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def classify_source(mac_hex):
    """VAX, OVMX, or None. None is a FAILURE, never a shrug."""
    if mac_hex in OVMX_HW_MACS:
        return OVMX
    if mac_hex.startswith(VAX_SOURCE_OUIS):
        return VAX
    return None


def media_name(v):
    """Decode an MSCP media type identifier (AA-L619A-TK sec 4.17 + Appendix C).

    32 bits: D0 (bits 31-27) and D1 (26-22) are the two characters of the
    DEVICE TYPE name; A0/A1/A2 (21-17, 16-12, 11-7) are one to three characters
    of the MEDIA name, left justified, 0 = absent; N (bits 6-0) holds the two
    decimal digits. "A" is encoded 1, "B" 2, and so on. Appendix C Table C-3
    publishes the worked value: RA80 = hex 2564,1050 -- which this function
    must reproduce, and rederive() checks that it does.
    """
    ch = lambda x: chr(64 + x) if x else ""
    dev = ch((v >> 27) & 0x1F) + ch((v >> 22) & 0x1F)
    media = "".join(ch((v >> s) & 0x1F) for s in (17, 12, 7))
    return "%s %s%02d" % (dev, media, v & 0x7F)


def status_name(st):
    """AA-L619A-TK sec 5.6: 5-bit major code + 11-bit sub-code."""
    return "%s/sub %d" % (MSCP_ST_NAMES.get(st & MSCP_ST_MASK, "?"),
                          st // MSCP_ST_SUB)


# ---------------------------------------------------------------------------
# EXPECTED -- the checked-in record of what the 48 lab-1 captures measured on
# 2026-08-06. Re-derived from the packets by rederive(); pinned to the prose of
# docs/cluster-protocol-spec.md sec 4(h)(1e) by tests/vmsscs/
# test_scs_type10_figures.py.
# ---------------------------------------------------------------------------
EXPECTED = {
    "pcaps_scanned": 48,
    "unclassified_sources": 0,

    # (A) the UNRESTRICTED census: (SCA content length, message type) -> frames.
    "vax_census": {
        (58, 5): 697, (58, 7): 223, (58, 8): 131, (58, 9): 89,
        (62, 3): 258, (62, 4): 453, (62, 6): 220, (66, 1): 778,
        (86, 10): 202, (94, 10): 3621,
        (110, 0): 1101, (110, 2): 324, (110, 10): 2889,
        (190, 10): 299224,
    },
    "ovmx_census": {
        (58, 5): 1, (58, 7): 42, (58, 9): 42,
        (62, 3): 123, (62, 4): 280, (62, 6): 42, (66, 1): 338,
        (94, 10): 585,
        (110, 0): 396, (110, 2): 70,
        (190, 10): 7446,
    },
    # Type 10 is not one class: it is the carrier of every SYSAP payload, and
    # it occupies four length classes. OVMX emits it in two of them and has
    # never emitted a 110-content one.
    "type10_lengths_vax": {86: 202, 94: 3621, 110: 2889, 190: 299224},
    "type10_lengths_ovmx": {94: 585, 190: 7446},

    # (B) the pairing that IDENTIFIES the class. Zero unmatched.
    "gus_end_frames": 2889,
    "gus_end_matched": 2889,
    "gus_end_unmatched": 0,
    "opcode_pairing": {(0x03, 0x83): 2889},
    "gus_end_sources": {"08:00:2b:78:56:b9": 1126,
                        "aa:00:04:00:01:04": 1194,
                        "08:00:2b:11:22:33": 569},

    # (C) the SYSAP filter. Con.ID pairs bound by a connect frame in the same
    # capture resolve to ONE SYSAP pair and no other; the rest have no connect
    # frame in-capture (the connection predates it).
    "gus_end_sysap_pairs": {("VMS$DISK_CL_DRVR", "MSCP$DISK"): 1723},
    "gus_end_sysap_unbound": 1166,
    # The category error, measured: body[8] on the 94-content class by SYSAP.
    "body8_mscp_connection": {0x03: 1715, 0x04: 202},
    "body8_directory_connection": {0x24: 942, 0x56: 178},
    "body8_mscp_all_table_a1": True,
    "body8_directory_any_table_a1": False,
    # The name-field roles the docstring's correction rests on.
    # The two ASYMMETRIC connections are the informative rows: the listener's
    # name is at [62:78] on the CONNECT_REQ and at [78:94] on the ACCEPT_REQ.
    # VMS$VAXcluster and SCA$TRANSPORT connect to their own namesake and so say
    # nothing either way -- recorded because a census that quietly drops its
    # uninformative rows is not a census.
    "connect_name_pairs": {
        (0, "MSCP$DISK", "VMS$DISK_CL_DRVR"): 809,
        (0, "SCS$DIRECTORY", "SCS$DIR_LOOKUP"): 201,
        (2, "SCS$DIR_LOOKUP", "SCS$DIRECTORY"): 134,
        (2, "VMS$DISK_CL_DRVR", "MSCP$DISK"): 101,
        (0, "VMS$VAXcluster", "VMS$VAXcluster"): 75,
        (2, "VMS$VAXcluster", "VMS$VAXcluster"): 73,
        (0, "SCA$TRANSPORT", "SCA$TRANSPORT"): 16,
        (2, "SCA$TRANSPORT", "SCA$TRANSPORT"): 16,
    },

    # (D) the field decode, Table A-7 + sec 6.12.
    "endcode_census": {0x83: 2889},
    "end_flags_census": {0x00: 2889},
    "status_census": {0x03: 2485, 0x04: 402, 0x23: 2},
    "status_all_documented": True,
    # sec 6.12's validity rule as a PARTITION: unit identifier zero iff the
    # status is the plain Unit-Offline code 3 (sub-code 0). Zero residuals.
    "status_unitid_partition": {(0x03, True): 2485, (0x04, False): 402,
                                (0x23, False): 2},
    "status_unitid_residuals": 0,
    "valid_unit_frames": 404,
    "reserved_violations_valid": 0,
    # unit -> (unit id, media id, decoded media name, multi-unit code,
    #          unit flags, frames)
    "unit_table": {
        0x4000: ("020000000000a112", 0x2564105C, "DU RA92", 0x0000, 0x8000, 101),
        0x4001: ("020000000100a112", 0x2564105C, "DU RA92", 0x0100, 0x8000, 101),
        0x4002: ("020000000200a112", 0x25652228, "DU RRD40", 0x0200, 0x8000, 101),
        0x4003: ("020000000300a112", 0x25652228, "DU RRD40", 0x0300, 0x8000, 101),
    },
    # geometry, valid-unit frames only: media -> (track, group, cylinder,
    # rct_size, rbns, rct_copies) -> frames.
    "geometry": {
        "DU RA92": {(73, 13, 1, 949, 1, 1): 188, (0, 0, 0, 0, 0, 0): 14},
        "DU RRD40": {(0, 0, 0, 0, 0, 0): 202},
    },
    "shadow_unit_census": {0x0000: 2889},
    # sec 6.12's Next Unit enumeration, read off the answered commands:
    # (command modifier word, unit answered) -> frames.
    "next_unit_walk": {(0x0000, 0x0000): 2384, (0x0001, 0x4000): 101,
                       (0x0001, 0x4001): 101, (0x0001, 0x4002): 101,
                       (0x0001, 0x4003): 101, (0x0001, 0x0000): 101},

    # (E) the residue -- what is NOT decoded.
    "trailer_48_census": {"6e00": 2889},
    "trailer_50_distinct": 32,
    "unit_flags_undocumented_bit": 0x8000,

    # Appendix C's own worked example, used as the calibration for media_name().
    "media_name_ra80_check": (0x25641050, "DU RA80"),
}

# vms-371: every EXPECTED figure is re-derived from the packets by rederive()
# except the ones a pcap cannot show.
NON_WIRE_KEYS = (
    # A published value from AA-L619A-TK Table C-3, not a capture measurement.
    # It calibrates media_name(); the captures cannot contain an RA80 because
    # the lab has none (vax.ini attaches ra92 and cdrom units only).
    "media_name_ra80_check",
)
WIRE_KEYS = tuple(k for k in EXPECTED if k not in NON_WIRE_KEYS)


def load_frames(path, read_pcap):
    """Every envelope-conformant SCA frame in one pcap, in wire order."""
    cg = _census_guard()
    out = []
    for ts, tu, _o, frame in read_pcap(path):
        if len(frame) < 16 or frame[12:14] != SCA_ETHERTYPE:
            continue
        pl = frame[14:]
        if len(pl) < 2:
            continue
        scalen = (pl[0] | (pl[1] << 8)) + 2
        if len(pl) < scalen:
            continue
        pl = bytes(pl[:scalen])
        if not cg.envelope_conformant(pl):
            continue
        out.append({
            "t": ts + tu / 1e6,
            "len": scalen,
            "pl": pl,
            "mt": u16(pl, OFF_MSGTYPE),
            "dst": u32(pl, OFF_CONID_DST),
            "src": u32(pl, OFF_CONID_SRC),
            "mac": frame[6:12].hex(),
            "pop": classify_source(frame[6:12].hex()),
        })
    return out


def measure(capdir):
    read_pcap = _read_pcap()
    files = lab1_only(sorted(glob.glob(os.path.join(capdir, "**", "*.pcap"),
                                       recursive=True)))
    cg = _census_guard()

    vax_census = collections.Counter()
    ovmx_census = collections.Counter()
    unclassified = collections.Counter()
    t10_len = {VAX: collections.Counter(), OVMX: collections.Counter()}
    name_pairs = collections.Counter()

    gus_end = 0
    matched = 0
    pairing = collections.Counter()
    sources = collections.Counter()
    sysap_pairs = collections.Counter()
    sysap_unbound = 0
    body8_mscp = collections.Counter()
    body8_dir = collections.Counter()
    endcodes = collections.Counter()
    end_flags = collections.Counter()
    statuses = collections.Counter()
    partition = collections.Counter()
    partition_residuals = 0
    valid_units = 0
    reserved_bad = 0
    unit_rows = collections.defaultdict(collections.Counter)
    geometry = collections.defaultdict(collections.Counter)
    shadow = collections.Counter()
    next_unit = collections.Counter()
    trailer48 = collections.Counter()
    trailer50 = collections.Counter()
    skipped = []

    for path in files:
        base = os.path.basename(path)
        try:
            frames = load_frames(path, read_pcap)
        except Exception as exc:                     # a truncated file must not
            skipped.append((base, str(exc)))         # hide the rest
            continue

        # (A) unrestricted census + Con.ID -> SYSAP bindings from connect frames.
        sysap = {}
        for f in frames:
            key = (f["len"], f["mt"])
            if f["pop"] == VAX:
                vax_census[key] += 1
            elif f["pop"] == OVMX:
                ovmx_census[key] += 1
            else:
                unclassified[f["mac"]] += 1
            if f["mt"] == MSGTYPE_APPLICATION and f["pop"] in (VAX, OVMX):
                t10_len[f["pop"]][f["len"]] += 1
            if f["len"] == LEN_MSCP_GUS_END and f["mt"] in (MSGTYPE_CONNECT_REQ,
                                                            MSGTYPE_ACCEPT_REQ):
                pl = f["pl"]
                dname = pl[OFF_NAME_DST[0]:OFF_NAME_DST[1]].decode("latin1").rstrip()
                sname = pl[OFF_NAME_SRC[0]:OFF_NAME_SRC[1]].decode("latin1").rstrip()
                if f["pop"] == VAX:
                    name_pairs[(f["mt"], dname, sname)] += 1
                if f["src"]:
                    sysap[f["src"]] = sname
                if f["dst"]:
                    sysap[f["dst"]] = dname

        # (C) body[8] by owning SYSAP on the command class.
        commands = {}
        for f in frames:
            if f["len"] != LEN_MSCP_COMMAND or f["mt"] != MSGTYPE_APPLICATION:
                continue
            b = f["pl"][OFF_BODY:]
            commands[(f["dst"], f["src"], u32(b, 0))] = f
            pair = frozenset((sysap.get(f["src"]), sysap.get(f["dst"])))
            if pair == frozenset(("MSCP$DISK", "VMS$DISK_CL_DRVR")):
                body8_mscp[b[8]] += 1
            elif pair == frozenset(("SCS$DIRECTORY", "SCS$DIR_LOOKUP")):
                body8_dir[b[8]] += 1

        # (B)/(D)/(E) the 110-content type-10 class itself.
        for f in frames:
            if f["len"] != LEN_MSCP_GUS_END or f["mt"] != MSGTYPE_APPLICATION:
                continue
            b = f["pl"][OFF_BODY:]
            gus_end += 1
            sources[":".join(f["mac"][i:i + 2] for i in range(0, 12, 2))] += 1
            endcodes[b[8]] += 1
            end_flags[b[9]] += 1
            st = u16(b, 10)
            statuses[st] += 1
            uid = int.from_bytes(b[20:28], "little")
            partition[(st, uid == 0)] += 1
            # sec 6.12: unit id zero <=> no characteristics. The residual is
            # "status says a unit is there but the identifier is zero", or the
            # reverse.
            if (st & MSCP_ST_MASK) in (4,) and uid == 0:
                partition_residuals += 1
            if st == 0x03 and uid != 0:
                partition_residuals += 1
            shadow[u16(b, 32)] += 1
            trailer48[b[48:50].hex()] += 1
            trailer50[b[50:52].hex()] += 1

            cmd = commands.get((f["src"], f["dst"], u32(b, 0)))
            if cmd is not None:
                matched += 1
                cb = cmd["pl"][OFF_BODY:]
                pairing[(cb[8], b[8])] += 1
                next_unit[(u16(cb, 10), u16(b, 4))] += 1

            names = frozenset((sysap.get(f["src"]), sysap.get(f["dst"])))
            if None in names:
                sysap_unbound += 1
            else:
                sysap_pairs[tuple(sorted(names, reverse=True))] += 1

            if uid:
                valid_units += 1
                for off, size in GUS_END_RESERVED:
                    if b[off:off + size] != b"\x00" * size:
                        reserved_bad += 1
                        break
                mid = u32(b, 28)
                unit_rows[u16(b, 4)][(b[20:28].hex(), mid, media_name(mid),
                                      u16(b, 12), u16(b, 14))] += 1
                geometry[media_name(mid)][(u16(b, 36), u16(b, 38), u16(b, 40),
                                           u16(b, 44), b[46], b[47])] += 1

    # vms-69c: the one restriction, with its reason, checked against the whole
    # unfiltered envelope-conformant population.
    conformant, raw = cg.population(files, read_pcap)
    guard = cg.check_census([LEN_MSCP_GUS_END], conformant, raw,
                            restrict_reason=TYPE10_RESTRICT_REASON,
                            label="scs_type10_measure.py: ")

    return {
        "n_captures": len(files),
        "skipped": skipped,
        "census_guard": guard,
        "vax_census": dict(vax_census),
        "ovmx_census": dict(ovmx_census),
        "unclassified": dict(unclassified),
        "type10_lengths_vax": dict(t10_len[VAX]),
        "type10_lengths_ovmx": dict(t10_len[OVMX]),
        "connect_name_pairs": dict(name_pairs),
        "gus_end_frames": gus_end,
        "gus_end_matched": matched,
        "gus_end_unmatched": gus_end - matched,
        "opcode_pairing": dict(pairing),
        "gus_end_sources": dict(sources),
        "gus_end_sysap_pairs": dict(sysap_pairs),
        "gus_end_sysap_unbound": sysap_unbound,
        "body8_mscp_connection": dict(body8_mscp),
        "body8_directory_connection": dict(body8_dir),
        "endcode_census": dict(endcodes),
        "end_flags_census": dict(end_flags),
        "status_census": dict(statuses),
        "status_unitid_partition": dict(partition),
        "status_unitid_residuals": partition_residuals,
        "valid_unit_frames": valid_units,
        "reserved_violations_valid": reserved_bad,
        "unit_table": {u: dict(rows) for u, rows in unit_rows.items()},
        "geometry": {m: dict(g) for m, g in geometry.items()},
        "shadow_unit_census": dict(shadow),
        "next_unit_walk": dict(next_unit),
        "trailer_48_census": dict(trailer48),
        "trailer_50_distinct": len(trailer50),
        "trailer_50_top": trailer50.most_common(8),
    }


def report(m):
    print("captures: %d" % m["n_captures"])
    for base, err in m["skipped"]:
        print("  SKIPPED %s: %s" % (base, err))
    print("\n=== CENSUS GUARD (vms-69c) ===")
    print(_census_guard().format_report(m["census_guard"]))

    print("\n(A) UNRESTRICTED census, (SCA content length, message type) -> frames")
    print("    real VAX:")
    for k in sorted(m["vax_census"]):
        print("      %3d B  mt=%-3d %7d" % (k[0], k[1], m["vax_census"][k]))
    print("    OVMX (never evidence about VMS, reported only):")
    for k in sorted(m["ovmx_census"]):
        print("      %3d B  mt=%-3d %7d" % (k[0], k[1], m["ovmx_census"][k]))
    print("    unclassified sources (MUST be empty): %s" % (m["unclassified"] or "{}"))
    print("    type 10 by length: VAX %s / OVMX %s"
          % (m["type10_lengths_vax"], m["type10_lengths_ovmx"]))

    print("\n(B) 110-content type-10 -> its MSCP command, by cmd-ref on the "
          "mirrored Con.ID pair")
    print("    frames=%d matched=%d unmatched=%d"
          % (m["gus_end_frames"], m["gus_end_matched"], m["gus_end_unmatched"]))
    for (cmd, end), n in sorted(m["opcode_pairing"].items()):
        print("    command opcode 0x%02x -> endcode 0x%02x   n=%d   (%s)"
              % (cmd, end, n,
                 "endcode == command | OP.END" if end == cmd | MSCP_OP_END
                 else "NOT command|OP.END"))
    print("    sources: %s" % m["gus_end_sources"])

    print("\n(C) owning SYSAP (Con.IDs bound by a connect frame in the same capture)")
    for pair, n in sorted(m["gus_end_sysap_pairs"].items()):
        print("    %s  n=%d" % (" <-> ".join(pair), n))
    print("    no connect frame in-capture: %d" % m["gus_end_sysap_unbound"])
    print("    body[8] on the 94-content class, MSCP connection:  %s"
          % {hex(k): v for k, v in sorted(m["body8_mscp_connection"].items())})
    print("    body[8] on the 94-content class, directory SYSAPs: %s"
          % {hex(k): v for k, v in sorted(m["body8_directory_connection"].items())})
    print("    connect-frame SYSAP name pairs (mt, [62:78], [78:94]):")
    for k, n in sorted(m["connect_name_pairs"].items(), key=lambda kv: -kv[1]):
        print("      mt=%d  %-16s %-16s n=%d" % (k[0], k[1], k[2], n))

    print("\n(D) field decode against AA-L619A-TK Table A-7 / sec 6.12")
    print("    endcode        %s" % {hex(k): v for k, v in m["endcode_census"].items()})
    print("    end flags      %s (Table A-3)"
          % {hex(k): v for k, v in m["end_flags_census"].items()})
    print("    status         %s"
          % {("0x%02x %s" % (k, status_name(k))): v
             for k, v in sorted(m["status_census"].items())})
    print("    status vs unit-id==0 partition: %s residuals=%d"
          % ({("0x%02x" % k[0], k[1]): v
              for k, v in sorted(m["status_unitid_partition"].items())},
             m["status_unitid_residuals"]))
    print("    valid-unit frames=%d  reserved-field violations=%d"
          % (m["valid_unit_frames"], m["reserved_violations_valid"]))
    for u in sorted(m["unit_table"]):
        for row, n in sorted(m["unit_table"][u].items()):
            print("    unit 0x%04x  unit-id %s  media 0x%08x = %-8s  "
                  "multi-unit 0x%04x  unit-flags 0x%04x  n=%d"
                  % (u, row[0], row[1], row[2], row[3], row[4], n))
    for media in sorted(m["geometry"]):
        print("    geometry %-8s %s" % (media, m["geometry"][media]))
    print("    shadow unit: %s"
          % {hex(k): v for k, v in m["shadow_unit_census"].items()})
    print("    Next Unit walk (command modifier, unit answered):")
    for k, n in sorted(m["next_unit_walk"].items()):
        print("      modifier 0x%04x -> unit 0x%04x  n=%d%s"
              % (k[0], k[1], n, "   (MD.NXU)" if k[0] & MSCP_MD_NXU else ""))

    print("\n(E) NOT DECODED -- the residue past Table A-7's last field (body[48])")
    print("    body[48:50]: %s  (constant; 0x006e == %d == this class's SCA "
          "content length, so a length echo and a constant are "
          "indistinguishable on a single-length population)"
          % (m["trailer_48_census"], LEN_MSCP_GUS_END))
    print("    body[50:52]: %d distinct values, top %s"
          % (m["trailer_50_distinct"], m["trailer_50_top"]))


def compare_results(m):
    """Every EXPECTED figure re-derived, as [(ok, message), ...]."""
    out = []

    def ck(cond, what):
        out.append((bool(cond), what))

    ck(m["n_captures"] == EXPECTED["pcaps_scanned"],
       "pcaps_scanned %d != %d" % (m["n_captures"], EXPECTED["pcaps_scanned"]))
    ck(sum(m["unclassified"].values()) == EXPECTED["unclassified_sources"],
       "unclassified_sources %s -- the OUI rule could not place a source; it "
       "must not silently join either population" % m["unclassified"])

    for key in ("vax_census", "ovmx_census", "type10_lengths_vax",
                "type10_lengths_ovmx", "connect_name_pairs", "opcode_pairing",
                "gus_end_sources", "gus_end_sysap_pairs",
                "body8_mscp_connection", "body8_directory_connection",
                "endcode_census", "end_flags_census", "status_census",
                "status_unitid_partition", "shadow_unit_census",
                "next_unit_walk", "trailer_48_census"):
        ck(m[key] == EXPECTED[key],
           "%s %r != %r" % (key, m[key], EXPECTED[key]))

    for key in ("gus_end_frames", "gus_end_matched", "gus_end_unmatched",
                "gus_end_sysap_unbound", "status_unitid_residuals",
                "valid_unit_frames", "reserved_violations_valid",
                "trailer_50_distinct"):
        ck(m[key] == EXPECTED[key],
           "%s %r != %r" % (key, m[key], EXPECTED[key]))

    # The unit table, flattened to the shape EXPECTED records.
    got_units = {}
    for u, rows in m["unit_table"].items():
        (row, n), = rows.items()
        got_units[u] = (row[0], row[1], row[2], row[3], row[4], n)
    ck(got_units == EXPECTED["unit_table"],
       "unit_table %r != %r" % (got_units, EXPECTED["unit_table"]))
    ck(m["geometry"] == EXPECTED["geometry"],
       "geometry %r != %r" % (m["geometry"], EXPECTED["geometry"]))

    # Derived claims the prose makes, each re-derived rather than restated.
    ck(all(op in MSCP_TABLE_A1_OPCODES for op in m["body8_mscp_connection"])
       == EXPECTED["body8_mscp_all_table_a1"],
       "body8_mscp_all_table_a1: %s" % sorted(m["body8_mscp_connection"]))
    ck(any(op in MSCP_TABLE_A1_OPCODES for op in m["body8_directory_connection"])
       == EXPECTED["body8_directory_any_table_a1"],
       "body8_directory_any_table_a1: %s"
       % sorted(m["body8_directory_connection"]))
    ck(all((st & MSCP_ST_MASK) in MSCP_ST_NAMES for st in m["status_census"])
       == EXPECTED["status_all_documented"],
       "status_all_documented: %s" % sorted(m["status_census"]))
    flags = set()
    for rows in m["unit_table"].values():
        for row in rows:
            flags.add(row[4])
    ck(flags == {EXPECTED["unit_flags_undocumented_bit"]},
       "unit_flags_undocumented_bit: measured %s" % sorted(flags))
    val, name = EXPECTED["media_name_ra80_check"]
    ck(media_name(val) == name,
       "media_name(0x%08x) = %r, Appendix C Table C-3 publishes %r"
       % (val, media_name(val), name))
    return out


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371)."""
    results = compare_results(measure(capdir))
    for key in WIRE_KEYS:                 # record coverage for _KeyRecorder
        EXPECTED[key]
    return results, set(WIRE_KEYS)


def compare(m):
    out = compare_results(m)
    fails = [what for ok, what in out if not ok]
    for what in fails:
        print("  FAIL %s" % what)
    print("%s: %d checks, %d failure(s)"
          % ("FAIL" if fails else "PASS", len(out), len(fails)))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="just_print", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        print("no capture directory at %s -- this script needs the host-only "
              "lab-1 captures (CLAUDE.md rule 8). Use --captures."
              % args.captures, file=sys.stderr)
        return 2
    m = measure(args.captures)
    report(m)
    if args.just_print:
        return 0
    print()
    return compare(m)


if __name__ == "__main__":
    sys.exit(main())
