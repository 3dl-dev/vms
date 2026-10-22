#!/usr/bin/env python3
"""ods2-header-tree.py SRC DST -- copy a C/C++ header tree onto names an ODS-2
volume can hold, and write the GCC `-remap' maps that find the renamed files.

ODS-2 file names are 1-39 characters of A-Z 0-9 $ _ - with one type of up to 39.
libstdc++'s tree has names outside that set: bits/c++config.h, c++allocator.h,
c++io.h, c++locale.h, stdc++.h, and some ext/pb_ds names longer than 39. GCC's
preprocessor has its own answer for file systems that restrict names: with
`-remap' it reads a file `header.gcc' in each include directory, pairs of
"<name the source includes> <name the file has>" (libcpp files.cc,
remap_filename). This script writes those pairs; GCC itself is unchanged.

Mapping, for a file whose name is not ODS-2 legal:
  - '.' before the last one -> '_'  (what the DEC C RTL does itself, a.b.c -> A_B.C)
  - '+' -> 'x'                       (c++config.h -> cxxconfig.h, the VMS "CXX" spelling)
  - any other character outside A-Z a-z 0-9 $ _ - -> '_'
  - a name part over 39 characters -> its first 32, '_', 6 hex digits of its SHA-1
Every directory's header.gcc lists every pair in the tree: a search directory
that lacks the file then says "not here" instead of failing on an invalid name.
Directory names are not remapped (remap_filename only maps the last component a
directory's own header.gcc names): an illegal directory name is an error. So is
an illegal type, and two names that land on the same ODS-2 name.

Exit 0 and one line per remapped file on stdout; exit 1 on an error.
"""
import hashlib
import os
import re
import shutil
import sys

LEGAL = re.compile(r'^[A-Za-z0-9$_-]{1,39}$')
NAME_MAX = 39


def split(name):
    """name -> (stem, type) at the last '.', type without the dot ('' if none)."""
    if '.' in name[1:]:
        i = name.rindex('.')
        return name[:i], name[i + 1:]
    return name, ''


def legal_type(t):
    return t == '' or LEGAL.match(t) is not None


def ods2_name(name):
    """The ODS-2 legal spelling of a file name (lower case; the master upcases)."""
    stem, typ = split(name)
    stem = stem.replace('.', '_').replace('+', 'x')
    stem = re.sub(r'[^A-Za-z0-9$_-]', '_', stem)
    if len(stem) > NAME_MAX:
        h = hashlib.sha1(stem.encode()).hexdigest()[:6]
        stem = stem[:32] + '_' + h
    if not legal_type(typ):
        raise ValueError('file type not ODS-2 legal: %s' % name)
    return (stem + ('.' + typ if typ else '')).lower()


def crtl_name(name):
    """What the DEC C RTL looks up for this Unix name (only the extra dots change)."""
    stem, typ = split(name)
    return (stem.replace('.', '_') + ('.' + typ if typ else '')).lower()


def main(argv):
    if len(argv) != 3:
        sys.stderr.write('usage: ods2-header-tree.py SRC DST\n')
        return 1
    src, dst = argv[1], argv[2]
    if not os.path.isdir(src):
        sys.stderr.write('ods2-header-tree: no directory %s\n' % src)
        return 1
    errors = 0
    pairs = {}                      # every name the tree cannot hold -> its ODS-2 spelling
    outdirs = []
    for root, dirs, files in os.walk(src, followlinks=True):
        dirs.sort()
        rel = os.path.relpath(root, src)
        out = dst if rel == '.' else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        outdirs.append(out)
        seen = {}
        for d in dirs:
            if not LEGAL.match(d):
                sys.stderr.write('ods2-header-tree: directory name not ODS-2 legal: %s\n'
                                 % os.path.join(rel, d))
                errors += 1
            seen.setdefault(d.lower() + '.dir', d)
        for f in sorted(files):
            try:
                o = ods2_name(f)
            except ValueError as e:
                sys.stderr.write('ods2-header-tree: %s/%s\n' % (rel, e))
                errors += 1
                continue
            if o in seen:
                sys.stderr.write('ods2-header-tree: %s and %s are both %s in %s\n'
                                 % (seen[o], f, o.upper(), rel))
                errors += 1
                continue
            seen[o] = f
            shutil.copyfile(os.path.join(root, f), os.path.join(out, o))
            if o != crtl_name(f):
                if pairs.get(f, o) != o:
                    sys.stderr.write('ods2-header-tree: %s maps two ways\n' % f)
                    errors += 1
                pairs[f] = o
                print('%s/%s -> %s' % (rel, f, o.upper()))
        if 'header.gcc' in seen:
            sys.stderr.write('ods2-header-tree: %s already has a header.gcc\n' % rel)
            errors += 1
    # Every directory carries every pair: the preprocessor looks for
    # <bits/c++config.h> in each search directory's bits/ in turn, and an
    # unmapped c++config.h there is an invalid ODS-2 name (a hard error), while a
    # mapped name that is absent is "not here" and the search goes on.
    if pairs and not errors:
        for out in outdirs:
            with open(os.path.join(out, 'header.gcc'), 'w') as m:
                for f in sorted(pairs):
                    m.write('%s %s\n' % (f, pairs[f]))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
