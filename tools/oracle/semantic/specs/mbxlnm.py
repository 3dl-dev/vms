# Semantic-oracle spec: WHERE $CREMBX puts a mailbox's logical name (rd vms-7dd).
# A temporary mailbox's name goes in the table LNM$TEMPORARY_MAILBOX names, a
# permanent one's in the table LNM$PERMANENT_MAILBOX names; the name goes away
# with the mailbox. The two table-name logicals themselves are read out of the
# directories. The equivalence string is the mailbox's device name, whose unit
# number differs from run to run: only its first three bytes are printed.
P = Probe("mbxlnm")

STRING, ATTR, TABLE, ACMODE = K("LNM$_STRING"), K("LNM$_ATTRIBUTES"), K("LNM$_TABLE"), K("LNM$_ACMODE")

for n, t in [("T_FILEDEV", "LNM$FILE_DEV"), ("T_JOB", "LNM$JOB"), ("T_SYS", "LNM$SYSTEM"),
             ("T_PDIR", "LNM$PROCESS_DIRECTORY"), ("T_SDIR", "LNM$SYSTEM_DIRECTORY"),
             ("N_TMPT", "LNM$TEMPORARY_MAILBOX"), ("N_PRMT", "LNM$PERMANENT_MAILBOX"),
             ("N_TMP", "SP_MBXL_TMP"), ("N_PRM", "SP_MBXL_PRM")]:
    P.desc(n, t)

P.buf("OSTR", 255)
P.word("OSTRL")
P.long("OATTR")
P.buf("OMODE", 4)
P.buf("OTAB", 31)
P.word("OTABL")
P.items("TRN", [(255, STRING, "OSTR", "OSTRL"), (4, ATTR, "OATTR", None),
                (1, ACMODE, "OMODE", None), (31, TABLE, "OTAB", "OTABL")])
P.long("TATTR")

STR = T("OSTR", "OSTRL", "W", "str", limit=255)
TAB = T("OTAB", "OTABL", "W", "table", limit=31)
TAB8 = T("OTAB", "OTABL", "W", "table", limit=31, clamp=8)   # LNM$JOB_<address>: prefix only
DIR = [STR, X("OATTR", "attr"), UB("OMODE", 0, "mode"), TAB]
MBX = [XB("OSTR", 3, 0, "head"), X("OATTR", "attr"), UB("OMODE", 0, "mode"), TAB8]

with P.sub("RESET"):
    P.fill("OSTR", byte=0x5A); P.setw("OSTRL", 0xFFFF); P.setl("OATTR", 0xA5A5A5A5)
    P.fill("OMODE", byte=0xEE); P.fill("OTAB", byte=0x5A); P.setw("OTABL", 0xFFFF)


def trn(cid, name, table, show):
    P.gosub("RESET")
    P.call(cid, "SYS$TRNLNM", R("TATTR"), R(table), R(name), None, R("TRN"), show=show)


P.word("CTMP", 0); P.word("CPRM", 0); P.word("COLD", 0)

# 0. a permanent mailbox outlives the image: delete any an earlier run left
#    (statuses ignored) so every run starts from the same state
P.do("SYS$ASSIGN", R("N_PRM"), R("COLD"), 0, None)
P.do("SYS$DELMBX", V("COLD"))
P.do("SYS$DASSGN", V("COLD"))

# 1. the two table-name logicals, in each directory
for nm, tag in (("N_TMPT", "TEMPORARY"), ("N_PRMT", "PERMANENT")):
    trn("MBXL.DIR.%s.SDIR" % tag, nm, "T_SDIR", DIR)
    trn("MBXL.DIR.%s.PDIR" % tag, nm, "T_PDIR", DIR)

# 2. a temporary mailbox's name: in LNM$JOB, not LNM$SYSTEM
P.call("MBXL.CREMBX.TMP", "SYS$CREMBX", 0, R("CTMP"), 40, 400, 0, None, R("N_TMP"), None)
trn("MBXL.TRN.TMP.FILEDEV", "N_TMP", "T_FILEDEV", MBX)
trn("MBXL.TRN.TMP.JOB", "N_TMP", "T_JOB", MBX)
trn("MBXL.TRN.TMP.SYSTEM", "N_TMP", "T_SYS", MBX)

# 3. a permanent mailbox's name: in LNM$SYSTEM
P.call("MBXL.CREMBX.PRM", "SYS$CREMBX", 1, R("CPRM"), 40, 400, 0, None, R("N_PRM"), None)
trn("MBXL.TRN.PRM.FILEDEV", "N_PRM", "T_FILEDEV", MBX)
trn("MBXL.TRN.PRM.SYSTEM", "N_PRM", "T_SYS", MBX)
trn("MBXL.TRN.PRM.JOB", "N_PRM", "T_JOB", MBX)

# 4. the name goes with the mailbox: the temporary one at its last $DASSGN,
#    the permanent one when it is marked for deletion and its last channel goes
P.call("MBXL.DASSGN.TMP", "SYS$DASSGN", V("CTMP"))
trn("MBXL.TRN.TMP.GONE", "N_TMP", "T_FILEDEV", MBX)
P.call("MBXL.DELMBX.PRM", "SYS$DELMBX", V("CPRM"))
P.call("MBXL.DASSGN.PRM", "SYS$DASSGN", V("CPRM"))
trn("MBXL.TRN.PRM.GONE", "N_PRM", "T_FILEDEV", MBX)
PROBE = P
