# Semantic-oracle spec: $GETJPI / $GETSYI / $GETDVI (rd vms-8d1).
# Item lists (sizes, return lengths, short and zero-length buffers, unknown item
# codes), PID / process-name / device-name / channel selection, wildcards, and
# the efn / IOSB / AST completion contract of the asynchronous forms (EF 0 is
# what an omitted efn means). Values that differ between systems (PIDs, node and
# device names) are shown as relations or not at all.
P = Probe("info")
SS_NOMOREPROC = K("SS$_NOMOREPROC")

P.quad("IOSB")
P.long("STATE")
P.long("PIDA", 0)
P.long("PIDB")
P.ast("AST1")
P.desc("NOPROC", "SP_NO_SUCH_PROC")
P.desc("LONGNAME", "P" * 16)

# --- $GETJPI ---------------------------------------------------------------
for n in ("J_PID", "J_MPID", "J_OWNER", "J_UIC", "J_GRP", "J_MEM", "J_MODE", "J_JOBTYPE",
          "J_STATE", "J_PRCCNT", "J_PRIB", "J_EFCS", "J_ASTEN", "J_STS"):
    P.long(n, 0xA5A5A5A5)
P.buf("J_USER", 12); P.word("J_USERL", 0xFFFF)
P.buf("J_PRCNAM", 15); P.word("J_PRCNAML", 0xFFFF)
P.buf("J_PRIV", 8)
P.buf("J_SHORT", 4); P.word("J_SHORTL", 0xFFFF)
P.long("J_SMALL", 0xA5A5A5A5); P.word("J_SMALLL", 0xFFFF)
P.items("JPI_A", [(4, K("JPI$_PID"), "J_PID", None), (4, K("JPI$_MASTER_PID"), "J_MPID", None),
                  (4, K("JPI$_OWNER"), "J_OWNER", None), (4, K("JPI$_UIC"), "J_UIC", None),
                  (4, K("JPI$_GRP"), "J_GRP", None), (4, K("JPI$_MEM"), "J_MEM", None),
                  (4, K("JPI$_MODE"), "J_MODE", None), (4, K("JPI$_JOBTYPE"), "J_JOBTYPE", None),
                  (4, K("JPI$_STATE"), "J_STATE", None), (4, K("JPI$_PRIB"), "J_PRIB", None),
                  (12, K("JPI$_USERNAME"), "J_USER", "J_USERL"),
                  (8, K("JPI$_AUTHPRIV"), "J_PRIV", None)])
P.items("JPI_PID", [(4, K("JPI$_PID"), "J_PID", None)])
P.items("JPI_SHORT", [(4, K("JPI$_USERNAME"), "J_SHORT", "J_SHORTL"), (2, K("JPI$_UIC"), "J_SMALL", "J_SMALLL")])
P.items("JPI_ZERO", [(0, K("JPI$_USERNAME"), "J_SHORT", "J_SHORTL")])
P.items("JPI_BAD", [(4, 9999, "J_PID", None)])
P.items("JPI_EMPTY", [])

JSHOW = [UW("IOSB", "iosb"), EQ("J_PID", "J_MPID", "pid=master"), X("J_OWNER", "owner"), X("J_UIC", "uic"),
         U("J_GRP", "grp"), U("J_MEM", "mem"), U("J_MODE", "mode"), U("J_JOBTYPE", "jobtype"),
         U("J_PRIB", "prib"), UW("J_USERL", "userlen"), T("J_USER", "J_USERL", label="user", limit=12),
         XB("J_PRIV", 8, label="authpriv")]



P.setl("PIDA", 0)
P.call("JPI.SELF.W", "SYS$GETJPIW", 0, None, None, R("JPI_A"), R("IOSB"), None, 0, show=JSHOW)
P.setl("PIDA", 0)
P.call("JPI.SELF.PIDADR0", "SYS$GETJPIW", 0, R("PIDA"), None, R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("PIDA", "J_PID", "pidadr=self")])
P.call("JPI.NOPROC.NAME", "SYS$GETJPIW", 0, None, R("NOPROC"), R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.call("JPI.LONGNAME", "SYS$GETJPIW", 0, None, R("LONGNAME"), R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.setl("PIDB", 0x00FFFFFF)
P.call("JPI.NOPROC.PID", "SYS$GETJPIW", 0, R("PIDB"), None, R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.call("JPI.SHORTBUF", "SYS$GETJPIW", 0, None, None, R("JPI_SHORT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), UW("J_SHORTL", "userlen"), T("J_SHORT", "J_SHORTL", label="user", limit=4),
             UW("J_SMALLL", "uiclen"), X("J_SMALL", "uic")])
P.call("JPI.ZEROLEN", "SYS$GETJPIW", 0, None, None, R("JPI_ZERO"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), UW("J_SHORTL", "userlen")])
P.call("JPI.BADITEM", "SYS$GETJPIW", 0, None, None, R("JPI_BAD"), R("IOSB"), None, 0)
P.call("JPI.NOITEMS", "SYS$GETJPIW", 0, None, None, R("JPI_EMPTY"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.call("JPI.NOIOSB", "SYS$GETJPIW", 0, None, None, R("JPI_PID"), None, None, 0)

# completion contract: efn cleared at request and set at completion, IOSB, AST
for efn in (0, 5):
    P.do("SYS$SETEF", efn)
    P.setl("AST1", 0); P.setl("AST1_P", 0); P.setl("STATE", 0xA5A5A5A5)
    P.call("JPI.ASYNC.EFN%d" % efn, "SYS$GETJPI", efn, None, None, R("JPI_PID"), R("IOSB"), AST("AST1"), 0x1234 + efn)
    P.call("JPI.ASYNC.EFN%d.WAIT" % efn, "SYS$WAITFR", efn, guard=True)
    P.call("JPI.ASYNC.EFN%d.DONE" % efn, "SYS$READEF", efn, R("STATE"),
           show=[UW("IOSB", "iosb"), U("AST1", "asts"), X("AST1_P", "astprm")])
P.do("SYS$SETEF", 0)
P.call("JPI.W.EFN0", "SYS$GETJPIW", 0, None, None, R("JPI_PID"), R("IOSB"), None, 0)
P.call("JPI.W.EFN0.AFTER", "SYS$READEF", 0, R("STATE"))
P.do("SYS$CLREF", 0)
P.call("JPI.W.EFN0.CLEAR", "SYS$GETJPIW", 0, None, None, R("JPI_PID"), R("IOSB"), None, 0)
P.call("JPI.W.EFN0.CLEAR.AFTER", "SYS$READEF", 0, R("STATE"))
P.call("JPI.EFN128", "SYS$GETJPIW", 128, None, None, R("JPI_PID"), R("IOSB"), None, 0, show=[UW("IOSB", "iosb")])
P.call("JPI.EFN255", "SYS$GETJPIW", 255, None, None, R("JPI_PID"), R("IOSB"), None, 0)
P.call("JPI.EFN64.UNASSOC", "SYS$GETJPIW", 64, None, None, R("JPI_PID"), R("IOSB"), None, 0)

# wildcard: -1 walks the processes; the first two calls both find one
P.setl("PIDB", 0xFFFFFFFF)
P.call("JPI.WILD.1", "SYS$GETJPIW", 0, R("PIDB"), None, R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("PIDB", "J_PID", "pidadr=found")])
P.call("JPI.WILD.2", "SYS$GETJPIW", 0, R("PIDB"), None, R("JPI_PID"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("PIDB", "J_PID", "pidadr=found")])

# --- $GETSYI ---------------------------------------------------------------
for n in ("S_PAGE", "S_MAXBUF", "S_ARCH", "S_ACTCPU", "S_DEFPRI", "S_CSID", "S_SMALL"):
    P.long(n, 0xA5A5A5A5)
P.buf("S_CLMEM", 1)
P.buf("S_ARCHNAME", 15); P.word("S_ARCHNAMEL", 0xFFFF)
P.buf("S_NODE", 15); P.word("S_NODEL", 0xFFFF)
P.buf("S_SCS", 8); P.word("S_SCSL", 0xFFFF)
P.word("S_SMALLL", 0xFFFF)
P.items("SYI_A", [(4, K("SYI$_PAGE_SIZE"), "S_PAGE", None), (4, K("SYI$_MAXBUF"), "S_MAXBUF", None),
                  (4, K("SYI$_ARCH_TYPE"), "S_ARCH", None), (15, K("SYI$_ARCH_NAME"), "S_ARCHNAME", "S_ARCHNAMEL"),
                  (1, K("SYI$_CLUSTER_MEMBER"), "S_CLMEM", None), (4, K("SYI$_DEFPRI"), "S_DEFPRI", None)])
P.items("SYI_NODE", [(15, K("SYI$_NODENAME"), "S_NODE", "S_NODEL"), (8, K("SYI$_SCSNODE"), "S_SCS", "S_SCSL")])
P.items("SYI_SHORT", [(2, K("SYI$_ARCH_NAME"), "S_ARCHNAME", "S_ARCHNAMEL"), (2, K("SYI$_PAGE_SIZE"), "S_SMALL", "S_SMALLL")])
P.items("SYI_BAD", [(4, 9999, "S_PAGE", None)])
# page size, architecture and cluster membership are the SYSTEM's, not the
# service's: show only that $GETSYI answered them (the sentinel was replaced)
P.long("SENTINEL", 0xA5A5A5A5)
P.call("SYI.LOCAL.W", "SYS$GETSYIW", 0, None, None, R("SYI_A"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("S_PAGE", "SENTINEL", "page_size_unset"),
             EQ("S_ARCH", "SENTINEL", "arch_type_unset"), U("S_DEFPRI", "defpri")])
P.call("SYI.NODE", "SYS$GETSYIW", 0, None, None, R("SYI_NODE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.call("SYI.SHORTBUF", "SYS$GETSYIW", 0, None, None, R("SYI_SHORT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), UW("S_ARCHNAMEL", "namelen"), UW("S_SMALLL", "pagelen")])
P.call("SYI.BADITEM", "SYS$GETSYIW", 0, None, None, R("SYI_BAD"), R("IOSB"), None, 0)
P.setl("S_CSID", 0x00012345)
P.call("SYI.NOSUCHCSID", "SYS$GETSYIW", 0, R("S_CSID"), None, R("SYI_A"), R("IOSB"), None, 0)
P.desc("NONODE", "SPNONODE")
P.call("SYI.NOSUCHNODE", "SYS$GETSYIW", 0, None, R("NONODE"), R("SYI_A"), R("IOSB"), None, 0)
P.do("SYS$SETEF", 6)
P.setl("AST1", 0); P.setl("AST1_P", 0)
P.call("SYI.ASYNC", "SYS$GETSYI", 6, None, None, R("SYI_NODE"), R("IOSB"), AST("AST1"), 0x5151)
P.call("SYI.ASYNC.WAIT", "SYS$WAITFR", 6, guard=True)
P.call("SYI.ASYNC.DONE", "SYS$READEF", 6, R("STATE"), show=[UW("IOSB", "iosb"), U("AST1", "asts"), X("AST1_P", "astprm")])

# --- $GETDVI / $ASSIGN -----------------------------------------------------
for n in ("D_CLASS", "D_TYPE", "D_CHAR", "D_CHAR2", "D_UNIT", "D_BUFSIZ", "D_MBX", "D_TRM", "D_SPL", "D_DEPEND"):
    P.long(n, 0xA5A5A5A5)
P.buf("D_NAM", 64); P.word("D_NAML", 0xFFFF)
P.word("CHAN", 0)
P.items("DVI_A", [(4, K("DVI$_DEVCLASS"), "D_CLASS", None), (4, K("DVI$_DEVTYPE"), "D_TYPE", None),
                  (4, K("DVI$_DEVCHAR"), "D_CHAR", None), (4, K("DVI$_DEVCHAR2"), "D_CHAR2", None),
                  (4, K("DVI$_UNIT"), "D_UNIT", None), (4, K("DVI$_DEVBUFSIZ"), "D_BUFSIZ", None),
                  (4, K("DVI$_MBX"), "D_MBX", None), (4, K("DVI$_TRM"), "D_TRM", None),
                  (4, K("DVI$_SPL"), "D_SPL", None)])
P.items("DVI_NAM", [(64, K("DVI$_DEVNAM"), "D_NAM", "D_NAML")])
DSHOW = [UW("IOSB", "iosb"), U("D_CLASS", "class"), U("D_TYPE", "type"), X("D_CHAR", "char"), X("D_CHAR2", "char2"),
         U("D_UNIT", "unit"), U("D_BUFSIZ", "bufsiz"), U("D_MBX", "mbx"), U("D_TRM", "trm"), U("D_SPL", "spl")]
for n, t in [("DV_NL", "NLA0:"), ("DV_NLNC", "NLA0"), ("DV_NLU", "_NLA0:"), ("DV_NL7", "NLA7:"),
             ("DV_NOSUCH", "QQA0:"), ("DV_BAD", "!!BAD:"), ("DV_EMPTY", ""), ("DV_CMD", "SYS$COMMAND"),
             ("DV_TT", "TT:"), ("DV_NLLC", "nla0:"), ("DV_FILE", "NLA0:[DIR]FILE.TXT")]:
    P.desc(n, t)


def dvi(cid, dev, show=DSHOW):
    for n in ("D_CLASS", "D_TYPE", "D_CHAR", "D_CHAR2", "D_UNIT", "D_BUFSIZ", "D_MBX", "D_TRM", "D_SPL"):
        P.setl(n, 0xA5A5A5A5)
    P.call(cid, "SYS$GETDVIW", 0, 0, R(dev), R("DVI_A"), R("IOSB"), None, 0, None, show=show)


dvi("DVI.NLA0", "DV_NL")
dvi("DVI.NLA0.NOCOLON", "DV_NLNC")
dvi("DVI.NLA0.UNDERSCORE", "DV_NLU")
dvi("DVI.NLA0.LOWERCASE", "DV_NLLC")
dvi("DVI.NLA0.FILESPEC", "DV_FILE")
dvi("DVI.NLA7", "DV_NL7", show=[UW("IOSB", "iosb")])
dvi("DVI.NOSUCHDEV", "DV_NOSUCH", show=[UW("IOSB", "iosb")])
dvi("DVI.BADNAME", "DV_BAD", show=[UW("IOSB", "iosb")])
dvi("DVI.EMPTYNAME", "DV_EMPTY", show=[UW("IOSB", "iosb")])
dvi("DVI.SYSCOMMAND", "DV_CMD", show=[UW("IOSB", "iosb"), U("D_CLASS", "class"), U("D_TRM", "trm")])
dvi("DVI.TT", "DV_TT", show=[UW("IOSB", "iosb"), U("D_CLASS", "class"), U("D_TRM", "trm")])
P.call("DVI.DEVNAM.NLA0", "SYS$GETDVIW", 0, 0, R("DV_NL"), R("DVI_NAM"), R("IOSB"), None, 0, None,
       show=[UW("IOSB", "iosb"), UW("D_NAML", "len"), XB("D_NAM", 1, label="first")])
P.call("ASSIGN.NLA0", "SYS$ASSIGN", R("DV_NL"), R("CHAN"), 0, None)
P.call("DVI.CHAN", "SYS$GETDVIW", 0, V("CHAN"), None, R("DVI_A"), R("IOSB"), None, 0, None, show=DSHOW)
P.call("DVI.CHAN.AND.NAME", "SYS$GETDVIW", 0, V("CHAN"), R("DV_CMD"), R("DVI_A"), R("IOSB"), None, 0, None,
       show=[UW("IOSB", "iosb"), U("D_CLASS", "class")])
P.call("DASSGN", "SYS$DASSGN", V("CHAN"))
P.call("DVI.CHAN.AFTER_DASSGN", "SYS$GETDVIW", 0, V("CHAN"), None, R("DVI_A"), R("IOSB"), None, 0, None)
P.call("DASSGN.AGAIN", "SYS$DASSGN", V("CHAN"))
P.call("DASSGN.0", "SYS$DASSGN", 0)
P.call("ASSIGN.NOSUCHDEV", "SYS$ASSIGN", R("DV_NOSUCH"), R("CHAN"), 0, None)
P.call("ASSIGN.BADNAME", "SYS$ASSIGN", R("DV_BAD"), R("CHAN"), 0, None)
P.do("SYS$SETEF", 7)
P.setl("AST1", 0); P.setl("AST1_P", 0)
P.call("DVI.ASYNC", "SYS$GETDVI", 7, 0, R("DV_NL"), R("DVI_A"), R("IOSB"), AST("AST1"), 0x4444, None)
P.call("DVI.ASYNC.WAIT", "SYS$WAITFR", 7, guard=True)
P.call("DVI.ASYNC.DONE", "SYS$READEF", 7, R("STATE"), show=[UW("IOSB", "iosb"), U("AST1", "asts"), X("AST1_P", "astprm"),
                                                         U("D_CLASS", "class")])
PROBE = P
