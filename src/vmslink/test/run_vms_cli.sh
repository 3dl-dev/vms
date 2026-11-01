#!/bin/sh
# run_vms_cli.sh -- LINK.EXE takes the VMS command syntax (vms-ce4):
#   LINK /EXECUTABLE=x file,file  |  file/OPTIONS  |  lib/LIBRARY  |  /NOINFORMATIONALS
# as the alpha-dec-vms GCC port's ld wrapper (vms-ld.c) writes it. Every accepted
# form must produce the image the GNU-style flags produce, byte for byte; every
# refused form must exit nonzero and write NO image (the negative controls).
# Architecture-independent (EVAX path): runs on the CI host with no Alpha toolchain.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/vms-cli-test}
rm -rf "$WORK"; mkdir -p "$WORK"
fails=0
ok()   { echo "ok:   $1"; }
bad()  { echo "FAIL: $1"; fails=$((fails+1)); }

echo "== build LINK.EXE (host tool) =="
$CC -std=gnu11 -O2 -Wall -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"
L="$WORK/LINK.EXE"
cp "$FIX/link_main.obj" "$FIX/link_helper.obj" "$WORK/"
cd "$WORK"
X="--base 0 --transfer MAIN_PROC"

# baseline: the GNU-style link
$L $X -o gnu.exe link_main.obj link_helper.obj 2> gnu.err
[ -s gnu.exe ] || { echo "FAIL: baseline link wrote no image"; exit 1; }

# 1. /EXECUTABLE=, comma list
$L $X /EXECUTABLE=v1.exe link_main.obj,link_helper.obj 2> v1.err
cmp -s gnu.exe v1.exe && ok "/EXECUTABLE=x a,b == GNU-style image" || bad "/EXECUTABLE=x a,b differs from the GNU-style image"

# 2. default type .OBJ and an abbreviated qualifier (/EXEC), two arguments
$L $X /EXEC=v2.exe link_main link_helper 2> v2.err
cmp -s gnu.exe v2.exe && ok "/EXEC=x with default .OBJ type == GNU-style image" || bad "/EXEC=x with default type differs"

# 3. options file: comment, continuation, case_sensitive/PSECT_ATTR keywords
printf '%s\n' '! a comment line' 'case_sensitive=yes' 'link_main.obj' 'link_helper.obj,-' '' 'PSECT_ATTR=LIB$INITIALIZE,GBL' > o3.opt
# (line 4 ends in a hyphen: continues onto the blank line, which adds nothing)
$L $X /EXECUTABLE=v3.exe o3.opt/OPTIONS 2> v3.err
cmp -s gnu.exe v3.exe && ok "options file == GNU-style image" || bad "options file image differs"
grep -q 'W-IGNORED' v3.err && bad "recognised option keywords drew a warning" || ok "case_sensitive / PSECT_ATTR=LIB\$INITIALIZE,GBL accepted silently"

# 4. default output name: first input's name, .EXE, in the current directory
mkdir d4 && ( cd d4 && cp ../link_main.obj ../link_helper.obj . && $L $X link_main.obj,link_helper.obj 2> ../v4.err )
[ -s d4/link_main.exe ] && cmp -s gnu.exe d4/link_main.exe && ok "default output name link_main.exe == GNU-style image" || bad "default output name"

# 5. a library: lib/LIBRARY pulls only what is needed, where it appears
rm -f lib.a; ar rc lib.a link_helper.obj
$L $X -o gnu5.exe link_main.obj --library lib.a 2>/dev/null
$L $X /EXECUTABLE=v5.exe link_main.obj lib.a/LIBRARY 2> v5.err
cmp -s gnu5.exe v5.exe && ok "file/LIBRARY == --library" || bad "file/LIBRARY differs from --library"
printf '%s\n' 'link_main.obj' 'lib.a/LIBRARY' > o5.opt
$L $X /EXECUTABLE=v5b.exe o5.opt/OPT 2> v5b.err
cmp -s gnu5.exe v5b.exe && ok "options file with /LIBRARY == --library" || bad "options file /LIBRARY differs"

# 6. /NOINFORMATIONALS silences %LINK-I-, the default does not
grep -q '%LINK-I-' v1.err && ok "default link prints %LINK-I- messages" || bad "baseline has no %LINK-I- messages to silence"
$L $X /NOINFORMATIONALS /EXECUTABLE=v6.exe link_main.obj,link_helper.obj 2> v6.err
grep -q '%LINK-I-' v6.err && bad "/NOINFORMATIONALS still printed %LINK-I-" || ok "/NOINFORMATIONALS prints no %LINK-I-"
cmp -s gnu.exe v6.exe && ok "/NOINFORMATIONALS image unchanged" || bad "/NOINFORMATIONALS changed the image"

# --- negative controls: refused, nonzero, no image ---
refuse() {  # refuse <label> <expected text> -- <args...>
    label=$1; want=$2; shift 3
    rm -f n.exe
    if $L $X "$@" 2> n.err >/dev/null; then bad "$label: accepted"; return; fi
    grep -q "$want" n.err || { bad "$label: refused without '$want' ($(head -1 n.err))"; return; }
    [ -e n.exe ] && { bad "$label: refused but wrote an image"; return; }
    ok "$label refused ($want)"
}
refuse "/MAP"                 NOTSUPP -- /EXECUTABLE=n.exe /MAP link_main.obj,link_helper.obj
refuse "/FULL"                NOTSUPP -- /EXECUTABLE=n.exe /FULL link_main.obj,link_helper.obj
refuse "/CROSS_REFERENCE"     NOTSUPP -- /EXECUTABLE=n.exe /CROSS_REFERENCE link_main.obj,link_helper.obj
refuse "/SHAREABLE (command)" NOTSUPP -- /EXECUTABLE=n.exe /SHAREABLE link_main.obj,link_helper.obj
refuse "/NOEXECUTABLE"        NOTSUPP -- /NOEXECUTABLE link_main.obj,link_helper.obj
refuse "/BOGUS"               IVQUAL  -- /EXECUTABLE=n.exe /BOGUS link_main.obj,link_helper.obj
refuse "missing options file" OPENIN  -- /EXECUTABLE=n.exe nonexistent.opt/OPTIONS
refuse "no input file"        NOFILES -- /EXECUTABLE=n.exe /NOINFORMATIONALS

# an options-file keyword the linker does not act on is a visible warning, not silence
printf '%s\n' 'link_main.obj' 'link_helper.obj' 'IDENTIFICATION="X"' > o7.opt
$L $X /EXECUTABLE=v7.exe o7.opt/OPT 2> v7.err
grep -q 'W-IGNORED.*IDENTIFICATION' v7.err && cmp -s gnu.exe v7.exe && ok "unacted keyword: W-IGNORED, link still correct" || bad "unacted keyword not reported"


# vms-5b3: CLUSTER=name,,,file -- the file is linked, ahead of the command's own files
# (option-declared clusters precede the default cluster): the GNU-style link that names
# it first is the reference. A base address / pfc is reported, not honoured silently.
$L $X -o gnu9.exe link_helper.obj link_main.obj 2> /dev/null
printf '%s\n' 'link_main.obj' 'cluster=EXTRA,,,link_helper.obj' > o9.opt
$L $X /EXECUTABLE=v9.exe o9.opt/OPT 2> v9.err
grep -q 'W-IGNORED' v9.err && bad "CLUSTER drew a warning: $(cat v9.err)" || ok "CLUSTER=name,,,file accepted"
cmp -s gnu9.exe v9.exe && ok "a CLUSTER file is linked ahead of the default cluster == GNU-style image" || bad "CLUSTER file not linked first"
cmp -s gnu.exe v9.exe && bad "CLUSTER order not distinguishable (fixture)" || true
printf '%s\n' 'link_main.obj' 'cluster=EXTRA,%X20000,,link_helper.obj' > o10.opt
$L $X /EXECUTABLE=v10.exe o10.opt/OPT 2> v10.err
grep -q 'W-IGNORED.*base address' v10.err && cmp -s gnu9.exe v10.exe && ok "CLUSTER base address: W-IGNORED, file still linked" || bad "CLUSTER base address not reported"

# --- the implicit search (/SYSSHR, /SYSLIB default): installed SYS$SHARE:DECC$SHR.EXE and
# SYS$LIBRARY:STARLET.A are added after what the command names; /NOSYSSHR /NOSYSLIB switch
# them off; a file the command already names is not added twice. On this host the logicals
# are files literally so named in the current directory; OVMX_LINK_VMS_ARGV shows the vector.
: > 'SYS$SHARE:DECC$SHR.EXE'; : > 'SYS$LIBRARY:STARLET.A'
argv() { OVMX_LINK_VMS_ARGV=1 $L $X "$@" 2>&1 >/dev/null | sed -n 's/^%LINK-I-ARGV, //p' | tr '\n' ' '; }
v=$(argv /EXECUTABLE=i1.exe link_main.obj,link_helper.obj)
case "$v" in *"--use SYS\$SHARE:DECC\$SHR.EXE"*"--library SYS\$LIBRARY:STARLET.A"*) ok "installed DECC\$SHR and STARLET are searched without being named";; *) bad "implicit search missing: $v";; esac
case "$v" in *LIBOTS_SHR*|*LIBVMS*) bad "an uninstalled producer was added: $v";; *) ok "only installed images are added";; esac
v=$(argv /EXECUTABLE=i2.exe /NOSYSSHR link_main.obj,link_helper.obj)
case "$v" in *"--use"*) bad "/NOSYSSHR still added a shareable: $v";; *"--library SYS\$LIBRARY:STARLET.A"*) ok "/NOSYSSHR drops the shareable, keeps STARLET";; *) bad "/NOSYSSHR dropped STARLET too: $v";; esac
v=$(argv /EXECUTABLE=i3.exe /NOSYSLIB link_main.obj,link_helper.obj)
case "$v" in *STARLET*) bad "/NOSYSLIB still searched STARLET: $v";; *"--use SYS\$SHARE:DECC\$SHR.EXE"*) ok "/NOSYSLIB drops STARLET, keeps the shareable";; *) bad "/NOSYSLIB dropped the shareable too: $v";; esac
printf '%s\n' 'link_main.obj' 'link_helper.obj' 'sys$share:decc$shr.exe/SHAREABLE' 'SYS$LIBRARY:starlet.a/LIBRARY' > i4.opt
v=$(argv /EXECUTABLE=i4.exe i4.opt/OPT)
[ "$(printf '%s' "$v" | grep -o -i 'decc.shr.exe' | wc -l)" = 1 ] && [ "$(printf '%s' "$v" | grep -o -i 'starlet.a' | wc -l)" = 1 ] \
    && ok "a producer / library the options file names is not added a second time (any case)" || bad "named image added twice: $v"
rm -f 'SYS$SHARE:DECC$SHR.EXE' 'SYS$LIBRARY:STARLET.A'
v=$(argv /EXECUTABLE=i5.exe link_main.obj,link_helper.obj)
case "$v" in *"--use"*|*"--library"*) bad "nothing installed, yet the vector names one: $v";; *) ok "nothing installed, nothing added";; esac

# GNU-style commands still work (an unrelated first argument must not flip the syntax)
$L $X -o gnu8.exe /nonexistent-dir-is-not-a-qualifier/link_main.obj 2>/dev/null && bad "bogus path accepted" || ok "GNU-style command with a /path argument stays GNU-style"

[ "$fails" -eq 0 ] || { echo "$fails VMS-CLI CHECK(S) FAILED"; exit 1; }
echo "ALL VMS COMMAND SYNTAX (vms-ce4) CHECKS PASSED"
