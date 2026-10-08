# Semantic-oracle spec: LOGICAL NAMES (rd vms-8d1).
# $CRELNM / $TRNLNM / $DELLNM: supersede, multi-valued names and LNM$_INDEX,
# attributes (CONCEALED, TERMINAL, CASE_BLIND, EXISTS), access modes, table
# search lists (LNM$FILE_DEV), the shareable tables, short output buffers, and
# the invalid-name / no-such-table error paths. Every name this probe creates is
# user-mode in LNM$PROCESS (gone at image rundown) or explicitly deleted.
P = Probe("lnm")

STRING, ATTR, INDEX, TABLE, LENGTH, ACMODE, MAXIDX = (
    K("LNM$_STRING"), K("LNM$_ATTRIBUTES"), K("LNM$_INDEX"), K("LNM$_TABLE"),
    K("LNM$_LENGTH"), K("LNM$_ACMODE"), K("LNM$_MAX_INDEX"))
M_CONCEALED, M_TERMINAL, M_CASE_BLIND = K("LNM$M_CONCEALED"), K("LNM$M_TERMINAL"), K("LNM$M_CASE_BLIND")
USER, SUPER, EXEC, KERNEL = K("PSL$C_USER"), K("PSL$C_SUPER"), K("PSL$C_EXEC"), K("PSL$C_KERNEL")

for n, t in [("T_PROC", "LNM$PROCESS_TABLE"), ("T_PROCL", "LNM$PROCESS"), ("T_JOB", "LNM$JOB"),
             ("T_GROUP", "LNM$GROUP"), ("T_SYS", "LNM$SYSTEM"), ("T_SYSTAB", "LNM$SYSTEM_TABLE"),
             ("T_FILEDEV", "LNM$FILE_DEV"), ("T_PDIR", "LNM$PROCESS_DIRECTORY"),
             ("T_SDIR", "LNM$SYSTEM_DIRECTORY"), ("T_NOSUCH", "SP_NO_SUCH_TABLE"),
             ("N_A", "SP_A"), ("N_A_LC", "sp_a"), ("N_A_COLON", "SP_A:"), ("N_B", "SP_MULTI"),
             ("N_C", "SP_CONC"), ("N_D", "SP_SEARCH"), ("N_E", "SP_SHORT"), ("N_F", "SP_EMPTY"), ("N_NONE", "SP_NEVER_DEFINED"),
             ("N_EMPTY", ""), ("N_LONG", "S" * 255), ("N_TOOLONG", "S" * 256), ("N_LOWER", "sp_lower"),
             ("N_SYSD", "SYS$DISK"), ("N_SYSCMD", "SYS$COMMAND"), ("N_SYSIN", "SYS$INPUT"),
             ("N_SYSOUT", "SYS$OUTPUT"), ("N_SYSLOGIN", "SYS$LOGIN"), ("N_SYSSYS", "SYS$SYSTEM"),
             ("N_SYSSCR", "SYS$SCRATCH"), ("N_TT", "TT"), ("N_SYSDIR", "LNM$DIRECTORIES"),
             ("N_SYSD_MODE", "SP_MODES"), ("N_WEIRD", "SP A")]:
    P.desc(n, t)

# --- translation item list (outputs reset before every $TRNLNM) ------------
P.long("IDX")
P.buf("OSTR", 255)
P.word("OSTRL")
P.long("OATTR")
P.long("OLEN")
P.long("OMAX")
P.buf("OMODE", 4)
P.buf("OTAB", 31)
P.word("OTABL")
P.items("TRN", [(4, INDEX, "IDX", None), (255, STRING, "OSTR", "OSTRL"), (4, ATTR, "OATTR", None),
                (4, LENGTH, "OLEN", None), (4, MAXIDX, "OMAX", None), (1, ACMODE, "OMODE", None),
                (31, TABLE, "OTAB", "OTABL")])
P.items("TRN_SHORT", [(3, STRING, "OSTR", "OSTRL"), (4, ATTR, "OATTR", None)])
P.items("TRN_ZERO", [(0, STRING, "OSTR", "OSTRL")])
P.long("TATTR")
P.long("AMODE")

# a $TRNLNM output never written keeps its sentinel: retlen 65535 prints as ""
STR = T("OSTR", "OSTRL", "W", "str", limit=255)
TAB = T("OTAB", "OTABL", "W", "table", limit=31)
TAB8 = T("OTAB", "OTABL", "W", "table", limit=31, clamp=8)   # LNM$JOB_<address>: prefix only
ALL = [STR, X("OATTR", "attr"), U("OLEN", "len"), S("OMAX", "maxidx"), UB("OMODE", 0, "mode"), TAB]
ALLJ = ALL[:-1] + [TAB8]


with P.sub("RESET"):
    P.fill("OSTR", byte=0x5A); P.setw("OSTRL", 0xFFFF); P.setl("OATTR", 0xA5A5A5A5)
    P.setl("OLEN", 0xA5A5A5A5); P.setl("OMAX", 0xA5A5A5A5); P.fill("OMODE", byte=0xEE)
    P.fill("OTAB", byte=0x5A); P.setw("OTABL", 0xFFFF)


def reset():
    P.gosub("RESET")


def trn(cid, name, table="T_FILEDEV", idx=0, attr=0, mode=None, items="TRN", show=None):
    reset(); P.setl("IDX", idx); P.setl("TATTR", attr)
    a = [R("TATTR"), R(table), R(name), None if mode is None else R("AMODE"), R(items)]
    if mode is not None:
        P.setl("AMODE", mode)
    P.call(cid, "SYS$TRNLNM", *a, show=ALL if show is None else show)


# --- equivalence-string buffers for $CRELNM ------------------------------
for n, t in [("E1", "VALUE1"), ("E2", "SECOND"), ("E3", "third value"), ("E4", "DKA100:[CONC.]"),
             ("E5", "X" * 255), ("E6", "")]:
    P.buf(n, max(len(t), 1), text=t)
P.buf("E7", 256, text="Y" * 256)
P.long("CATTR")
P.items("C1", [(6, STRING, "E1", None)])
P.items("C1B", [(6, STRING, "E2", None)])
P.items("C3", [(6, STRING, "E1", None), (6, STRING, "E2", None), (11, STRING, "E3", None)])
P.items("CCONC", [(4, ATTR, "CATTR", None), (14, STRING, "E4", None)])
P.items("C255", [(255, STRING, "E5", None)])
P.items("C256", [(256, STRING, "E7", None)])
P.items("CEMPTY", [(0, STRING, "E6", None)])
P.items("CNONE", [])


def cre(cid, name, items, table="T_PROC", mode=None, attr=0):
    P.setl("TATTR", attr)
    if mode is not None:
        P.setl("AMODE", mode)
    P.call(cid, "SYS$CRELNM", R("TATTR"), R(table), R(name), None if mode is None else R("AMODE"), R(items))


def dele(cid, name, table="T_PROC", mode=None):
    if mode is not None:
        P.setl("AMODE", mode)
    P.call(cid, "SYS$DELLNM", R(table), R(name), None if mode is None else R("AMODE"))


# 0. the probe runs as SYSTEM (CMKRNL, SYSNAM), so a name it creates in an inner
#    access mode or a shareable table outlives the image: clear any left by an
#    earlier run (statuses ignored) so every run starts from the same state
for m in (KERNEL, EXEC, SUPER, USER):
    P.setl("AMODE", m)
    for n in ("N_SYSD_MODE", "N_A", "N_B", "N_C", "N_E", "N_F", "N_LONG", "N_LOWER", "N_WEIRD"):
        P.do("SYS$DELLNM", R("T_PROC"), R(n), R("AMODE"))
    for t in ("T_JOB", "T_SYS", "T_GROUP"):
        P.do("SYS$DELLNM", R(t), R("N_D"), R("AMODE"))

# 1. create, supersede, translate back
cre("LNM.CRE.NEW", "N_A", "C1")
trn("LNM.TRN.NEW", "N_A")
trn("LNM.TRN.NEW.PROCTAB", "N_A", table="T_PROC")
trn("LNM.TRN.NEW.PROCLOG", "N_A", table="T_PROCL")
cre("LNM.CRE.SUPERSEDE", "N_A", "C1B")
trn("LNM.TRN.SUPERSEDED", "N_A")
trn("LNM.TRN.LOWERCASE", "N_A_LC")
trn("LNM.TRN.CASE_BLIND", "N_A_LC", attr=M_CASE_BLIND)
trn("LNM.TRN.COLON", "N_A_COLON")
trn("LNM.TRN.NONE", "N_NONE")
trn("LNM.TRN.SHORTBUF", "N_A", items="TRN_SHORT", show=[UW("OSTRL", "strlen"), STR, X("OATTR", "attr")])
trn("LNM.TRN.ZEROBUF", "N_A", items="TRN_ZERO", show=[UW("OSTRL", "strlen"), STR])
trn("LNM.TRN.EXEC_MODE", "N_A", mode=EXEC)
trn("LNM.TRN.USER_MODE", "N_A", mode=USER)
cre("LNM.CRE.LOWERNAME", "N_LOWER", "C1")
trn("LNM.TRN.LOWERNAME", "N_LOWER")
cre("LNM.CRE.SPACE", "N_WEIRD", "C1")
trn("LNM.TRN.SPACE", "N_WEIRD")

# 2. multi-valued names and LNM$_INDEX
cre("LNM.CRE.MULTI", "N_B", "C3")
for i in (0, 1, 2, 3, 127, 128):
    trn("LNM.TRN.MULTI.%d" % i, "N_B", idx=i)

# 3. attributes and lengths
P.setl("CATTR", M_CONCEALED | M_TERMINAL)
cre("LNM.CRE.CONCEALED", "N_C", "CCONC")
trn("LNM.TRN.CONCEALED", "N_C")
cre("LNM.CRE.255", "N_E", "C255")
trn("LNM.TRN.255", "N_E", show=[UW("OSTRL", "strlen"), U("OLEN", "len"), X("OATTR", "attr")])
cre("LNM.CRE.256", "N_E", "C256")
trn("LNM.TRN.AFTER256", "N_E", show=[UW("OSTRL", "strlen"), U("OLEN", "len")])
cre("LNM.CRE.EMPTYEQ", "N_F", "CEMPTY")
trn("LNM.TRN.EMPTYEQ", "N_F")
cre("LNM.CRE.NOITEMS", "N_F", "CNONE")
trn("LNM.TRN.NOITEMS", "N_F")
cre("LNM.CRE.NAME255", "N_LONG", "C1")
trn("LNM.TRN.NAME255", "N_LONG", show=[STR])
cre("LNM.CRE.NAME256", "N_TOOLONG", "C1")
trn("LNM.TRN.NAME256", "N_TOOLONG", show=[])
cre("LNM.CRE.NAME0", "N_EMPTY", "C1")
trn("LNM.TRN.NAME0", "N_EMPTY", show=[])

# 4. tables, search lists and access modes
cre("LNM.CRE.NOSUCHTABLE", "N_A", "C1", table="T_NOSUCH")
trn("LNM.TRN.NOSUCHTABLE", "N_A", table="T_NOSUCH", show=[])
cre("LNM.CRE.SEARCHLIST", "N_A", "C1", table="T_FILEDEV")
trn("LNM.TRN.SEARCHLIST", "N_A", table="T_FILEDEV")
cre("LNM.CRE.JOB", "N_D", "C1", table="T_JOB")
trn("LNM.TRN.JOB.VIA_FILEDEV", "N_D", show=ALLJ)
cre("LNM.CRE.PROC_OVER_JOB", "N_D", "C1B")
trn("LNM.TRN.PROC_OVER_JOB", "N_D")
dele("LNM.DEL.PROC_OVER_JOB", "N_D")
trn("LNM.TRN.JOB.AGAIN", "N_D", show=ALLJ)
dele("LNM.DEL.JOB", "N_D", table="T_JOB")
cre("LNM.CRE.SYSTEM", "N_D", "C1", table="T_SYS")
trn("LNM.TRN.SYSTEM", "N_D", table="T_SYS")
dele("LNM.DEL.SYSTEM", "N_D", table="T_SYS")
cre("LNM.CRE.GROUP", "N_D", "C1", table="T_GROUP")
dele("LNM.DEL.GROUP", "N_D", table="T_GROUP")
cre("LNM.CRE.KERNEL_ASKED", "N_SYSD_MODE", "C1", mode=KERNEL)
trn("LNM.TRN.KERNEL_ASKED", "N_SYSD_MODE")
cre("LNM.CRE.SUPER", "N_SYSD_MODE", "C1B", mode=SUPER)
trn("LNM.TRN.TWO_MODES", "N_SYSD_MODE")
trn("LNM.TRN.TWO_MODES.SUPERQ", "N_SYSD_MODE", mode=SUPER)
dele("LNM.DEL.USERMODE_ONLY", "N_SYSD_MODE", mode=USER)
trn("LNM.TRN.AFTER_USERDEL", "N_SYSD_MODE")
dele("LNM.DEL.SUPER", "N_SYSD_MODE", mode=SUPER)
trn("LNM.TRN.AFTER_SUPERDEL", "N_SYSD_MODE")

# 5. names every process has (values differ between systems: print only shape)
# (not the length: a process-permanent name's equivalence carries the node's
#  device name, whose length differs from system to system)
SHAPE = [X("OATTR", "attr"), S("OMAX", "maxidx"), UB("OMODE", 0, "mode"), TAB, XB("OSTR", 4, 0, "head")]
for n in ("N_SYSCMD", "N_SYSIN", "N_SYSOUT"):
    trn("LNM.TRN.%s" % n[2:], n, show=SHAPE)
for n in ("N_SYSD", "N_SYSLOGIN", "N_SYSSYS", "N_SYSSCR", "N_TT"):
    trn("LNM.TRN.%s" % n[2:], n, show=[X("OATTR", "attr"), S("OMAX", "maxidx"), UB("OMODE", 0, "mode"), TAB8])
# (LNM$FILE_DEV's search list is site configuration: its length is not shown)
trn("LNM.TRN.FILEDEV.IN_SDIR", "T_FILEDEV", table="T_SDIR", show=[X("OATTR", "attr"), UB("OMODE", 0, "mode"), TAB])
trn("LNM.TRN.PROCTAB.IN_PDIR", "T_PROC", table="T_PDIR", show=[X("OATTR", "attr"), S("OMAX", "maxidx"),
                                                               UB("OMODE", 0, "mode"), TAB])

# 6. deletion
dele("LNM.DEL.A", "N_A")
dele("LNM.DEL.A.AGAIN", "N_A")
trn("LNM.TRN.AFTER_DEL", "N_A", show=[])
dele("LNM.DEL.NOSUCHTABLE", "N_A", table="T_NOSUCH")
dele("LNM.DEL.NAME0", "N_EMPTY")
dele("LNM.DEL.EXEC_NAME_FROM_USER", "N_SYSLOGIN", mode=USER)
P.call("LNM.DEL.ALL_IN_TABLE.NONAME", "SYS$DELLNM", R("T_PROC"), None, None)
trn("LNM.TRN.AFTER_DELALL", "N_B", show=[])
trn("LNM.TRN.LOGIN_SURVIVES", "N_SYSLOGIN", show=[UB("OMODE", 0, "mode")])
dele("LNM.DEL.KERNEL_NAME", "N_SYSD_MODE", mode=KERNEL)
trn("LNM.TRN.AFTER_KERNELDEL", "N_SYSD_MODE", show=[])
PROBE = P
