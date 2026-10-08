# Semantic-oracle spec: mailbox PROTECTION and the mailbox-full write modifier
# (rd vms-c6d1). $CREMBX's promsk (a SOGW protection mask, a SET bit DENIES; per
# category bit 0 = read, bit 1 = write, bits 2/3 = logical/physical I/O), what
# $ASSIGN and the $QIO read/write of an unprivileged process are then allowed, which
# privileges override the mask (READALL, SYSPRV, BYPASS), whether the CREATOR's own
# channel is checked too, and IO$M_NOW|IO$M_NORSWAIT on a full mailbox.
#
# The probe runs as SYSTEM, a system-group UIC that is also the mailboxes' owner,
# so a mask only denies it when it denies EVERY category (the 0x?111 / 0x?222 /
# 0xFFFF shapes below); categories themselves are not separable from one process.
# Every write carries IO$M_NOW: a mailbox write without it waits for a reader, and
# this probe is its own reader.
P = Probe("mbxprot")
WRITEV, READV = K("IO$_WRITEVBLK"), K("IO$_READVBLK")
NOW, NORSWAIT = K("IO$M_NOW"), K("IO$M_NORSWAIT")

MASKS = [("ALL", 0xFFFF),    # every access denied to every category
         ("NOR", 0x1111),    # read denied, write allowed (a request mailbox)
         ("NOW", 0x2222),    # write denied, read allowed
         ("RW", 0x3333),     # read and write denied, logical/physical allowed
         ("OPEN", 0x0000)]   # the default: everything allowed

P.quad("IOSB")
P.buf("WBUF", 64, text="PROT")
P.buf("RBUF", 64)
P.buf("ALLPRV", 8, fill=0xFF); P.buf("PREVPRV", 8, fill=0)
P.quad("P_READALL"); P.quad("P_SYSPRV"); P.quad("P_BYPASS")
for tag, _ in MASKS:
    P.word("C" + tag, 0)                    # the creator's channel
    P.word("A" + tag, 0)                    # $ASSIGN with no privilege enabled
    P.desc("N" + tag, "SP_MBXP_" + tag)
for tag in ("RALL", "SALL", "BALL", "SNOR"):
    P.word(tag, 0)
P.word("CFULL", 0)

IOS = [UW("IOSB", "iosb"), UW("IOSB", "count", off=2)]

with P.sub("RESET"):
    P.fill("IOSB", byte=0xA5)
    P.fill("RBUF", byte=0x5A)


def wr(cid, chan, mod=0, n=4):
    P.gosub("RESET")
    P.call(cid, "SYS$QIOW", 0, V(chan), WRITEV | NOW | mod, R("IOSB"), None, 0, R("WBUF"), n, 0, 0, 0, 0,
           show=IOS)


def rd(cid, chan):
    P.gosub("RESET")
    P.call(cid, "SYS$QIOW", 0, V(chan), READV | NOW, R("IOSB"), None, 0, R("RBUF"), 40, 0, 0, 0, 0,
           show=IOS)


# PRV$V_READALL = 35, PRV$V_SYSPRV = 28, PRV$V_BYPASS = 29 (the $PRVDEF bit numbers)
P.setl("P_READALL", 0, 0); P.setl("P_READALL", 1 << 3, 4)
P.setl("P_SYSPRV", 1 << 28, 0); P.setl("P_SYSPRV", 0, 4)
P.setl("P_BYPASS", 1 << 29, 0); P.setl("P_BYPASS", 0, 4)

# 1. create one temporary mailbox per mask (privileges as the image starts)
for tag, mask in MASKS:
    P.call("MBXP.CREMBX." + tag, "SYS$CREMBX", 0, R("C" + tag), 40, 400, mask, None, R("N" + tag), None)

# 2. every privilege disabled: the creator's channel, then a fresh $ASSIGN, of each
P.call("MBXP.SETPRV.OFF", "SYS$SETPRV", 0, R("ALLPRV"), 0, R("PREVPRV"))
for tag, _ in MASKS:
    wr("MBXP.%s.CREATOR.WRITE" % tag, "C" + tag)
    rd("MBXP.%s.CREATOR.READ" % tag, "C" + tag)
for tag, _ in MASKS:
    P.call("MBXP.%s.ASSIGN" % tag, "SYS$ASSIGN", R("N" + tag), R("A" + tag), 0, None)
    wr("MBXP.%s.WRITE" % tag, "A" + tag)
    rd("MBXP.%s.READ" % tag, "A" + tag)

# 3. one privilege at a time against the deny-everything mailbox (and SYSPRV against
#    the read-denied one: does SYSPRV only qualify for the SYSTEM category?)
P.call("MBXP.READALL.ON", "SYS$SETPRV", 1, R("P_READALL"), 0, None)
P.call("MBXP.READALL.ASSIGN", "SYS$ASSIGN", R("NALL"), R("RALL"), 0, None)
wr("MBXP.READALL.WRITE", "RALL")
rd("MBXP.READALL.READ", "RALL")
P.call("MBXP.READALL.OFF", "SYS$SETPRV", 0, R("P_READALL"), 0, None)

P.call("MBXP.SYSPRV.ON", "SYS$SETPRV", 1, R("P_SYSPRV"), 0, None)
P.call("MBXP.SYSPRV.ASSIGN", "SYS$ASSIGN", R("NALL"), R("SALL"), 0, None)
wr("MBXP.SYSPRV.WRITE", "SALL")
P.call("MBXP.SYSPRV.NOR.ASSIGN", "SYS$ASSIGN", R("NNOR"), R("SNOR"), 0, None)
rd("MBXP.SYSPRV.NOR.READ", "SNOR")
P.call("MBXP.SYSPRV.OFF", "SYS$SETPRV", 0, R("P_SYSPRV"), 0, None)

P.call("MBXP.BYPASS.ON", "SYS$SETPRV", 1, R("P_BYPASS"), 0, None)
P.call("MBXP.BYPASS.ASSIGN", "SYS$ASSIGN", R("NALL"), R("BALL"), 0, None)
wr("MBXP.BYPASS.WRITE", "BALL")
rd("MBXP.BYPASS.READ", "BALL")
P.call("MBXP.BYPASS.OFF", "SYS$SETPRV", 0, R("P_BYPASS"), 0, None)

# 4. the channels assigned while unprivileged, now that a privilege is back on:
#    is the access decided at $ASSIGN or at each I/O?
P.call("MBXP.BYPASS.ON2", "SYS$SETPRV", 1, R("P_BYPASS"), 0, None)
rd("MBXP.NOR.READ.LATER", "ANOR")
P.call("MBXP.BYPASS.OFF2", "SYS$SETPRV", 0, R("P_BYPASS"), 0, None)

P.call("MBXP.SETPRV.ON", "SYS$SETPRV", 1, R("PREVPRV"), 0, None)

# 5. a full mailbox: 40-byte messages, 60 bytes of buffer quota. IO$M_NORSWAIT asks
#    for SS$_MBFULL instead of a resource wait.
P.call("MBXP.CREMBX.FULL", "SYS$CREMBX", 0, R("CFULL"), 40, 60, 0, None, None, None)
wr("MBXP.FULL.WRITE1", "CFULL", 0, 40)
wr("MBXP.FULL.WRITE2.NORSWAIT", "CFULL", NORSWAIT, 40)
wr("MBXP.FULL.WRITE3.NORSWAIT.SMALL", "CFULL", NORSWAIT, 4)
rd("MBXP.FULL.READ1", "CFULL")
rd("MBXP.FULL.READ2", "CFULL")
rd("MBXP.FULL.READ3", "CFULL")
PROBE = P
