# Semantic-oracle spec: PRIVILEGE CHECK SITES -- ALTPRI, SYSLCK, OPER (rd vms-768).
# The probe disables every privilege it holds ($SETPRV), then asks for what each
# privilege guards, without and with that one privilege: $SETPRI above the
# authorized base priority (ALTPRI), a system-wide lock resource (LCK$M_SYSTEM,
# SYSLCK) -- also whether a system-wide resource is a different resource from a
# group one of the same name -- and a broadcast to every terminal ($BRKTHRUW
# BRK$C_ALLTERMS / ALLUSERS, OPER). Priorities print as numbers; lock IDs only
# as "is it zero".
P = Probe("privchk")
EX, NOQUEUE, SYSTEM = K("LCK$K_EXMODE"), K("LCK$M_NOQUEUE"), K("LCK$M_SYSTEM")
DEV, ALLT, ALLU = K("BRK$C_DEVICE"), K("BRK$C_ALLTERMS"), K("BRK$C_ALLUSERS")
GENERAL = K("BRK$C_GENERAL")


def privq(name, bit):
    b = [0] * 8
    b[bit // 8] = 1 << (bit % 8)
    P.buf(name, 8, text="".join(chr(c) for c in b))


P.buf("ALLPRV", 8, fill=0xFF); P.buf("PREVPRV", 8, fill=0)
privq("P_ALTPRI", K("PRV$V_ALTPRI"))
privq("P_SYSLCK", K("PRV$V_SYSLCK"))
privq("P_OPER", K("PRV$V_OPER"))
P.long("PRIB", 0); P.long("AUTHPRI", 0); P.long("PRV", 0xA5A5A5A5)
P.items("JPI_PRI", [(4, K("JPI$_PRIB"), "PRIB", None), (4, K("JPI$_AUTHPRI"), "AUTHPRI", None)])
P.quad("IOSB")
for n in ("LA", "LB", "LC", "LD"):
    P.buf(n, 24, fill=0)
P.long("ZERO", 0)
P.desc("RES", "SP_PRIVCHK_LOCK")
P.desc("MSG", "SP_PRIVCHK_MESSAGE")
P.desc("TT", "TT:")


def jpi(cid):
    P.setl("PRIB", 0); P.setl("AUTHPRI", 0)
    P.call(cid, "SYS$GETJPIW", 0, None, None, R("JPI_PRI"), R("IOSB"), None, 0,
           show=[U("PRIB", "prib"), U("AUTHPRI", "authpri")])


def setpri(cid, pri):
    P.setl("PRV", 0xA5A5A5A5)
    P.call(cid, "SYS$SETPRI", None, None, pri, R("PRV"), None, None, show=[U("PRV", "prev")])


def enqw(cid, lksb, flags):
    P.call(cid, "SYS$ENQW", 0, EX, R(lksb), flags, R("RES"), 0, None, 0, None, 0, 0,
           show=[UW(lksb, "lksb", 0), EQ(lksb, "ZERO", "lkid0", aoff=4)])


def brk(cid, to, typ):
    P.setl("IOSB", 0xA5A5A5A5, 0); P.setl("IOSB", 0xA5A5A5A5, 4)
    P.call(cid, "SYS$BRKTHRUW", 0, R("MSG"), None if to is None else R(to), typ, R("IOSB"),
           0x20, 0, GENERAL, 0, None, 0, show=[UW("IOSB", "iosb")])


P.call("PRV.OFF", "SYS$SETPRV", 0, R("ALLPRV"), 0, R("PREVPRV"))
# 1. ALTPRI
jpi("PRI.START")
setpri("PRI.LOWER", 2)
jpi("PRI.AFTER.LOWER")
setpri("PRI.RAISE.TO.AUTH", 4)
setpri("PRI.RAISE.ABOVE", 10)
jpi("PRI.AFTER.RAISE")
P.call("PRV.ALTPRI.ON", "SYS$SETPRV", 1, R("P_ALTPRI"), 0, None)
setpri("PRI.ALTPRI.RAISE", 10)
jpi("PRI.AFTER.ALTPRI")
setpri("PRI.RESTORE", 4)
P.call("PRV.ALTPRI.OFF", "SYS$SETPRV", 0, R("P_ALTPRI"), 0, None)
# 2. SYSLCK
enqw("LCK.GROUP.EX", "LA", 0)
enqw("LCK.SYSTEM.NOPRIV", "LB", SYSTEM | NOQUEUE)
P.call("PRV.SYSLCK.ON", "SYS$SETPRV", 1, R("P_SYSLCK"), 0, None)
enqw("LCK.SYSTEM.SYSLCK", "LC", SYSTEM | NOQUEUE)
enqw("LCK.GROUP.AGAIN.NOQUEUE", "LD", NOQUEUE)
P.call("PRV.SYSLCK.OFF", "SYS$SETPRV", 0, R("P_SYSLCK"), 0, None)
# 3. OPER
brk("BRK.OWN.NOPRIV", "TT", DEV)
brk("BRK.ALLTERMS.NOPRIV", None, ALLT)
brk("BRK.ALLUSERS.NOPRIV", None, ALLU)
P.call("PRV.OPER.ON", "SYS$SETPRV", 1, R("P_OPER"), 0, None)
brk("BRK.ALLTERMS.OPER", None, ALLT)
P.call("PRV.OPER.OFF", "SYS$SETPRV", 0, R("P_OPER"), 0, None)
PROBE = P
