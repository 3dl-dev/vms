#!/usr/bin/env python3
"""extract.py -- the DLM resource-name hash capture corpus (rd vms-9c95).

Reads lab pcaps of a real OpenVMS VAX cluster and writes, for every ROOT
resource a real VAX looked up across the wire, the (name, access mode, group)
the VAX asked about and the 32-bit directory-hash VALUE the VAX itself put in
the request.  That value is the sole input to the black-box determination Baron
ruled in on rd vms-dc2: it is a number real VMS broadcasts in the clear, never
anything read out of a binary.

Grounding for every offset: tests/lab/captures/vms-4fb-dir-hash-20261004/README.md
and src/kernel-core/vms_cluster_codec_dlm.h (struct vms_dlm_res_ident).

SENDER FILTER (INV-6).  Only frames from a sender this run could POSITIVELY
identify as a non-OVMX node are kept: the sender's SCSNODE is read from the
multicast discovery (HELLO) frames it sent in the same input set.  A sender
that never identified itself is EXCLUDED and counted, not assumed to be a VAX;
a sender whose SCSNODE matches --exclude-prefix (default OVMX) is excluded by
name.  Learning OVMX's own arithmetic back would be circular, and an
unidentified sender cannot be shown not to be OVMX.

  extract.py [--dedupe] [--exclude-prefix OVMX] PCAP...
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcapio  # noqa: E402

COLUMNS = ("name_hex", "name_len", "mode", "group", "value",
           "sender", "sender_mac", "pcap", "frame")


def identify_senders(paths):
    """mac -> set(SCSNODE) from every multicast discovery frame in the inputs."""
    seen = collections.defaultdict(set)
    for path in paths:
        for _, frame in pcapio.frames(path):
            if not pcapio.is_sca(frame):
                continue
            name = pcapio.disc_node_name(frame)
            if name:
                seen[pcapio.src_mac(frame)].add(name)
    return seen


def sender_verdict(macs, mac, exclude_prefix):
    """(node_name, reason-if-rejected)."""
    names = macs.get(mac)
    if not names:
        return None, "unidentified-sender"
    if len(names) > 1:
        return None, "ambiguous-sender"
    name = next(iter(names))
    if name.upper().startswith(exclude_prefix.upper()):
        return None, "excluded-node"
    return name, None


class Row(object):
    __slots__ = ("name", "mode", "group", "value", "sender", "mac", "pcap", "frame")

    def __init__(self, name, mode, group, value, sender, mac, pcap, frame):
        self.name, self.mode, self.group, self.value = name, mode, group, value
        self.sender, self.mac, self.pcap, self.frame = sender, mac, pcap, frame

    def key(self):
        return (self.name, self.mode, self.group)

    def tsv(self):
        return "\t".join((self.name.hex(), str(len(self.name)), str(self.mode),
                          str(self.group), "0x%08x" % self.value, self.sender,
                          self.mac, self.pcap, str(self.frame)))


def source_label(path):
    """<run>/<file>: the lab rigs all name their capture s8.pcap.gz, so the
    basename alone is not provenance."""
    head, base = os.path.split(os.path.abspath(path))
    return os.path.join(os.path.basename(head), base)


def scan(paths, macs, exclude_prefix, stats):
    """Yield one Row per kept cat-02 op-01 ROOT lookup request."""
    for path in paths:
        label = source_label(path)
        for idx, frame in pcapio.frames(path):
            if not pcapio.is_sca(frame):
                continue
            body = pcapio.sysap_body(frame)
            if body is None:
                continue
            hit = pcapio.root_enq_request(body)
            if hit is None:
                continue
            stats["op01_root"] += 1
            mac = pcapio.src_mac(frame)
            sender, reason = sender_verdict(macs, mac, exclude_prefix)
            if reason:
                stats[reason] += 1
                stats["rejected_mac:" + mac] += 1
                continue
            stats["kept"] += 1
            yield Row(hit[0], hit[1], hit[2], hit[3], sender, mac, label, idx)


def dedupe(rows):
    """(ordered unique-key -> first Row, key -> {value: observation count})."""
    first, values = collections.OrderedDict(), collections.OrderedDict()
    for row in rows:
        key = row.key()
        first.setdefault(key, row)
        values.setdefault(key, collections.Counter())[row.value] += 1
    return first, values


def report_conflicts(values, out):
    bad = [(k, v) for k, v in values.items() if len(v) > 1]
    for key, seen in bad:
        out.write("CONFLICT %s mode=%d group=%d -> %s\n"
                  % (key[0].hex(), key[1], key[2],
                     ", ".join("0x%08x x%d" % (v, n) for v, n in sorted(seen.items()))))
    return len(bad)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcaps", nargs="+")
    ap.add_argument("--dedupe", action="store_true",
                    help="one row per unique (name, mode, group); conflicts are fatal")
    ap.add_argument("--exclude-prefix", default="OVMX",
                    help="drop senders whose SCSNODE starts with this (default OVMX)")
    args = ap.parse_args(argv)

    macs = identify_senders(args.pcaps)
    stats = collections.Counter()
    rows = list(scan(args.pcaps, macs, args.exclude_prefix, stats))

    print("\t".join(COLUMNS))
    if args.dedupe:
        first, values = dedupe(rows)
        for key in sorted(first):
            print(first[key].tsv())
        nconf = report_conflicts(values, sys.stderr)
    else:
        for row in rows:
            print(row.tsv())
        nconf = 0

    senders = sorted({"%s=%s" % (m, sorted(n)[0]) for m, n in macs.items()})
    sys.stderr.write("senders identified: %s\n" % ", ".join(senders))
    for k in sorted(stats):
        sys.stderr.write("  %-22s %d\n" % (k, stats[k]))
    if args.dedupe:
        sys.stderr.write("  unique keys           %d\n" % len(first))
        sys.stderr.write("  conflicting keys      %d\n" % nconf)
    return 2 if nconf else 0


if __name__ == "__main__":
    sys.exit(main())
