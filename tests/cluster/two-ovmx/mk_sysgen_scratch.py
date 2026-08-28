#!/usr/bin/env python3
"""mk_sysgen_scratch.py <out.dat> <SCSNODE> <SCSSYSTEMID> [ALLOCLASS] [RECNXINTERVAL]

Generate a SYSG v2 OVMXVMSSYS.PAR store FROM SCRATCH -- no lab template needed.

The two-OVMX harness must be HERMETIC (CLAUDE.md: no VAX images, runs anywhere),
so it cannot depend on tests/lab/tools/mk_sysgen.py, which patches a lab-1-only
proven-good template (/data/training/vax/cluster/work/sysgen-s8.dat). SCSD.EXE
reads exactly four SYSGEN params -- SCSNODE (string), SCSSYSTEMID, ALLOCLASS,
RECNXINTERVAL (numeric) -- via sysgen_read_string/sysgen_read_param over the
OVMX_SYSGEN_PATH literal-file override (src/libvms/include/sysgen_params.h).
Every other field in a real store is irrelevant to SCSD, so a minimal store
carrying only these four is byte-sufficient for its identity adoption.

Binary layout (src/libvms/include/sysgen_params.h, v2, little-endian):
  header : magic u32 0x53595347 "GSYS" | version u32 = 2 | count u32
  param  : name[32] current u32 default u32 min u32 max u32 flags u8
           description[80] type u8 str_current[8] str_default[8]
           = 146 bytes, padded to 148 by struct 4-byte alignment
  file   : 12 + 64*148 = 9484 bytes (SYSGEN_MAX_PARAMS = 64)
"""
import struct, sys

MAGIC, VERSION, MAXP = 0x53595347, 2, 64
HDR, PSZ = 12, 148
TYPE_NUMERIC, TYPE_STRING = 0, 1


def param(name, *, current=0, default=0, minv=0, maxv=0, flags=0,
          desc="", ptype=TYPE_NUMERIC, strcur="", strdef=""):
    b = bytearray(PSZ)
    b[0:32] = name.encode("ascii").ljust(32, b"\0")[:32]
    struct.pack_into("<IIII", b, 32, current, default, minv, maxv)
    b[48] = flags
    b[49:49 + 80] = desc.encode("ascii").ljust(80, b"\0")[:80]
    b[129] = ptype
    b[130:138] = strcur.encode("ascii").ljust(8, b"\0")[:8]
    b[138:146] = strdef.encode("ascii").ljust(8, b"\0")[:8]
    return bytes(b)


def main():
    a = sys.argv[1:]
    if not (3 <= len(a) <= 5):
        sys.exit("usage: mk_sysgen_scratch.py <out> <SCSNODE> <SCSSYSTEMID> "
                 "[ALLOCLASS] [RECNXINTERVAL]")
    out, node, sysid = a[0], a[1], int(a[2])
    alloc = int(a[3]) if len(a) >= 4 else 0
    recnx = int(a[4]) if len(a) >= 5 else 20
    if len(node) > 6 or not node.isalnum() or not node.isascii():
        sys.exit("SCSNODE must be <=6 alphanumeric ASCII chars (VMS truncates to 6)")
    if not (0 < sysid < 65536):
        sys.exit("SCSSYSTEMID out of range 1..65535")

    params = [
        param("SCSNODE", ptype=TYPE_STRING, strcur=node, strdef=node,
              desc="cluster node name"),
        param("SCSSYSTEMID", current=sysid, default=sysid, maxv=65535,
              desc="cluster system id"),
        param("ALLOCLASS", current=alloc, default=alloc, maxv=255,
              desc="allocation class"),
        param("RECNXINTERVAL", current=recnx, default=recnx, minv=1, maxv=65535,
              desc="reconnection interval seconds"),
    ]
    buf = bytearray(struct.pack("<III", MAGIC, VERSION, len(params)))
    for p in params:
        buf += p
    buf += bytes(PSZ * (MAXP - len(params)))
    assert len(buf) == HDR + MAXP * PSZ, len(buf)
    with open(out, "wb") as f:
        f.write(buf)
    print("mk_sysgen_scratch: %s SCSNODE=%s SCSSYSTEMID=%d ALLOCLASS=%d RECNXINTERVAL=%d"
          % (out, node, sysid, alloc, recnx))


main()
