# Semantic-oracle spec: TERMINAL $QIO (rd vms-8d1). A channel to this process's
# own terminal: SENSEMODE / SENSECHAR (device class, page width and length),
# SETMODE with what SENSEMODE returned, WRITEVBLK formatted / NOFORMAT / empty
# and its IOSB count, timed READVBLK and READPROMPT with a zero timeout after a
# typeahead purge (SS$_TIMEOUT, nothing read) and their terminator fields, a
# function the terminal driver does not have, $CANCEL, and the channel after
# $DASSGN. Terminal type and characteristics bits differ between consoles, so
# only class / width / page print.
P = Probe("tt")
SENSEMODE, SENSECHAR, SETMODE = K("IO$_SENSEMODE"), K("IO$_SENSECHAR"), K("IO$_SETMODE")
WRITEV, READV, READP = K("IO$_WRITEVBLK"), K("IO$_READVBLK"), K("IO$_READPROMPT")
TIMED, PURGE, NOFORMAT = K("IO$M_TIMED"), K("IO$M_PURGE"), K("IO$M_NOFORMAT")
ACCESS = K("IO$_ACCESS")

P.long("CHAN", 0)
P.quad("IOSB")
P.buf("CHAR", 12, fill=0)
P.buf("RBUF", 20, fill=0x5A)
P.desc("TT", "TT:")
P.buf("WLINE", 12, text="SP_TT_LINE_1")
P.buf("PROMPT", 4, text="SP> ")

IOS = [UW("IOSB", "iosb"), UW("IOSB", "count", off=2)]
RDS = IOS + [UW("IOSB", "term", off=4), UW("IOSB", "termlen", off=6)]
CHS = IOS + [UB("CHAR", 0, "class"), UW("CHAR", "width", off=2), UB("CHAR", 7, "page")]


def qiow(cid, func, p1=0, p2=0, p3=0, p4=0, p5=0, p6=0, show=IOS):
    P.setl("IOSB", 0xA5A5A5A5, 0); P.setl("IOSB", 0xA5A5A5A5, 4)
    P.call(cid, "SYS$QIOW", 0, V("CHAN"), func, R("IOSB"), None, 0, p1, p2, p3, p4, p5, p6, show=show)


P.call("TT.ASSIGN", "SYS$ASSIGN", R("TT"), R("CHAN"), 0, None)
qiow("TT.SENSEMODE", SENSEMODE, R("CHAR"), 12, show=CHS)
qiow("TT.SENSECHAR", SENSECHAR, R("CHAR"), 12, show=CHS)
qiow("TT.SENSEMODE.SHORT", SENSEMODE, R("CHAR"), 4, show=IOS)
qiow("TT.SETMODE.SAME", SETMODE, R("CHAR"), 12)
qiow("TT.WRITE", WRITEV, R("WLINE"), 12, 0, 0x20)
qiow("TT.WRITE.NOFORMAT", WRITEV | NOFORMAT, R("WLINE"), 12)
qiow("TT.WRITE.ZERO", WRITEV, R("WLINE"), 0)
qiow("TT.READ.TIMED0", READV | TIMED | PURGE, R("RBUF"), 20, 0, show=RDS)
qiow("TT.READ.TIMED0.ZEROLEN", READV | TIMED | PURGE, R("RBUF"), 0, 0, show=RDS)
qiow("TT.READPROMPT.TIMED0", READP | TIMED | PURGE, R("RBUF"), 20, 0, 0, R("PROMPT"), 4, show=RDS)
qiow("TT.ILLFUNC", ACCESS)
P.call("TT.CANCEL", "SYS$CANCEL", V("CHAN"))
P.call("TT.DASSGN", "SYS$DASSGN", V("CHAN"))
qiow("TT.AFTER.DASSGN", SENSEMODE, R("CHAR"), 12)
PROBE = P
