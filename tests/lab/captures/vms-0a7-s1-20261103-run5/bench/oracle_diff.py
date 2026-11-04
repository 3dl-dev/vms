#!/usr/bin/env python3
"""Diff OVMXA's DIRECTORY / TYPE / COPY output (console log of this run, passwords XX)
against two oracles:

 (A) the VAX-to-VAX oracle of ../decnet-fal-verbs-20261008/vax-to-vax-sys-login/,
     after normalising VAXC"DNTEST password":: to its VAX2:: (it used no access string),
     exactly as the vms-6a4 bench did;
 (B) the same-run VAX loopback: VAXC's own DCL typed the SAME command line against
     VAXC's FAL, with the same fixtures, accounts and access strings.

Blank lines are compared too. $STATUS is compared in (B).

usage: oracle_diff.py <ovmxa-console.log> <vaxc-console.log> <falverbs-v2v-vax1-console.txt>"""
import difflib, re, sys

def clean(path):
    return open(path, 'rb').read().decode('latin1').replace('\x00', '').replace('\r', '')

o, v, orc = clean(sys.argv[1]), clean(sys.argv[2]), clean(sys.argv[3])

def block(log, cmd, start=0):
    """Output of '$ <cmd>' up to its '$ SHOW SYMBOL $STATUS', and that $STATUS."""
    i = log.index('$ ' + cmd + '\n', start) + len(cmd) + 3
    j = log.index('$ SHOW SYMBOL $STATUS', i)
    m = re.search(r'%X[0-9A-F]{8}', log[j:j + 80])
    return log[i:j], m.group(0) if m else '?'

def strip_opcom(s):
    # the VAX console gets OPCOM security-audit broadcasts (network login failure)
    # (with a bell) in the middle of the command output; they are not DCL output.
    return re.sub(r'\n?\x07?\n*%%%%%%%%%%%  OPCOM .*?Status: +%LOGIN-F-INVPWD, invalid password\n+', '', s, flags=re.S)

def show(name_a, a, name_b, b):
    d = list(difflib.unified_diff(b.split('\n'), a.split('\n'), name_b, name_a, lineterm='', n=1))
    print('\n'.join(d) if d else 'IDENTICAL (byte for byte, blank lines included)')

print('#### (A) against the VAX1->VAX2 oracle (decnet-fal-verbs-20261008), node string normalised')
for a, b in [('DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'DIRECTORY VAX2::SYS$LOGIN:BRK*.TXT'),
             ('DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:NOSUCH.TXT', 'DIRECTORY VAX2::SYS$LOGIN:NOSUCH.TXT'),
             ('TYPE VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'TYPE VAX2::SYS$LOGIN:BRK*.TXT'),
             ('DIRECTORY/FULL VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT', 'DIRECTORY/FULL VAX2::SYS$LOGIN:BRK*.TXT')]:
    A = block(o, a)[0].replace('VAXC"DNTEST password"::', 'VAX2::')
    i = orc.index(b + '\n'); j = orc.index('$ ', i + len(b))
    print('=====', b)
    show('OVMXA->VAXC (node string normalised)', A, 'oracle VAX1->VAX2', orc[i + len(b) + 1:j])

print()
print('#### (B) against the same-run VAX loopback (VAXC DCL -> VAXC FAL, same command line)')
# VAXC runs these after deleting BRK1RT.TXT (made by OVMXA's 3c after its 1d/1f/2c),
# so both list the same directory.
loop_from = v.index('$ DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT')
for c in ['DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT',
          'DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:NOSUCH.TXT',
          'DIRECTORY/FULL VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT',
          'TYPE VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT',
          'TYPE VAXC"DNTEST XX"::SYS$LOGIN:BRKP.TXT',
          'DIRECTORY VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT@bad',
          'TYPE VAXC"DNTEST XX"::SYS$LOGIN:BRK1.TXT@bad']:
    cmd = c.split('@')[0]
    if c.endswith('@bad'):
        # the wrong-password lines: both consoles show XX after redaction, so
        # take the occurrence after the good-password one
        oa = o.index('$ ' + cmd + '\n'); oa = o.index('$ ' + cmd + '\n', oa + 1) if o.count('$ ' + cmd + '\n') > 1 else oa
        va = v.index('$ ' + cmd + '\n', loop_from); va = v.index('$ ' + cmd + '\n', va + 1) if v.count('$ ' + cmd + '\n', loop_from) > 1 else va
        A, sa = block(o, cmd, oa); B, sb = block(v, cmd, va)
    else:
        A, sa = block(o, cmd); B, sb = block(v, cmd, loop_from)
    B = strip_opcom(B)
    print('=====', c.replace('@bad', '   (wrong password)'))
    show('OVMXA DCL -> VAXC FAL', A, 'VAXC DCL -> VAXC FAL', B)
    print(f'$STATUS  VAXC {sb}   OVMXA {sa}' + ('   SAME' if sa == sb else '   DIFFERENT'))

print('===== DIRECTORY/FULL VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT   (L1f0: the VAX ran it right after OVMX\'s 1f, same directory)')
c = 'DIRECTORY/FULL VAXC"DNTEST XX"::SYS$LOGIN:BRK*.TXT'
A, sa = block(o, c); B, sb = block(v, c)        # the VAX's first run of it is L1f0
show('OVMXA DCL -> VAXC FAL', A, 'VAXC DCL -> VAXC FAL (L1f0)', strip_opcom(B))
print(f'$STATUS  VAXC {sb}   OVMXA {sa}' + ('   SAME' if sa == sb else '   DIFFERENT'))

print('===== COPY/LOG of a remote wildcard (target directories differ by design)')
# OVMXA's console wraps the echoed command line at column 80
A, sa = block(o, 'COPY/LOG VAXC"SYSTEM XX"::SYS$SYSDEVICE:[SYSMGR]BRK*.TXT SYS$LOGIN:*\n.*')
B, sb = block(v, 'COPY/LOG VAXC"SYSTEM XX"::SYS$SYSDEVICE:[SYSMGR]BRK*.TXT SYS$SYSDEVICE:[LOOPOUT]*.*')
A = A.replace('SYS$SYSROOT:[SYSMGR]', '<target>'); B = B.replace('SYS$SYSDEVICE:[LOOPOUT]', '<target>')
A = re.sub(r'(<target>\w+\.TXT);\d+', r'\1;n', A); B = re.sub(r'(<target>\w+\.TXT);\d+', r'\1;n', B)
show('OVMXA COPY/LOG', A, 'VAXC COPY/LOG', B)
print(f'$STATUS  VAXC {sb}   OVMXA {sa}' + ('   SAME' if sa == sb else '   DIFFERENT'))

print('===== COPY/LOG of one 2000-record file (the " (N blocks)" suffix)')
A, sa = block(o, 'COPY/LOG VAXC"SYSTEM XX"::SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$LOGIN:BI\nGL.TXT')
B, sb = block(v, 'COPY/LOG VAXC"SYSTEM XX"::SYS$SYSDEVICE:[SYSMGR]BIG.TXT SYS$SYSDEVICE:[LOOPOUT]BIGL.TXT')
A = A.replace('SYS$SYSROOT:[SYSMGR]', '<target>'); B = B.replace('SYS$SYSDEVICE:[LOOPOUT]', '<target>')
A = re.sub(r'(<target>\w+\.TXT);\d+', r'\1;n', A); B = re.sub(r'(<target>\w+\.TXT);\d+', r'\1;n', B)
show('OVMXA COPY/LOG', A, 'VAXC COPY/LOG', B)
print(f'$STATUS  VAXC {sb}   OVMXA {sa}' + ('   SAME' if sa == sb else '   DIFFERENT'))
