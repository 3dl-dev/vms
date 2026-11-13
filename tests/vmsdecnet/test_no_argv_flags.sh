#!/bin/bash
# rd vms-0a7 S3 gate (b): no SHIPPED image under src/vmsdecnet parses argv "--"
# flags. A VMS image takes its parameters the VMS way -- NETACP none at all
# (NCP SET EXECUTOR STATE ON creates it; it reads the executor database), a
# network object its link through SYS$NET, a utility its command line through
# the CLI -- and the self-tests that used to hide behind --flags in the shipped
# DECNETD.EXE live in the test image tests/vmsdecnet/netacp_test.c.
#
# A shipped image is a C file under src/vmsdecnet that defines main(). Inside
# it, after comments are stripped, the gate flags
#     strcmp/strncmp/strcasecmp(argv[...], "--...")   getopt / getopt_long
#     argv[...][0] == '-'
# Known debt is listed by file and flag in tests/vmsdecnet/argv_gate_baseline.txt
# (empty since RTPAD.EXE took SET HOST, rd vms-0a7 S2). A flag not in the baseline fails; a baseline
# line whose flag is gone fails too, so the list only shrinks.
#
# NEGATIVE CONTROL: a "--fal-selftest" argv test planted in a copy of decnetd.c,
# and a getopt() planted in a copy of fal_main.c, must each be caught.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASELINE="$ROOT/tests/vmsdecnet/argv_gate_baseline.txt"

strip() {
    sed -e 's://.*$::' "$1" | awk '
        { line=$0
          while (1) {
            if (inc) { e=index(line,"*/"); if (e==0) { line=""; break } line=substr(line,e+2); inc=0 }
            s=index(line,"/*"); if (s==0) break
            pre=substr(line,1,s-1); rest=substr(line,s+2); e=index(rest,"*/")
            if (e==0) { line=pre; inc=1; break } line=pre substr(rest,e+2)
          }
          print line }'
}

# flags FILE -> one "--flag" (or "getopt"/"argv-dash") per offending use
flags() {
    local s; s=$(strip "$1")
    printf '%s\n' "$s" | grep -oE 'str(n|case|ncase)?cmp\s*\(\s*argv\s*\[[^]]*\]\s*,\s*"--[A-Za-z0-9-]*"' \
        | grep -oE '"--[A-Za-z0-9-]*"' | tr -d '"'
    printf '%s\n' "$s" | grep -qE '\bgetopt(_long)?\s*\(' && echo getopt
    printf '%s\n' "$s" | grep -qE "argv\s*\[[^]]*\]\s*\[\s*0\s*\]\s*==\s*'-'" && echo argv-dash
    return 0
}

check() {   # check DIR BASE -> prints problems; returns 1 on any
    local dir=$1 base=$2 bad=0 f rel fl
    local seen; seen=$(mktemp)
    while IFS= read -r f; do
        strip "$f" | grep -qE '\bint\s+main\s*\(' || continue     # not an image
        rel=${f#"$dir"/}
        for fl in $(flags "$f" | sort -u); do
            if awk -v f="$rel" -v g="$fl" '$1==f && $2==g {ok=1} END {exit !ok}' "$base"; then
                echo "$rel $fl" >> "$seen"
            else
                echo "  $rel parses argv '$fl'"; bad=1
            fi
        done
    done < <(find "$dir" -name '*.c' | sort)
    while read -r f fl; do
        case "$f" in ''|\#*) continue ;; esac
        grep -qxF "$f $fl" "$seen" || { echo "  baseline line '$f $fl' is gone from the code -- delete it"; bad=1; }
    done < "$base"
    rm -f "$seen"
    return $bad
}

cd "$ROOT"
fail=0
if check src/vmsdecnet "$BASELINE"; then
    echo "ok: no shipped image under src/vmsdecnet parses argv '--' flags beyond the baseline"
else
    echo "FAIL: argv flag parsing in a shipped src/vmsdecnet image"; fail=1
fi
# NETACP.EXE itself takes no command line at all.
if awk '/^#if defined\(NETACP_IMAGE\)/,/^#elif/' src/vmsdecnet/engine/decnetd.c | grep -qE '^int main\(void\)$'; then
    echo "ok: NETACP.EXE's entry is main(void)"
else
    echo "FAIL: NETACP.EXE's entry is not main(void)"; fail=1
fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
nc=0
mkdir -p "$T/a/engine" "$T/b/fal"
cp src/vmsdecnet/engine/decnetd.c "$T/a/engine/decnetd.c"
printf 'int main(int argc, char **argv) { if (!strcmp(argv[1], "--fal-selftest")) return 1; return 0; }\n' >> "$T/a/engine/decnetd.c"
cp "$BASELINE" "$T/base"
if check "$T/a" "$T/base" >/dev/null; then echo "FAIL: negative control: a planted --fal-selftest was not caught"; fail=1; else nc=$((nc+1)); fi
cp src/vmsdecnet/fal/fal_main.c "$T/b/fal/fal_main.c"
printf 'static int planted(int c, char **v) { return getopt(c, v, "x"); }\n' >> "$T/b/fal/fal_main.c"
: > "$T/base2"
if check "$T/b" "$T/base2" >/dev/null; then echo "FAIL: negative control: a planted getopt() was not caught"; fail=1; else nc=$((nc+1)); fi
echo "ok: negative control caught $nc/2 planted argv parsers"
[ "$nc" = 2 ] || fail=1
[ "$fail" = 0 ] && echo NO_ARGV_FLAGS_OK || echo NO_ARGV_FLAGS_FAIL
exit $fail
