# Semantic-oracle spec: $ASSIGN / $DASSGN / $QIO / $QIOW / $CANCEL basics (rd vms-8d1).
# The null device and a temporary mailbox (the two devices every system has):
# write/read/write-EOF, the IOSB layout (status, byte count, the mailbox sender's
# PID), IO$M_NOW on an empty mailbox, message size limits, an illegal function
# code, invalid and deassigned channels, $CANCEL of a pending read (IOSB + AST),
# and the efn / IOSB / AST completion contract of $QIO.
P = Probe("io")
WRITEV, READV, WRITEOF, SENSEMODE = K("IO$_WRITEVBLK"), K("IO$_READVBLK"), K("IO$_WRITEOF"), K("IO$_SENSEMODE")
NOW = K("IO$M_NOW")

P.word("CHAN", 0); P.word("MBX", 0); P.word("MBX2", 0)
P.quad("IOSB"); P.quad("IOSB2")
P.long("MYPID", 0)
P.long("STATE")
P.ast("AST1"); P.ast("AST2")
P.desc("NL", "NLA0:")
P.desc("MBXNAME", "SP_PROBE_MBX")
P.buf("WBUF", 64, text="HELLO, MAILBOX")
P.buf("RBUF", 64)
P.buf("BIG", 300, fill=0x42)
P.items("JPI_PID", [(4, K("JPI$_PID"), "MYPID", None)])
P.do("SYS$GETJPIW", 0, None, None, R("JPI_PID"), None, None, 0)

IOS = [UW("IOSB", "iosb"), UW("IOSB", "count", off=2)]


with P.sub("RESET"):
    P.fill("IOSB", byte=0xA5)
    P.fill("RBUF", byte=0x5A)


def reset():
    P.gosub("RESET")


# 1. the null device
P.call("IO.ASSIGN.NL", "SYS$ASSIGN", R("NL"), R("CHAN"), 0, None)
reset()
P.call("IO.NL.WRITE", "SYS$QIOW", 0, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.NL.READ", "SYS$QIOW", 0, V("CHAN"), READV, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0,
       show=IOS + [XB("RBUF", 2, label="buf")])
reset()
P.call("IO.NL.WRITEOF", "SYS$QIOW", 0, V("CHAN"), WRITEOF, R("IOSB"), None, 0, 0, 0, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.NL.ILLFUNC", "SYS$QIOW", 0, V("CHAN"), 63, R("IOSB"), None, 0, 0, 0, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.NL.SENSEMODE", "SYS$QIOW", 0, V("CHAN"), SENSEMODE, R("IOSB"), None, 0, 0, 0, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.NL.WRITE.ZERO", "SYS$QIOW", 0, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 0, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.BADCHAN", "SYS$QIOW", 0, 12345, WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.CHAN0", "SYS$QIOW", 0, 0, WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0, show=IOS)

# 2. the asynchronous contract: $QIO clears the efn at request, sets it and the
#    IOSB at completion, then delivers the AST with its parameter
for efn in (0, 5):
    P.do("SYS$SETEF", efn)
    P.setl("AST1", 0); P.setl("AST1_P", 0)
    reset()
    P.call("IO.QIO.EFN%d" % efn, "SYS$QIO", efn, V("CHAN"), WRITEV, R("IOSB"), AST("AST1"), 0x7700 + efn,
           R("WBUF"), 14, 0, 0, 0, 0)
    P.call("IO.QIO.EFN%d.WAIT" % efn, "SYS$WAITFR", efn, guard=True)
    P.call("IO.QIO.EFN%d.DONE" % efn, "SYS$READEF", efn, R("STATE"),
           show=IOS + [U("AST1", "asts"), X("AST1_P", "astprm")])
P.call("IO.QIO.EFN255", "SYS$QIO", 255, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0)
P.call("IO.QIO.EFN64.UNASSOC", "SYS$QIO", 64, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0)
reset()
P.call("IO.QIOW.EFN128", "SYS$QIOW", 128, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0, show=IOS)
P.call("IO.DASSGN.NL", "SYS$DASSGN", V("CHAN"))
reset()
P.call("IO.AFTER_DASSGN", "SYS$QIOW", 0, V("CHAN"), WRITEV, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0, show=IOS)

# 3. a temporary mailbox: max message 40, buffer quota 200 (a read asking for more
#    than the maximum message size is SS$_MBTOOSML, so reads ask for 40)
P.call("IO.CREMBX", "SYS$CREMBX", 0, R("MBX"), 40, 200, 0, None, R("MBXNAME"), None)
P.call("IO.ASSIGN.MBX.BYNAME", "SYS$ASSIGN", R("MBXNAME"), R("MBX2"), 0, None)
reset()
P.call("IO.MBX.READ.EMPTY.NOW", "SYS$QIOW", 0, V("MBX"), READV | NOW, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.WRITE.NOW", "SYS$QIOW", 0, V("MBX2"), WRITEV | NOW, R("IOSB"), None, 0, R("WBUF"), 14, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.READ", "SYS$QIOW", 0, V("MBX"), READV, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0,
       show=IOS + [EQ("MYPID", "IOSB", "sender=self", boff=4), T("RBUF", "IOSB", label="data", limit=64, lenoff=2)])
reset()
P.call("IO.MBX.WRITE.TOOBIG", "SYS$QIOW", 0, V("MBX"), WRITEV | NOW, R("IOSB"), None, 0, R("BIG"), 41, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.WRITE.MAX", "SYS$QIOW", 0, V("MBX"), WRITEV | NOW, R("IOSB"), None, 0, R("BIG"), 40, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.READ.SHORT", "SYS$QIOW", 0, V("MBX"), READV, R("IOSB"), None, 0, R("RBUF"), 10, 0, 0, 0, 0,
       show=IOS + [T("RBUF", "IOSB", label="data", limit=64, lenoff=2)])
reset()
P.call("IO.MBX.READ.AFTER_SHORT", "SYS$QIOW", 0, V("MBX"), READV | NOW, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.WRITEOF", "SYS$QIOW", 0, V("MBX"), WRITEOF | NOW, R("IOSB"), None, 0, 0, 0, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.MBX.READ.EOF", "SYS$QIOW", 0, V("MBX"), READV, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0, show=IOS)
reset()
P.call("IO.MBX.WRITE.ZERO", "SYS$QIOW", 0, V("MBX"), WRITEV | NOW, R("IOSB"), None, 0, R("WBUF"), 0, 0, 0, 0, 0,
       show=IOS)
reset()
P.call("IO.MBX.READ.ZERO", "SYS$QIOW", 0, V("MBX"), READV, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0, show=IOS)

# 4. $CANCEL of a pending read: the IOSB, the efn, the AST
P.setl("AST2", 0); P.setl("AST2_P", 0)
P.setl("IOSB2", 0xA5A5A5A5)
P.call("IO.MBX.READ.PEND", "SYS$QIO", 6, V("MBX"), READV, R("IOSB2"), AST("AST2"), 0x6666, R("RBUF"), 40, 0, 0, 0, 0,
       show=[UW("IOSB2", "iosb")])
P.call("IO.CANCEL", "SYS$CANCEL", V("MBX"))
P.call("IO.CANCEL.WAIT", "SYS$WAITFR", 6, guard=True)
P.show("IO.CANCEL.DONE", UW("IOSB2", "iosb"), UW("IOSB2", "count", off=2), U("AST2", "asts"), X("AST2_P", "astprm"))
P.call("IO.CANCEL.BADCHAN", "SYS$CANCEL", 12345)
P.call("IO.DASSGN.MBX2", "SYS$DASSGN", V("MBX2"))
P.call("IO.DELMBX", "SYS$DELMBX", V("MBX"))
P.call("IO.DASSGN.MBX", "SYS$DASSGN", V("MBX"))
P.call("IO.ASSIGN.MBX.GONE", "SYS$ASSIGN", R("MBXNAME"), R("MBX2"), 0, None)
PROBE = P
