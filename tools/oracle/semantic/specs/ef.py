# Semantic-oracle spec: EVENT FLAGS (rd vms-8d1).
# $SETEF $CLREF $READEF $WAITFR $WFLOR $WFLAND $ASCEFC $DACEFC $DLCEFC, the flag a
# timer ($SETIMR) clears at request and sets on expiry, EF 0, every cluster
# boundary and illegal numbers. Event flag 30 is reserved for the hang guard.
P = Probe("ef")
P.long("STATE", 0xA5A5A5A5)
P.quad("DELTA")
P.desc("DT", "0 00:00:00.20")
P.desc("CNAME", "SEMPROBE_EFC")
P.desc("CNAME2", "SEMPROBE_EFC2")

EFNS = [0, 1, 23, 24, 31, 32, 63, 64, 95, 96, 127, 128, 255, 256, 1024, 0xFFFFFFFF]


def lbl(e):
    return "M1" if e == 0xFFFFFFFF else str(e)


def readef(cid, efn):
    """$READEF; flags 24-31 are reserved to the system, and on a real node
    printing a line (RMS + the terminal driver) can set EF 0, so in cluster 0 only
    flags 1-23 are shown -- EF 0 has its own print-free cases (section 6)"""
    P.setl("STATE", 0xA5A5A5A5)
    if not (0 <= efn < 32 or 256 <= efn < 256 + 32 or efn == 1024):
        P.call(cid, "SYS$READEF", efn, R("STATE"), show=[X("STATE", "state")])
    else:
        P.do("SYS$READEF", efn, R("STATE"))  # status re-read below (WASSET/WASCLR)
        P.andl("STATE", 0x00FFFFFE)
        P.call(cid, "SYS$READEF", efn, R("PROBE_SINK"), show=[X("STATE", "state")])


P.long("PROBE_SINK")

# 1. local event flags survive image rundown (they belong to the PROCESS), so
#    what an image finds at start depends on what ran before it: start from a
#    known state -- every flag this probe owns clear
for e in list(range(0, 24)) + list(range(32, 64)):
    P.do("SYS$CLREF", e)
readef("EF.START.C0", 0)
readef("EF.START.C1", 32)

# 2. set/clear return values across every boundary + illegal numbers
for e in EFNS:
    P.call("EF.SETEF.%s" % lbl(e), "SYS$SETEF", e)
    P.call("EF.SETEF2.%s" % lbl(e), "SYS$SETEF", e)
    P.call("EF.CLREF.%s" % lbl(e), "SYS$CLREF", e)
    P.call("EF.CLREF2.%s" % lbl(e), "SYS$CLREF", e)

# 3. $READEF returns the whole cluster longword
for e in (0, 5, 31, 33, 62):
    P.do("SYS$SETEF", e)
readef("EF.READ.C0", 0)
readef("EF.READ.C0.VIA17", 17)
readef("EF.READ.C1", 40)
for e in EFNS:
    readef("EF.READ.%s" % lbl(e), e)
for e in (0, 5, 31, 33, 62):
    P.do("SYS$CLREF", e)

# 4. waits that must return at once, and the error paths
P.do("SYS$SETEF", 7)
P.call("EF.WAITFR.SET", "SYS$WAITFR", 7, guard=True)
P.call("EF.WAITFR.64.UNASSOC", "SYS$WAITFR", 64, guard=True)
P.call("EF.WAITFR.128", "SYS$WAITFR", 128, guard=True)
P.call("EF.WFLOR.SET", "SYS$WFLOR", 7, 1 << 7, guard=True)
P.do("SYS$SETEF", 39)
P.call("EF.WFLOR.C1.SET", "SYS$WFLOR", 40, 1 << 7, guard=True)
P.do("SYS$SETEF", 3)
P.call("EF.WFLAND.BOTH", "SYS$WFLAND", 0, (1 << 7) | (1 << 3), guard=True)
P.call("EF.WFLOR.64.UNASSOC", "SYS$WFLOR", 64, 1, guard=True)
P.call("EF.WFLAND.128", "SYS$WFLAND", 128, 1, guard=True)
for e in (3, 7, 39):
    P.do("SYS$CLREF", e)

# 5. common event flag clusters
P.call("EF.ASCEFC.64", "SYS$ASCEFC", 64, R("CNAME"), 0, 0)
readef("EF.CEF.READ0", 64)
P.call("EF.CEF.SETEF.64", "SYS$SETEF", 64)
P.call("EF.CEF.SETEF2.64", "SYS$SETEF", 64)
P.call("EF.CEF.CLREF.95", "SYS$CLREF", 95)
P.call("EF.CEF.SETEF.95", "SYS$SETEF", 95)
readef("EF.CEF.READ70", 70)
P.call("EF.ASCEFC.96.SAMENAME", "SYS$ASCEFC", 96, R("CNAME"), 0, 0)
readef("EF.CEF.READ96.ALIAS", 96)
P.call("EF.ASCEFC.0", "SYS$ASCEFC", 0, R("CNAME2"), 0, 0)
P.call("EF.ASCEFC.32", "SYS$ASCEFC", 32, R("CNAME2"), 0, 0)
P.call("EF.ASCEFC.65", "SYS$ASCEFC", 65, R("CNAME2"), 0, 0)
P.call("EF.ASCEFC.128", "SYS$ASCEFC", 128, R("CNAME2"), 0, 0)
P.call("EF.DACEFC.96", "SYS$DACEFC", 96)
P.call("EF.DACEFC.70", "SYS$DACEFC", 70)
P.call("EF.CEF.SETEF.AFTERDAC", "SYS$SETEF", 64)
P.call("EF.DACEFC.64.AGAIN", "SYS$DACEFC", 64)
P.call("EF.DACEFC.0", "SYS$DACEFC", 0)
P.call("EF.ASCEFC.64.REASSOC", "SYS$ASCEFC", 64, R("CNAME"), 0, 0)
readef("EF.CEF.REASSOC.READ", 64)
P.call("EF.DLCEFC", "SYS$DLCEFC", R("CNAME"))
P.call("EF.DLCEFC.AGAIN", "SYS$DLCEFC", R("CNAME"))
P.do("SYS$DACEFC", 64)

# 6. a timer clears its flag at request and sets it on expiry; EF 0 likewise.
#    Measured with no output in between (see readef).
P.call("EF.BINTIM", "SYS$BINTIM", R("DT"), R("DELTA"))
for efn in (9, 0):
    P.do("SYS$SETEF", efn)
    P.callq("T1", "SYS$SETIMR", efn, R("DELTA"), None, 0, 0)
    P.callq("T2", "SYS$READEF", efn, R("STATE"))
    P.show("EF.SETIMR.%d" % efn, X("T1", "st"), X("T2", "readef"), BIT("STATE", efn, "flag_at_request"))
    P.callq("T1", "SYS$WAITFR", efn, guard_case="EF.SETIMR.%d.WAIT" % efn)
    P.callq("T2", "SYS$READEF", efn, R("STATE"))
    P.show("EF.SETIMR.%d.WAIT" % efn, X("T1", "st"), X("T2", "readef"), BIT("STATE", efn, "flag_after"))
P.call("EF.SETIMR.128", "SYS$SETIMR", 128, R("DELTA"), None, 0, 0)
P.call("EF.SETIMR.64.UNASSOC", "SYS$SETIMR", 64, R("DELTA"), None, 0, 0)
P.call("EF.SETIMR.1024", "SYS$SETIMR", 1024, R("DELTA"), None, 0, 0)
P.call("EF.CANTIM.ALL", "SYS$CANTIM", 0, 0)

# 7. LAST (a real VAX V7.3 never returns from it, so the guard ends the probe
#    here): $WFLOR with an empty mask
P.do("SYS$SETEF", 7)
P.call("EF.WFLOR.MASK0", "SYS$WFLOR", 7, 0, guard=True)
PROBE = P
