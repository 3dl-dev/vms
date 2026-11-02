#!/usr/bin/env python3
"""Diff OVMXA's DIRECTORY / TYPE output (console log of this run) against the
VAX-to-VAX oracle, after normalising the node string VAXC"DNTEST password"::
to the oracle's VAX2:: (the oracle used no access-control string).

usage: oracle_diff.py <ovmxa-console.log> <falverbs-v2v-vax1-console.txt>"""
import difflib, sys
o = open(sys.argv[1], 'rb').read().decode('latin1').replace('\x00', '').replace('\r', '')
orc = open(sys.argv[2]).read().replace('\r', '')

def ovmx(cmd):
    i = o.index(cmd); j = o.index('$ SHOW SYMBOL', i)
    return o[i + len(cmd):j].replace('VAXC"DNTEST password"::', 'VAX2::')

def oracle(cmd):
    i = orc.index(cmd + '\n'); j = orc.index('$ ', i + len(cmd))
    return orc[i + len(cmd):j]

for a, b in [('DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT'),
             ('DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:NOSUCH.TXT', 'DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT'),
             ('TYPE VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'TYPE VAX2::SYS$LOGIN:BRK*.TXT'),
             ('DIRECTORY/FULL VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'DIRECTORY/FULL VAX2::SYS$LOGIN:BRK*.TXT')]:
    A, B = ovmx(a), oracle(b)
    print('=====', b)
    d = list(difflib.unified_diff(B.split('\n'), A.split('\n'), 'oracle VAX1->VAX2',
                                  'OVMXA->VAXC (node string normalised)', lineterm='', n=1))
    print('\n'.join(d) if d else 'IDENTICAL (byte for byte, blank lines included)')
