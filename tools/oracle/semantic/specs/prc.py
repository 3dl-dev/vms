# Semantic-oracle spec: PROCESS CREATION AND CONTROL -- $CREPRC / $DELPRC /
# $SUSPND / $RESUME / $WAKE / $FORCEX and the process state $GETJPI reports
# (rd vms-8d1). A subprocess created hibernating (PRC$M_HIBER: it hibernates
# before activating its image) is inspected, suspended, resumed and deleted; a
# second one is woken into an image that does not exist and so goes away by
# itself. Duplicate and over-long process names, and every control service on
# a PID that does not exist. PIDs differ between systems: only relations print.
# Waits are 1 s timers ($SETIMR + $WAITFR on EF 5), so each GETJPI after a
# control call sees the state the target settled into, not a race.
P = Probe("prc")
HIBER = K("PRC$M_HIBER")

P.long("SELF", 0)
P.long("SELFMASTER", 0)
P.long("SUB1", 0)
P.long("SUB2", 0)
P.long("SUB3", 0)
P.long("BOGUS", 0x00FFFFF0)
P.quad("IOSB")
P.quad("DELTA")
for n in ("J_STATE", "J_OWNER", "J_MPID", "J_PRIB", "J_PID", "J_PRCCNT", "J_MODE", "J_GRP", "J_STS"):
    P.long(n, 0xA5A5A5A5)
P.buf("J_NAME", 15)
P.word("J_NAMEL", 0xFFFF)
P.desc("IMG_HIB", "SYS$SYSTEM:LOGINOUT.EXE")
P.desc("IMG_NONE", "SYS$SYSTEM:SP_NO_SUCH_IMAGE.EXE")
P.desc("NAME1", "SP_PRC_ONE")
P.desc("NAME2", "SP_PRC_TWO")
P.desc("NAME3", "SP_PRC_THREE")
P.desc("NAMELONG", "SP_PRC_SIXTEEN_C")
P.desc("NAMEBOGUS", "SP_PRC_NOBODY")

P.items("JPI_SELF", [(4, K("JPI$_PID"), "SELF", None), (4, K("JPI$_MASTER_PID"), "SELFMASTER", None)])
P.items("JPI_SUB", [(4, K("JPI$_PID"), "J_PID", None), (4, K("JPI$_STATE"), "J_STATE", None),
                    (4, K("JPI$_OWNER"), "J_OWNER", None), (4, K("JPI$_MASTER_PID"), "J_MPID", None),
                    (4, K("JPI$_PRIB"), "J_PRIB", None), (4, K("JPI$_MODE"), "J_MODE", None),
                    (15, K("JPI$_PRCNAM"), "J_NAME", "J_NAMEL")])
P.items("JPI_STATE", [(4, K("JPI$_STATE"), "J_STATE", None)])
P.items("JPI_CNT", [(4, K("JPI$_PRCCNT"), "J_PRCCNT", None)])

# 1 s delta time: -10,000,000 100-ns units
P.setl("DELTA", 0xFF676980, 0)
P.setl("DELTA", 0xFFFFFFFF, 4)


def wait1():
    P.do("SYS$SETIMR", 5, R("DELTA"), None, 0, 0)
    P.do("SYS$WAITFR", 5)


def creprc(cid, pid, img, name, flags, pri=4):
    # pidadr image input output error prvadr quota prcnam baspri uic mbxunt
    # stsflg itmlst node home_dir -- all 15 (fewer can be SS$_INSFARG)
    P.call(cid, "SYS$CREPRC", R(pid), R(img), None, None, None, None, None, R(name), pri, 0, 0,
           flags, 0, 0, 0, show=[EQ(pid, "ZERO4", "pid0")])


P.long("ZERO4", 0)
P.do("SYS$GETJPIW", 0, None, None, R("JPI_SELF"), R("IOSB"), None, 0)
P.call("PRC.SELF.COUNT0", "SYS$GETJPIW", 0, None, None, R("JPI_CNT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_PRCCNT", "prccnt")])

# 1. create a hibernating subprocess and look at it
creprc("PRC.CREATE.HIBER", "SUB1", "IMG_HIB", "NAME1", HIBER)
wait1()
P.call("PRC.JPI.SUB", "SYS$GETJPIW", 0, R("SUB1"), None, R("JPI_SUB"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("J_PID", "SUB1", "pid=sub"), U("J_STATE", "state"),
             EQ("J_OWNER", "SELF", "owner=self"), EQ("J_MPID", "SELFMASTER", "master=mine"),
             U("J_PRIB", "prib"), U("J_MODE", "mode"), UW("J_NAMEL", "namel"),
             T("J_NAME", "J_NAMEL", label="name", limit=15)])
P.setl("J_PID", 0)
P.call("PRC.JPI.BYNAME", "SYS$GETJPIW", 0, None, R("NAME1"), R("JPI_SUB"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), EQ("J_PID", "SUB1", "pid=sub")])
P.call("PRC.SELF.COUNT1", "SYS$GETJPIW", 0, None, None, R("JPI_CNT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_PRCCNT", "prccnt")])

# 2. names
creprc("PRC.CREATE.DUPNAME", "SUB3", "IMG_HIB", "NAME1", HIBER)
creprc("PRC.CREATE.LONGNAME", "SUB3", "IMG_HIB", "NAMELONG", HIBER)

# 3. suspend / resume / wake / force-exit a hibernating process
P.call("PRC.SUSPND", "SYS$SUSPND", R("SUB1"), None, 0)
wait1()
P.call("PRC.SUSPND.STATE", "SYS$GETJPIW", 0, R("SUB1"), None, R("JPI_STATE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_STATE", "state")])
P.call("PRC.SUSPND.AGAIN", "SYS$SUSPND", R("SUB1"), None, 0)
P.call("PRC.RESUME", "SYS$RESUME", R("SUB1"), None)
wait1()
P.call("PRC.RESUME.STATE", "SYS$GETJPIW", 0, R("SUB1"), None, R("JPI_STATE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_STATE", "state")])
P.call("PRC.RESUME.AGAIN", "SYS$RESUME", R("SUB1"), None)

# 4. delete it
P.call("PRC.DELPRC", "SYS$DELPRC", R("SUB1"), None)
wait1()
P.call("PRC.DELPRC.GONE", "SYS$GETJPIW", 0, R("SUB1"), None, R("JPI_STATE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])
P.call("PRC.DELPRC.AGAIN", "SYS$DELPRC", R("SUB1"), None)
P.call("PRC.SELF.COUNT2", "SYS$GETJPIW", 0, None, None, R("JPI_CNT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_PRCCNT", "prccnt")])

# 5. the name is free again once its owner is gone
creprc("PRC.CREATE.REUSE", "SUB3", "IMG_HIB", "NAME1", HIBER)
P.call("PRC.FORCEX", "SYS$FORCEX", R("SUB3"), None, 0x2C)
wait1()
P.call("PRC.DELPRC.REUSE", "SYS$DELPRC", R("SUB3"), None)
wait1()

# 6. woken into an image that does not exist: it runs down on its own
creprc("PRC.CREATE.NOIMAGE", "SUB2", "IMG_NONE", "NAME2", HIBER)
wait1()
P.call("PRC.NOIMAGE.STATE", "SYS$GETJPIW", 0, R("SUB2"), None, R("JPI_STATE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_STATE", "state")])
P.call("PRC.WAKE", "SYS$WAKE", R("SUB2"), None)
wait1()
wait1()
P.call("PRC.NOIMAGE.GONE", "SYS$GETJPIW", 0, R("SUB2"), None, R("JPI_STATE"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb")])

# 7. every control service on a PID / name that names no process
P.call("PRC.BOGUS.DELPRC", "SYS$DELPRC", R("BOGUS"), None)
P.call("PRC.BOGUS.SUSPND", "SYS$SUSPND", R("BOGUS"), None, 0)
P.call("PRC.BOGUS.RESUME", "SYS$RESUME", R("BOGUS"), None)
P.call("PRC.BOGUS.WAKE", "SYS$WAKE", R("BOGUS"), None)
P.call("PRC.BOGUS.FORCEX", "SYS$FORCEX", R("BOGUS"), None, 0x2C)
P.call("PRC.BOGUS.BYNAME", "SYS$DELPRC", None, R("NAMEBOGUS"))
P.call("PRC.SELF.COUNT3", "SYS$GETJPIW", 0, None, None, R("JPI_CNT"), R("IOSB"), None, 0,
       show=[UW("IOSB", "iosb"), U("J_PRCCNT", "prccnt")])
PROBE = P
