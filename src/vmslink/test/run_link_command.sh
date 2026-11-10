#!/bin/sh
# run_link_command.sh -- SYS$SYSTEM:LINK.EXE takes the VMS LINK command (rd vms-3cb1).
#
# Usage: run_link_command.sh <LINK.EXE image> <build-tool LINK.EXE>
#
# The image (link.c built with OVMX_LINK_IMAGE, CMake target vmslink_image) gets
# its command from the CLI with LIB$GET_FOREIGN and parses it with CLI$DCL_PARSE
# against the LINK command table. Off the executive (this host test) LIB$GET_FOREIGN
# reads VMS_FOREIGN_CMD, the channel DCL sets for a foreign command when /dev/vms
# is absent; on a real executive DCL records the tail in the PCB's CLI context.
#
# Every accepted command must produce the image the build tool's flags produce,
# byte for byte; every refused command must exit nonzero and write NO image (the
# negative controls). And the image must hold none of the build tool's flags
# (the shipped-image strings gate). Objects are host ELF (shareable links need no
# C RTL producer), so this runs on any CI host with gcc and ar.
set -e
L=${1:?usage: run_link_command.sh <LINK.EXE image> <build-tool LINK.EXE>}
T=${2:?need the build-tool LINK.EXE}
CC=${CC:-gcc}
WORK=${WORK:-/tmp/link-command-test}
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails+1)); }
link() { VMS_FOREIGN_CMD="$1" "$L"; }

echo "== strings gate: the shipped LINK.EXE carries no build flag =="
if strings -a "$L" | grep -E -- '--(use|library|executable|shareable|symbol-vector|gsmatch|transfer|allow-undefined)\b'; then
    bad "the LINK.EXE image holds a build flag (above)"
else
    ok "no --use/--library/--executable/... string in the LINK.EXE image"
fi
strings -a "$T" | grep -q -- '--use' && ok "(control) the build tool does hold --use" \
    || bad "control: the build tool holds no --use, so the gate proves nothing"

cat > a.c <<'EOF'
int helper(int);
int foo(int x) { return helper(x) + 1; }
EOF
cat > b.c <<'EOF'
int helper(int x) { return x * 2; }
EOF
cat > c.c <<'EOF'
int bar(void) { return 42; }
EOF
for m in a b c; do $CC -fPIC -O2 -c -o $m.obj $m.c; done
SV="foo=PROCEDURE,bar=PROCEDURE"
$T --shareable --symbol-vector "$SV" --gsmatch LEQUAL,1,0 -o gnu.exe a.obj b.obj c.obj 2>gnu.err
[ -s gnu.exe ] || { echo "FAIL: baseline build-tool link wrote no image"; cat gnu.err; exit 1; }
printf '%s\n' 'SYMBOL_VECTOR=(foo=PROCEDURE,bar=PROCEDURE)' 'GSMATCH=LEQUAL,1,0' > sv.opt

# 1. /SHAREABLE=, comma list, options file with SYMBOL_VECTOR/GSMATCH
link '/SHAREABLE=v1.exe a.obj,b.obj,c.obj,sv.opt/OPTIONS' 2>v1.err || true
cmp -s gnu.exe v1.exe && ok "LINK/SHAREABLE=x a,b,c,opt/OPTIONS == build-tool image" || { bad "/SHAREABLE=x differs"; cat v1.err; }

# 2. default type .OBJ / .OPT, abbreviations (/SHARE, /OPT), '+' list separator
link '/SHARE=v2.exe a+b,c,sv/OPT' 2>v2.err || true
cmp -s gnu.exe v2.exe && ok "default .OBJ/.OPT types, abbreviations, '+' == build-tool image" || bad "default types / abbreviations"

# 3. options file: comment, continuation, case_sensitive / PSECT_ATTR / SYMBOL_VECTOR across lines
printf '%s\n' '! a comment line' 'case_sensitive=yes' 'a.obj' 'b.obj,-' 'c.obj' \
    'PSECT_ATTR=LIB$INITIALIZE,GBL' 'SYMBOL_VECTOR=(foo=PROCEDURE,-' '   bar=PROCEDURE)' 'GSMATCH=LEQUAL,1,0' > o3.opt
link '/SHAREABLE=v3.exe o3.opt/OPTIONS' 2>v3.err || true
cmp -s gnu.exe v3.exe && ok "options file == build-tool image" || { bad "options file image differs"; cat v3.err; }
grep -q 'W-IGNORED' v3.err && bad "recognised option keywords drew a warning" || ok "recognised keywords accepted silently"

# 4. default image name: the first input's name, .EXE, in the current directory
mkdir d4 && ( cd d4 && cp ../a.obj ../b.obj ../c.obj ../sv.opt . && VMS_FOREIGN_CMD='/SHAREABLE a.obj,b.obj,c.obj,sv/OPT' "$L" 2>../v4.err ) || true
[ -s d4/a.exe ] && cmp -s gnu.exe d4/a.exe && ok "default image name a.exe == build-tool image" || bad "default image name"

# 5. file/LIBRARY: an .OLB searched for what the link needs
ar rc lib.olb b.obj c.obj
link '/SHAREABLE=v5.exe a.obj,lib.olb/LIBRARY,sv/OPT' 2>v5.err || true
cmp -s gnu.exe v5.exe && ok "file/LIBRARY == build-tool image" || { bad "file/LIBRARY differs"; cat v5.err; }
printf '%s\n' 'a.obj' 'lib/LIBRARY' 'SYMBOL_VECTOR=(foo=PROCEDURE,bar=PROCEDURE)' 'GSMATCH=LEQUAL,1,0' > o5.opt
link '/SHAREABLE=v5b.exe o5/OPT' 2>v5b.err || true
cmp -s gnu.exe v5b.exe && ok "options-file lib/LIBRARY (default .OLB) == build-tool image" || bad "options-file /LIBRARY differs"

# 6. file/INCLUDE=(module): the named module only, the library not searched
ar rc lib2.olb c.obj b.obj
link '/SHAREABLE=v6.exe a.obj,b.obj,lib2.olb/INCLUDE=(C),sv/OPT' 2>v6.err || true
cmp -s gnu.exe v6.exe && ok "file/INCLUDE=(C) == build-tool image" || { bad "/INCLUDE differs"; cat v6.err; }
rm -f n.exe; if link '/SHAREABLE=n.exe a.obj,lib2.olb/INCLUDE=(B),sv/OPT' 2>n6.err; then bad "/INCLUDE searched the library (bar resolved)"; else ok "/INCLUDE alone does not search the library (bar stays undefined)"; fi
rm -f n.exe; link '/SHAREABLE=n.exe a.obj,b.obj,lib2.olb/INCLUDE=(NOSUCH),sv/OPT' 2>n6b.err && bad "unknown /INCLUDE module accepted" || \
    { grep -q NOSUCHMOD n6b.err && [ ! -e n.exe ] && ok "unknown module: %LINK-F-NOSUCHMOD, no image" || bad "unknown module not reported"; }

# 6b. a shareable whose only input is a module /INCLUDE takes from a library
printf '%s\n' 'SYMBOL_VECTOR=(bar=PROCEDURE)' > c.opt
$T --shareable --symbol-vector "bar=PROCEDURE" -o gnu6b.exe c.obj 2>/dev/null
link '/SHAREABLE=v6b.exe lib2.olb/INCLUDE=(C),c/OPT' 2>v6b.err || true
cmp -s gnu6b.exe v6b.exe && ok "/INCLUDE as the only input == build-tool image" || { bad "/INCLUDE-only shareable"; cat v6b.err; }

# 7. /NOINFORMATIONALS silences %LINK-I-, the default does not
grep -q '%LINK-I-' v5.err && ok "default link prints %LINK-I- messages" || bad "baseline has no %LINK-I- to silence"
link '/NOINFORMATIONALS/SHAREABLE=v7.exe a.obj,lib.olb/LIBRARY,sv/OPT' 2>v7.err || true
grep -q '%LINK-I-' v7.err && bad "/NOINFORMATIONALS still printed %LINK-I-" || ok "/NOINFORMATIONALS prints no %LINK-I-"
cmp -s gnu.exe v7.exe && ok "/NOINFORMATIONALS image unchanged" || bad "/NOINFORMATIONALS changed the image"

# 8. /MAP and /MAP/BRIEF (Linker manual, Image Map sections)
link '/SHAREABLE=v8.exe/MAP a.obj,lib.olb/LIBRARY,sv/OPT' 2>v8.err || true
cmp -s gnu.exe v8.exe && ok "/MAP leaves the image unchanged" || bad "/MAP changed the image"
if [ -s a.map ]; then
    for sec in 'Object Module Synopsis' 'Symbols By Name' 'Image Synopsis' 'Link Run Statistics'; do
        grep -q "! $sec !" a.map && ok "default map name a.map has '$sec'" || bad "map lacks '$sec'"
    done
    grep -qE '^foo +[0-9A-F]{16} +a.obj' a.map && ok "map: foo defined by a.obj with its value" || bad "map symbol line for foo"
    grep -q 'lib.olb(c.obj' a.map && ok "map: the library module pulled is listed" || bad "map module from lib.olb"
else bad "/MAP wrote no a.map"; fi
link '/SHAREABLE=v8b.exe/MAP=brief.map/BRIEF a.obj,lib.olb/LIBRARY,sv/OPT' 2>/dev/null || true
grep -q '! Object Module Synopsis !' brief.map && ! grep -q 'Symbols By Name' brief.map \
    && ok "/MAP=file/BRIEF: synopsis, no Symbols By Name" || bad "/BRIEF map"

# 9. the implicit search (/SYSSHR default): the installed SYS$SHARE:DECC$SHR.EXE (where
# binding opens it: /run/ovmx-boot or the SYS$SYSROOT tree) is bound without being named;
# /NOSYSSHR switches it off; a shareable of that name the command names is not added twice.
SYSDECC=""
for d in /run/ovmx-boot /vms/SYS0/SYSCOMMON/SYSLIB; do [ -r "$d/DECC\$SHR.EXE" ] && { SYSDECC="$d/DECC\$SHR.EXE"; break; }; done
link '/SHAREABLE=i1.exe/MAP=i1.map a.obj,b.obj,c.obj,sv/OPT' 2>i1.err || true
if [ -n "$SYSDECC" ]; then
    grep -q 'DECC$SHR.EXE (shareable image)' i1.map && ok "installed DECC\$SHR ($SYSDECC) is bound without being named" || bad "implicit SYS\$SHARE:DECC\$SHR missing"
else
    [ -s i1.map ] && ! grep -q '(shareable image)' i1.map && ok "no DECC\$SHR installed: nothing bound implicitly" || { bad "implicit search with nothing installed"; cat i1.err; }
fi
link '/SHAREABLE=i2.exe/MAP=i2.map/NOSYSSHR a.obj,b.obj,c.obj,sv/OPT' 2>/dev/null || true
[ -s i2.map ] && ! grep -q '(shareable image)' i2.map && ok "/NOSYSSHR binds none" || bad "/NOSYSSHR still bound a shareable"
cp gnu.exe 'DECC$SHR.EXE'
printf '%s\n' 'a.obj' 'b.obj' 'c.obj' 'DECC$SHR.EXE/SHAREABLE' 'SYMBOL_VECTOR=(foo=PROCEDURE,bar=PROCEDURE)' > i3.opt
link '/SHAREABLE=i3.exe/MAP=i3.map i3/OPT' 2>i3.err || true
[ "$(grep -ci 'decc.shr.exe (shareable image)' i3.map 2>/dev/null)" = 1 ] && ok "a DECC\$SHR the options file names is bound once, not again implicitly" || { bad "named shareable bound twice / not at all"; cat i3.err; }

# 10. CLUSTER=name,,,file: the file is linked ahead of the command's own files
$T --shareable --symbol-vector "$SV" --gsmatch LEQUAL,1,0 -o gnu10.exe c.obj a.obj b.obj 2>/dev/null
printf '%s\n' 'a.obj' 'b.obj' 'cluster=EXTRA,,,c.obj' 'SYMBOL_VECTOR=(foo=PROCEDURE,bar=PROCEDURE)' 'GSMATCH=LEQUAL,1,0' > o10.opt
link '/SHAREABLE=v10.exe o10/OPT' 2>v10.err || true
cmp -s gnu10.exe v10.exe && ok "a CLUSTER file is linked ahead of the default cluster" || bad "CLUSTER file not linked first"
cmp -s gnu.exe gnu10.exe && bad "CLUSTER order not distinguishable (fixture)" || true

# 11. an options-file keyword the linker does not act on: a visible warning
printf '%s\n' 'a.obj' 'b.obj' 'c.obj' 'IDENTIFICATION="X"' 'SYMBOL_VECTOR=(foo=PROCEDURE,bar=PROCEDURE)' 'GSMATCH=LEQUAL,1,0' > o11.opt
link '/SHAREABLE=v11.exe o11/OPT' 2>v11.err || true
grep -q 'W-IGNORED.*IDENTIFICATION' v11.err && cmp -s gnu.exe v11.exe && ok "unacted keyword: W-IGNORED, link still correct" || bad "unacted keyword not reported"

# --- negative controls: refused, nonzero, no image ---
refuse() {  # refuse <label> <expected text> <command>
    rm -f n.exe
    if link "$3" 2>n.err >/dev/null; then bad "$1: accepted"; return; fi
    grep -q "$2" n.err || { bad "$1: refused without '$2' ($(head -1 n.err))"; return; }
    [ -e n.exe ] && { bad "$1: refused but wrote an image"; return; }
    ok "$1 refused ($2)"
}
refuse "/FULL"                NOTSUPP  '/SHAREABLE=n.exe/MAP/FULL a.obj,b.obj,c.obj,sv/OPT'
refuse "/CROSS_REFERENCE"     NOTSUPP  '/SHAREABLE=n.exe/MAP/CROSS_REFERENCE a.obj,b.obj,c.obj,sv/OPT'
refuse "/SYSTEM"              NOTSUPP  '/SYSTEM/SHAREABLE=n.exe a.obj,b.obj,c.obj,sv/OPT'
refuse "/NOEXECUTABLE"        NOTSUPP  '/NOEXECUTABLE a.obj,b.obj,c.obj'
refuse "/BOGUS"               IVQUAL   '/SHAREABLE=n.exe/BOGUS a.obj,b.obj,c.obj,sv/OPT'
refuse "/LIBRARY after verb"  IVQUAL   '/LIBRARY/SHAREABLE=n.exe a.obj,b.obj,c.obj,sv/OPT'
refuse "--use (a build flag)" LINK-F-   '--use x.exe /SHAREABLE=n.exe a.obj'
refuse "missing options file" OPENIN   '/SHAREABLE=n.exe a.obj,nonexistent.opt/OPTIONS'
refuse "no input file"        NOFILES  '/SHAREABLE=n.exe'

[ "$fails" -eq 0 ] || { echo "$fails LINK COMMAND CHECK(S) FAILED"; exit 1; }
echo "ALL LINK COMMAND (rd vms-3cb1) CHECKS PASSED"
