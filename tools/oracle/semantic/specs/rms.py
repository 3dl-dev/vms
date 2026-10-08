# Semantic-oracle spec: RMS $CREATE/$OPEN/$CONNECT/$PUT/$GET/$PARSE/$SEARCH/
# $ERASE/$CLOSE (rd vms-8d1).
# Default-filespec merging (DNA), the NAM name/type/version parts and FNB flags,
# new versions vs FOP SUP, sequential variable and fixed records, a record too
# big for the user buffer, end of file, wildcards with $SEARCH, and the error
# paths: file not found, bad syntax, wrong access, no stream. Device and
# directory parts differ between systems and are never printed; files live in
# SYS$LOGIN and are erased before and after.
P = Probe("rms")
GET, PUT = K("FAB$M_GET"), K("FAB$M_PUT")
VAR, FIX, SEQ = K("FAB$C_VAR"), K("FAB$C_FIX"), K("FAB$C_SEQ")
CR = K("FAB$M_CR")

P.cb("FAB1", "FAB"); P.cb("RAB1", "RAB"); P.cb("NAM1", "NAM")
P.buf("ESA", 255); P.buf("RSA", 255)
P.buf("UBF", 64); P.buf("SMALL", 4)
for n, t in [("F_TEST", "SP_RMS_TEST"), ("D_DAT", "SYS$LOGIN:.DAT"), ("F_TEST1", "SYS$LOGIN:SP_RMS_TEST.DAT;1"),
             ("F_TEST2", "SYS$LOGIN:SP_RMS_TEST.DAT;2"), ("F_TEST3", "SYS$LOGIN:SP_RMS_TEST.DAT;3"),
             ("F_FIX", "SYS$LOGIN:SP_RMS_FIX.DAT"),
             ("F_TEST0", "SYS$LOGIN:SP_RMS_TEST.DAT;0"), ("F_FIX0", "SYS$LOGIN:SP_RMS_FIX.DAT;0"), ("F_NONE", "SYS$LOGIN:SP_RMS_NO_SUCH.DAT"),
             ("F_NODIR", "SYS$LOGIN:[.SP_NO_SUCH_DIR]X.DAT"), ("F_SYN", "A[B"), ("F_SYN2", "A.B.C.D"),
             ("F_WILD", "SYS$LOGIN:SP_RMS_TEST.*;*"), ("F_WILDNONE", "SYS$LOGIN:SP_RMS_NONE*.*"),
             ("F_X", "X"), ("D_FULL", "SYS$LOGIN:DEFNAME.TXT;5"), ("F_DOT", "X."), ("F_SEMI", "X.Y;"),
             ("F_LONGNAME", "A" * 40 + ".DAT"), ("F_BIGVER", "X.Y;32768"), ("F_NEGVER", "X.Y;-1"),
             ("F_LOWER", "sp_rms_test.dat"), ("F_EMPTY", ""), ("F_DEVONLY", "SYS$LOGIN:"),
             ("REC1", "first"), ("REC3", "the third record"), ("REC10", "0123456789")]:
    P.buf(n, max(len(t), 1), text=t)
    P.long(n + "_L", len(t))

FAB = [FLD("FAB1", "fab$l_sts", "sts"), FLD("FAB1", "fab$l_stv", "stv")]
NAMP = [FLD("NAM1", "nam$l_fnb", "fnb"), TPTR("NAM1", "nam$l_name", "nam$b_name", "name"),
        TPTR("NAM1", "nam$l_type", "nam$b_type", "type"), TPTR("NAM1", "nam$l_ver", "nam$b_ver", "ver")]
RAB = [FLD("RAB1", "rab$l_sts", "sts"), FLD("RAB1", "rab$w_rsz", "rsz", "U")]


def fab(fna, dna=None, fac=GET, fop=0, rfm=VAR, mrs=0):
    P.gosub("NEWFAB")
    P.cset("FAB1", "fab$l_fna", R(fna)); P.cset("FAB1", "fab$b_fns", len_of[fna])
    if dna:
        P.cset("FAB1", "fab$l_dna", R(dna)); P.cset("FAB1", "fab$b_dns", len_of[dna])
    P.cset("FAB1", "fab$b_fac", fac); P.cset("FAB1", "fab$l_fop", fop)
    P.cset("FAB1", "fab$b_rfm", rfm); P.cset("FAB1", "fab$w_mrs", mrs)


len_of = {n: len(t) for n, t in [(d[1], d[2][2]) for d in P.data if d[0] == "buf" and d[2][2] is not None]}

with P.sub("NEWFAB"):
    P.cset("FAB1", "fab$l_nam", R("NAM1"))
    P.cset("FAB1", "fab$l_dna", 0); P.cset("FAB1", "fab$b_dns", 0)
    P.cset("FAB1", "fab$l_sts", 0); P.cset("FAB1", "fab$l_stv", 0)
    P.cset("FAB1", "fab$b_org", SEQ); P.cset("FAB1", "fab$b_rat", CR)
    P.cset("NAM1", "nam$l_esa", R("ESA")); P.cset("NAM1", "nam$b_ess", 255)
    P.cset("NAM1", "nam$l_rsa", R("RSA")); P.cset("NAM1", "nam$b_rss", 255)
    P.cset("NAM1", "nam$b_esl", 0); P.cset("NAM1", "nam$b_rsl", 0)
    P.cset("NAM1", "nam$l_fnb", 0)
    P.cset("NAM1", "nam$b_name", 0); P.cset("NAM1", "nam$b_type", 0); P.cset("NAM1", "nam$b_ver", 0)


def rab(ubf="UBF", usz=64):
    P.cset("RAB1", "rab$l_fab", R("FAB1"))
    P.cset("RAB1", "rab$l_ubf", R(ubf)); P.cset("RAB1", "rab$w_usz", usz)
    P.cset("RAB1", "rab$l_sts", 0); P.cset("RAB1", "rab$w_rsz", 0xFFFF)


def put(cid, rec, n):
    P.cset("RAB1", "rab$l_rbf", R(rec)); P.cset("RAB1", "rab$w_rsz", n)
    P.call(cid, "SYS$PUT", R("RAB1"), show=RAB[:1])


# remove every version an earlier run left behind (;0 = the highest; statuses
# ignored)
with P.sub("CLEANUP"):
    for n in ["F_TEST0"] * 8 + ["F_FIX0"] * 3:
        fab(n)
        P.cset("FAB1", "fab$l_nam", 0)      # no NAM: the clean-up only needs the erase
        P.do("SYS$ERASE", R("FAB1"))
P.gosub("CLEANUP")

# 1. $CREATE with a default spec; $PUT three records; $CLOSE
fab("F_TEST", "D_DAT", fac=PUT)
P.call("RMS.CREATE", "SYS$CREATE", R("FAB1"), show=FAB + NAMP)
rab()
P.call("RMS.CONNECT", "SYS$CONNECT", R("RAB1"), show=RAB[:1])
put("RMS.PUT.1", "REC1", 5)
put("RMS.PUT.EMPTY", "REC1", 0)
put("RMS.PUT.3", "REC3", 16)
P.call("RMS.GET.WRITEONLY", "SYS$GET", R("RAB1"), show=RAB[:1])
P.call("RMS.CLOSE", "SYS$CLOSE", R("FAB1"), show=FAB[:1])
P.call("RMS.PUT.AFTER_CLOSE", "SYS$PUT", R("RAB1"), show=RAB[:1])

# 2. $OPEN and read back: record sizes, EOF, a too-small buffer
fab("F_TEST", "D_DAT")
P.call("RMS.OPEN", "SYS$OPEN", R("FAB1"), show=FAB + NAMP + [FLD("FAB1", "fab$b_rfm", "rfm", "U"),
                                                          FLD("FAB1", "fab$b_rat", "rat", "U")])
rab()
P.call("RMS.CONNECT.R", "SYS$CONNECT", R("RAB1"), show=RAB[:1])
for i in (1, 2, 3, 4, 5):
    P.fill("UBF", byte=0x2E)
    P.cset("RAB1", "rab$w_rsz", 0xFFFF)
    P.call("RMS.GET.%d" % i, "SYS$GET", R("RAB1"), show=RAB + [TPTR("RAB1", "rab$l_rbf", "rab$w_rsz", "rec", limit=64)])
P.call("RMS.PUT.READONLY", "SYS$PUT", R("RAB1"), show=RAB[:1])
P.call("RMS.REWIND", "SYS$REWIND", R("RAB1"), show=RAB[:1])
P.cset("RAB1", "rab$l_ubf", R("SMALL")); P.cset("RAB1", "rab$w_usz", 4)
P.cset("RAB1", "rab$w_rsz", 0xFFFF)
P.call("RMS.GET.SMALLBUF", "SYS$GET", R("RAB1"), show=RAB + [XB("SMALL", 4, label="buf")])
P.call("RMS.DISCONNECT", "SYS$DISCONNECT", R("RAB1"), show=RAB[:1])
P.call("RMS.GET.DISCONNECTED", "SYS$GET", R("RAB1"), show=RAB[:1])
P.call("RMS.CLOSE.R", "SYS$CLOSE", R("FAB1"), show=FAB[:1])
P.call("RMS.CLOSE.AGAIN", "SYS$CLOSE", R("FAB1"), show=FAB[:1])

# 3. versions: a second $CREATE makes ;2, FOP SUP supersedes it, MXV
fab("F_TEST", "D_DAT", fac=PUT)
P.call("RMS.CREATE.V2", "SYS$CREATE", R("FAB1"), show=FAB[:1] + NAMP)
P.do("SYS$CLOSE", R("FAB1"))
fab("F_TEST", "D_DAT", fac=PUT, fop=K("FAB$M_SUP"))
P.call("RMS.CREATE.SUP", "SYS$CREATE", R("FAB1"), show=FAB[:1] + NAMP)
P.do("SYS$CLOSE", R("FAB1"))
fab("F_TEST1", fac=PUT)
P.call("RMS.CREATE.EXPLICIT_V1", "SYS$CREATE", R("FAB1"), show=FAB[:1] + NAMP)
P.do("SYS$CLOSE", R("FAB1"))

# 4. fixed-length records
fab("F_FIX", fac=PUT | GET, rfm=FIX, mrs=10)
P.call("RMS.CREATE.FIX", "SYS$CREATE", R("FAB1"), show=FAB[:1])
rab()
P.do("SYS$CONNECT", R("RAB1"))
put("RMS.PUT.FIX.SHORT", "REC1", 5)
put("RMS.PUT.FIX.EXACT", "REC10", 10)
P.call("RMS.REWIND.FIX", "SYS$REWIND", R("RAB1"), show=RAB[:1])
P.cset("RAB1", "rab$w_rsz", 0xFFFF)
P.call("RMS.GET.FIX", "SYS$GET", R("RAB1"), show=RAB + [TPTR("RAB1", "rab$l_rbf", "rab$w_rsz", "rec", limit=64)])
P.do("SYS$CLOSE", R("FAB1"))

# 5. errors from $OPEN
fab("F_NONE")
P.call("RMS.OPEN.FNF", "SYS$OPEN", R("FAB1"), show=FAB + NAMP)
fab("F_NODIR")
P.call("RMS.OPEN.DNF", "SYS$OPEN", R("FAB1"), show=FAB)
fab("F_SYN")
P.call("RMS.OPEN.SYNTAX", "SYS$OPEN", R("FAB1"), show=FAB)
fab("F_EMPTY", "D_DAT")
P.call("RMS.OPEN.EMPTYNAME", "SYS$OPEN", R("FAB1"), show=FAB + NAMP)
fab("F_LOWER", "D_DAT")
P.call("RMS.OPEN.LOWERCASE", "SYS$OPEN", R("FAB1"), show=FAB[:1] + NAMP)
P.do("SYS$CLOSE", R("FAB1"))

# 6. $PARSE: default merging, explicit-field flags, odd syntax
for cid, fna, dna in [("RMS.PARSE.DEFAULTS", "F_X", "D_FULL"), ("RMS.PARSE.NODEFAULT", "F_X", None),
                      ("RMS.PARSE.DOT", "F_DOT", "D_FULL"), ("RMS.PARSE.SEMI", "F_SEMI", "D_FULL"),
                      ("RMS.PARSE.DEVONLY", "F_DEVONLY", "D_FULL"), ("RMS.PARSE.WILD", "F_WILD", None),
                      ("RMS.PARSE.LONGNAME", "F_LONGNAME", None), ("RMS.PARSE.BIGVER", "F_BIGVER", None),
                      ("RMS.PARSE.NEGVER", "F_NEGVER", None), ("RMS.PARSE.SYNTAX", "F_SYN", None),
                      ("RMS.PARSE.SYNTAX2", "F_SYN2", None), ("RMS.PARSE.LOWER", "F_LOWER", None)]:
    fab(fna, dna)
    P.call(cid, "SYS$PARSE", R("FAB1"), show=FAB + NAMP)

# 7. $SEARCH: a wildcard finds the versions, then NMF; a wildcard with no match
fab("F_WILD")
P.call("RMS.SEARCH.PARSE", "SYS$PARSE", R("FAB1"), show=FAB[:1])
for i in (1, 2, 3, 4):
    P.call("RMS.SEARCH.%d" % i, "SYS$SEARCH", R("FAB1"), show=FAB + NAMP)
fab("F_WILDNONE")
P.call("RMS.SEARCH.NONE.PARSE", "SYS$PARSE", R("FAB1"), show=FAB[:1])
P.call("RMS.SEARCH.NONE", "SYS$SEARCH", R("FAB1"), show=FAB)
fab("F_TEST1")
P.call("RMS.SEARCH.NOPARSE", "SYS$SEARCH", R("FAB1"), show=FAB)

# 8. $ERASE: each version, then again
for v in ("F_TEST1", "F_TEST2", "F_TEST3"):
    fab(v)
    P.call("RMS.ERASE.%s" % v[2:], "SYS$ERASE", R("FAB1"), show=FAB + NAMP)
fab("F_TEST1")
P.call("RMS.ERASE.AGAIN", "SYS$ERASE", R("FAB1"), show=FAB)
fab("F_FIX")
P.call("RMS.ERASE.FIX", "SYS$ERASE", R("FAB1"), show=FAB[:1])
fab("F_TEST", "D_DAT")
P.call("RMS.OPEN.AFTER_ERASE", "SYS$OPEN", R("FAB1"), show=FAB[:1])
P.do("SYS$CLOSE", R("FAB1"))
P.gosub("CLEANUP")
PROBE = P
