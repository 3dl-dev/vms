#!/bin/bash
# rd vms-0a7 S3 gate (a): no Linux-libc thread, wait or pipe in the DECnet code
# or DCL -- the libc RATCHET for pthread_create / poll / pipe (Baron 2026-11-02:
# "don't drop to linux LIBC, use all VMS-authentic API").
#
#   src/vmsdecnet/**  must contain NONE: NETACP waits with $WFLOR on an async
#                     $QIO and $SETIMR, its servers are $CREPRC'd processes;
#                     the test peers that used threads live in tests/.
#   src/vmsdcl/*      may contain no MORE than tests/vmsdecnet/libc_wait_baseline.txt
#                     records per file (the compat lane's existing debt), and a
#                     file whose count FALLS must lower its line (a ratchet).
#   NETACP.EXE's link (src/vmsdecnet/engine/CMakeLists.txt) names no Threads
#                     and no sysoutput_rms.c (the pipe + reader thread + poll()
#                     run-time routine NETACP replaced by writing NETACP.LOG
#                     through RMS itself).
#
# NEGATIVE CONTROL: each forbidden call planted in a copy of netacp.c, and one
# planted in a copy of a DCL file over its baseline, must be caught.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BASELINE="$ROOT/tests/vmsdecnet/libc_wait_baseline.txt"
FORBID='\b(pthread_create|thrd_create|poll|ppoll|select|pselect|epoll_wait|epoll_pwait|pipe|pipe2)\s*\('

count() {   # count FILE -> number of forbidden calls (comments + strings stripped)
    sed -e 's://.*$::' "$1" | awk '
        { line=$0
          while (1) {
            if (inc) { e=index(line,"*/"); if (e==0) { line=""; break } line=substr(line,e+2); inc=0 }
            s=index(line,"/*"); if (s==0) break
            pre=substr(line,1,s-1); rest=substr(line,s+2); e=index(rest,"*/")
            if (e==0) { line=pre; inc=1; break } line=pre substr(rest,e+2)
          }
          gsub(/"([^"\\]|\\.)*"/, "\"\"", line); print line }' \
        | grep -cE "$FORBID"
}

check_tree() {  # check_tree NETACPDIR DCLDIR CMAKE -> prints problems, returns 1 on any
    local bad=0 f n want
    while IFS= read -r f; do
        n=$(count "$f")
        if [ "$n" != 0 ]; then echo "  $f: $n thread/wait/pipe call(s) -- none allowed under src/vmsdecnet"; bad=1; fi
    done < <(find "$1" -name '*.[ch]' | sort)
    while IFS= read -r f; do
        n=$(count "$f")
        rel=${f#"$ROOTX"/}
        want=$(awk -v f="$rel" '$1==f {print $2}' "$BASELINE")
        want=${want:-0}
        if [ "$n" -gt "$want" ]; then echo "  $rel: $n thread/wait/pipe call(s), baseline $want"; bad=1; fi
        if [ "$n" -lt "$want" ]; then echo "  $rel: $n call(s) but baseline says $want -- lower its line in $(basename "$BASELINE")"; bad=1; fi
    done < <(find "$2" -name '*.[ch]' | sort)
    if awk '/add_executable\(netacp_exe/,/^install\(TARGETS netacp_exe/' "$3" | grep -qE 'Threads::Threads|sysoutput_rms'; then
        echo "  netacp_exe links Threads or sysoutput_rms.c"; bad=1
    fi
    return $bad
}

cd "$ROOT"
ROOTX="$ROOT"
fail=0
if check_tree src/vmsdecnet src/vmsdcl src/vmsdecnet/engine/CMakeLists.txt; then
    echo "ok: src/vmsdecnet has no pthread_create/poll/pipe; DCL is at its baseline; NETACP links no Threads"
else
    echo "FAIL: libc thread/wait/pipe ratchet"; fail=1
fi

# negative controls, on a copy of the tree
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/dn" "$T/dcl"
cp src/vmsdcl/*.[ch] "$T/dcl/"
ROOTX="$T"
sed 's|^src/vmsdcl/|dcl/|' "$BASELINE" > "$T/baseline"
BASELINE="$T/baseline"
nc=0; total=0
for call in 'pthread_create(&t, 0, f, 0);' 'poll(&p, 1, 0);' 'ppoll(&p, 1, 0, 0);' 'select(1, 0, 0, 0, 0);' 'pipe(fds);' 'pipe2(fds, 0);'; do
    total=$((total+1))
    rm -f "$T/dn/"*; cp src/vmsdecnet/engine/netacp.c "$T/dn/netacp.c"
    printf 'static void planted(void) { %s }\n' "$call" >> "$T/dn/netacp.c"
    if check_tree "$T/dn" "$T/dcl" src/vmsdecnet/engine/CMakeLists.txt >/dev/null; then
        echo "FAIL: negative control: '$call' planted in netacp.c was not caught"; fail=1
    else nc=$((nc+1)); fi
done
rm -f "$T/dn/"*
total=$((total+1))
printf 'static void planted(void) { pthread_create(&t, 0, f, 0); }\n' >> "$T/dcl/dcl_cmd_set.c"
if check_tree "$T/dn" "$T/dcl" src/vmsdecnet/engine/CMakeLists.txt >/dev/null; then
    echo "FAIL: negative control: a pthread_create over DCL's baseline was not caught"; fail=1
else nc=$((nc+1)); fi
cp src/vmsdcl/dcl_cmd_set.c "$T/dcl/dcl_cmd_set.c"
total=$((total+1))
sed 's/^\(add_executable(netacp_exe.*\)$/\1\ntarget_link_libraries(netacp_exe PRIVATE Threads::Threads)/' \
    src/vmsdecnet/engine/CMakeLists.txt > "$T/CMakeLists.txt"
if check_tree "$T/dn" "$T/dcl" "$T/CMakeLists.txt" >/dev/null; then
    echo "FAIL: negative control: Threads in NETACP's link was not caught"; fail=1
else nc=$((nc+1)); fi
echo "ok: negative control caught $nc/$total planted calls"
[ "$nc" = "$total" ] || fail=1
[ "$fail" = 0 ] && echo NETACP_NO_LIBC_WAIT_OK || echo NETACP_NO_LIBC_WAIT_FAIL
exit $fail
