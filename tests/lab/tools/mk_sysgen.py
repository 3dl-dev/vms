#!/usr/bin/env python3
"""mk_sysgen.py <out.dat> <SCSNODE> <SCSSYSTEMID> [template.dat]

x86_64 replacement for tools/mk_sysgen, which is an aarch64 ELF binary with no
surviving source and therefore does not run on the workshop dev host
(vms-2f3 bring-up 2026-08-02).

It does NOT regenerate the store from scratch: it copies a known-good template
and patches only SCSNODE and SCSSYSTEMID in place. The default template is the
store used by runs s8A/s8B, both of which JOINED, so every other field is
byte-identical to a store proven to work on this lab.

Layout is from src/libvms/include/sysgen_params.h (v2, SYSGEN_MAX_PARAMS=64):
  header  = magic u32 "GSYS" | version u32 | count u32          (12 bytes)
  param   = name[32] current u32 default u32 min u32 max u32
            flags u8 description[80] type u8 str_current[8] str_default[8]
            -> 146 bytes, padded to 148 by 4-byte alignment
  file    = 12 + 64*148 = 9484 bytes

VMS truncates SCSNODE to 6 characters, so OVMXR11 and OVMXR12 both go on the
wire as OVMXR1 and a grep for the full name silently finds nothing and looks
like a failed run. Names longer than 6 chars are rejected here instead.
"""
import shutil, struct, sys, os

HDR = 12
PSZ = 148
NAME, CUR, DEF, FLAGS, DESC, TYPE, STRCUR, STRDEF = 0, 32, 36, 48, 49, 129, 130, 138
DEFAULT_TEMPLATE = "/home/baron/vax/cluster/work/sysgen-s8.dat"

def die(m):
    print("mk_sysgen.py: FATAL -- " + m, file=sys.stderr)
    sys.exit(2)

def main():
    if len(sys.argv) not in (4, 5):
        die("usage: mk_sysgen.py <out.dat> <SCSNODE> <SCSSYSTEMID> [template.dat]")
    out, node, sysid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    tmpl = sys.argv[4] if len(sys.argv) == 5 else DEFAULT_TEMPLATE

    if len(node) > 6:
        die("SCSNODE %r is %d chars; VMS truncates to 6 and the run becomes "
            "indistinguishable from another identity." % (node, len(node)))
    if not node.isascii() or not node.isalnum():
        die("SCSNODE %r must be alphanumeric ASCII" % node)
    if not (0 < sysid < 65536):
        die("SCSSYSTEMID %d out of range" % sysid)
    if not os.path.isfile(tmpl):
        die("template %s not found" % tmpl)

    buf = bytearray(open(tmpl, "rb").read())
    if len(buf) != HDR + 64 * PSZ:
        die("template is %d bytes, expected %d" % (len(buf), HDR + 64 * PSZ))
    magic, version, count = struct.unpack_from("<III", buf, 0)
    if magic != 0x53595347 or version != 2:
        die("template magic/version %08x/%d is not SYSG v2" % (magic, version))

    seen = {}
    for i in range(count):
        base = HDR + i * PSZ
        name = buf[base:base + 32].split(b"\0")[0].decode()
        seen[name] = base
        if name == "SCSNODE":
            if buf[base + TYPE] != 1:
                die("SCSNODE in template is not string-typed")
            for off in (STRCUR, STRDEF):
                buf[base + off:base + off + 8] = node.encode().ljust(8, b"\0")
        elif name == "SCSSYSTEMID":
            if buf[base + TYPE] != 0:
                die("SCSSYSTEMID in template is not numeric-typed")
            struct.pack_into("<I", buf, base + CUR, sysid)
            struct.pack_into("<I", buf, base + DEF, sysid)

    for req in ("SCSNODE", "SCSSYSTEMID"):
        if req not in seen:
            die("template has no %s parameter" % req)

    open(out, "wb").write(buf)

    # The prior-admission record (vms-2f3 sec 4d.2) lives beside the store and IS
    # keyed to this identity. A store minted fresh must not inherit one, or the
    # "fresh identity" positive control is really a rejoin.
    side = out + ".cluster"
    if os.path.exists(side):
        os.remove(side)
        print("mk_sysgen.py: removed stale %s" % side)

    print("mk_sysgen.py: %s SCSNODE=%s SCSSYSTEMID=%d (template %s)"
          % (out, node, sysid, tmpl))

main()
