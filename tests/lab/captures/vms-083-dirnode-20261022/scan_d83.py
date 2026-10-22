#!/usr/bin/env python3
"""scan_d83.py -- the read this arm's FIRE.md is scored from (rd vms-083).

usage: scan_d83.py <pcap> [<name-prefix>]      default prefix D83

Prints, for every frame in the capture whose counted resource name begins with
the prefix, the sender and destination, the DLM category and opcode, the
outcome byte, the 32-bit directory hash at body[128:132] and the handle at
body[20:24]. The arm's whole claim is a COUNT over the op-0x13 lines of this
output, per tree name, so the read is a rule and not an eyeball.

Clean-room: it decodes nothing new -- the offsets are the ones
docs/cluster-protocol-spec.md 4(f) already grounds, reached through the shared
walker in tools/cluster/remaster_census.py.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'tools',
                                'cluster'))
import remaster_census as rc  # noqa: E402


def rows(path, prefix):
    for idx, src, dst, body in rc.sca_bodies_with_src(path):
        if len(body) < 132:
            continue
        n = body[47]
        if not (1 <= n <= 31):
            continue
        name = bytes(body[48:48 + n])
        if not name.startswith(prefix):
            continue
        yield idx, src, dst, body, name


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    prefix = (argv[1] if len(argv) > 1 else 'D83').encode('latin1')
    for idx, src, dst, body, name in rows(argv[0], prefix):
        print("f%-5d %s->%s cat=%02x op=%02x name=%-7s out=%02x hash=%08x "
              "b20=%08x"
              % (idx, src.hex()[-4:], dst.hex()[-4:], body[8], body[9],
                 name.decode('latin1'), body[34],
                 int.from_bytes(body[128:132], 'little'),
                 int.from_bytes(body[20:24], 'little')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
