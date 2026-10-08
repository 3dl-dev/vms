#!/usr/bin/env python3
"""spgen.py - semantic-oracle probe generator (rd vms-8d1).

One probe SPEC (tools/oracle/semantic/specs/<family>.py) describes a table of
service calls and what to print after each. This tool turns it into TWO programs
that print the SAME canonical transcript:

  * MACRO-32 (--mar), for the real OpenVMS nodes. VAX V7.3 has MACRO + LINK and
    no C compiler; Alpha V8.4 compiles the same VAX MACRO source with its MACRO-32
    compiler. So one .MAR runs on both real systems.
  * C (--c), for OVMX: compiled with the OVMX toolchain (cc -> LINK.EXE) and run
    in the booted runtime under the real executive.

The transcript printer is independent of the services under test: the MACRO side
formats with $FAOL + LIB$PUT_OUTPUT (so a $FAO probe is the only place $FAO's own
semantics are tested, by explicit calls), the C side with printf.

Every constant a spec uses comes from the oracle header dumps
(docs/oracle/vax73-starlet-defs, falling back to alpha84-starlet-defs) through K(),
never from OVMX's own headers -- the probe must not inherit a wrong OVMX value.

Usage:
  spgen.py --mar <spec.py>      MACRO-32 source on stdout
  spgen.py --c   <spec.py>      C source on stdout
  spgen.py --list <spec.py>     the case ids, one per line

Clean-room (AGENTS.md Rule 8): this generates programs whose OBSERVED output on a
real system is the oracle. Nothing is derived from VSI/HPE source.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

_K = None


def K(name):
    """A constant's value as the real system's STARLET defines it."""
    global _K
    if _K is None:
        _K = {}
        for d in ("alpha84-starlet-defs", "vax73-starlet-defs"):
            for f in glob.glob(os.path.join(ROOT, "docs", "oracle", d, "*.txt")):
                for ln in open(f, errors="ignore"):
                    m = re.match(r"\$EQU\s+(\S+)\s+(-?\d+)\s*$", ln)
                    if m:
                        _K[m.group(1)] = int(m.group(2))
    if name not in _K:
        raise KeyError("constant %s not in the oracle dumps" % name)
    return _K[name] & 0xFFFFFFFF


# ---- argument kinds -------------------------------------------------------
class V:                       # pass the CONTENTS of a long/word data item by value
    def __init__(self, n): self.n = n


class R:                       # pass the ADDRESS of a data item (by reference)
    def __init__(self, n, off=0): self.n, self.off = n, off


class RT:                      # the address of a P.desc() descriptor's TEXT (by-reference bytes)
    def __init__(self, n): self.n = n


class AST:                     # the address of an AST routine declared with P.ast()
    def __init__(self, n): self.n = n


# R is also implied by a bare str argument; an int is an immediate by value;
# None is a zero (an omitted optional argument).

# ---- print fields ---------------------------------------------------------
class F:
    def __init__(self, kind, label, *a):
        self.kind, self.label, self.a = kind, label, a


# numeric fields read the long/word/byte at data item <n> + <off>
def X(n, label=None, off=0):  return F("X", label or n, n, off)    # long, 8 hex digits
def U(n, label=None, off=0):  return F("U", label or n, n, off)    # long, unsigned decimal
def S(n, label=None, off=0):  return F("S", label or n, n, off)    # long, signed decimal
def UW(n, label=None, off=0): return F("UW", label or n, n, off)   # word, unsigned decimal
def XW(n, label=None, off=0): return F("XW", label or n, n, off)   # word, 4 hex digits
def UB(n, off=0, label=None): return F("UB", label or n, n, off)   # byte, unsigned dec


def EQ(a, b, label, aoff=0, boff=0):
    """1 if the longs at data items a(+aoff) and b(+boff) are equal, else 0 (a
    relation, for values that differ between systems -- 'my PID == the master PID')"""
    return F("EQ", label, a, b, aoff, boff)


def T(buf, lenvar, lenkind="W", label=None, clamp=None, limit=None, lenoff=0):
    """text of <buf> whose length is in data item <lenvar> (W word / L long);
    non-printable bytes print as '.' (FAO !AF). clamp = print at most this many
    bytes; limit = a length above this prints as "" (an unwritten sentinel --
    show the raw length with UW/U when that matters)."""
    return F("T", label or buf, buf, lenvar, lenkind, clamp, limit, lenoff)


def TF(buf, n, label=None):                               # fixed-length text
    return F("TF", label or buf, buf, n)


def XB(buf, n, off=0, label=None):                        # n bytes as hex pairs
    return F("XB", label or buf, buf, n, off)


def DL(desc, label=None):     # the current length field of a descriptor
    return F("DL", label or desc + ".len", desc)


def FLD(cb, field, label=None, fmt="X"):
    """a field of an RMS control block (P.cb), by its VMS name, e.g.
    FLD("FAB1", "fab$l_sts"); fmt X (hex) or U (unsigned decimal)"""
    return F("FLD", label or field.split("_", 1)[1], cb, field, fmt)


def TPTR(cb, ptrfield, lenfield, label, limit=None):
    """the text a control-block POINTER field addresses, its length in another
    field -- e.g. a NAM's nam$l_name / nam$b_name. limit = a longer length
    prints "" (a length the service never set must not walk off into memory)"""
    return F("TPTR", label, cb, ptrfield, lenfield, limit)


def DTEXT(desc, label, limit=None):
    """the text a string descriptor currently describes (pointer + length) --
    for a dynamic (CLASS_D) result descriptor; limit = a longer length prints ''"""
    return F("DTEXT", label, desc, limit)


def DTAIL(desc, n, label):
    """the LAST <n> characters a string descriptor describes -- e.g. the
    NAME.TYPE;VER end of a resultant file spec, whose device and directory
    differ between systems"""
    return F("DTAIL", label, desc, n)


def BIT(n, bit, label):
    """bit <bit> (0 = lsb) of the long at data item <n>, as 0 or 1"""
    return F("BIT", label, n, bit)


def ST(label="st"):          # the status of the last call
    return F("ST", label)


class Probe:
    def __init__(self, family):
        self.family = family
        self.data = []          # (kind, name, args)
        self.names = {}
        self.ops = []
        self.cases = []
        self.routines = set()
        self.subs = {}
        self.asts = []
        self.ovmx_absent = set()

    # ---- data ------------------------------------------------------------
    RESERVED = {"ST", "PRM", "FAOBUF", "FAODSC", "OUTLEN", "GCASE", "GDELTA", "GDT", "FBEGIN", "FEND",
                "FABORT", "START"}

    def _decl(self, kind, name, *a):
        assert re.match(r"^[A-Z][A-Z0-9_]{0,30}$", name), name
        assert name not in self.RESERVED and not re.match(r"^(PR\d|CID\d|GH\d|SUB_|AST_|FMT\d|R\d+$|AP$|FP$|SP$|PC$)", name), \
            "data item name %s is reserved by the generator" % name
        assert name not in self.names, "duplicate data item " + name
        self.names[name] = kind
        self.data.append((kind, name, a))
        return name

    def long(self, n, v=0): return self._decl("long", n, v)
    def word(self, n, v=0): return self._decl("word", n, v)
    def quad(self, n): return self._decl("quad", n)
    def buf(self, n, size, fill=0x5A, text=None):
        """a byte buffer of <size>, every byte <fill>; <text> (if given) is
        written at its start when the image starts"""
        assert text is None or len(text) <= size
        return self._decl("buf", n, size, fill, text)

    def desc(self, n, text):
        """a static string descriptor (CLASS_S, DTYPE_T) of <text>"""
        return self._decl("desc", n, text)

    def bdesc(self, n, bufname, length):
        """a string descriptor over buffer <bufname> with length <length>"""
        assert self.names.get(bufname) == "buf"
        return self._decl("bdesc", n, bufname, length)

    def items(self, n, lst):
        """an item_list_3: [(buflen, code, bufname|None, retlenword|None), ...]
        terminated by a zero longword"""
        return self._decl("items", n, lst)

    # ---- ops -------------------------------------------------------------
    def _case(self, cid):
        assert re.match(r"^[A-Z0-9][A-Z0-9_.$-]*$", cid), cid
        assert cid not in self.cases, "duplicate case " + cid
        self.cases.append(cid)

    def call(self, cid, routine, *args, show=(), guard=True):
        """call <routine>(args), then print '<cid> st=XXXXXXXX <show...>'.
        Every call is hang-guarded (a 10 s timer whose AST prints '<cid> HUNG' and
        ends the probe): a service that blocks where the real system returns
        must cost one case, not the whole run."""
        self._case(cid)
        self.routines.add(routine.upper())
        self.ops.append(("call", cid, routine.upper(), args, (ST(),) + tuple(show), guard))

    def callq(self, into, routine, *args, guard_case=None):
        """call <routine> and keep its status in long <into> WITHOUT printing --
        for sequences that must not be interleaved with output (printing on a
        real VMS node goes through RMS and the terminal driver, which can touch
        event flags)"""
        if into not in self.names:
            self.long(into)
        self.routines.add(routine.upper())
        self.ops.append(("callq", into, routine.upper(), args, guard_case))

    def do(self, routine, *args):
        """call <routine> for its side effect only (setup), print nothing"""
        self.routines.add(routine.upper())
        self.ops.append(("do", routine.upper(), args))

    def show(self, cid, *fields):
        self._case(cid)
        self.ops.append(("show", cid, fields))

    def setl(self, n, v): self.ops.append(("setl", n, v))
    def setw(self, n, v): self.ops.append(("setw", n, v))
    def andl(self, n, mask): self.ops.append(("andl", n, mask))   # long &= mask
    def copyl(self, dst, src): self.ops.append(("copyl", dst, src))  # long dst = long src

    def fill(self, bufname, text=None, byte=None):
        """overwrite a buffer: with <text> (rest unchanged) or every byte = <byte>"""
        self.ops.append(("fill", bufname, text, byte))

    def sub(self, name):
        """record the ops issued inside `with P.sub(NAME):` as a subroutine;
        P.gosub(NAME) then runs it (keeps the MACRO source short)"""
        assert re.match(r"^[A-Z][A-Z0-9_]{0,20}$", name) and name not in self.subs
        probe = self

        class _Ctx:
            def __enter__(self_):
                self_.saved = probe.ops
                probe.subs[name] = []
                probe.ops = probe.subs[name]

            def __exit__(self_, *exc):
                probe.ops = self_.saved
        return _Ctx()

    def gosub(self, name):
        assert name in self.subs
        self.ops.append(("gosub", name))

    def ddesc(self, n):
        """an empty dynamic string descriptor (CLASS_D) for an RTL result"""
        return self._decl("ddesc", n)

    def absent_on_ovmx(self, *routines):
        """routines OVMX does not provide at all: the C side cannot link a call
        to them, so it prints 'st=ABSENT' for those cases (a difference the gate
        tracks like any other) instead of calling"""
        self.ovmx_absent |= {r.upper() for r in routines}

    def cb(self, n, kind):
        """an RMS control block (FAB, RAB, NAM) initialised as $FAB / cc$rms_fab
        etc. do; set fields with P.cset"""
        assert kind in ("FAB", "RAB", "NAM")
        return self._decl("cb", n, kind)

    def cset(self, cb, field, value):
        """store <value> (an int, or R(name) = an address) in a control-block field"""
        assert self.names.get(cb) == "cb" and re.match(r"^(fab|rab|nam)\$[lwb]_[a-z0-9]+$", field), field
        self.ops.append(("cset", cb, field, value))

    def plist(self, n, vals):
        """a parameter vector ($FAOL): each element an int or R(name) (an
        address). Longwords on the real nodes; on OVMX the element width its
        $FAOL takes (uint64_t, src/libvms/include/starlet.h)."""
        return self._decl("plist", n, vals)

    def ast(self, n):
        """declare an AST routine: each delivery increments long <n>, stores its
        AST parameter in long <n>_P and the delivery sequence number (a probe-wide
        counter ASTSEQ) in long <n>_Q -- so cases can show THAT it fired, WITH
        WHAT, and IN WHICH ORDER"""
        if "ASTSEQ" not in self.names:
            self.long("ASTSEQ")
        self.long(n); self.long(n + "_P"); self.long(n + "_Q")
        self.asts.append(n)
        return n

    def setdlen(self, desc, length):
        """set a descriptor's length field"""
        self.ops.append(("setdlen", desc, length))


# ===========================================================================
# MACRO-32 backend
# ===========================================================================
def _mac_str(s):
    # a MACRO string literal: pick a delimiter absent from s
    for d in ('"', "/", "|", "%", "\\", "~", "+", "@"):
        if d not in s:
            return d + s + d
    raise ValueError("no delimiter for " + s)


def _mac_text(label, text, chunk=60):
    """MACRO data lines for <text>: printable runs as .ASCII (at most <chunk>
    characters a line -- a long typed line does not survive a VMS console), and
    anything else as .BYTE (a raw control character would not survive either)"""
    out = []
    first = [True]

    def emit(line):
        out.append(("%s: " % label if first[0] and label else "        ") + line)
        first[0] = False
    run = ""
    for ch in text:
        if 0x20 <= ord(ch) < 0x7F:
            run += ch
            if len(run) == chunk:
                emit(".ASCII  %s" % _mac_str(run)); run = ""
        else:
            if run:
                emit(".ASCII  %s" % _mac_str(run)); run = ""
            emit(".BYTE   %d" % ord(ch))
    if run:
        emit(".ASCII  %s" % _mac_str(run))
    return out


def _mac_ascid(label, text):
    """a string descriptor of <text> at <label>: .ASCID when short, else an
    explicit CLASS_S descriptor over chunked .ASCII lines"""
    if len(text) <= 60 and all(0x20 <= ord(c) < 0x7F for c in text):
        return ["%s: .ASCID  %s" % (label, _mac_str(text))]
    return (["%s: .WORD   %d" % (label, len(text)), "        .BYTE   14,1",
             "        .ADDRESS %s_X" % label] + _mac_text(label + "_X", text))


def _sig(fields):
    return tuple((f.kind, f.label) + tuple(f.a) for f in fields)


def gen_mar(p):
    o = []
    w = o.append
    title = ("SP_" + p.family.upper())[:31]
    w("; GENERATED by tools/oracle/semantic/spgen.py from specs/%s.py -- do not edit." % p.family)
    w("; Semantic-oracle probe (rd vms-8d1). Assemble: MACRO; LINK; RUN.")
    w("        .TITLE  %s" % title)
    w("        .MACRO  SPCALL RTN, N")
    w("        CALLS   #N, G^RTN")
    w("        MOVL    R0, ST")
    w("        .ENDM   SPCALL")
    # local routines are CALLS/.ENTRY, not JSB/RSB: the Alpha MACRO-32 compiler
    # needs .JSB_ENTRY for a JSB target and VAX MACRO has no such directive
    w("        .MACRO  SPGRD GH")
    w("        MOVAB   GH, GCASE")
    w("        $SETIMR_S EFN=#30, DAYTIM=GDELTA, ASTADR=GUARD_AST, REQIDT=#^X5E3A")
    w("        .ENDM   SPGRD")
    w("        .MACRO  SPUNG")
    w("        $CANTIM_S REQIDT=#^X5E3A")
    w("        .ENDM   SPUNG")
    w("        .MACRO  SPPR CID, PR")
    w("        MOVAB   CID, PRM")
    w("        CALLS   #0, PR")
    w("        .ENDM   SPPR")
    if any(k == "cb" for k, _, _ in p.data):
        w("        $FABDEF")
        w("        $RABDEF")
        w("        $NAMDEF")
    w("        .PSECT  SP_DATA,NOEXE,WRT,LONG")
    w("ST:     .LONG   0")
    w("PRM:    .BLKL   64")
    w("FAOBUF: .BLKB   1024")
    w("FAODSC: .WORD   1024")
    w("        .BYTE   14,1")
    w("        .ADDRESS FAOBUF")
    w("OUTLEN: .WORD   0")
    w("GCASE:  .LONG   0")
    w("GDELTA: .QUAD   0")
    w("GDT:    .ASCID  /0 00:00:10.00/")
    w("FBEGIN: .ASCID  /=== SEMPROBE %s BEGIN ===/" % p.family)
    w("FEND:   .ASCID  /=== SEMPROBE %s END ===/" % p.family)
    w("FABORT: .ASCID  /=== SEMPROBE aborted END ===/")
    for kind, n, a in p.data:
        if kind == "long":
            w("%s: .LONG   %d" % (n, a[0] & 0xFFFFFFFF))
        elif kind == "word":
            w("%s: .WORD   %d" % (n, a[0] & 0xFFFF))
        elif kind == "quad":
            w("%s: .QUAD   0" % n)
        elif kind == "buf":
            size, fillb, text = a
            w("%s:" % n)
            if text:
                o.extend(_mac_text(None, text))
            if size - len(text or ""):
                w("        .REPT   %d" % (size - len(text or "")))
                w("        .BYTE   %d" % fillb)
                w("        .ENDR")
        elif kind == "desc":
            text = a[0]
            w("%s: .WORD   %d" % (n, len(text)))
            w("        .BYTE   14,1")
            w("        .ADDRESS %s_T" % n)
            if text:
                o.extend(_mac_text(n + "_T", text))
            else:
                w("%s_T: .BLKB   1" % n)
        elif kind == "bdesc":
            w("%s: .WORD   %d" % (n, a[1]))
            w("        .BYTE   14,1")
            w("        .ADDRESS %s" % a[0])
        elif kind == "cb":
            w("        .ALIGN  LONG")
            w("%s: $%s" % (n, a[0]))
        elif kind == "ddesc":
            w("        .ALIGN  LONG")
            w("%s: .WORD   0" % n)
            w("        .BYTE   14,2")
            w("        .LONG   0")
        elif kind == "plist":
            w("        .ALIGN  LONG")
            w("%s:" % n)
            for v in a[0]:
                w("        .ADDRESS %s" % v.n if isinstance(v, R) else "        .LONG   %d" % (v & 0xFFFFFFFF))
        elif kind == "items":
            w("        .ALIGN  LONG")
            w("%s:" % n)
            for (blen, code, bufn, retn) in a[0]:
                w("        .WORD   %d,%d" % (blen & 0xFFFF, code & 0xFFFF))
                w("        .ADDRESS %s" % (bufn if bufn else "0"))
                w("        .ADDRESS %s" % (retn if retn else "0"))
            w("        .LONG   0")

    printers = {}     # field signature -> (label, control string, body lines)
    cids = []
    hung = {}

    def printer(fields):
        sig = _sig(fields)
        if sig in printers:
            return printers[sig][0]
        lbl = "PR%d" % len(printers)
        ctl = ["!AS"]
        body = []
        slot = [1]          # PRM+0 is the case-id descriptor

        def nxt():
            d = "PRM+%d" % (4 * slot[0])
            slot[0] += 1
            return d
        for f in fields:
            k = f.kind
            if k == "ST":
                ctl.append("%s=!XL" % f.label); body.append("        MOVL    ST, %s" % nxt())
            elif k in ("X", "U", "S", "UW", "XW", "UB"):
                fao = {"X": "!XL", "U": "!UL", "S": "!SL", "UW": "!UL", "XW": "!XW", "UB": "!UL"}[k]
                ins = {"X": "MOVL  ", "U": "MOVL  ", "S": "MOVL  ", "UW": "MOVZWL", "XW": "MOVZWL", "UB": "MOVZBL"}[k]
                src = f.a[0] + ("+%d" % f.a[1] if f.a[1] else "")
                ctl.append("%s=%s" % (f.label, fao)); body.append("        %s  %s, %s" % (ins, src, nxt()))
            elif k == "DL":
                ctl.append("%s=!UL" % f.label); body.append("        MOVZWL  %s, %s" % (f.a[0], nxt()))
            elif k == "FLD":
                cbn, field, fmt = f.a
                ins = {"l": "MOVL  ", "w": "MOVZWL", "b": "MOVZBL"}[field.split("$")[1][0]]
                ctl.append("%s=%s" % (f.label, "!XL" if fmt == "X" else "!UL"))
                body.append("        %s  %s+%s, %s" % (ins, cbn, field.upper(), nxt()))
            elif k == "TPTR":
                cbn, pf, lf, limit = f.a
                ins = {"l": "MOVL  ", "w": "MOVZWL", "b": "MOVZBL"}[lf.split("$")[1][0]]
                ctl.append('%s="!AF"' % f.label)
                d = nxt()
                body.append("        %s  %s+%s, %s" % (ins, cbn, lf.upper(), d))
                if limit is not None:
                    cl = "%s_P%s" % (lbl, d.split("+")[1])
                    body += ["        CMPL    %s, #%d" % (d, limit), "        BLEQU   %s" % cl,
                             "        CLRL    %s" % d, "%s:" % cl]
                body.append("        MOVL    %s+%s, %s" % (cbn, pf.upper(), nxt()))
            elif k == "DTEXT":
                dn, limit = f.a
                ctl.append('%s="!AF"' % f.label)
                d = nxt()
                body.append("        MOVZWL  %s, %s" % (dn, d))
                if limit is not None:
                    cl = "%s_D%s" % (lbl, d.split("+")[1])
                    body += ["        CMPL    %s, #%d" % (d, limit), "        BLEQU   %s" % cl,
                             "        CLRL    %s" % d, "%s:" % cl]
                body.append("        MOVL    %s+4, %s" % (dn, nxt()))
            elif k == "DTAIL":
                dn, n = f.a
                ctl.append('%s="!AF"' % f.label)
                d1, d2 = nxt(), nxt()
                cl = "%s_T%s" % (lbl, d1.split("+")[1])
                body += ["        MOVZWL  %s, R0" % dn, "        MOVL    %s+4, R1" % dn,
                         "        SUBL3   #%d, R0, R2" % n, "        BLEQ    %s" % cl,
                         "        ADDL2   R2, R1", "        MOVL    #%d, R0" % n, "%s:" % cl,
                         "        MOVL    R0, %s" % d1, "        MOVL    R1, %s" % d2]
            elif k == "BIT":
                ctl.append("%s=!UL" % f.label)
                body.append("        EXTZV   #%d, #1, %s, %s" % (f.a[1], f.a[0], nxt()))
            elif k == "EQ":
                d = nxt()
                cl = "%s_E%s" % (lbl, d.split("+")[1])
                ctl.append("%s=!UL" % f.label)
                ea = f.a[0] + ("+%d" % f.a[2] if f.a[2] else "")
                eb = f.a[1] + ("+%d" % f.a[3] if f.a[3] else "")
                body += ["        CLRL    %s" % d, "        CMPL    %s, %s" % (ea, eb),
                         "        BNEQ    %s" % cl, "        MOVL    #1, %s" % d, "%s:" % cl]
            elif k == "T":
                buf, lenvar, lk, clamp, limit, lenoff = f.a
                ctl.append('%s="!AF"' % f.label)
                d = nxt()
                body.append("        %s  %s%s, %s" % ("MOVZWL" if lk == "W" else "MOVL  ", lenvar,
                                                   "+%d" % lenoff if lenoff else "", d))
                if limit is not None:
                    cl = "%s_L%s" % (lbl, d.split("+")[1])
                    body += ["        CMPL    %s, #%d" % (d, limit), "        BLEQU   %s" % cl,
                             "        CLRL    %s" % d, "%s:" % cl]
                if clamp is not None:
                    cl = "%s_C%s" % (lbl, d.split("+")[1])
                    body += ["        CMPL    %s, #%d" % (d, clamp), "        BLEQU   %s" % cl,
                             "        MOVL    #%d, %s" % (clamp, d), "%s:" % cl]
                body.append("        MOVAB   %s, %s" % (buf, nxt()))
            elif k == "TF":
                buf, n = f.a
                ctl.append('%s="!AF"' % f.label)
                body.append("        MOVL    #%d, %s" % (n, nxt()))
                body.append("        MOVAB   %s, %s" % (buf, nxt()))
            elif k == "XB":
                buf, n, off = f.a
                ctl.append("%s=!%d(XB)" % (f.label, n) if n else "%s=" % f.label)
                for i in range(n):
                    body.append("        MOVZBL  %s+%d, %s" % (buf, off + i, nxt()))
            else:
                raise ValueError(k)
        assert slot[0] <= 64
        printers[sig] = (lbl, " ".join(ctl), body)
        return lbl

    def emit_print(code, cid, fields):
        assert "!" not in cid
        pr = printer(fields)
        cids.append(cid)
        code.append("        SPPR    CID%d, %s" % (len(cids) - 1, pr))

    def arg_push(code, a):
        if a is None:
            code.append("        PUSHL   #0")
        elif isinstance(a, int):
            code.append("        PUSHL   #%d" % (a & 0xFFFFFFFF))
        elif isinstance(a, V):
            if p.names[a.n] == "word":
                code.append("        MOVZWL  %s, R1" % a.n)
                code.append("        PUSHL   R1")
            else:
                code.append("        PUSHL   %s" % a.n)
        elif isinstance(a, R):
            code.append("        PUSHAB  %s+%d" % (a.n, a.off) if a.off else "        PUSHAB  %s" % a.n)
        elif isinstance(a, str):
            code.append("        PUSHAB  %s" % a)
        elif isinstance(a, AST):
            code.append("        PUSHAB  AST_%s" % a.n)
        elif isinstance(a, RT):
            assert p.names[a.n] == "desc"
            code.append("        PUSHAB  %s_T" % a.n)
        else:
            raise TypeError(a)

    def emit_ops(ops, code):
        for op in ops:
            t = op[0]
            if t in ("call", "do"):
                if t == "call":
                    _, cid, routine, args, fields, guard = op
                else:
                    _, routine, args = op
                    guard = False
                if guard:
                    hung[cid] = len(hung)
                    code.append("        SPGRD   GH%d" % hung[cid])
                for a in reversed(args):
                    arg_push(code, a)
                if t == "call":
                    code.append("        SPCALL  %s, %d" % (routine, len(args)))
                else:
                    code.append("        CALLS   #%d, G^%s" % (len(args), routine))
                if guard:
                    code.append("        SPUNG")
                if t == "call":
                    emit_print(code, cid, fields)
            elif t == "callq":
                _, into, routine, args, gcase = op
                if gcase:
                    hung[gcase] = len(hung)
                    code.append("        SPGRD   GH%d" % hung[gcase])
                for a in reversed(args):
                    arg_push(code, a)
                code.append("        CALLS   #%d, G^%s" % (len(args), routine))
                code.append("        MOVL    R0, %s" % into)
                if gcase:
                    code.append("        SPUNG")
            elif t == "show":
                emit_print(code, op[1], op[2])
            elif t == "setl":
                code.append("        MOVL    #%d, %s" % (op[2] & 0xFFFFFFFF, op[1]))
            elif t in ("setw", "setdlen"):
                code.append("        MOVW    #%d, %s" % (op[2] & 0xFFFF, op[1]))
            elif t == "andl":
                code.append("        BICL2   #^X%08X, %s" % (~op[2] & 0xFFFFFFFF, op[1]))
            elif t == "copyl":
                code.append("        MOVL    %s, %s" % (op[2], op[1]))
            elif t == "cset":
                _, cbn, field, v = op
                if isinstance(v, R):
                    code.append("        MOVAB   %s, %s+%s" % (v.n, cbn, field.upper()))
                else:
                    ins = {"l": "MOVL", "w": "MOVW", "b": "MOVB"}[field.split("$")[1][0]]
                    code.append("        %s    #%d, %s+%s" % (ins, v & 0xFFFFFFFF, cbn, field.upper()))
            elif t == "fill":
                _, bufn, text, byte = op
                if text is not None:
                    for i, ch in enumerate(text.encode("latin-1")):
                        code.append("        MOVB    #%d, %s+%d" % (ch, bufn, i))
                else:
                    kind, a0 = [(k, a) for k, n, a in p.data if n == bufn][0]
                    size = {"quad": 8, "long": 4, "word": 2}.get(kind) or a0[0]
                    code.append("        MOVC5   #0, (R0), #%d, #%d, %s" % (byte, size, bufn))
            elif t == "gosub":
                code.append("        CALLS   #0, SUB_%s" % op[1])
            else:
                raise ValueError(t)

    main = []
    emit_ops(p.ops, main)
    subs = []
    for name, ops in p.subs.items():
        subs.append("        .ENTRY  SUB_%s, ^M<R2,R3,R4,R5,R6,R7,R8,R9,R10,R11>" % name)
        emit_ops(ops, subs)
        subs.append("        RET")

    for i, cid in enumerate(cids):
        o.extend(_mac_ascid("CID%d" % i, cid))
    for lbl, ctl, body in printers.values():
        o.extend(_mac_ascid("%s_F" % lbl, ctl))
    for cid, i in hung.items():
        o.extend(_mac_ascid("GH%d" % i, cid + " HUNG"))
    w("        .PSECT  SP_CODE,EXE,NOWRT,LONG")
    for lbl, ctl, body in printers.values():
        w("        .ENTRY  %s, ^M<R2,R3,R4,R5,R6,R7,R8,R9,R10,R11>" % lbl)
        o.extend(body)
        w("        MOVW    #1024, FAODSC")
        w("        $FAOL_S CTRSTR=%s_F, OUTLEN=OUTLEN, OUTBUF=FAODSC, PRMLST=PRM" % lbl)
        w("        MOVW    OUTLEN, FAODSC")
        w("        PUSHAB  FAODSC")
        w("        CALLS   #1, G^LIB$PUT_OUTPUT")
        w("        RET")
    o.extend(subs)
    for n in p.asts:
        w("        .ENTRY  AST_%s, ^M<R2,R3,R4,R5,R6,R7,R8,R9,R10,R11>" % n)
        w("        INCL    %s" % n)
        w("        MOVL    4(AP), %s_P" % n)
        w("        INCL    ASTSEQ")
        w("        MOVL    ASTSEQ, %s_Q" % n)
        w("        RET")
    w("        .ENTRY  GUARD_AST, ^M<R2,R3,R4,R5,R6,R7,R8,R9,R10,R11>")
    w("        PUSHL   GCASE")
    w("        CALLS   #1, G^LIB$PUT_OUTPUT")
    w("        PUSHAB  FABORT")
    w("        CALLS   #1, G^LIB$PUT_OUTPUT")
    w("        $EXIT_S #1")
    w("        RET")
    w("        .ENTRY  START, ^M<R2,R3,R4,R5,R6,R7,R8,R9,R10,R11>")
    w("        $BINTIM_S TIMBUF=GDT, TIMADR=GDELTA")
    w("        PUSHAB  FBEGIN")
    w("        CALLS   #1, G^LIB$PUT_OUTPUT")
    o.extend(main)
    w("        PUSHAB  FEND")
    w("        CALLS   #1, G^LIB$PUT_OUTPUT")
    w("        MOVL    #1, R0")
    w("        RET")
    w("        .END    START")
    return "\n".join(o) + "\n"


# ===========================================================================
# C backend (OVMX)
# ===========================================================================
def _c_str(s):
    """a C string literal; every byte outside printable ASCII as an octal escape"""
    out = []
    for ch in s:
        o = ord(ch)
        if ch in "\\\"":
            out.append("\\" + ch)
        elif 0x20 <= o < 0x7F and ch != "?":
            out.append(ch)
        else:
            out.append("\\%03o" % o)
    return '"' + "".join(out) + '"'


def ovmx_absent():
    """routines OVMX does not provide yet (tools/oracle/semantic/ovmx_absent.txt):
    the C probe prints st=ABSENT for them instead of linking a call. A list
    OUTSIDE the specs, so implementing a routine (delete its line) does not
    change a spec and invalidate the real-VMS goldens captured from it."""
    path = os.path.join(HERE, "ovmx_absent.txt")
    if not os.path.exists(path):
        return set()
    return {ln.split("#", 1)[0].strip().upper() for ln in open(path) if ln.split("#", 1)[0].strip()}


PADARGS = 12


def gen_c(p):
    p.ovmx_absent = set(p.ovmx_absent) | (ovmx_absent() & p.routines)
    o = []
    w = o.append
    w("/* GENERATED by tools/oracle/semantic/spgen.py from specs/%s.py -- do not edit." % p.family)
    w(" * Semantic-oracle probe (rd vms-8d1), OVMX side. */")
    w("#include <stdio.h>")
    w("#include <stdlib.h>")
    w("#include <string.h>")
    w("#include <signal.h>")
    w("#include <descrip.h>")
    w("#include <iledef.h>")
    if any(k == "cb" for k, _, _ in p.data):
        w("#include <rms/fab.h>")
        w("#include <rms/rab.h>")
        w("#include <rms/nam.h>")
    w("")
    for r in sorted((p.routines - p.ovmx_absent) | {"SYS$SETIMR", "SYS$CANTIM", "SYS$BINTIM"}):
        w("extern int %s();" % r.lower())
    w("static unsigned int ST;")
    for kind, n, a in p.data:
        if kind == "long":
            w("static unsigned int %s = %uu;" % (n, a[0] & 0xFFFFFFFF))
        elif kind == "word":
            w("static unsigned short %s = %u;" % (n, a[0] & 0xFFFF))
        elif kind == "quad":
            w("static unsigned int %s[2];" % n)
        elif kind == "buf":
            w("static unsigned char %s[%d];" % (n, a[0]))
        elif kind == "desc":
            w("static char %s_T[] = %s;" % (n, _c_str(a[0])))
            w("static struct dsc$descriptor_s %s;" % n)
        elif kind == "bdesc":
            w("static struct dsc$descriptor_s %s;" % n)
        elif kind == "items":
            w("static ILE3 %s[%d];" % (n, len(a[0]) + 1))
        elif kind == "plist":
            w("static unsigned long long %s[%d];" % (n, len(a[0])))
        elif kind == "cb":
            w("static struct %s %s;" % (a[0], n))
        elif kind == "ddesc":
            w("static struct dsc$descriptor_s %s = {0, DSC$K_DTYPE_T, DSC$K_CLASS_D, 0};" % n)
    w("")
    w("static void sp_text(const unsigned char *b, unsigned int n)")
    w("{   /* FAO !AF: a non-printable byte prints as '.' */")
    w("    unsigned int i;")
    w("    for (i = 0; i < n; i++) putchar(b[i] >= 0x20 && b[i] < 0x7f ? b[i] : '.');")
    w("}")
    w("static const char *GCASE;")
    w("static unsigned int GDELTA[2];")
    w("static void crash_handler(int sig)")
    w("{   /* a service that faults the image: name the case it was running */")
    w('    printf("%s CRASHED signal=%d\\n=== SEMPROBE aborted END ===\\n", GCASE ? GCASE : "?", sig);')
    w("    fflush(stdout);")
    w("    exit(1);")
    w("}")
    w("static void guard_ast(int p)")
    w("{")
    w("    (void)p;")
    w('    printf("%s HUNG\\n=== SEMPROBE aborted END ===\\n", GCASE);')
    w("    fflush(stdout);")
    w("    exit(1);")
    w("}")

    def carg(a):
        if a is None:
            # an unprototyped call passes an int in the low half of a 64-bit
            # register; a null POINTER must be a full-width zero
            return "(void *)0"
        if isinstance(a, int):
            return "%uu" % (a & 0xFFFFFFFF)
        if isinstance(a, V):
            return "(unsigned int)" + a.n
        if isinstance(a, R):
            k = p.names[a.n]
            base = a.n if k in ("buf", "quad", "items", "plist") else "&" + a.n
            return "(void *)((char *)%s + %d)" % (base, a.off) if a.off else "(void *)" + base
        if isinstance(a, str):
            k = p.names[a]
            return a if k in ("buf", "quad", "items") else "&" + a
        if isinstance(a, AST):
            return "(void *)ast_" + a.n
        if isinstance(a, RT):
            return "(void *)%s_T" % a.n
        raise TypeError(a)

    def cargs(args):
        """the C argument list, padded with null pointers to PADARGS: a VAX/Alpha
        callee sees how many arguments it was given (the argument count) and
        treats the rest as omitted; an OVMX C callee has no count, so an omitted
        trailing optional argument must arrive as an explicit 0, as an OVMX
        program would write it"""
        a = [carg(x) for x in args]
        return ", ".join(a + ["(void *)0"] * max(0, PADARGS - len(a)))

    def caddr(n):
        return n if p.names[n] in ("buf", "quad", "items", "plist") else "&" + n

    def cprint(out, cid, fields, absent=False):
        parts = ['printf("%s");' % cid]
        for f in fields:
            k = f.kind
            if k == "ST":
                parts.append('printf(" %s=ABSENT");' % f.label if absent else 'printf(" %s=%%08X", ST);' % f.label)
            elif k in ("X", "U", "S", "UW", "XW", "UB"):
                ty = {"X": "unsigned int", "U": "unsigned int", "S": "int", "UW": "unsigned short",
                      "XW": "unsigned short", "UB": "unsigned char"}[k]
                fmt = {"X": "%08X", "U": "%u", "S": "%d", "UW": "%u", "XW": "%04X", "UB": "%u"}[k]
                val = "*(%s *)((unsigned char *)%s + %d)" % (ty, caddr(f.a[0]), f.a[1])
                parts.append('printf(" %s=%s", (%s)%s);' % (f.label, fmt, "int" if k == "S" else "unsigned int", val))
            elif k == "DL":
                parts.append('printf(" %s=%%u", (unsigned int)%s.dsc$w_length);' % (f.label, f.a[0]))
            elif k == "FLD":
                cbn, field, fmt = f.a
                parts.append('printf(" %s=%s", (unsigned int)%s.%s);' % (f.label, "%08X" if fmt == "X" else "%u", cbn, field))
            elif k == "TPTR":
                cbn, pf, lf, limit = f.a
                ln = "(unsigned int)%s.%s" % (cbn, lf)
                if limit is not None:
                    ln = "(%s > %du ? 0u : %s)" % (ln, limit, ln)
                parts.append('printf(" %s=\\"");' % f.label)
                parts.append("if (%s.%s) sp_text((const unsigned char *)%s.%s, %s);" % (cbn, pf, cbn, pf, ln))
                parts.append("putchar('\"');")
            elif k == "DTEXT":
                dn, limit = f.a
                ln = "(unsigned int)%s.dsc$w_length" % dn
                if limit is not None:
                    ln = "(%s > %du ? 0u : %s)" % (ln, limit, ln)
                parts.append('printf(" %s=\\"");' % f.label)
                parts.append("if (%s.dsc$a_pointer) sp_text((const unsigned char *)%s.dsc$a_pointer, %s);" % (dn, dn, ln))
                parts.append("putchar('\"');")
            elif k == "DTAIL":
                dn, n = f.a
                parts.append('printf(" %s=\\"");' % f.label)
                parts.append("{ unsigned int l_ = %s.dsc$w_length; const unsigned char *a_ = (const unsigned char *)%s.dsc$a_pointer;"
                             " if (l_ > %du) { a_ += l_ - %du; l_ = %du; } if (a_) sp_text(a_, l_); }" % (dn, dn, n, n, n))
                parts.append("putchar('\"');")
            elif k == "BIT":
                parts.append('printf(" %s=%%u", (unsigned int)((%s >> %d) & 1u));' % (f.label, f.a[0], f.a[1]))
            elif k == "EQ":
                parts.append('printf(" %s=%%u", (unsigned int)(*(unsigned int *)((unsigned char *)%s + %d)'
                             ' == *(unsigned int *)((unsigned char *)%s + %d)));'
                             % (f.label, caddr(f.a[0]), f.a[2], caddr(f.a[1]), f.a[3]))
            elif k == "T":
                buf, lenvar, lk, clamp, limit, lenoff = f.a
                ln = "(unsigned int)*(%s *)((unsigned char *)%s + %d)" % (
                    "unsigned short" if lk == "W" else "unsigned int", caddr(lenvar), lenoff)
                if limit is not None:
                    ln = "(%s > %du ? 0u : %s)" % (ln, limit, ln)
                if clamp is not None:
                    ln = "(%s > %du ? %du : %s)" % (ln, clamp, clamp, ln)
                b = buf if p.names[buf] == "buf" else "(unsigned char *)&" + buf
                parts.append('printf(" %s=\\"");' % f.label)
                parts.append("sp_text(%s, %s);" % (b, ln))
                parts.append("putchar('\"');")
            elif k == "TF":
                buf, n = f.a
                parts.append('printf(" %s=\\"");' % f.label)
                parts.append("sp_text(%s, %d);" % (buf, n))
                parts.append("putchar('\"');")
            elif k == "XB":
                buf, n, off = f.a
                b = buf if p.names[buf] == "buf" else "((unsigned char *)&%s)" % buf
                parts.append('printf(" %s=");' % f.label)
                parts.append('{ int i_; for (i_ = 0; i_ < %d; i_++) printf("%%02X", %s[%d + i_]); }' % (n, b, off))
        parts.append("putchar('\\n');")
        out.append("    " + " ".join(parts))

    def emit_ops(ops, out):
        for op in ops:
            t = op[0]
            if t in ("call", "do"):
                if t == "call":
                    _, cid, routine, args, fields, guard = op
                else:
                    _, routine, args = op
                    guard = False
                if guard:
                    out.append('    fflush(stdout); GCASE = "%s";' % cid)
                    out.append("    sys$setimr(30, GDELTA, guard_ast, 0x5E3A, 0);")
                absent = routine in p.ovmx_absent
                if t == "do":
                    out.append('    GCASE = "(setup %s)";' % routine)
                call = "%s(%s)" % (routine.lower(), cargs(args))
                if absent:
                    out.append("    /* %s: not provided by OVMX (spec: absent_on_ovmx) */" % routine)
                else:
                    out.append("    %s%s;" % ("ST = " if t == "call" else "", call))
                if guard:
                    out.append("    sys$cantim(0x5E3A, 0);")
                if t == "call":
                    cprint(out, cid, fields, absent)
            elif t == "callq":
                _, into, routine, args, gcase = op
                if gcase:
                    out.append('    fflush(stdout); GCASE = "%s";' % gcase)
                    out.append("    sys$setimr(30, GDELTA, guard_ast, 0x5E3A, 0);")
                if routine in p.ovmx_absent:
                    out.append("    %s = 0xFFFFFFFFu; /* %s: not provided by OVMX */" % (into, routine))
                else:
                    out.append("    %s = %s(%s);" % (into, routine.lower(), cargs(args)))
                if gcase:
                    out.append("    sys$cantim(0x5E3A, 0);")
            elif t == "show":
                cprint(out, op[1], op[2])
            elif t == "setl":
                out.append("    *(unsigned int *)%s = %uu;" % (caddr(op[1]), op[2] & 0xFFFFFFFF))
            elif t == "setw":
                out.append("    *(unsigned short *)%s = %u;" % (caddr(op[1]), op[2] & 0xFFFF))
            elif t == "setdlen":
                out.append("    %s.dsc$w_length = %u;" % (op[1], op[2] & 0xFFFF))
            elif t == "andl":
                out.append("    %s &= 0x%08Xu;" % (op[1], op[2] & 0xFFFFFFFF))
            elif t == "copyl":
                out.append("    *(unsigned int *)%s = *(unsigned int *)%s;" % (caddr(op[1]), caddr(op[2])))
            elif t == "cset":
                _, cbn, field, v = op
                if isinstance(v, R):
                    out.append("    %s.%s = (void *)%s;" % (cbn, field, caddr(v.n)))
                else:
                    out.append("    %s.%s = %uu;" % (cbn, field, v & 0xFFFFFFFF))
            elif t == "fill":
                _, bufn, text, byte = op
                if text is not None:
                    out.append("    memcpy(%s, %s, %d);" % (bufn, _c_str(text), len(text)))
                else:
                    out.append("    memset(%s, %d, sizeof %s);" % (bufn, byte, bufn))
            elif t == "gosub":
                out.append("    sub_%s();" % op[1])
            else:
                raise ValueError(t)

    for n in p.asts:
        w("static void ast_%s(unsigned long prm)" % n)
        w("{ %s++; %s_P = (unsigned int)prm; ASTSEQ++; %s_Q = ASTSEQ; }" % (n, n, n))
    for name, ops in p.subs.items():
        w("static void sub_%s(void)" % name)
        w("{")
        emit_ops(ops, o)
        w("}")
    w("")
    w("int main(void)")
    w("{")
    w('    static char gdt[] = "0 00:00:10.00";')
    w("    struct dsc$descriptor_s gd;")
    w("    gd.dsc$w_length = sizeof gdt - 1; gd.dsc$b_dtype = DSC$K_DTYPE_T;")
    w("    gd.dsc$b_class = DSC$K_CLASS_S; gd.dsc$a_pointer = gdt;")
    w("    sys$bintim(&gd, GDELTA);")
    w("    (void)sp_text; (void)guard_ast;")
    w("    signal(SIGSEGV, crash_handler); signal(SIGBUS, crash_handler); signal(SIGILL, crash_handler);")
    for kind, n, a in p.data:
        if kind == "buf":
            w("    memset(%s, %d, sizeof %s);" % (n, a[1], n))
            if a[2]:
                w("    memcpy(%s, %s, %d);" % (n, _c_str(a[2]), len(a[2])))
        elif kind == "desc":
            w("    %s.dsc$w_length = %d; %s.dsc$b_dtype = DSC$K_DTYPE_T; %s.dsc$b_class = DSC$K_CLASS_S;"
              % (n, len(a[0]), n, n))
            w("    %s.dsc$a_pointer = %s_T;" % (n, n))
        elif kind == "bdesc":
            w("    %s.dsc$w_length = %d; %s.dsc$b_dtype = DSC$K_DTYPE_T; %s.dsc$b_class = DSC$K_CLASS_S;"
              % (n, a[1], n, n))
            w("    %s.dsc$a_pointer = (char *)%s;" % (n, a[0]))
        elif kind == "items":
            for i, (blen, code, bufn, retn) in enumerate(a[0]):
                w("    %s[%d].ile3$w_length = %d; %s[%d].ile3$w_code = %d;" % (n, i, blen & 0xFFFF, n, i, code & 0xFFFF))
                w("    %s[%d].ile3$ps_bufaddr = %s; %s[%d].ile3$ps_retlen_addr = %s;"
                  % (n, i, ("(void *)" + ("&" if p.names[bufn] in ("long", "word") else "") + bufn) if bufn else "0",
                     n, i, ("(void *)&" + retn) if retn else "0"))
            w("    memset(&%s[%d], 0, sizeof %s[0]);" % (n, len(a[0]), n))
        elif kind == "cb":
            w("    %s = cc$rms_%s;" % (n, a[0].lower()))
        elif kind == "plist":
            for i, v in enumerate(a[0]):
                w("    %s[%d] = %s;" % (n, i, "(unsigned long long)(unsigned long)" + caddr(v.n) if isinstance(v, R)
                                         else "%uu" % (v & 0xFFFFFFFF)))
    w('    printf("=== SEMPROBE %s BEGIN ===\\n");' % p.family)
    emit_ops(p.ops, o)
    w('    printf("=== SEMPROBE %s END ===\\n");' % p.family)
    w("    return 1;")
    w("}")
    return "\n".join(o) + "\n"



def load(spec):
    g = {"__name__": "spec", "__file__": spec}
    for k, v in list(globals().items()):
        if not k.startswith("_") or k in ("_K",):
            g[k] = v
    exec(compile(open(spec).read(), spec, "exec"), g)
    return g["PROBE"]


def main(argv):
    if len(argv) != 3 or argv[1] not in ("--mar", "--c", "--list"):
        print(__doc__)
        return 2
    p = load(argv[2])
    if argv[1] == "--mar":
        sys.stdout.write(gen_mar(p))
    elif argv[1] == "--c":
        sys.stdout.write(gen_c(p))
    else:
        print("\n".join(p.cases))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
