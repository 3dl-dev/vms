#!/usr/bin/env python3
"""gen_heldout_run.py -- build the driven held-out run for rd vms-c6e leg 2.

ONE generator writes all four artifacts, so the names the VAX locks, the names
in the prediction file and the names in the lab report can never drift apart:

    names.tsv      the name set, with the lock flavour each is driven with
    predicted.tsv  the hash this tree predicts for every PRE-REGISTERED triple
    C6EDRV.MAR     the MACRO-32 driver, with the table assembled IN
    C6EDRV.COM     MACRO + LINK + RUN

WHY THE TABLE IS ASSEMBLED IN rather than read from a data file: the name set
deliberately contains NUL, high-bit and control bytes (real VMS resource names
do -- see the LMF$<a2>_ names in the corpus), and a data file would have to
survive FTP, an RMS record format and a record terminator before the VAX ever
locked anything. A `.BYTE` list in the source is plain ASCII all the way to
the assembler, so what the VAX locks is exactly what was predicted.

    gen_heldout_run.py --outdir tests/lab/captures/vms-c6e-heldout-20261008
    gen_heldout_run.py --outdir ... --check      # the drift gate
"""
import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refhash  # noqa: E402

# Lock flavours. A lock's access mode is the access mode of the $ENQ caller;
# LCK$M_SYSTEM selects the system-wide resource namespace (group 0) instead of
# the caller's UIC group. All four reach the directory, so all four put an
# op-0x01 on the wire.
FLV_USER_SYS = 0   # user mode  + LCK$M_SYSTEM -> mode 3, group 0
FLV_USER_GRP = 1   # user mode,  no SYSTEM     -> mode 3, group = UIC group
FLV_EXEC_GRP = 2   # exec mode,  no SYSTEM     -> mode 1, group = UIC group
FLV_EXEC_SYS = 3   # exec mode  + LCK$M_SYSTEM -> mode 1, group 0

FLV_NAME = {FLV_USER_SYS: "user+SYSTEM", FLV_USER_GRP: "user+group",
            FLV_EXEC_GRP: "exec+group", FLV_EXEC_SYS: "exec+SYSTEM"}
FLV_MODE = {FLV_USER_SYS: 3, FLV_USER_GRP: 3, FLV_EXEC_GRP: 1, FLV_EXEC_SYS: 1}
SYS_FLAVOURS = (FLV_USER_SYS, FLV_EXEC_SYS)

# The UIC groups the run recipe uses. SYSTEM is [1,4]; the recipe also creates
# [300,1] and [16382,1], which between them exercise group-word bits 0..13.
# The whole corpus only ever showed group 0 and group 1.
UIC_GROUPS = (1, 300, 16382)

RECSZ = 33          # length byte, flavour byte, 31 name bytes
NAME_FIELD = refhash.NAME_MAX


# ------------------------------------------------------------------ #
# The name set
# ------------------------------------------------------------------ #

def length_sweep():
    """One name per length 1..31. The corpus is missing lengths 1, 2, 4, 6, 9,
    19, 20, 23, 28, 29 and 31 entirely; a full sweep is cheaper to reason
    about than a subset and costs the lab nothing extra."""
    out = []
    filler = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"
    for n in range(1, NAME_FIELD + 1):
        if n == 1:
            name = b"A"
        elif n == 2:
            name = b"C6"
        elif n == 3:
            name = b"C6E"
        else:
            name = (("C6E%02d" % n).encode("ascii") + filler)[:n]
        out.append((name, FLV_USER_SYS,
                    "length sweep, %d byte%s" % (n, "" if n == 1 else "s")))
    return out


def byte_value_stress():
    """Byte values the corpus never exercised, plus shapes VMS itself uses."""
    return [
        (b"0123456789", FLV_USER_SYS, "digits only"),
        (b"C6E$$$$$$$", FLV_USER_SYS, "dollar run, the VMS facility separator"),
        (b"C6E_UNDER_SCORE", FLV_USER_SYS, "underscores"),
        (b"C6E.:-+=*%@", FLV_USER_SYS, "punctuation run"),
        (b"C6E<>[]()?!", FLV_USER_SYS, "bracket and quote-class punctuation"),
        (b"c6e_lower_case", FLV_USER_SYS, "lower case, which VMS does not fold"),
        (b"C6E       ", FLV_USER_SYS, "trailing spaces, as F11B$a names carry"),
        (b"C6ENUL\x00\x00\x00", FLV_USER_SYS, "embedded NUL bytes"),
        (b"C6E\x01\x02\x1f\x7f", FLV_USER_SYS, "control bytes 01 02 1F 7F"),
        (b"C6E\x80\x81\xfe\xff", FLV_USER_SYS, "high-bit bytes 80 81 FE FF"),
        (b"\xff" * NAME_FIELD, FLV_USER_SYS,
         "31 bytes of FF, every name bit set"),
        (b"\x00" * NAME_FIELD, FLV_USER_SYS,
         "31 NUL bytes, every name bit clear"),
        (b"\xa2_C6E", FLV_USER_SYS, "leading A2, as the real LMF$ names do"),
        (b"C6E\x55\xaa\x55\xaa", FLV_USER_SYS, "alternating 55 AA"),
        (b"C6E\x80\x00\x00\x00", FLV_USER_SYS,
         "one high bit alone in a longword"),
    ]


def mode_and_group_set():
    """The flavours that move the mode byte off 3 and the group word off 0."""
    out = []
    for tag, flavour in (("X", FLV_EXEC_SYS), ("G", FLV_USER_GRP),
                         ("E", FLV_EXEC_GRP)):
        for j in range(1, 6):
            out.append((("C6E%s%02d" % (tag, j)).encode("ascii"), flavour,
                        FLV_NAME[flavour]))
    return out


def name_set():
    names = length_sweep() + byte_value_stress() + mode_and_group_set()
    seen = set()
    for name, _, _ in names:
        if name in seen:
            raise AssertionError("duplicate name %r" % name)
        if not 1 <= len(name) <= NAME_FIELD:
            raise AssertionError("bad length %r" % name)
        seen.add(name)
    return names


def triples(flavour):
    """The (mode, group) pairs a flavour is PRE-REGISTERED for.

    A SYSTEM lock has one: group 0. A group lock has one per UIC group the
    recipe runs the image under -- that is a pre-registration OF THE RECIPE,
    not a spread bet: a group the recipe never asked for is reported by
    check_heldout_run.py as NOT pre-registered rather than scored.
    """
    mode = FLV_MODE[flavour]
    if flavour in SYS_FLAVOURS:
        return [(mode, 0)]
    return [(mode, g) for g in UIC_GROUPS]


def pre_registered(names):
    for name, flavour, _ in names:
        for mode, group in triples(flavour):
            yield name, mode, group


# ------------------------------------------------------------------ #
# names.tsv / predicted.tsv
# ------------------------------------------------------------------ #

def write_names(path, names):
    with open(path, "w") as fh:
        fh.write("# rd vms-c6e leg 2: the names a real OpenVMS VAX V7.3 is asked\n"
                 "# to $ENQ. GENERATED by tools/cluster/dlm_hash/gen_heldout_run.py.\n")
        fh.write("name_hex\tname_len\tflavour\tflavour_name\texpect_mode\t"
                 "expect_group\tnote\n")
        for name, flavour, note in names:
            group = "0" if flavour in SYS_FLAVOURS else "UIC"
            fh.write("%s\t%d\t%d\t%s\t%d\t%s\t%s\n"
                     % (name.hex(), len(name), flavour, FLV_NAME[flavour],
                        FLV_MODE[flavour], group, note))


def write_predicted(path, names):
    with open(path, "w") as fh:
        fh.write("# rd vms-c6e leg 2: the value THIS TREE PREDICTS for every\n"
                 "# pre-registered (name, mode, group). Committed and pushed\n"
                 "# BEFORE the VAX is asked to lock anything -- the commit\n"
                 "# timestamp is the proof. Nothing here is a capture.\n")
        fh.write("name_hex\tname_len\tmode\tgroup\tpredicted\n")
        for name, mode, group in pre_registered(names):
            fh.write("%s\t%d\t%d\t%d\t0x%08x\n"
                     % (name.hex(), len(name), mode, group,
                        refhash.name_hash(name, mode, group)))


# ------------------------------------------------------------------ #
# C6EDRV.MAR
# ------------------------------------------------------------------ #

def mar_table(names):
    """Three source lines per record, so no line approaches MACRO-32's
    comfortable width: the header pair, then the 31 name bytes in two rows."""
    lines = []
    for name, flavour, note in names:
        body = list(name) + [0] * (NAME_FIELD - len(name))
        lines.append(" .BYTE %d,%d\t\t; %s"
                     % (len(name), flavour, note.replace(";", ",")))
        lines.append(" .BYTE " + ",".join(str(b) for b in body[:16]))
        lines.append(" .BYTE " + ",".join(str(b) for b in body[16:]))
    return lines


MAR_TEMPLATE = """ .TITLE C6EDRV
;
; C6EDRV -- rd vms-c6e leg 2. $ENQ every name in the table below, so a real
; OpenVMS VAX V7.3 puts ITS OWN directory-hash value for each one on the
; cluster wire; hold them ten seconds so the lab can SHOW RESOURCE; release.
;
; GENERATED by tools/cluster/dlm_hash/gen_heldout_run.py. Do not hand-edit:
; the table must stay byte-identical to names.tsv and predicted.tsv, and the
; drift gate (check_heldout_run.py) fails if it does not.
;
; Record = length byte, flavour byte, 31 name bytes (%(recsz)d bytes).
; Flavour: 0 user+LCK$M_SYSTEM, 1 user+group, 2 exec+group, 3 exec+LCK$M_SYSTEM.
; The exec flavours go through $CMEXEC so the lock's access mode is exec.
;
; Usage:  RUN C6EDRV            all flavours (needs SYSLCK and CMEXEC)
;         C6EDRV :== $dev:[dir]C6EDRV
;         C6EDRV GRP            flavours 1 and 2 only (needs CMEXEC, no SYSLCK)
;
 $LCKDEF
 $SSDEF
NREC = %(nrec)d
RECSZ = %(recsz)d
 .PSECT DAT,WRT,NOEXE,LONG
TABLE:
%(table)s
 .ALIGN LONG
LKIDS: .BLKL NREC
NGOT: .LONG 0
CURREC: .ADDRESS 0
CURFLG: .LONG 0
NOARG: .LONG 0
GRPONLY: .LONG 0
LKSB: .BLKB 8
RESDSC: .WORD 0
 .BYTE 14
 .BYTE 1
 .ADDRESS 0
DELTA: .LONG ^XFA0A1F00, -1
CMDBUF: .BLKB 32
CMDDSC: .WORD 32
 .BYTE 14
 .BYTE 1
 .ADDRESS CMDBUF
CMDLEN: .WORD 0
 .WORD 0
 .PSECT COD,EXE,NOWRT
;
; ENQ1 -- lock the record CURREC points at with the flags in CURFLG. Runs in
; whatever access mode it is called in, and that is what sets the lock's mode.
;
ENQ1: .WORD ^M<R2>
 MOVL CURREC,R2
 MOVZBL (R2),R0
 MOVW R0,RESDSC
 MOVAB 2(R2),RESDSC+4
 $ENQW_S EFN=#1,LKMODE=#LCK$K_NLMODE,LKSB=LKSB,FLAGS=CURFLG,RESNAM=RESDSC
 BLBC R0,100$
 MOVL NGOT,R1
 MOVL LKSB+4,LKIDS[R1]
 INCL NGOT
100$: RET
;
; DEQALL -- release every lock ENQ1 took. Called in exec mode, which may
; dequeue a user-mode lock as well as an exec-mode one.
;
DEQALL: .WORD ^M<R3>
 CLRL R3
200$: CMPL R3,NGOT
 BLSSU 205$
 BRB 210$
205$: $DEQ_S LKID=LKIDS[R3]
 INCL R3
 BRW 200$
210$: RET
;
 .ENTRY C6EDRV,^M<R2,R3,R4,R5,R6>
 PUSHAW CMDLEN
 PUSHL #0
 PUSHAB CMDDSC
 CALLS #3,G^LIB$GET_FOREIGN
 CLRL GRPONLY
 TSTW CMDLEN
 BEQL 300$
 CMPB CMDBUF,#^A/G/
 BNEQ 300$
 MOVL #1,GRPONLY
300$: CLRL NGOT
 MOVAB TABLE,R2
 CLRL R3
;
; Every branch that spans the loop body is a BRW: the body is around eighty
; bytes today and a conditional branch only reaches 127, so a later name or
; flavour must not be able to push it out of range.
;
310$: CMPL R3,#NREC
 BLSSU 315$
 BRW 400$
315$: MOVZBL 1(R2),R4
 BLBC GRPONLY,330$
 CMPL R4,#0
 BEQL 380$
 CMPL R4,#3
 BEQL 380$
330$: MOVL R2,CURREC
 CLRL CURFLG
 CMPL R4,#0
 BEQL 340$
 CMPL R4,#3
 BNEQ 350$
340$: MOVL #LCK$M_SYSTEM,CURFLG
350$: CMPL R4,#2
 BLSSU 360$
 $CMEXEC_S ROUTIN=ENQ1,ARGLST=NOARG
 BRB 380$
360$: CALLS #0,ENQ1
380$: ADDL2 #RECSZ,R2
 INCL R3
 BRW 310$
400$: $SETIMR_S EFN=#2,DAYTIM=DELTA
 $WAITFR_S EFN=#2
 $CMEXEC_S ROUTIN=DEQALL,ARGLST=NOARG
 MOVL #SS$_NORMAL,R0
 RET
 .END C6EDRV
"""


def write_mar(path, names):
    with open(path, "w") as fh:
        fh.write(MAR_TEMPLATE % {"nrec": len(names), "recsz": RECSZ,
                                 "table": "\n".join(mar_table(names))})


COM_TEXT = """$! C6EDRV.COM -- rd vms-c6e leg 2. MACRO + LINK + RUN the driver.
$! GENERATED by tools/cluster/dlm_hash/gen_heldout_run.py.
$!
$! P1 is passed straight to the image: blank for all flavours (needs SYSLCK
$! and CMEXEC, i.e. SYSTEM), or GRP for the group-lock flavours only (needs
$! CMEXEC, no SYSLCK) when running from a second UIC group.
$!
$ SET VERIFY
$ SHOW PROCESS/PRIVILEGE
$ SHOW PROCESS/ALL
$ MACRO C6EDRV
$ LINK C6EDRV
$ C6EDRV :== $'F$ENVIRONMENT("DEFAULT")'C6EDRV.EXE
$ C6EDRV 'P1'
$ WRITE SYS$OUTPUT "C6EDRV status: " + F$STRING($STATUS)
$ EXIT
"""


def write_com(path, _names):
    with open(path, "w") as fh:
        fh.write(COM_TEXT)


ARTIFACTS = (("names.tsv", write_names), ("predicted.tsv", write_predicted),
             ("C6EDRV.MAR", write_mar), ("C6EDRV.COM", write_com))


def emit(outdir, names):
    for fname, writer in ARTIFACTS:
        writer(os.path.join(outdir, fname), names)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--check", action="store_true",
                    help="regenerate into a scratch dir and fail on any "
                         "difference from the committed files")
    args = ap.parse_args(argv)

    names = name_set()
    ntrip = sum(1 for _ in pre_registered(names))

    if not args.check:
        emit(args.outdir, names)
        print("%d names, %d pre-registered triples written to %s"
              % (len(names), ntrip, args.outdir))
        return 0

    tmp = tempfile.mkdtemp()
    emit(tmp, names)
    drift = [f for f, _ in ARTIFACTS
             if open(os.path.join(args.outdir, f), "rb").read()
             != open(os.path.join(tmp, f), "rb").read()]
    for d in drift:
        sys.stderr.write("DRIFT: %s is not what the generator writes\n" % d)
    print("%d names, %d pre-registered triples: %s"
          % (len(names), ntrip, "DRIFT" if drift else "OK"))
    return 1 if drift else 0


if __name__ == "__main__":
    sys.exit(main())
