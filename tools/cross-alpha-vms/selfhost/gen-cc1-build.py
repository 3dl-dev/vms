#!/usr/bin/env python3
"""gen-cc1-build.py -- the DCL command procedure that builds cc1 on OVMX (vms-465).

usage: gen-cc1-build.py <plan-dir> <roots-file> <out.com> [--only N]

<plan-dir> holds the records planlog wrote while GCC's own Makefiles ran
`make all-gcc` for the alpha-dec-vms host (build-host-gcc.sh): one file per
compiler or archiver invocation -- time, working directory, program, every
argument. <roots-file> names the trees those records refer to (SRC=, BLD=,
HOST=, P32=). This script follows cc1's link record back through the archives
it names to every compile that made an object in it, and writes BUILD_CC1.COM:
for each of those compiles, in the order GCC's build ran them, the same
arguments (paths moved to the trees staged on the OVMX disk) in a response
file the native driver reads (@file, so DCL changes the case of none of
them), each run in its record's working directory (SET DEFAULT), as make
runs it, so a relative name GCC's build wrote into a generated source (a
#line "../../src/gcc-14.2.0/gcc/config/alpha/alpha.md") names the same file;
then the archives, made by the port's AR; then cc1, linked by the native g++
driver. Only the dependency-tracking options (-MT -MMD -MP -MF),
which write files for make to read, are left out: they do not change the code.

The trees on OVMX (rooted logicals the proof defines):
  /GCCSRC/SRC   the source trees: gcc-14.2.0/ is GCC's  (SRC=, the host's src/)
  /GCCSRC/BLD   GCC's build tree (generated sources and headers)  (BLD=)
  /GCCSRC/HOST  GMP/MPFR/MPC, installed      (HOST=)
  /GCCOBJ/<lib> the objects and archives this procedure writes
  /GNU/CXX      the C++ headers; SYS$SYSTEM: libstdc++.a (P32= on the host)

--only N writes a procedure for the first N compiles only (no archives, no
link): a timing probe. --trace N types the response file of the first N
compiles. --extra ARG (repeatable) adds ARG to every compile (a diagnosis:
-ftime-report); --link-extra ARG to the link (-v: the driver's and ld's commands). --pick REGEX keeps only the compiles whose object name
matches (a probe of chosen sources; no archives, no link). --shard K/N keeps
every Nth compile from the Kth (1-based) in GCC's build order, no archives and
no link: one of N CI jobs that build cc1 together (rd vms-b85); the job that
links finds every object made and compiles none of them again.
"""
import os
import sys

DEP_OPTS_WITH_ARG = {"-MT", "-MF", "-MQ"}
DEP_OPTS = {"-MMD", "-MP", "-MD"}
COMPILERS = {"alpha-dec-vms-gcc": "GCC", "alpha-dec-vms-g++": "GXX"}


def load(plan):
    recs = []
    for f in sorted(os.listdir(plan)):
        if not f.endswith(".rec"):
            continue
        p = open(os.path.join(plan, f), "rb").read().split(b"\0")
        if p and p[-1] == b"":
            p = p[:-1]
        p = [x.decode() for x in p]
        recs.append({"t": int(p[0]), "cwd": p[1], "tool": p[2], "args": p[3:]})
    recs.sort(key=lambda r: r["t"])
    return recs


def roots(path):
    r = {}
    for line in open(path):
        line = line.strip()
        if "=" in line:
            k, v = line.split("=", 1)
            r[k] = os.path.normpath(v)
    for k in ("SRC", "BLD", "HOST", "P32"):
        if k not in r:
            sys.exit("gen-cc1-build: %s missing from %s" % (k, path))
    return r


def absn(cwd, p):
    return os.path.normpath(p if p.startswith("/") else os.path.join(cwd, p))


class Mapper:
    def __init__(self, r):
        self.r = r
        self.tree = [(r["SRC"], "/GCCSRC/SRC"), (r["BLD"], "/GCCSRC/BLD"), (r["HOST"], "/GCCSRC/HOST")]

    def path(self, cwd, p):
        a = absn(cwd, p)
        for host, vms in self.tree:
            if a == host or a.startswith(host + "/"):
                return vms + a[len(host):]
        sys.exit("gen-cc1-build: a path outside the staged trees: %s (cwd %s)" % (p, cwd))

    def vms_dir(self, cwd):
        """a working directory as the DCL directory spec SET DEFAULT takes:
        /GCCSRC/BLD/gcc -> GCCSRC:[BLD.gcc] (a dot in a name is ^.)"""
        parts = self.path(cwd, ".").strip("/").split("/")
        sub = [x.replace("^", "^^").replace(".", "^.") for x in parts[1:]]
        return "%s:[%s]" % (parts[0], ".".join(sub) if sub else "000000")

    def compile_args(self, rec, out_vms):
        """the record's arguments as the native driver gets them"""
        a, cwd, out = rec["args"], rec["cwd"], []
        P32 = self.r["P32"]
        i = 0
        while i < len(a):
            x = a[i]
            if x in DEP_OPTS_WITH_ARG:
                i += 2
                continue
            if x in DEP_OPTS:
                i += 1
                continue
            if x == "-o":
                out += ["-o", out_vms]
                i += 2
                continue
            if x == "-isystem" and i + 1 < len(a) and a[i + 1].startswith(P32 + "/include/c++/"):
                tail = a[i + 1][len(P32 + "/include/c++/"):].split("/", 1)
                out += ["-isystem", "/GNU/CXX" + ("/" + tail[1].upper() if len(tail) > 1 else "")]
                i += 2
                continue
            if x == "-L" + P32 + "/lib":
                out.append("-L/SYS$SYSTEM")
                i += 1
                continue
            if x == "-I" and i + 1 < len(a):
                # the separate form (libbacktrace's `-I ../../src/.../include')
                out += ["-I", self.path(cwd, a[i + 1])]
                i += 2
                continue
            if x.startswith("-I") and len(x) > 2:
                out.append("-I" + self.path(cwd, x[2:]))
                i += 1
                continue
            if not x.startswith("-") and (x.endswith(".c") or x.endswith(".cc")):
                out.append(self.path(cwd, x))
                i += 1
                continue
            out.append(x)
            i += 1
        if "-o" not in a:
            out += ["-o", out_vms]
        return out


def rsp_quote(arg):
    """one argument as libiberty's buildargv reads it back unchanged"""
    return "".join("\\" + c if c in " \t\n\\\"'" else c for c in arg)


def dcl_str(s):
    if "'" in s:
        sys.exit("gen-cc1-build: an apostrophe DCL would substitute: %r" % s)
    return '"' + s.replace('"', '""') + '"'


def main():
    argv = sys.argv[1:]
    only, trace, extra, pick, shard = None, 0, [], None, None
    if "--shard" in argv:
        k = argv.index("--shard")
        try:
            sk, sn = (int(x) for x in argv[k + 1].split("/"))
        except ValueError:
            sys.exit("gen-cc1-build: --shard K/N")
        if not 1 <= sk <= sn:
            sys.exit("gen-cc1-build: --shard K/N needs 1 <= K <= N")
        shard = (sk, sn)
        del argv[k:k + 2]
    if "--pick" in argv:
        k = argv.index("--pick")
        pick = argv[k + 1]
        del argv[k:k + 2]
    while "--extra" in argv:
        k = argv.index("--extra")
        extra.append(argv[k + 1])
        del argv[k:k + 2]
    link_extra = []
    while "--link-extra" in argv:
        k = argv.index("--link-extra")
        link_extra.append(argv[k + 1])
        del argv[k:k + 2]
    if "--trace" in argv:
        k = argv.index("--trace")
        trace = int(argv[k + 1])
        del argv[k:k + 2]
    if "--only" in argv:
        k = argv.index("--only")
        only = int(argv[k + 1])
        del argv[k:k + 2]
    if len(argv) != 3:
        sys.exit(__doc__)
    plan, rootsf, outcom = argv
    r = roots(rootsf)
    m = Mapper(r)
    recs = [x for x in load(plan) if "/build-" not in x["cwd"]]

    def out_of(rec):
        a = rec["args"]
        if "-o" in a:
            return absn(rec["cwd"], a[a.index("-o") + 1])
        src = [x for x in a if not x.startswith("-") and (x.endswith(".c") or x.endswith(".cc"))]
        if "-c" in a and len(src) == 1:
            # no -o: the driver names the object <stem>.obj in the working directory
            return absn(rec["cwd"], os.path.splitext(os.path.basename(src[0]))[0] + ".obj")
        return None

    links = [x for x in recs if x["tool"] in COMPILERS and "-c" not in x["args"]
             and "-o" in x["args"] and os.path.basename(out_of(x)) == "cc1.exe"]
    if not links:
        sys.exit("gen-cc1-build: no cc1 link record")
    link = links[-1]                  # the latest (a rerun of make relinks)
    compiles = {}
    for x in recs:
        if x["tool"] in COMPILERS and "-c" in x["args"] and out_of(x):
            compiles[out_of(x)] = x
    ars = {}
    for x in recs:
        if x["tool"] == "ar" and len(x["args"]) >= 2 and x["args"][1].endswith(".a"):
            ars.setdefault(absn(x["cwd"], x["args"][1]), []).append(x)

    # cc1's link inputs, in order: objects, archives, -L/-l
    objs_direct, archives, lib_args = [], [], []
    for x in link["args"][link["args"].index("-o") + 2:]:
        if x.endswith(".o") or x.endswith(".obj"):
            objs_direct.append(absn(link["cwd"], x))
        elif x.endswith(".a"):
            a = absn(link["cwd"], x)
            if a not in archives:
                archives.append(a)
        elif x.startswith("-L") or x.startswith("-l"):
            lib_args.append(x)

    def tag(lib):  # the GCCOBJ directory of an archive's objects
        b = os.path.basename(lib)
        return b[3:-2].upper() if b.startswith("lib") else b[:-2].upper()

    objdir = {}       # host object path -> VMS path of the native object
    members = {}      # archive -> [host object paths]
    for a in archives:
        if a not in ars:
            # an archive a -l names (zlib's libz.a) or one GCC's build did not ar here
            sys.exit("gen-cc1-build: no ar record for %s" % a)
        mem = []
        for x in ars[a]:
            for o in x["args"][2:]:
                ho = absn(x["cwd"], o)
                if ho not in mem:
                    mem.append(ho)
        members[a] = mem
    # -L./../zlib -lz: an archive this build made, found the way the driver finds it
    for la in lib_args:
        if la.startswith("-l"):
            for d in [absn(link["cwd"], x[2:]) for x in lib_args if x.startswith("-L")]:
                cand = os.path.join(d, "lib" + la[2:] + ".a")
                if cand in ars and cand not in members:
                    members[cand] = [absn(x["cwd"], o) for x in ars[cand] for o in x["args"][2:]]
                    archives.append(cand)
    for a in archives:
        seen = set()
        for ho in members[a]:
            b = os.path.basename(ho)
            if b in seen:
                sys.exit("gen-cc1-build: two members named %s in %s" % (b, a))
            seen.add(b)
            objdir[ho] = "/GCCOBJ/%s/%s" % (tag(a), b)
    for ho in objs_direct:
        objdir[ho] = "/GCCOBJ/CC1/%s" % os.path.basename(ho)
    missing = [o for o in objdir if o not in compiles]
    if missing:
        sys.exit("gen-cc1-build: no compile record for %s" % missing[:5])

    # the compiles, in the order GCC's build ran them
    todo = sorted(objdir, key=lambda o: compiles[o]["t"])
    if pick:
        import re
        todo = [o for o in todo if re.search(pick, os.path.basename(o))]
        only = only or len(todo)
    if shard:
        todo = [o for i, o in enumerate(todo) if i % shard[1] == shard[0] - 1]
        only = only or len(todo)
    if only:
        todo = todo[:only]
    dirs = sorted({os.path.dirname(v) for v in objdir.values()})

    L = []
    w = L.append
    w("$! BUILD_CC1.COM -- build GCC's cc1 on OpenVMS Alpha with the native toolchain")
    w("$! (rd vms-465). Generated by tools/cross-alpha-vms/selfhost/gen-cc1-build.py from")
    w("$! the compile, archive and link commands GCC's own Makefiles ran: every object")
    w("$! compiled by GCC.EXE / GXX.EXE (CC1.EXE / CC1PLUS.EXE), the libraries made by")
    w("$! AR.EXE (LIBRARY), cc1 linked by GXX.EXE (ld -> LINK). An object already present")
    w("$! is not compiled again, so the procedure can be run again after an interruption.")
    w("$! Needs the rooted logicals GCCSRC (the trees) and GCCOBJ (the output).")
    w("$!")
    w("$ SET NOON")
    w("$ GCC := $SYS$SYSTEM:XGCC.EXE")
    w("$ GXX := $SYS$SYSTEM:GXX.EXE")
    w("$ AR := $SYS$SYSTEM:AR.EXE")
    w("$ FAILS = 0")
    w("$ D0 = F$ENVIRONMENT(\"DEFAULT\")")
    for d in dirs:
        w("$ CREATE/DIRECTORY GCCOBJ:[%s]" % d.split("/")[2])
    w("$ T1 = F$TIME()")
    w("$ WRITE SYS$OUTPUT \"CC1B: BEGIN %d compiles ''T1'\"" % len(todo))
    for n, ho in enumerate(todo, 1):
        rec = compiles[ho]
        vo = objdir[ho]
        drv = COMPILERS[rec["tool"]]
        args = ["-BSYS$SYSTEM:"] + extra + m.compile_args(rec, vo)
        src = [x for x in rec["args"] if not x.startswith("-") and (x.endswith(".c") or x.endswith(".cc"))]
        label = os.path.relpath(absn(rec["cwd"], src[-1]), r["SRC"]) if src else vo
        vdir, vname = vo.split("/")[2], vo.split("/")[3]
        w("$! %d/%d %s" % (n, len(todo), label))
        w("$ IF F$SEARCH(\"GCCOBJ:[%s]%s\") .EQS. \"\" THEN GOTO M%d" % (vdir, vname, n))
        # made before (an earlier run of this procedure, or a CI shard): said so
        w("$ WRITE SYS$OUTPUT \"CC1B: TU %d/%d FOUND %s\"" % (n, len(todo), label))
        w("$ GOTO C%d" % n)
        w("$M%d:" % n)
        w("$ OPEN/WRITE R GCCOBJ:[000000]TU.RSP")
        for a in args:
            w("$ WRITE R %s" % dcl_str(rsp_quote(a)))
        w("$ CLOSE R")
        if n <= trace:
            w("$ TYPE GCCOBJ:[000000]TU.RSP")
        vd = m.vms_dir(rec["cwd"])
        if "^" in vd:   # an ODS-5 name on the command line needs the extended parse style
            w("$ SET PROCESS/PARSE_STYLE=EXTENDED")
        w("$ SET DEFAULT %s" % vd)
        if "^" in vd:
            w("$ SET PROCESS/PARSE_STYLE=TRADITIONAL")
        w("$ T0 = F$TIME()")
        w("$ %s @GCCOBJ:[000000]TU.RSP" % drv)
        w("$ S = $STATUS")
        w("$ T1 = F$TIME()")
        # GCC exits 0 (SS$_NORMAL) when it compiled: any other code, even one
        # with success severity (DEC C __posix_exit(4), an internal compiler
        # error, is %X0035A021), or no object, is a failed compile
        w("$ IF S .EQ. 1 .AND. F$SEARCH(\"GCCOBJ:[%s]%s\") .EQS. \"\" THEN S = %%X10000002" % (vdir, vname))
        w("$ WRITE SYS$OUTPUT \"CC1B: TU %d/%d ''S' ''T0' ''T1' %s\"" % (n, len(todo), label))
        w("$ IF S .NE. 1 THEN FAILS = FAILS + 1")
        w("$ SET DEFAULT 'D0'")
        w("$ DELETE GCCOBJ:[000000]TU.RSP;*")
        w("$C%d:" % n)
    if not only:
        w("$ T1 = F$TIME()")
        w("$ WRITE SYS$OUTPUT \"CC1B: COMPILED FAILS=''FAILS' ''T1'\"")
        w("$ IF FAILS .NE. 0 THEN GOTO DONE")
        for a in archives:
            lib = "/GCCOBJ/%s/%s" % (tag(a), os.path.basename(a))
            mem = [objdir[o] for o in members[a]]
            for k in range(0, len(mem), 12):
                w("$ AR rc %s %s" % (lib, " ".join(mem[k:k + 12])))
                w("$ IF .NOT. $STATUS THEN FAILS = FAILS + 1")
            w("$ WRITE SYS$OUTPUT \"CC1B: AR %s %d members FAILS=''FAILS'\"" % (lib, len(mem)))
        # the link: the record's arguments, objects and archives moved to GCCOBJ
        la, cwd, P32 = link["args"], link["cwd"], r["P32"]
        largs = ["-BSYS$SYSTEM:"] + link_extra
        i = 0
        while i < len(la):
            x = la[i]
            if x == "-o":
                largs += ["-o", "/GCCOBJ/CC1/cc1.exe"]
                i += 2
                continue
            if x == "-isystem" and la[i + 1].startswith(P32):
                tail = la[i + 1][len(P32 + "/include/c++/"):].split("/", 1)
                largs += ["-isystem", "/GNU/CXX" + ("/" + tail[1].upper() if len(tail) > 1 else "")]
                i += 2
                continue
            if x == "-L" + P32 + "/lib":
                largs.append("-L/SYS$SYSTEM")
            elif x.endswith(".o") or x.endswith(".obj"):
                largs.append(objdir[absn(cwd, x)])
            elif x.endswith(".a"):
                a = absn(cwd, x)
                largs.append("/GCCOBJ/%s/%s" % (tag(a), os.path.basename(a)))
            elif x.startswith("-L"):
                d = absn(cwd, x[2:])
                hit = [a for a in archives if os.path.dirname(a) == d]
                largs.append("-L/GCCOBJ/%s" % tag(hit[0]) if hit else "-L" + m.path(cwd, x[2:]))
            else:
                largs.append(x)
            i += 1
        w("$ IF FAILS .NE. 0 THEN GOTO DONE")
        w("$ OPEN/WRITE R GCCOBJ:[000000]LINK.RSP")
        for a in largs:
            w("$ WRITE R %s" % dcl_str(rsp_quote(a)))
        w("$ CLOSE R")
        w("$ T0 = F$TIME()")
        w("$ GXX @GCCOBJ:[000000]LINK.RSP")
        w("$ S = $STATUS")
        w("$ T1 = F$TIME()")
        w("$ WRITE SYS$OUTPUT \"CC1B: LINK ''S' ''T0' ''T1'\"")
        w("$ IF .NOT. S THEN FAILS = FAILS + 1")
    w("$DONE:")
    w("$ T1 = F$TIME()")
    w("$ WRITE SYS$OUTPUT \"CC1B: END FAILS=''FAILS' ''T1'\"")
    w("$ IF FAILS .EQ. 0 THEN EXIT 1")
    w("$ EXIT 44")
    open(outcom, "w").write("\n".join(L) + "\n")
    print("gen-cc1-build: %d compiles, %d archives, cc1 linked from %d objects -> %s"
          % (len(todo), len(archives), len(objs_direct), outcom))


main()
