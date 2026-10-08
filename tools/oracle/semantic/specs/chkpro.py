# Semantic-oracle spec: $CHKPRO and $CREATE_USER_PROFILE (rd vms-d404).
# What $CHKPRO decides for an object described by the item list (CHP$_OWNER,
# CHP$_PROT, CHP$_ACL) and a subject that is either a user profile from
# $CREATE_USER_PROFILE (the DEFAULT account, [200,200], no privileges that
# matter here) or the calling process (SYSTEM, [1,4], with and without its
# privileges): the four protection categories, read/write/execute/delete/
# control, an identifier ACE that grants, one that denies (group/world vs
# owner/system), a DEFAULT ACE, a wildcard ACE, and what CHP$_UIC / CHP$_PRIV /
# CHP$_FLAGS change. A profile's bytes are opaque and differ between systems:
# only the statuses are shown.
P = Probe("chkpro")
RD, WR, EX, DL, CT = (K("ARM$M_READ"), K("ARM$M_WRITE"), K("ARM$M_EXECUTE"),
                      K("ARM$M_DELETE"), K("ARM$M_CONTROL"))
OBSERVE, ALTER = K("CHP$M_OBSERVE"), K("CHP$M_ALTER")
SYSPRV_BIT, BYPASS_BIT, READALL_BIT = K("PRV$V_SYSPRV"), K("PRV$V_BYPASS"), K("PRV$V_READALL")

DEFAULT_UIC = 0o200 << 16 | 0o200
SYS_UIC = 1 << 16 | 4
OTHER_UIC = 0o300 << 16 | 0o1          # a UIC in neither subject's group
GROUP_UIC = 0o200 << 16 | 0o1          # DEFAULT's group, another member


def ace(ident, access, flags=0):
    b = bytes([12, 1, flags & 0xFF, flags >> 8]) + access.to_bytes(4, "little") + ident.to_bytes(4, "little")
    return "".join(chr(c) for c in b)


for n, t in [("N_DEFAULT", "DEFAULT"), ("N_NONE", "SP_NO_SUCH_USER"), ("N_EMPTY", "")]:
    P.desc(n, t)
P.long("ACC", 0); P.long("FLG", 0); P.long("OWN", 0); P.long("PROT", 0); P.long("UICV", 0)
P.buf("PRIVQ", 8, fill=0)
P.buf("UPRO", 4096, fill=0)
P.bdesc("UPROD", "UPRO", 4096)      # its first longword receives the profile length
P.buf("ALLPRV", 8, fill=0xFF); P.buf("PREVPRV", 8, fill=0)

ACLS = {
    "A_DEFREAD": ace(DEFAULT_UIC, RD),                       # [200,200] READ
    "A_DEFNONE": ace(DEFAULT_UIC, 0),                        # [200,200] NONE
    "A_GRPNONE": ace(0o200 << 16 | 0xFFFF, 0),               # [200,*] NONE
    "A_WILDRW": ace(0x3FFFFFFF, RD | WR),                    # [*,*] READ+WRITE
    "A_OTHER": ace(OTHER_UIC, 0),                            # [300,1] NONE (no match)
    "A_DEFLT": ace(DEFAULT_UIC, RD, flags=0x0100),  # ACE$M_DEFAULT (docs/oracle/vax73-acl/acedef.txt)
    "A_CTRL": ace(DEFAULT_UIC, CT),                          # [200,200] CONTROL
    "A_SYSNONE": ace(SYS_UIC, 0),                            # [1,4] NONE
}
for n, t in ACLS.items():
    P.buf(n, len(t), text=t)

BASE = [(4, K("CHP$_ACCESS"), "ACC", None), (4, K("CHP$_FLAGS"), "FLG", None),
        (4, K("CHP$_OWNER"), "OWN", None), (4, K("CHP$_PROT"), "PROT", None)]
P.items("IT_NOACL", BASE)
for n in ACLS:
    P.items("IT" + n[1:], BASE + [(12, K("CHP$_ACL"), n, None)])
P.items("IT_UIC", BASE + [(4, K("CHP$_UIC"), "UICV", None)])
P.items("IT_PRIV", BASE + [(8, K("CHP$_PRIV"), "PRIVQ", None)])
P.items("IT_UICPRIV", BASE + [(4, K("CHP$_UIC"), "UICV", None), (8, K("CHP$_PRIV"), "PRIVQ", None)])
P.items("IT_EMPTY", [])

# --- 1. user profiles --------------------------------------------------------
P.setl("UPROD", 0)
P.call("CUP.LENGTH", "SYS$CREATE_USER_PROFILE", R("N_DEFAULT"), None, 0, None, R("UPROD"), None)
P.call("CUP.DEFAULT", "SYS$CREATE_USER_PROFILE", R("N_DEFAULT"), None, 0, R("UPRO"), R("UPROD"), None)
P.setl("PROT", 0)
P.call("CUP.NOSUCH", "SYS$CREATE_USER_PROFILE", R("N_NONE"), None, 0, R("UPRO"), R("PROT"), None)
P.setl("PROT", 0)
P.call("CUP.EMPTYNAME", "SYS$CREATE_USER_PROFILE", R("N_EMPTY"), None, 0, R("UPRO"), R("PROT"), None)


def chk(cid, items, acc, own, prot, flg=0, subj=R("UPROD")):
    P.setl("ACC", acc); P.setl("OWN", own); P.setl("PROT", prot); P.setl("FLG", flg)
    P.call(cid, "SYS$CHKPRO", R(items), None, subj)


# --- 2. DEFAULT [200,200]: the protection code -------------------------------
chk("UP.WORLD.NONE.READ", "IT_NOACL", RD, SYS_UIC, 0xFF00)        # S:RWED,O:RWED,G,W
chk("UP.WORLD.ALL.READ", "IT_NOACL", RD, SYS_UIC, 0x0000)
chk("UP.WORLD.R.READ", "IT_NOACL", RD, SYS_UIC, 0xEF00)           # W:R
chk("UP.WORLD.R.WRITE", "IT_NOACL", WR, SYS_UIC, 0xEF00)
chk("UP.WORLD.R.RW", "IT_NOACL", RD | WR, SYS_UIC, 0xEF00)
chk("UP.WORLD.E.EXECUTE", "IT_NOACL", EX, SYS_UIC, 0xBF00)        # W:E
chk("UP.WORLD.D.DELETE", "IT_NOACL", DL, SYS_UIC, 0x7F00)         # W:D
chk("UP.GROUP.R.READ", "IT_NOACL", RD, GROUP_UIC, 0xFE00)         # G:R
chk("UP.OWNER.RWED.READ", "IT_NOACL", RD, DEFAULT_UIC, 0xFF0F)    # S,O:RWED,G,W
chk("UP.OWNER.NONE.READ", "IT_NOACL", RD, DEFAULT_UIC, 0x00F0)    # S:RWED,O,G:RWED,W:RWED
chk("UP.CONTROL.OWNER", "IT_NOACL", CT, DEFAULT_UIC, 0xFFFF)
chk("UP.CONTROL.WORLD", "IT_NOACL", CT, SYS_UIC, 0x0000)
chk("UP.CONTROL.GROUP", "IT_NOACL", CT, GROUP_UIC, 0x0000)
chk("UP.CONTROL.WORLD.NONE", "IT_NOACL", CT, SYS_UIC, 0xFF00)
chk("UP.CONTROL.WORLD.R", "IT_NOACL", CT, SYS_UIC, 0xEF00)
chk("UP.CONTROL.WORLD.W", "IT_NOACL", CT, SYS_UIC, 0xDF00)
chk("UP.CONTROL.WORLD.RWE", "IT_NOACL", CT, SYS_UIC, 0x8F00)
chk("UP.CONTROL.WORLD.D", "IT_NOACL", CT, SYS_UIC, 0x7F00)
chk("UP.CONTROL.GROUP.NONE", "IT_NOACL", CT, GROUP_UIC, 0x0F00)
chk("UP.CONTROL.GROUP.W", "IT_NOACL", CT, GROUP_UIC, 0xFD00)
chk("UP.CONTROL.OWNER.NONE", "IT_NOACL", CT, DEFAULT_UIC, 0x00F0)
chk("UP.CONTROL.RW", "IT_NOACL", CT | RD, SYS_UIC, 0xEF00)
chk("UP.ZERO.ACCESS", "IT_NOACL", 0, SYS_UIC, 0xFFFF)
chk("UP.OBSERVE.READ", "IT_NOACL", RD, SYS_UIC, 0xEF00, flg=OBSERVE)
chk("UP.ALTER.WRITE", "IT_NOACL", WR, SYS_UIC, 0xEF00, flg=ALTER)

# --- 3. DEFAULT [200,200]: the ACL -------------------------------------------
chk("UP.ACL.GRANT", "IT_DEFREAD", RD, SYS_UIC, 0xFF00)
chk("UP.ACL.GRANT.WRITE", "IT_DEFREAD", WR, SYS_UIC, 0xFF00)
chk("UP.ACL.DENY.WORLD", "IT_DEFNONE", RD, SYS_UIC, 0x0000)
chk("UP.ACL.DENY.GROUP", "IT_GRPNONE", RD, GROUP_UIC, 0x0000)
chk("UP.ACL.DENY.OWNER", "IT_DEFNONE", RD, DEFAULT_UIC, 0xFF0F)
chk("UP.ACL.WILD", "IT_WILDRW", RD | WR, SYS_UIC, 0xFF00)
chk("UP.ACL.NOMATCH", "IT_OTHER", RD, SYS_UIC, 0xEF00)
chk("UP.ACL.DEFAULTACE", "IT_DEFLT", RD, SYS_UIC, 0xFF00)
chk("UP.ACL.CONTROL", "IT_CTRL", CT, SYS_UIC, 0xFF00)
chk("UP.ACL.CONTROL.DENY", "IT_DEFNONE", CT, SYS_UIC, 0x0000)
chk("UP.ACL.CONTROL.READACE", "IT_DEFREAD", CT, SYS_UIC, 0x0000)
chk("UP.ACL.PARTIAL", "IT_DEFREAD", RD | WR, SYS_UIC, 0x0000)
chk("UP.ACL.PARTIAL.OWNER", "IT_DEFREAD", RD | WR, DEFAULT_UIC, 0xFF0F)

# --- 4. the calling process: SYSTEM [1,4] -------------------------------------
chk("ME.ALLPRV.NONE", "IT_NOACL", RD | WR, OTHER_UIC, 0xFFFF, subj=None)
chk("ME.ALLPRV.ACLDENY", "IT_SYSNONE", RD, OTHER_UIC, 0xFFFF, subj=None)
P.call("ME.SETPRV.OFF", "SYS$SETPRV", 0, R("ALLPRV"), 0, R("PREVPRV"))
chk("ME.NOPRV.NONE", "IT_NOACL", RD, OTHER_UIC, 0xFFFF, subj=None)
chk("ME.NOPRV.SYSTEM", "IT_NOACL", RD | WR, OTHER_UIC, 0xFFF0, subj=None)   # S:RWED only
chk("ME.NOPRV.SYSTEM.ACLDENY", "IT_SYSNONE", RD, OTHER_UIC, 0xFFF0, subj=None)
chk("ME.NOPRV.CONTROL.SYSTEM", "IT_NOACL", CT, OTHER_UIC, 0xFFF0, subj=None)
chk("ME.NOPRV.OWNER", "IT_NOACL", RD, SYS_UIC, 0xFF0F, subj=None)
# CHP$_UIC / CHP$_PRIV in the item list, the calling process as subject
P.setl("UICV", DEFAULT_UIC)
chk("ME.UIC.DEFAULT.WORLDNONE", "IT_UIC", RD, SYS_UIC, 0xFF00, subj=None)
chk("ME.UIC.DEFAULT.OWNER", "IT_UIC", RD, DEFAULT_UIC, 0xFF0F, subj=None)
P.fill("PRIVQ", byte=0)
chk("ME.PRIV.ZERO", "IT_PRIV", RD, OTHER_UIC, 0xFFF0, subj=None)
P.fill("PRIVQ", byte=0); P.setl("PRIVQ", 1 << BYPASS_BIT)
chk("ME.PRIV.BYPASS", "IT_PRIV", RD | WR, OTHER_UIC, 0xFFFF, subj=None)
P.fill("PRIVQ", byte=0); P.setl("PRIVQ", 1 << SYSPRV_BIT)
chk("ME.PRIV.SYSPRV", "IT_PRIV", RD, OTHER_UIC, 0xFFF0, subj=None)
chk("ME.PRIV.SYSPRV.NOSYSFIELD", "IT_PRIV", RD, OTHER_UIC, 0xFFFF, subj=None)
# READALL is bit 35: the high longword
P.fill("PRIVQ", text="\0\0\0\0" + chr(1 << (READALL_BIT - 32)) + "\0\0\0")
chk("ME.PRIV.READALL.READ", "IT_PRIV", RD, OTHER_UIC, 0xFFFF, subj=None)
chk("ME.PRIV.READALL.WRITE", "IT_PRIV", WR, OTHER_UIC, 0xFFFF, subj=None)
chk("ME.PRIV.READALL.OBSERVE", "IT_PRIV", RD, OTHER_UIC, 0xFFFF, flg=OBSERVE, subj=None)
chk("ME.PRIV.READALL.USEREADALL", "IT_PRIV", RD, OTHER_UIC, 0xFFFF, flg=K("CHP$M_USEREADALL"), subj=None)
chk("ME.PRIV.READALL.USEREADALL.WRITE", "IT_PRIV", WR, OTHER_UIC, 0xFFFF, flg=K("CHP$M_USEREADALL"), subj=None)
chk("ME.PRIV.READALL.USEREADALL.EXECUTE", "IT_PRIV", EX, OTHER_UIC, 0xFFFF, flg=K("CHP$M_USEREADALL"), subj=None)
chk("ME.PRIV.READALL.USEREADALL.ACLDENY", "IT_SYSNONE", RD, OTHER_UIC, 0xFFFF, flg=K("CHP$M_USEREADALL"), subj=None)
# GRPPRV: the system category for an object of the subject's own group
P.setl("UICV", DEFAULT_UIC)
P.fill("PRIVQ", byte=0)
P.fill("PRIVQ", text="\0\0\0\0" + chr(1 << (K("PRV$V_GRPPRV") - 32)) + "\0\0\0")
chk("ME.PRIV.GRPPRV.OWNGROUP", "IT_UICPRIV", RD, GROUP_UIC, 0xFFF0, subj=None)
chk("ME.PRIV.GRPPRV.OTHERGROUP", "IT_UICPRIV", RD, OTHER_UIC, 0xFFF0, subj=None)
P.fill("PRIVQ", byte=0)
chk("ME.UICPRIV.ZERO.OWNGROUP", "IT_UICPRIV", RD, GROUP_UIC, 0xFFF0, subj=None)
# a profile still answers for its user after the caller dropped its privileges
chk("UP.AFTER.SETPRV", "IT_NOACL", RD, SYS_UIC, 0xEF00)
P.call("CHKPRO.EMPTY.ITEMS", "SYS$CHKPRO", R("IT_EMPTY"), None, None)
P.call("CHKPRO.NO.ITEMS", "SYS$CHKPRO", None, None, None)
PROBE = P
