# Semantic-oracle spec: $FAO / $FAOL / $GETMSG output (rd vms-8d1).
# The FAO directives programs actually use -- strings (!AS !AD !AC !AF), numbers
# in every radix and width (!UL !SL !XL !OL !ZL !UW !UB !XW !XB, !5UL, signed
# padding, overflow asterisks), repeat and field directives (!n*c !n<..!> !- !+),
# !%S plurals, !%U UICs, !%D / !%T with a fixed binary time, !/ !_ !^ !!, a
# too-small output buffer, an unknown directive -- and $GETMSG's flag bits,
# unknown message numbers and its outadr argument-count byte.
P = Probe("fao")
P.buf("OBUF", 200)
P.word("OLEN", 0xFFFF)
P.bdesc("ODSC", "OBUF", 200)
P.buf("SBUF", 8)
P.bdesc("SDSC", "SBUF", 8)
P.desc("STR", "Hello")
P.desc("EMPTY", "")
P.buf("CSTR", 6, text="\x05World")
P.buf("NONPRINT", 4, text="A\x01\x7fZ")
P.quad("TIME")
P.desc("TIMESTR", "7-OCT-2026 13:45:56.78")
P.do("SYS$BINTIM", R("TIMESTR"), R("TIME"))
OUT = [UW("OLEN", "len"), T("OBUF", "OLEN", label="out", limit=200)]

n = [0]


def fao(cid, ctl, *prms, out=OUT, odesc="ODSC"):
    n[0] += 1
    d = "C%d" % n[0]
    P.desc(d, ctl)
    P.setw("OLEN", 0xFFFF)
    P.fill("OBUF", byte=0x2E)
    P.call(cid, "SYS$FAO", R(d), R("OLEN"), R(odesc), *prms, show=out)


fao("FAO.LITERAL", "plain text")
fao("FAO.AS", "[!AS]", R("STR"))
fao("FAO.AS.EMPTY", "[!AS]", R("EMPTY"))
fao("FAO.AD", "[!AD]", 3, RT("STR"))
fao("FAO.AC", "[!AC]", R("CSTR"))
fao("FAO.AF", "[!AF]", 4, R("NONPRINT"))
fao("FAO.AS.WIDTH.PAD", "[!8AS]", R("STR"))
fao("FAO.AS.WIDTH.TRUNC", "[!3AS]", R("STR"))
fao("FAO.UL", "!UL !UL !UL", 0, 1, 4294967295)
fao("FAO.SL", "!SL !SL !SL", 0, 0xFFFFFFFF, 0x80000000)
fao("FAO.XL", "!XL !XL", 0x1234ABCD, 0)
fao("FAO.OL", "!OL", 8)
fao("FAO.ZL", "!ZL !5ZL", 42, 42)
fao("FAO.UW.UB", "!UW !UB !UW !UB", 0x12345678, 0x12345678, 65535, 255)
fao("FAO.XW.XB", "!XW !XB", 0x12345678, 0x12345678)
fao("FAO.SW.SB", "!SW !SB", 0xFFFF, 0x80)
fao("FAO.WIDTH", "[!5UL][!5SL][!5XL][!3UL]", 42, 0xFFFFFFFF, 0xAB, 123456)
fao("FAO.WIDTH.ZERO", "[!0UL]", 42)
fao("FAO.REPEAT.CHAR", "[!5*-]")
fao("FAO.REPEAT.DIR", "[!3(UL)]", 1, 2, 3)
fao("FAO.REPEAT.WIDTH", "[!2(4UL)]", 7, 8)
fao("FAO.FIELD", "[!10<!UL-!UL!>]", 12, 34)
fao("FAO.FIELD.OVER", "[!3<!UL!>]", 123456)
fao("FAO.REUSE", "!UL !-!UL !UL", 5, 6)
fao("FAO.SKIP", "!UL !+!UL", 5, 6, 7)
fao("FAO.PLURAL", "!UL file!%S, !UL file!%S", 1, 1, 2, 2)
fao("FAO.PLURAL.WIDTH", "!UL fil!%S", 0, 0)
fao("FAO.UIC", "!%U", 0x00010004)
fao("FAO.UIC.ID", "!%I", 0x00010004)
fao("FAO.DATE", "!%D", R("TIME"))
fao("FAO.TIME", "!%T", R("TIME"))
fao("FAO.DATE.WIDTH", "[!11%D]", R("TIME"))
fao("FAO.CTLCHARS", "a!/b!_c!^d!!e")
fao("FAO.UNKNOWN", "[!Q]")
fao("FAO.TRAILING.BANG", "abc!")
fao("FAO.BUFFEROVF", "0123456789ABC", out=[UW("OLEN", "len"), T("SBUF", "OLEN", label="out", limit=8)], odesc="SDSC")
fao("FAO.BUFFEROVF.AS", "[!AS!AS]", R("STR"), R("STR"), out=[UW("OLEN", "len"), T("SBUF", "OLEN", label="out", limit=8)],
    odesc="SDSC")

# $FAOL: the same directives, parameters from an in-memory vector
P.plist("PL1", [2, 3, 5, R("STR")])
P.desc("FL1", "!UL+!UL=!UL !AS")
P.setw("OLEN", 0xFFFF)
P.fill("OBUF", byte=0x2E)
P.call("FAOL.BASIC", "SYS$FAOL", R("FL1"), R("OLEN"), R("ODSC"), R("PL1"), show=OUT)
P.desc("FL2", "no params")
P.setw("OLEN", 0xFFFF)
P.call("FAOL.NOPRM", "SYS$FAOL", R("FL2"), R("OLEN"), R("ODSC"), None, show=OUT)
P.setw("OLEN", 0xFFFF)
P.call("FAO.NOOUTLEN", "SYS$FAO", R("FL2"), None, R("ODSC"), show=[T("OBUF", "ODSC", label="out", clamp=12)])

# $GETMSG: the four message parts, an unknown message, the outadr vector
P.buf("MBUF", 256)
P.word("MLEN", 0xFFFF)
P.bdesc("MDSC", "MBUF", 256)
P.buf("MSHORT", 10)
P.bdesc("MSDSC", "MSHORT", 10)
P.buf("OUTADR", 4)
MOUT = [UW("MLEN", "len"), T("MBUF", "MLEN", label="msg", limit=256)]
for cid, msg, flags in [("MSG.NORMAL.ALL", K("SS$_NORMAL"), 15), ("MSG.ACCVIO.ALL", K("SS$_ACCVIO"), 15),
                        ("MSG.ACCVIO.TEXT", K("SS$_ACCVIO"), 1), ("MSG.ACCVIO.IDENT", K("SS$_ACCVIO"), 2),
                        ("MSG.ACCVIO.SEV", K("SS$_ACCVIO"), 4), ("MSG.ACCVIO.FAC", K("SS$_ACCVIO"), 8),
                        ("MSG.ACCVIO.FLAGS0", K("SS$_ACCVIO"), 0), ("MSG.ACCVIO.SEV_TEXT", K("SS$_ACCVIO"), 5),
                        ("MSG.RMS_FNF", K("RMS$_FNF"), 15), ("MSG.BADPARAM", K("SS$_BADPARAM"), 15),
                        ("MSG.WASSET", K("SS$_WASSET"), 15), ("MSG.SEV_CHANGED", K("SS$_ACCVIO") | 1, 15),
                        ("MSG.UNKNOWN", 0x0FFF0002, 15), ("MSG.UNKNOWN.TEXT", 0x0FFF0002, 1),
                        ("MSG.ZERO", 0, 15), ("MSG.UNKNOWN.NONZERO", 0x0FFF8012, 15), ("MSG.EXQUOTA", K("SS$_EXQUOTA"), 15)]:
    P.setw("MLEN", 0xFFFF)
    P.fill("OUTADR", byte=0xEE)
    P.call(cid, "SYS$GETMSG", msg, R("MLEN"), R("MDSC"), flags, R("OUTADR"),
           show=MOUT + [XB("OUTADR", 4, label="outadr")])
P.setw("MLEN", 0xFFFF)
P.call("MSG.SHORTBUF", "SYS$GETMSG", K("SS$_ACCVIO"), R("MLEN"), R("MSDSC"), 15, None,
       show=[UW("MLEN", "len"), T("MSHORT", "MLEN", label="msg", limit=10)])
P.setw("MLEN", 0xFFFF)
P.call("MSG.FAO_INTO", "SYS$GETMSG", K("SS$_ACCVIO"), R("MLEN"), R("MDSC"), 1, None, show=MOUT)
PROBE = P
