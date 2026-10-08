# Semantic-oracle spec: RIGHTS DATABASE services (rd vms-8d1, for vms-7d5a).
# $ADD_IDENT $REM_IDENT $ADD_HOLDER $REM_HOLDER $ASCTOID $IDTOASC $FIND_HELD
# $FIND_HOLDER $FINISH_RDB $GRANTID $REVOKID: argument shapes and condition
# values for success, duplicate, missing and no-privilege. The probe makes its
# own identifiers -- two UIC identifiers to hold things ([355,355], [355,356]) and
# a general identifier -- so every list it walks is its own, and removes them
# before and after. A general identifier's VALUE is assigned by the system, so
# only its form (bits 31/30) and relations to it are shown.
P = Probe("rights")
RESOURCE, DYNAMIC = K("KGB$M_RESOURCE"), K("KGB$M_DYNAMIC")
UIC1, UIC2, UICX = 0o355 << 16 | 0o355, 0o355 << 16 | 0o356, 0o355 << 16 | 0o357

for n, t in [("N_GEN", "SP_RIGHTS_ID"), ("N_GEN2", "SP_RIGHTS_ID2"), ("N_UIC1", "SP_RIGHTS_U1"),
             ("N_UIC2", "SP_RIGHTS_U2"), ("N_NONE", "SP_RIGHTS_NO_SUCH"), ("N_BAD", "1BAD"),
             ("N_EMPTY", ""), ("N_LONG", "R" * 32), ("N_SYSTEM", "SYSTEM")]:
    P.desc(n, t)
P.long("ID", 0)          # the general identifier the system assigns
P.long("ID2", 0)
P.long("TMP", 0)
P.long("ATTR", 0xA5A5A5A5)
P.long("CTX", 0)
P.long("XID", 0xA5A5A5A5)
P.buf("H1", 8); P.buf("H2", 8); P.buf("HX", 8); P.buf("HOUT", 8)
P.buf("IDQ", 8)          # {id, attributes} for $GRANTID / $REVOKID
P.buf("NAMBUF", 64); P.word("NAMLEN", 0xFFFF); P.bdesc("NAMDSC", "NAMBUF", 64)
P.buf("ALLPRV", 8, fill=0xFF); P.buf("PREVPRV", 8, fill=0)
P.long("NOSUCH", 0x8FFFFFF0)


# --- clean up anything an earlier run left (statuses ignored) ---------------
with P.sub("CLEANUP"):
    for n in ("N_GEN", "N_GEN2", "N_UIC1", "N_UIC2", "N_BAD", "N_LONG"):
        P.setl("TMP", 0)
        P.do("SYS$ASCTOID", R(n), R("TMP"), None)
        P.do("SYS$REM_IDENT", V("TMP"))
P.gosub("CLEANUP")

# holder quadwords: {UIC, 0}
for b, u in (("H1", UIC1), ("H2", UIC2), ("HX", UICX)):
    P.fill(b, byte=0)
    P.setl(b, u)

# 1. identifiers
P.call("RDB.ADD_IDENT.UIC1", "SYS$ADD_IDENT", R("N_UIC1"), UIC1, 0, R("TMP"), show=[X("TMP", "resid")])
P.call("RDB.ADD_IDENT.UIC2", "SYS$ADD_IDENT", R("N_UIC2"), UIC2, 0, None)
P.setl("ID", 0)
P.call("RDB.ADD_IDENT.GENERAL", "SYS$ADD_IDENT", R("N_GEN"), 0, RESOURCE, R("ID"),
       show=[BIT("ID", 31, "general"), BIT("ID", 30, "bit30")])
P.call("RDB.ADD_IDENT.DUP_NAME", "SYS$ADD_IDENT", R("N_GEN"), 0, 0, R("TMP"))
P.call("RDB.ADD_IDENT.DUP_VALUE", "SYS$ADD_IDENT", R("N_GEN2"), V("ID"), 0, R("TMP"))
P.call("RDB.ADD_IDENT.DUP_UIC", "SYS$ADD_IDENT", R("N_GEN2"), UIC1, 0, R("TMP"))
P.call("RDB.ADD_IDENT.BADNAME", "SYS$ADD_IDENT", R("N_BAD"), 0, 0, R("TMP"))
P.call("RDB.ADD_IDENT.EMPTYNAME", "SYS$ADD_IDENT", R("N_EMPTY"), 0, 0, R("TMP"))
P.call("RDB.ADD_IDENT.LONGNAME", "SYS$ADD_IDENT", R("N_LONG"), 0, 0, R("TMP"))
P.setl("ID2", 0)
P.call("RDB.ADD_IDENT.GENERAL2", "SYS$ADD_IDENT", R("N_GEN2"), 0, DYNAMIC, R("ID2"),
       show=[BIT("ID2", 31, "general")])
P.setl("TMP", 0)
P.show("RDB.GENERAL.NEXT_VALUE", EQ("ID2", "ID", "same"))

# 2. name <-> value
P.setl("XID", 0xA5A5A5A5); P.setl("ATTR", 0xA5A5A5A5)
P.call("RDB.ASCTOID.GENERAL", "SYS$ASCTOID", R("N_GEN"), R("XID"), R("ATTR"),
       show=[EQ("XID", "ID", "id=added"), X("ATTR", "attr")])
P.setl("XID", 0xA5A5A5A5); P.setl("ATTR", 0xA5A5A5A5)
P.call("RDB.ASCTOID.UIC", "SYS$ASCTOID", R("N_UIC1"), R("XID"), R("ATTR"), show=[X("XID", "id"), X("ATTR", "attr")])
P.call("RDB.ASCTOID.NONE", "SYS$ASCTOID", R("N_NONE"), R("XID"), R("ATTR"))
P.setw("NAMLEN", 0xFFFF); P.setl("ATTR", 0xA5A5A5A5)
P.call("RDB.IDTOASC.GENERAL", "SYS$IDTOASC", V("ID"), R("NAMLEN"), R("NAMDSC"), None, R("ATTR"), None,
       show=[UW("NAMLEN", "len"), T("NAMBUF", "NAMLEN", label="name", limit=64), X("ATTR", "attr")])
P.setw("NAMLEN", 0xFFFF)
P.call("RDB.IDTOASC.UIC", "SYS$IDTOASC", UIC2, R("NAMLEN"), R("NAMDSC"), None, None, None,
       show=[T("NAMBUF", "NAMLEN", label="name", limit=64)])
P.call("RDB.IDTOASC.NONE", "SYS$IDTOASC", V("NOSUCH"), R("NAMLEN"), R("NAMDSC"), None, None, None)

# 3. holders: one identifier, two holders
P.call("RDB.ADD_HOLDER.1", "SYS$ADD_HOLDER", V("ID"), R("H1"), 0)
P.call("RDB.ADD_HOLDER.2", "SYS$ADD_HOLDER", V("ID"), R("H2"), RESOURCE)
P.call("RDB.ADD_HOLDER.DUP", "SYS$ADD_HOLDER", V("ID"), R("H1"), 0)
P.call("RDB.ADD_HOLDER.NOSUCH_HOLDER", "SYS$ADD_HOLDER", V("ID"), R("HX"), 0)
P.call("RDB.ADD_HOLDER.NOSUCH_ID", "SYS$ADD_HOLDER", V("NOSUCH"), R("H1"), 0)
P.call("RDB.ADD_HOLDER.ID2", "SYS$ADD_HOLDER", V("ID2"), R("H1"), 0)

# 4. walks: who holds ID (in the order the database returns them), what H1 holds
P.setl("CTX", 0)
for i in (1, 2, 3):
    P.fill("HOUT", byte=0xEE); P.setl("ATTR", 0xA5A5A5A5)
    P.call("RDB.FIND_HOLDER.%d" % i, "SYS$FIND_HOLDER", V("ID"), R("HOUT"), R("ATTR"), R("CTX"),
           show=[X("HOUT", "holder"), X("HOUT", "holder_hi", off=4), X("ATTR", "attr")])
P.call("RDB.FINISH_RDB.HOLDER", "SYS$FINISH_RDB", R("CTX"), show=[X("CTX", "ctx")])
P.setl("CTX", 0)
for i in (1, 2, 3):
    P.setl("XID", 0xA5A5A5A5); P.setl("ATTR", 0xA5A5A5A5)
    P.call("RDB.FIND_HELD.%d" % i, "SYS$FIND_HELD", R("H1"), R("XID"), R("ATTR"), R("CTX"),
           show=[EQ("XID", "ID", "id=ID"), EQ("XID", "ID2", "id=ID2"), X("ATTR", "attr")])
P.call("RDB.FINISH_RDB.HELD", "SYS$FINISH_RDB", R("CTX"), show=[X("CTX", "ctx")])
P.setl("CTX", 0)
P.call("RDB.FIND_HELD.H2", "SYS$FIND_HELD", R("H2"), R("XID"), R("ATTR"), R("CTX"),
       show=[EQ("XID", "ID", "id=ID")])
P.do("SYS$FINISH_RDB", R("CTX"))
P.setl("CTX", 0)
P.call("RDB.FIND_HOLDER.NOSUCH_ID", "SYS$FIND_HOLDER", V("NOSUCH"), R("HOUT"), R("ATTR"), R("CTX"))
P.do("SYS$FINISH_RDB", R("CTX"))
P.setl("CTX", 0)
P.call("RDB.FINISH_RDB.ZERO", "SYS$FINISH_RDB", R("CTX"))

# 5. grant to / revoke from this process
P.fill("IDQ", byte=0)
P.copyl("IDQ", "ID")
P.call("RDB.GRANTID.BY_ID", "SYS$GRANTID", None, None, R("IDQ"), None, None)
P.call("RDB.GRANTID.AGAIN", "SYS$GRANTID", None, None, R("IDQ"), None, None)
P.call("RDB.REVOKID.BY_ID", "SYS$REVOKID", None, None, R("IDQ"), None, None)
P.call("RDB.REVOKID.AGAIN", "SYS$REVOKID", None, None, R("IDQ"), None, None)
P.call("RDB.GRANTID.BY_NAME", "SYS$GRANTID", None, None, None, R("N_GEN"), None)
P.call("RDB.REVOKID.BY_NAME", "SYS$REVOKID", None, None, None, R("N_GEN"), None)
P.call("RDB.GRANTID.NONAME", "SYS$GRANTID", None, None, None, R("N_NONE"), None)

# 6. removal
P.call("RDB.REM_HOLDER", "SYS$REM_HOLDER", V("ID"), R("H1"))
P.call("RDB.REM_HOLDER.AGAIN", "SYS$REM_HOLDER", V("ID"), R("H1"))
P.call("RDB.REM_HOLDER.NOSUCH_ID", "SYS$REM_HOLDER", V("NOSUCH"), R("H1"))
P.call("RDB.REM_IDENT", "SYS$REM_IDENT", V("ID"))
P.call("RDB.REM_IDENT.AGAIN", "SYS$REM_IDENT", V("ID"))
P.setl("CTX", 0)
P.call("RDB.FIND_HOLDER.AFTER_REM", "SYS$FIND_HOLDER", V("ID"), R("HOUT"), R("ATTR"), R("CTX"))
P.do("SYS$FINISH_RDB", R("CTX"))
P.setl("CTX", 0)
P.setl("XID", 0xA5A5A5A5)
P.call("RDB.FIND_HELD.AFTER_REM", "SYS$FIND_HELD", R("H1"), R("XID"), R("ATTR"), R("CTX"),
       show=[EQ("XID", "ID2", "id=ID2")])
P.do("SYS$FINISH_RDB", R("CTX"))
# 7. with every privilege disabled for this image (the image runs as SYSTEM, whose
#    system UIC still gives it RIGHTSLIST access by file protection; $GRANTID needs
#    CMKRNL). Works on the second identifier and holder, which still exist.
P.call("RDB.SETPRV.OFF", "SYS$SETPRV", 0, R("ALLPRV"), 0, R("PREVPRV"))
P.copyl("IDQ", "ID2")
P.call("RDB.NOPRIV.GRANTID", "SYS$GRANTID", None, None, R("IDQ"), None, None)
P.call("RDB.NOPRIV.ADD_IDENT", "SYS$ADD_IDENT", R("N_GEN2"), 0, 0, R("TMP"))
P.call("RDB.NOPRIV.ADD_HOLDER", "SYS$ADD_HOLDER", V("ID2"), R("H2"), 0)
P.call("RDB.NOPRIV.REM_HOLDER", "SYS$REM_HOLDER", V("ID2"), R("H1"))
P.call("RDB.NOPRIV.REM_IDENT", "SYS$REM_IDENT", V("ID2"))
P.setl("XID", 0xA5A5A5A5)
P.call("RDB.NOPRIV.ASCTOID", "SYS$ASCTOID", R("N_UIC1"), R("XID"), None, show=[X("XID", "id")])
P.setl("CTX", 0)
P.call("RDB.NOPRIV.FIND_HELD", "SYS$FIND_HELD", R("H1"), R("XID"), R("ATTR"), R("CTX"))
P.do("SYS$FINISH_RDB", R("CTX"))
P.call("RDB.SETPRV.ON", "SYS$SETPRV", 1, R("PREVPRV"), 0, None)

P.gosub("CLEANUP")
P.setl("XID", 0xA5A5A5A5)
P.call("RDB.ASCTOID.AFTER_CLEANUP", "SYS$ASCTOID", R("N_UIC1"), R("XID"), None)
PROBE = P
