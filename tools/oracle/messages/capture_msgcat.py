#!/usr/bin/env python3
"""capture_msgcat.py - capture the OpenVMS message catalog from a lab node (rd vms-546).

  capture_msgcat.py <vax|alpha> <pod> <SP_MSGCAT*.MAR> <out.txt> [PASSWORD]

Types the MACRO-32 program into the node (tools/oracle/semantic/capture.py's
console typist), assembles, links and runs it, and keeps its "MC ..." lines:
the condition value, the $GETMSG outadr longword and the message $GETMSG
returned with flags 15. Observed output only (clean-room Rule 8). The table
$GETMSG answers from is generated from these files by gen_msgcat.py.
"""
import datetime
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "semantic"))
from capture import Lab, ARCH  # noqa: E402


def main(a):
    arch, pod, mar, out = a[:4]
    lab = Lab(arch, pod)
    lab.login(a[4] if len(a) > 4 else "semprobe2026")
    lab.cmd("SET DEFAULT SYS$LOGIN")
    lab.cmd("REPLY/DISABLE", 60)
    name = os.path.splitext(os.path.basename(mar))[0]
    lab.push(open(mar).read(), name + ".MAR")
    lab.cmd("MACRO " + name, 300)
    lab.cmd("LINK " + name, 300)
    s = lab.cmd("RUN " + name, 3600)
    if "MC-END" not in s:
        sys.exit("capture_msgcat: the program did not finish:\n" + s[-1500:])
    lines = [l for l in s.split("\n") if l.startswith("MC ") and "NOMSG, Message number" not in l]
    node, key, desc = ARCH[arch]
    with open(out, "w") as f:
        f.write("# provenance: %s, ovmx-lab pod %s node %s, captured %s by\n" % (
            desc, pod, node.upper(), datetime.date.today().isoformat()))
        f.write("#   tools/oracle/messages/capture_msgcat.py running %s (the node's own MACRO + LINK, RUN as\n"
                "#   SYSTEM): condition value, $GETMSG outadr longword, message ($GETMSG flags 15, severity 0).\n"
                "#   Observed output of the real system (clean-room Rule 8); nothing disassembled. NOMSG lines dropped.\n"
                % os.path.basename(mar))
        for l in lines:
            f.write(l + "\n")
    print("capture_msgcat: %d messages -> %s" % (len(lines), out))


if __name__ == "__main__":
    main(sys.argv[1:])
