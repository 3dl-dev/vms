# Semantic-oracle spec: who a mailbox belongs to, as the system reports it, and
# what a server may ask about a user's access to one (rd vms-046). $GETDVI's
# DVI$_PID / DVI$_OWNUIC / DVI$_VPROT / DVI$_REFCNT on a temporary mailbox (by
# channel and by name), and $CHECK_ACCESS (ACL$C_DEVICE) of SYSTEM and of the
# non-system DEFAULT account to a write-only (W:W) and an open mailbox.
P = Probe("mbxown")
P.long("MYPID", 0); P.long("MYUIC", 0); P.long("ZERO", 0)
P.items("JPI", [(4, K("JPI$_PID"), "MYPID", None), (4, K("JPI$_UIC"), "MYUIC", None)])
P.do("SYS$GETJPIW", 0, None, None, R("JPI"), None, None, 0)

P.word("CREQ", 0); P.word("COPEN", 0); P.word("A2", 0)
P.desc("NREQ", "SP_MBXO_REQ"); P.desc("NOPEN", "SP_MBXO_OPEN")
P.long("D_PID"); P.long("D_OWNUIC"); P.long("D_VPROT"); P.long("D_REFCNT")
P.items("DVI", [(4, K("DVI$_PID"), "D_PID", None), (4, K("DVI$_OWNUIC"), "D_OWNUIC", None),
                (4, K("DVI$_VPROT"), "D_VPROT", None), (4, K("DVI$_REFCNT"), "D_REFCNT", None)])
P.quad("IOSB")

DV = [EQ("D_PID", "MYPID", "pid=self"), EQ("D_PID", "ZERO", "pid=0"),
      EQ("D_OWNUIC", "MYUIC", "ownuic=self"), X("D_VPROT", "vprot"), U("D_REFCNT", "refcnt")]

with P.sub("RESET"):
    P.setl("D_PID", 0xA5A5A5A5); P.setl("D_OWNUIC", 0xA5A5A5A5)
    P.setl("D_VPROT", 0xA5A5A5A5); P.setl("D_REFCNT", 0xA5A5A5A5)

P.call("MBXO.CREMBX.REQ", "SYS$CREMBX", 0, R("CREQ"), 40, 400, 0xDF00, None, R("NREQ"), None)
P.call("MBXO.CREMBX.OPEN", "SYS$CREMBX", 0, R("COPEN"), 40, 400, 0, None, R("NOPEN"), None)

P.gosub("RESET")
P.call("MBXO.DVI.REQ.CHAN", "SYS$GETDVIW", 0, V("CREQ"), None, R("DVI"), R("IOSB"), None, 0, None,
       show=DV)
P.gosub("RESET")
P.call("MBXO.DVI.REQ.NAME", "SYS$GETDVIW", 0, 0, R("NREQ"), R("DVI"), R("IOSB"), None, 0, None,
       show=DV)
P.call("MBXO.ASSIGN.REQ", "SYS$ASSIGN", R("NREQ"), R("A2"), 0, None)
P.gosub("RESET")
P.call("MBXO.DVI.REQ.ASSIGNED", "SYS$GETDVIW", 0, V("A2"), None, R("DVI"), R("IOSB"), None, 0, None,
       show=DV)
P.gosub("RESET")
P.call("MBXO.DVI.OPEN.CHAN", "SYS$GETDVIW", 0, V("COPEN"), None, R("DVI"), R("IOSB"), None, 0, None,
       show=DV)

# $CHECK_ACCESS (a server asking about a USER's access): ACL$C_DEVICE objects,
# ARM$M_READ / ARM$M_WRITE wanted.
P.long("OT_DEV", K("ACL$C_DEVICE"))
P.long("ACC_R", K("ARM$M_READ")); P.long("ACC_W", K("ARM$M_WRITE"))
P.items("CHP_R", [(4, K("CHP$_ACCESS"), "ACC_R", None)])
P.items("CHP_W", [(4, K("CHP$_ACCESS"), "ACC_W", None)])
P.desc("U_SYS", "SYSTEM"); P.desc("U_DEF", "DEFAULT"); P.desc("U_NONE", "SP_NOSUCHUSER")
for obj in ("REQ", "OPEN"):
    for user in ("SYS", "DEF"):
        for acc in ("R", "W"):
            P.call("MBXO.CHKACC.%s.%s.%s" % (obj, user, acc), "SYS$CHECK_ACCESS", R("OT_DEV"),
                   R("N" + obj), R("U_" + user), R("CHP_" + acc), None, None, None, None)
P.call("MBXO.CHKACC.NOSUCHUSER", "SYS$CHECK_ACCESS", R("OT_DEV"), R("NREQ"), R("U_NONE"),
       R("CHP_R"), None, None, None, None)
P.absent_on_ovmx("SYS$CHECK_ACCESS")
PROBE = P
