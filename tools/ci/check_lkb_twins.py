#!/usr/bin/env python3
"""check_lkb_twins.py -- the LOCK BLOCK's three twins must declare the same fields.

usage: check_lkb_twins.py [repo-root]

WHY THIS EXISTS, and it is a defect this gate's own author made. `struct
vms_lock_entry` -- the LKB every path in src/kernel-core/vms_lock.c reads and
writes -- is declared THREE times, once per place that has to see it:

    src/kernel/vms_internal.h            the Linux substrate's twin
    src/kernel-netbsd/vms_internal.h     the NetBSD substrate's twin
    tests/cluster/host/lock_host_internal.h   the R1 host bed's twin

They are hand-maintained and deliberately NOT byte-layout-identical (the host
twin mirrors field presence and type, not layout -- see its own header). Adding
a field to vms_lock.c therefore means adding it to all three, and the ONLY one
whose omission is caught today is the host twin, because the host bed is what
`ctest` compiles. A field added on Linux and forgotten on NetBSD builds clean
here and fails only when somebody builds the NetBSD module -- which needs a
syssrc tree and happens far from the change (rd vms-b67a added
`parent_master_lkid` and the host twin's omission was caught by the compiler in
seconds; the NetBSD one would not have been).

WHAT IT CHECKS, and nothing more: the SET OF FIELD NAMES declared inside each
`struct vms_lock_entry { ... }`. Not order, not type, not layout -- those are
each substrate's own business, and the host twin says so. A name present in one
twin and absent from another is the drift this exists to red.

CLEAN-ROOM: reads this repository's own headers only.
"""
import os
import re
import sys

TWINS = (
    ('Linux', 'src/kernel/vms_internal.h'),
    ('NetBSD', 'src/kernel-netbsd/vms_internal.h'),
    ('host bed', 'tests/cluster/host/lock_host_internal.h'),
)
STRUCT = 'struct vms_lock_entry {'

# A declaration line: optional qualifiers, a type, then the member name(s).
# Comments are stripped first, so a field named inside a comment cannot count.
DECL = re.compile(r'^\s*(?:const\s+|volatile\s+|unsigned\s+|signed\s+|struct\s+|'
                  r'enum\s+|union\s+)*[A-Za-z_][A-Za-z0-9_]*\s*\**\s*'
                  r'(?P<names>[A-Za-z_][A-Za-z0-9_\[\], \*]*)\s*;\s*$')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def struct_body(path):
    """The text between `struct vms_lock_entry {` and its matching brace."""
    src = open(path, encoding='utf-8', errors='replace').read()
    i = src.find(STRUCT)
    if i < 0:
        return None
    i += len(STRUCT) - 1          # at the '{'
    depth, j = 0, i
    while j < len(src):
        if src[j] == '{':
            depth += 1
        elif src[j] == '}':
            depth -= 1
            if depth == 0:
                return src[i + 1:j]
        j += 1
    return None


def fields(path):
    body = struct_body(path)
    if body is None:
        return None
    out = set()
    for line in strip_comments(body).split('\n'):
        m = DECL.match(line)
        if not m:
            continue
        for nm in m.group('names').split(','):
            nm = nm.strip().lstrip('*').strip()
            nm = nm.split('[')[0].strip()
            if nm:
                out.add(nm)
    return out


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', '..')
    sets, bad = {}, 0
    for label, rel in TWINS:
        path = os.path.join(root, rel)
        f = fields(path)
        if f is None:
            print('check_lkb_twins: FATAL -- no `%s` in %s' % (STRUCT, rel))
            return 2
        sets[label] = f
        print('%-10s %-42s %d fields' % (label, rel, len(f)))

    union = set()
    for f in sets.values():
        union |= f
    for name in sorted(union):
        missing = [lbl for lbl, f in sets.items() if name not in f]
        if missing:
            bad += 1
            print('DRIFT: `%s` is declared in %s but NOT in %s'
                  % (name,
                     ', '.join(sorted(lbl for lbl in sets if name in sets[lbl])),
                     ', '.join(sorted(missing))))
    if bad:
        print('\ncheck_lkb_twins: FAIL -- %d field(s) present in some twins and '
              'not others. Every field of the LKB must be declared in all three '
              '(see this file\'s header for why NetBSD is the one a compiler '
              'here cannot catch).' % bad)
        return 1
    print('\ncheck_lkb_twins: PASS -- all three twins declare the same %d fields'
          % len(union))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
