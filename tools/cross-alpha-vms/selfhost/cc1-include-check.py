#!/usr/bin/env python3
"""cc1-include-check.py -- before an OVMX boot spends hours on BUILD_CC1.COM, find
out in minutes whether every compile in it can find its sources and headers in
the trees staged for the OVMX disk (rd vms-465).

usage: cc1-include-check.py <BUILD_CC1.COM> <out.sh>

Reads each compile's response file exactly as BUILD_CC1.COM writes it (DCL
string, then libiberty's buildargv quoting) and writes a shell script that runs
the cross driver with the same arguments, -c replaced by -E (preprocess only,
output discarded), inside a container where /GCCSRC and /GNU/CXX are the staged
trees. The script prints one FAIL line per compile whose preprocessing failed
and exits non-zero if any did. It checks the staging, not OVMX: the compiles
themselves run on OVMX.
"""
import re
import sys


def unquote(dcl):
    s = dcl[1:-1].replace('""', '"')
    a, i = "", 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            a += s[i + 1]
            i += 2
        else:
            a += s[i]
            i += 1
    return a


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out = ["#!/bin/bash", "fails=0; n=0"]
    args, label = None, "?"
    for line in open(sys.argv[1]).read().split("\n"):
        if re.match(r"\$! \d+/\d+ ", line):
            label = line.split(" ", 2)[2]
        elif line.startswith("$ OPEN/WRITE R GCCOBJ:[000000]TU.RSP"):
            args = []
        elif line.startswith("$ WRITE R ") and args is not None:
            args.append(unquote(line[len("$ WRITE R "):]))
        elif (line.startswith("$ GCC @") or line.startswith("$ GXX @")) and args is not None:
            drv = "alpha-dec-vms-gcc" if line.startswith("$ GCC") else "alpha-dec-vms-g++"
            a2, k = [], 0
            while k < len(args):
                x = args[k]
                if x.startswith("-B"):
                    k += 1
                elif x == "-o":
                    k += 2
                elif x == "-c":
                    a2.append("-E")
                    k += 1
                else:
                    a2.append(x)
                    k += 1
            q = " ".join("'" + x.replace("'", "'\\''") + "'" for x in a2)
            out.append("n=$((n+1)); %s %s -o /dev/null 2>/tmp/cc1ic.err || { fails=$((fails+1)); "
                       "echo \"cc1-include-check: FAIL %s: $(grep -m1 -E 'fatal error|error:' /tmp/cc1ic.err)\"; }"
                       % (drv, q, label))
            args = None
    out.append('echo "cc1-include-check: $n compiles preprocessed against the staged trees, $fails failed"')
    out.append('[ "$fails" -eq 0 ]')
    open(sys.argv[2], "w").write("\n".join(out) + "\n")


main()
