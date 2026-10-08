# Semantic-oracle spec: LIB$ / STR$ core routines (rd vms-8d1).
# String descriptors in and out (dynamic CLASS_D results, fixed CLASS_S results
# that pad and truncate), the STR$ editing / searching / comparison routines,
# CLI symbols (LIB$SET/GET/DELETE_SYMBOL), LIB$FIND_FILE with a default spec and
# a wildcard, numeric text conversion, LIB$SYS_FAO, LIB$GETJPI/GETSYI/GETDVI,
# virtual memory, and the LIB$ character scans. Return values that are positions
# or results (STR$POSITION, LIB$INDEX...) print as st=.
P = Probe("rtl")

P.ddesc("D1"); P.ddesc("D2")
P.buf("SBUF", 5); P.bdesc("SDSC", "SBUF", 5)
P.buf("OBUF", 128); P.bdesc("ODSC", "OBUF", 128)
P.word("OLEN", 0xFFFF)
P.long("RES", 0xA5A5A5A5)
P.long("CTX", 0)
P.long("ADDR", 0)
for n, v in [("N0", 0), ("N1", 1), ("N2", 2), ("N3", 3), ("N4", 4), ("N5", 5), ("N6", 6), ("N7", 7),
             ("N9", 9), ("N64", 64), ("NNEG", 0xFFFFFFFF), ("LOCALTBL", 1), ("GLOBALTBL", 2),
             ("JPI_USER", K("JPI$_USERNAME")), ("SYI_DEFPRI", K("SYI$_DEFPRI")), ("DVI_CLASS", K("DVI$_DEVCLASS")),
             ("JPI_UIC", K("JPI$_UIC"))]:
    P.long(n, v)
for n, t in [("S_ABC", "abc"), ("S_ABCDEFG", "abcdefg"), ("S_AB", "ab"), ("S_CD", "cd"), ("S_EF", "ef"),
             ("S_XYZ", "xyz"), ("S_HELLO", "Hello, World"), ("S_TRAIL", "  ab  \t "), ("S_HW", "hello world"),
             ("S_O", "o"), ("S_Q", "q"), ("S_ABD", "abd"), ("S_ABCPAD", "abc  "), ("S_ABCDEF", "abcdef"),
             ("S_XY", "XY"), ("S_ABCABC", "abcabc"), ("S_XYZU", "XYZ"), ("S_LIST", "a,b,c"), ("S_COMMA", ","),
             ("S_EMPTY", ""), ("S_X", "x"), ("S_SET", "xyl"), ("S_LL", "ll"), ("S_L", "l"), ("S_HELLOL", "hello"),
             ("S_H", "h"), ("SYM", "SP_RTL_SYM"), ("SYM_LC", "sp_rtl_sym"), ("SYMV", "value one"),
             ("SYM_BAD", "1BAD"), ("SYM_NONE", "SP_RTL_NO_SUCH_SYM"),
             ("FF_DEF", "SYS$SYSTEM:.EXE"), ("FF_NAME", "LOGINOUT"), ("FF_WILD", "SYS$SYSTEM:LOGINOU*.EXE;*"),
             ("FF_NONE", "SYS$SYSTEM:SP_NO_SUCH_FILE.EXE"), ("FF_BAD", "A[B"),
             ("DEC", "1234"), ("DECBAD", "12a4"), ("HEX", "FF"), ("OCT", "777"), ("DECBIG", "4294967296"),
             ("FAO1", "!UL item!%S"), ("NLA0", "NLA0:")]:
    P.desc(n, t)

D1 = [DTEXT("D1", "d1", limit=128), DL("D1", "len")]
SD = [T("SBUF", "SDSC", label="s")]


def str_(cid, routine, *args, show=D1):
    P.call(cid, routine, *args, show=show)


# STR$ into a dynamic descriptor
str_("STR.COPY_DX", "STR$COPY_DX", R("D1"), R("S_ABC"))
str_("STR.COPY_DX.EMPTY", "STR$COPY_DX", R("D1"), R("S_EMPTY"))
str_("STR.CONCAT", "STR$CONCAT", R("D1"), R("S_AB"), R("S_CD"), R("S_EF"))
str_("STR.APPEND", "STR$APPEND", R("D1"), R("S_XYZ"))
str_("STR.PREFIX", "STR$PREFIX", R("D1"), R("S_AB"))
str_("STR.UPCASE", "STR$UPCASE", R("D1"), R("S_HELLO"))
P.setw("OLEN", 0xFFFF)
str_("STR.TRIM", "STR$TRIM", R("D1"), R("S_TRAIL"), R("OLEN"), show=D1 + [UW("OLEN", "outlen")])
str_("STR.LEFT", "STR$LEFT", R("D1"), R("S_ABCDEF"), R("N3"))
str_("STR.RIGHT", "STR$RIGHT", R("D1"), R("S_ABCDEF"), R("N4"))
str_("STR.LEN_EXTR", "STR$LEN_EXTR", R("D1"), R("S_ABCDEF"), R("N2"), R("N3"))
str_("STR.LEN_EXTR.PAST_END", "STR$LEN_EXTR", R("D1"), R("S_ABCDEF"), R("N5"), R("N9"))
str_("STR.LEN_EXTR.NEGSTART", "STR$LEN_EXTR", R("D1"), R("S_ABCDEF"), R("N0"), R("N3"))
str_("STR.POS_EXTR", "STR$POS_EXTR", R("D1"), R("S_ABCDEF"), R("N2"), R("N4"))
str_("STR.DUPL_CHAR", "STR$DUPL_CHAR", R("D1"), R("N5"), RT("S_X"))
str_("STR.TRANSLATE", "STR$TRANSLATE", R("D1"), R("S_ABCABC"), R("S_XY"), R("S_AB"))
str_("STR.REPLACE", "STR$REPLACE", R("D1"), R("S_ABCDEF"), R("N2"), R("N4"), R("S_XYZU"))
str_("STR.ELEMENT.1", "STR$ELEMENT", R("D1"), R("N1"), R("S_COMMA"), R("S_LIST"))
str_("STR.ELEMENT.5", "STR$ELEMENT", R("D1"), R("N5"), R("S_COMMA"), R("S_LIST"))
str_("STR.FREE1_DX", "STR$FREE1_DX", R("D1"), show=[DL("D1", "len")])
# into a fixed 5-byte CLASS_S descriptor: pad and truncate
str_("STR.COPY_DX.FIXED.PAD", "STR$COPY_DX", R("SDSC"), R("S_ABC"), show=SD)
str_("STR.COPY_DX.FIXED.TRUNC", "STR$COPY_DX", R("SDSC"), R("S_ABCDEFG"), show=SD)
str_("LIB.SCOPY_DXDX.PAD", "LIB$SCOPY_DXDX", R("S_AB"), R("SDSC"), show=SD)
str_("LIB.SCOPY_DXDX.DYN", "LIB$SCOPY_DXDX", R("S_HELLO"), R("D2"), show=[DTEXT("D2", "d2", 128)])
# searches and comparisons: the return value is the answer
for cid, r, a in [("STR.POSITION", "STR$POSITION", (R("S_HW"), R("S_O"))),
                  ("STR.POSITION.START6", "STR$POSITION", (R("S_HW"), R("S_O"), R("N6"))),
                  ("STR.POSITION.NONE", "STR$POSITION", (R("S_HW"), R("S_Q"))),
                  ("STR.POSITION.EMPTYSUB", "STR$POSITION", (R("S_HW"), R("S_EMPTY"))),
                  ("STR.COMPARE.LT", "STR$COMPARE", (R("S_ABC"), R("S_ABD"))),
                  ("STR.COMPARE.GT", "STR$COMPARE", (R("S_ABD"), R("S_ABC"))),
                  ("STR.COMPARE.PAD", "STR$COMPARE", (R("S_ABC"), R("S_ABCPAD"))),
                  ("STR.COMPARE_EQL.PAD", "STR$COMPARE_EQL", (R("S_ABC"), R("S_ABCPAD"))),
                  ("STR.COMPARE_EQL.SAME", "STR$COMPARE_EQL", (R("S_ABC"), R("S_ABC"))),
                  ("STR.FIND_FIRST_IN_SET", "STR$FIND_FIRST_IN_SET", (R("S_HELLOL"), R("S_SET"))),
                  ("STR.FIND_FIRST_NOT_IN_SET", "STR$FIND_FIRST_NOT_IN_SET", (R("S_HELLOL"), R("S_H"))),
                  ("LIB.INDEX", "LIB$INDEX", (R("S_HELLOL"), R("S_LL"))),
                  ("LIB.INDEX.NONE", "LIB$INDEX", (R("S_HELLOL"), R("S_Q"))),
                  ("LIB.INDEX.EMPTY", "LIB$INDEX", (R("S_HELLOL"), R("S_EMPTY"))),
                  ("LIB.MATCHC", "LIB$MATCHC", (R("S_LL"), R("S_HELLOL"))),
                  ("LIB.LOCC", "LIB$LOCC", (R("S_L"), R("S_HELLOL"))),
                  ("LIB.LOCC.NONE", "LIB$LOCC", (R("S_Q"), R("S_HELLOL"))),
                  ("LIB.SKPC", "LIB$SKPC", (R("S_H"), R("S_HELLOL")))]:
    P.call(cid, r, *a)

# CLI symbols
P.do("LIB$DELETE_SYMBOL", R("SYM"), R("LOCALTBL"))
P.do("LIB$DELETE_SYMBOL", R("SYM"), R("GLOBALTBL"))
P.call("LIB.SET_SYMBOL", "LIB$SET_SYMBOL", R("SYM"), R("SYMV"), R("LOCALTBL"))
P.setw("OLEN", 0xFFFF); P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GET_SYMBOL", "LIB$GET_SYMBOL", R("SYM"), R("ODSC"), R("OLEN"), R("RES"),
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="value", limit=128), U("RES", "table")])
P.setw("OLEN", 0xFFFF)
P.call("LIB.GET_SYMBOL.LOWERCASE", "LIB$GET_SYMBOL", R("SYM_LC"), R("ODSC"), R("OLEN"), None,
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="value", limit=128)])
P.call("LIB.GET_SYMBOL.DYN", "LIB$GET_SYMBOL", R("SYM"), R("D2"), None, None, show=[DTEXT("D2", "d2", 128)])
P.call("LIB.SET_SYMBOL.GLOBAL", "LIB$SET_SYMBOL", R("SYM"), R("S_ABC"), R("GLOBALTBL"))
P.setw("OLEN", 0xFFFF); P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GET_SYMBOL.LOCAL_WINS", "LIB$GET_SYMBOL", R("SYM"), R("ODSC"), R("OLEN"), R("RES"),
       show=[T("OBUF", "OLEN", label="value", limit=128), U("RES", "table")])
P.call("LIB.DELETE_SYMBOL", "LIB$DELETE_SYMBOL", R("SYM"), R("LOCALTBL"))
P.setw("OLEN", 0xFFFF); P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GET_SYMBOL.GLOBAL_LEFT", "LIB$GET_SYMBOL", R("SYM"), R("ODSC"), R("OLEN"), R("RES"),
       show=[T("OBUF", "OLEN", label="value", limit=128), U("RES", "table")])
P.call("LIB.DELETE_SYMBOL.GLOBAL", "LIB$DELETE_SYMBOL", R("SYM"), R("GLOBALTBL"))
P.call("LIB.DELETE_SYMBOL.AGAIN", "LIB$DELETE_SYMBOL", R("SYM"), R("LOCALTBL"))
P.call("LIB.GET_SYMBOL.NONE", "LIB$GET_SYMBOL", R("SYM_NONE"), R("ODSC"), R("OLEN"), None)
P.call("LIB.SET_SYMBOL.BADNAME", "LIB$SET_SYMBOL", R("SYM_BAD"), R("SYMV"), R("LOCALTBL"))
P.call("LIB.SET_SYMBOL.EMPTYNAME", "LIB$SET_SYMBOL", R("S_EMPTY"), R("SYMV"), R("LOCALTBL"))

# LIB$FIND_FILE: default spec, wildcard walk, end of search, not found
P.setl("CTX", 0)
P.call("LIB.FIND_FILE.DEFAULT", "LIB$FIND_FILE", R("FF_NAME"), R("D1"), R("CTX"), R("FF_DEF"), None, None, None,
       show=[DTAIL("D1", 15, "tail")])
P.call("LIB.FIND_FILE.SAME_CTX_AGAIN", "LIB$FIND_FILE", R("FF_NAME"), R("D1"), R("CTX"), R("FF_DEF"), None, None, None)
P.call("LIB.FIND_FILE_END", "LIB$FIND_FILE_END", R("CTX"))
P.setl("CTX", 0)
for i in (1, 2):
    P.call("LIB.FIND_FILE.WILD.%d" % i, "LIB$FIND_FILE", R("FF_WILD"), R("D1"), R("CTX"), None, None, None, None,
           show=[DTAIL("D1", 15, "tail")])
P.do("LIB$FIND_FILE_END", R("CTX"))
P.setl("CTX", 0)
P.call("LIB.FIND_FILE.NONE", "LIB$FIND_FILE", R("FF_NONE"), R("D1"), R("CTX"), None, None, None, None)
P.do("LIB$FIND_FILE_END", R("CTX"))
P.setl("CTX", 0)
P.call("LIB.FIND_FILE.SYNTAX", "LIB$FIND_FILE", R("FF_BAD"), R("D1"), R("CTX"), None, None, None, None)
P.do("LIB$FIND_FILE_END", R("CTX"))

# numeric text conversion
for cid, r, n, d in [("LIB.CVT_DTB", "LIB$CVT_DTB", 4, "DEC"), ("LIB.CVT_DTB.BAD", "LIB$CVT_DTB", 4, "DECBAD"),
                     ("LIB.CVT_DTB.OVERFLOW", "LIB$CVT_DTB", 10, "DECBIG"), ("LIB.CVT_HTB", "LIB$CVT_HTB", 2, "HEX"),
                     ("LIB.CVT_OTB", "LIB$CVT_OTB", 3, "OCT"), ("LIB.CVT_DTB.ZEROLEN", "LIB$CVT_DTB", 0, "DEC")]:
    P.setl("RES", 0xA5A5A5A5)
    P.call(cid, r, n, RT(d), R("RES"), show=[X("RES", "res")])

# LIB$SYS_FAO, LIB$GETJPI / GETSYI / GETDVI
P.setw("OLEN", 0xFFFF)
P.call("LIB.SYS_FAO", "LIB$SYS_FAO", R("FAO1"), R("OLEN"), R("ODSC"), 2, 2,
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="out", limit=128)])
P.setw("OLEN", 0xFFFF)
P.call("LIB.GETJPI.USERNAME", "LIB$GETJPI", R("JPI_USER"), None, None, None, R("ODSC"), R("OLEN"),
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="out", limit=128)])
P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GETJPI.UIC.VALUE", "LIB$GETJPI", R("JPI_UIC"), None, None, R("RES"), None, None, show=[X("RES", "res")])
P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GETJPI.BADITEM", "LIB$GETJPI", R("N9"), None, None, R("RES"), None, None, show=[X("RES", "res")])
P.setw("OLEN", 0xFFFF)
P.call("LIB.GETJPI.UIC.STRING", "LIB$GETJPI", R("JPI_UIC"), None, None, None, R("ODSC"), R("OLEN"),
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="out", limit=128)])
P.setl("RES", 0xA5A5A5A5)
# (DEFPRI, not PAGE_SIZE: the page size is the architecture's)
P.call("LIB.GETSYI.DEFPRI", "LIB$GETSYI", R("SYI_DEFPRI"), R("RES"), None, None, None, None, show=[U("RES", "res")])
P.setw("OLEN", 0xFFFF)
P.call("LIB.GETSYI.DEFPRI.STRING", "LIB$GETSYI", R("SYI_DEFPRI"), None, R("ODSC"), R("OLEN"), None, None,
       show=[UW("OLEN", "len"), T("OBUF", "OLEN", label="out", limit=128)])
P.setl("RES", 0xA5A5A5A5)
P.call("LIB.GETDVI.DEVCLASS", "LIB$GETDVI", R("DVI_CLASS"), None, R("NLA0"), R("RES"), None, None, show=[U("RES", "res")])

# virtual memory
P.setl("ADDR", 0)
P.call("LIB.GET_VM", "LIB$GET_VM", R("N64"), R("ADDR"))
P.call("LIB.FREE_VM", "LIB$FREE_VM", R("N64"), R("ADDR"))
P.call("LIB.FREE_VM.AGAIN", "LIB$FREE_VM", R("N64"), R("ADDR"))
P.call("LIB.GET_VM.ZERO", "LIB$GET_VM", R("N0"), R("ADDR"))
P.call("LIB.GET_VM.NEG", "LIB$GET_VM", R("NNEG"), R("ADDR"))
PROBE = P
