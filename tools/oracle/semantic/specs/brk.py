# Semantic-oracle spec: BROADCAST AND OPERATOR MESSAGES -- $BRKTHRUW / $SNDOPR
# (rd vms-8d1). $BRKTHRUW to this process's own terminal, to a device that does
# not exist, to a user who is not logged in, with an out-of-range send type and
# message class, an empty and an over-long message; the IOSB's terminal counts.
# $SNDOPR: a request to the central operator with and without a reply mailbox
# channel, a bad channel, a short and an unknown-type message buffer.
# OPC$_RQ_RQST (3) and OPC$M_NM_CENTRL (1) are the documented request type and
# operator class (OpenVMS System Services Reference, $SNDOPR); they are not in
# the STARLET dumps, so they are literals here.
P = Probe("brk")
DEV, USER = K("BRK$C_DEVICE"), K("BRK$C_USERNAME")
GENERAL, USER1 = K("BRK$C_GENERAL"), K("BRK$C_USER1")

P.quad("IOSB")
P.long("CHAN", 0)
P.long("BADCHAN", 0xFFF0)
P.desc("MSG", "SP_BRK_MESSAGE")
P.desc("EMPTY", "")
P.desc("HUGE", "B" * 2000)
P.desc("TT", "TT:")
P.desc("NODEV", "SP_NODEV0:")
P.desc("NOUSER", "SP_NOBODY")
P.desc("MBXNAM", "SP_BRK_MBX")

# an operator request: type byte, 3-byte operator mask, request id, text
REQ_TEXT = "SP_SNDOPR_PROBE"
P.buf("OPREQ", 8 + len(REQ_TEXT), text="\x03\x01\x00\x00\x00\x00\x00\x00" + REQ_TEXT)
P.bdesc("OPREQD", "OPREQ", 8 + len(REQ_TEXT))
P.bdesc("OPSHORT", "OPREQ", 3)
P.buf("OPBAD", 8 + len(REQ_TEXT), text="\x63\x01\x00\x00\x00\x00\x00\x00" + REQ_TEXT)
P.bdesc("OPBADD", "OPBAD", 8 + len(REQ_TEXT))

BSHOW = [UW("IOSB", "iosb"), UW("IOSB", "sent", off=2), UW("IOSB", "timedout", off=4),
         UW("IOSB", "refused", off=6)]


def brk(cid, msg, to, typ, cls=GENERAL, timout=0, show=None):
    # efn msgbuf sendto sndtyp iosb carcon flags reqid timout astadr astprm
    P.setl("IOSB", 0xA5A5A5A5, 0); P.setl("IOSB", 0xA5A5A5A5, 4)
    # carcon 0x20: the message on a line of its own (LF before, CR after), so
    # it never lands inside a case line of the transcript
    P.call(cid, "SYS$BRKTHRUW", 0, R(msg), None if to is None else R(to), typ, R("IOSB"),
           0x20, 0, cls, timout, None, 0, show=BSHOW if show is None else show)


brk("BRK.TT", "MSG", "TT", DEV)
brk("BRK.TT.USERCLASS", "MSG", "TT", DEV, USER1)
brk("BRK.TT.BADCLASS", "MSG", "TT", DEV, 99)
brk("BRK.TT.EMPTY", "EMPTY", "TT", DEV)
brk("BRK.TT.HUGE", "HUGE", "TT", DEV)
brk("BRK.NODEV", "MSG", "NODEV", DEV)
brk("BRK.NOUSER", "MSG", "NOUSER", USER)
brk("BRK.BADTYPE", "MSG", "TT", 99)
brk("BRK.TYPE0", "MSG", "TT", 0)
brk("BRK.DEVICE.NOSENDTO", "MSG", None, DEV)

P.call("OPR.REQUEST", "SYS$SNDOPR", R("OPREQD"), 0)
P.call("OPR.SHORT", "SYS$SNDOPR", R("OPSHORT"), 0)
P.call("OPR.BADTYPE", "SYS$SNDOPR", R("OPBADD"), 0)
P.call("OPR.BADCHAN", "SYS$SNDOPR", R("OPREQD"), V("BADCHAN"))
P.do("SYS$CREMBX", 0, R("CHAN"), 0, 0, 0, 0, R("MBXNAM"), 0, 0)
P.call("OPR.MBXCHAN", "SYS$SNDOPR", R("OPREQD"), V("CHAN"))
P.do("SYS$DASSGN", V("CHAN"))
PROBE = P
