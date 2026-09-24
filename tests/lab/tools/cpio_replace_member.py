#!/usr/bin/env python3
"""cpio_replace_member.py - replace ONE file inside a gzipped newc cpio archive.

Written for the in-browser cluster proof (rd vms-151): the demo's OVMX/x86
initramfs carries the executive as `lib/modules/vms.ko`, and proving a change to
the executive in the browser means putting the REBUILT module into the SAME
image the proven boot already uses -- not rebuilding the whole distribution and
changing twenty other things at once.

It is deliberately dumb: it walks the newc headers, rewrites the named member's
data and its `filesize` field, and copies every other byte through. Nothing else
in the archive moves, so a diff of the two images is exactly the module.

  usage: cpio_replace_member.py <in.cpio.gz> <member-path> <new-file> <out.cpio.gz>
"""
import gzip
import sys

NEWC_MAGIC = b"070701"
HDR_LEN = 110


def _pad4(n):
    return (n + 3) & ~3


def _hex8(n):
    return b"%08X" % n


def replace(data, member, payload):
    """Return `data` with `member`'s contents replaced by `payload`."""
    out = bytearray()
    i = 0
    hits = 0
    while i < len(data):
        if data[i:i + 6] != NEWC_MAGIC:
            raise SystemExit("not a newc cpio at offset %d" % i)
        namesize = int(data[i + 94:i + 102], 16)
        filesize = int(data[i + 54:i + 62], 16)
        name = data[i + HDR_LEN:i + HDR_LEN + namesize - 1].decode("latin1")
        name_end = _pad4(i + HDR_LEN + namesize)
        data_end = _pad4(name_end + filesize)

        if name == member:
            hits += 1
            hdr = bytearray(data[i:i + HDR_LEN])
            hdr[54:62] = _hex8(len(payload))
            out += hdr
            out += data[i + HDR_LEN:name_end]
            out += payload
            out += b"\0" * (_pad4(len(payload)) - len(payload))
        else:
            out += data[i:data_end]
        i = data_end
        if name == "TRAILER!!!":
            out += data[i:]        # whatever padding follows the trailer
            break
    if hits != 1:
        raise SystemExit("expected exactly 1 %r, found %d" % (member, hits))
    return bytes(out)


def main(argv):
    if len(argv) != 5:
        raise SystemExit(__doc__)
    src, member, newfile, dst = argv[1:]
    with gzip.open(src, "rb") as f:
        data = f.read()
    with open(newfile, "rb") as f:
        payload = f.read()
    out = replace(data, member, payload)
    with gzip.open(dst, "wb") as f:
        f.write(out)
    print("replaced %s (%d bytes) in %s -> %s" % (member, len(payload), src, dst))


if __name__ == "__main__":
    main(sys.argv)
