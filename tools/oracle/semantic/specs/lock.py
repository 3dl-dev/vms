# Semantic-oracle spec: LOCAL LOCKING -- $ENQ / $ENQW / $DEQ (rd vms-8d1).
# Lock modes and their compatibility inside one process (a second lock on the
# same resource, NOQUEUE so an incompatible request is refused rather than
# waited for), conversions up and down, the lock value block (written on a
# down-conversion from PW/EX, read with VALBLK), parent/sub-locks, resource-name
# limits, an invalid lock ID, $DEQ of a parent with sub-locks, LCK$M_DEQALL,
# SYNCSTS, and the asynchronous $ENQ completion contract (efn, LKSB, AST).
# Lock IDs differ between systems: only "is it zero" and "are two equal" print.
P = Probe("lock")
NL, CR, CW, PR, PW, EX = (K("LCK$K_NLMODE"), K("LCK$K_CRMODE"), K("LCK$K_CWMODE"),
                          K("LCK$K_PRMODE"), K("LCK$K_PWMODE"), K("LCK$K_EXMODE"))
NOQUEUE, CONVERT, VALBLK, SYNCSTS = (K("LCK$M_NOQUEUE"), K("LCK$M_CONVERT"),
                                     K("LCK$M_VALBLK"), K("LCK$M_SYNCSTS"))
DEQALL = K("LCK$M_DEQALL")

# an LKSB: status word, reserved word, lock ID longword, 16-byte value block
for n in ("LA", "LB", "LC", "LP", "LS", "LX"):
    P.buf(n, 24, fill=0)
for n, t in [("RA", "SP_LOCK_A"), ("RB", "SP_LOCK_B"), ("RP", "SP_LOCK_PARENT"), ("RS", "SP_LOCK_SUB"),
             ("RN31", "L" * 31), ("RN32", "L" * 32), ("RN33", "L" * 33), ("RN0", "")]:
    P.desc(n, t)
P.long("ZERO", 0)
P.long("BADID", 0x7FFFFFFF)
P.long("STATE")
P.ast("AST1")


def lk(b):
    """the LKSB status and whether a lock ID was returned"""
    return [UW(b, "lksb", 0), EQ(b, "ZERO", "lkid0", aoff=4)]


def enqw(cid, mode, lksb, flags=0, res=None, parid=None, show=None):
    args = [0, mode, R(lksb), flags, None if res is None else R(res),
            0 if parid is None else VL(parid, 4)]
    P.call(cid, "SYS$ENQW", *args, show=lk(lksb) if show is None else show)


def deq(cid, lksb, flags=0, valblk=False, show=()):
    P.call(cid, "SYS$DEQ", VL(lksb, 4), R(lksb, 8) if valblk else None, 0, flags, show=show)


# 1. modes and compatibility within one process
enqw("LOCK.NL", NL, "LA", res="RA")
enqw("LOCK.CVT.EX", EX, "LA", CONVERT)
enqw("LOCK.SECOND.CR.NOQUEUE", CR, "LB", NOQUEUE, res="RA")
enqw("LOCK.SECOND.NL", NL, "LB", res="RA", show=lk("LB") + [EQ("LA", "LB", "same_id", aoff=4, boff=4)])
enqw("LOCK.CVT.B.PR.NOQUEUE", PR, "LB", CONVERT | NOQUEUE)
enqw("LOCK.CVT.A.PR", PR, "LA", CONVERT)
enqw("LOCK.CVT.B.PR", PR, "LB", CONVERT | NOQUEUE)
enqw("LOCK.CVT.B.EX.NOQUEUE", EX, "LB", CONVERT | NOQUEUE)
enqw("LOCK.CVT.B.CW.NOQUEUE", CW, "LB", CONVERT | NOQUEUE)
enqw("LOCK.CVT.B.NL", NL, "LB", CONVERT)
enqw("LOCK.CVT.A.PW", PW, "LA", CONVERT)
enqw("LOCK.SECOND.PR.NOQUEUE", PR, "LC", NOQUEUE, res="RA")
enqw("LOCK.SECOND.CR", CR, "LC", NOQUEUE, res="RA")
enqw("LOCK.CVT.C.CW.NOQUEUE", CW, "LC", CONVERT | NOQUEUE)
enqw("LOCK.CVT.SAME", PW, "LA", CONVERT)
deq("LOCK.DEQ.C", "LC")

# 2. the lock value block: written by a down-conversion from PW/EX, read by VALBLK
P.setl("LA", 0x31414C56, 8); P.setl("LA", 0x32414C56, 12)
P.setl("LA", 0x33414C56, 16); P.setl("LA", 0x34414C56, 20)
enqw("LOCK.VAL.WRITE", NL, "LA", CONVERT | VALBLK)
enqw("LOCK.VAL.READ", CR, "LB", CONVERT | VALBLK, show=lk("LB") + [XB("LB", 16, 8, "valblk")])
enqw("LOCK.VAL.NEWLOCK", NL, "LX", VALBLK, res="RA", show=lk("LX") + [XB("LX", 16, 8, "valblk")])
deq("LOCK.DEQ.X", "LX")

# 3. parent / sub-locks
enqw("LOCK.PARENT", EX, "LP", res="RP")
enqw("LOCK.SUB", EX, "LS", res="RS", parid="LP")
enqw("LOCK.SUB.SAME.NOQUEUE", EX, "LX", NOQUEUE, res="RS", parid="LP")
enqw("LOCK.SUB.UNDER.OTHER", EX, "LX", NOQUEUE, res="RS", parid="LA")
deq("LOCK.DEQ.X2", "LX")
deq("LOCK.DEQ.PARENT.WITH.SUB", "LP")
deq("LOCK.DEQ.SUB", "LS")
deq("LOCK.DEQ.PARENT", "LP")

# 4. resource names and lock IDs
enqw("LOCK.NAME31", NL, "LX", res="RN31")
deq("LOCK.DEQ.NAME31", "LX")
enqw("LOCK.NAME32", NL, "LX", res="RN32")
deq("LOCK.DEQ.NAME32", "LX")
enqw("LOCK.NAME33", NL, "LX", res="RN33", show=[UW("LX", "lksb", 0)])
enqw("LOCK.NAME0", NL, "LX", res="RN0", show=[UW("LX", "lksb", 0)])
enqw("LOCK.NONAME", NL, "LX", show=[UW("LX", "lksb", 0)])
enqw("LOCK.BADMODE", 9, "LX", res="RA", show=[UW("LX", "lksb", 0)])
P.call("LOCK.DEQ.BADID", "SYS$DEQ", V("BADID"), None, 0, 0)
P.call("LOCK.DEQ.ZEROID", "SYS$DEQ", 0, None, 0, 0)
P.call("LOCK.CVT.BADID", "SYS$ENQW", 0, NL, R("LX"), CONVERT, None, 0, show=[UW("LX", "lksb", 0)])

# 5. SYNCSTS: a request granted at once reports SS$_SYNCH and sets no event flag
P.do("SYS$CLREF", 7)
P.call("LOCK.SYNCSTS", "SYS$ENQ", 7, NL, R("LX"), SYNCSTS, R("RB"), 0, show=lk("LX"))
P.call("LOCK.SYNCSTS.EF", "SYS$READEF", 7, R("STATE"))
deq("LOCK.DEQ.SYNCSTS", "LX")

# 6. $ENQ completion: efn cleared at request, set at grant; LKSB; AST
P.do("SYS$SETEF", 6)
P.setl("AST1", 0); P.setl("AST1_P", 0)
P.call("LOCK.ASYNC", "SYS$ENQ", 6, CR, R("LX"), 0, R("RB"), 0, AST("AST1"), 0x5151)
P.call("LOCK.ASYNC.WAIT", "SYS$WAITFR", 6)
P.call("LOCK.ASYNC.DONE", "SYS$READEF", 6, R("STATE"),
       show=lk("LX") + [U("AST1", "asts"), X("AST1_P", "astprm")])
deq("LOCK.DEQ.ASYNC", "LX")

# 7. $DEQ with LCK$M_DEQALL: every lock this process holds at its access mode goes
P.call("LOCK.DEQALL", "SYS$DEQ", 0, None, 0, DEQALL)
P.call("LOCK.AFTER.DEQALL.A", "SYS$DEQ", VL("LA", 4), None, 0, 0)
P.call("LOCK.AFTER.DEQALL.B", "SYS$DEQ", VL("LB", 4), None, 0, 0)
PROBE = P
