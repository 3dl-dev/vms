#!/bin/bash
# rd vms-0a7 S2: RTPAD.EXE (SET HOST) does its work only through VMS services --
# CLI$GET_VALUE / CLI$PRESENT for its command, $ASSIGN _NET: + $QIOW for the
# link, $ASSIGN TT: + $QIOW for the terminal, $GETDVIW, $SETIMR + $WFLOR to wait
# -- never Linux libc (Baron 2026-11-02: "don't drop to linux LIBC, use all
# VMS-authentic API"). This scans RTPAD and the CTERM codec it runs on for libc
# / raw-syscall I/O, terminal, process, thread, wait and stdio entry points, and
# checks that the image takes no argv at all (no Unix --flags, rd vms-18c9).
#
# NEGATIVE CONTROL: the same scan over a copy of rtpad.c with one forbidden
# call planted (each in turn), and a copy whose main takes argv, must FAIL.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FILES="src/vmsdecnet/rtpad/rtpad.c src/vmsdecnet/rtpad/rtpad_session.c src/vmsdecnet/cterm/dnet_cterm.c src/vmsdecnet/cterm/dnet_cterm_hostfsm.c"
FORBID='\b(read|write|open|openat|close|pipe|pipe2|poll|ppoll|select|pselect|epoll_wait|socket|connect|bind|listen|accept|send|recv|sendto|recvfrom|ioctl|fcntl|dup|dup2|fork|vfork|execv|execve|execvp|system|popen|nanosleep|usleep|sleep|syscall|pthread_create|fopen|fdopen|isatty|tcgetattr|tcsetattr|cfmakeraw|openpty|printf|fprintf|puts|fputs|fflush|getchar|fgets|getenv|signal|sigaction)\s*\('

strip() {   # FILE -> numbered source with comments and string literals blanked
    sed -e 's://.*$::' "$1" | awk '
        { line=$0
          while (1) {
            if (inc) { e=index(line,"*/"); if (e==0) { line=""; break } line=substr(line,e+2); inc=0 }
            s=index(line,"/*"); if (s==0) break
            pre=substr(line,1,s-1); rest=substr(line,s+2); e=index(rest,"*/")
            if (e==0) { line=pre; inc=1; break } line=pre substr(rest,e+2)
          }
          gsub(/"([^"\\]|\\.)*"/, "\"\"", line); print NR": "line }'
}

scan() {    # FILE... -> offending lines; returns 1 if any
    local bad=0 f hits
    for f in "$@"; do
        hits=$(strip "$f" | grep -E "$FORBID" | grep -vE 'sys\$|cli\$|->(send|recv|open|close|read_line|read_record|show|record|link_send)\b|\.(send|recv|open|close)\b')
        if [ -n "$hits" ]; then echo "$f:"; echo "$hits" | sed 's/^/    /'; bad=1; fi
    done
    return $bad
}

noargv() {  # FILE -> 0 when main takes no arguments
    strip "$1" | grep -qE '\bint[[:space:]]+main[[:space:]]*\([[:space:]]*void[[:space:]]*\)' &&
        ! strip "$1" | grep -qE '\bargv\b|\bargc\b'
}

cd "$ROOT"
fail=0
if scan $FILES; then echo "ok: RTPAD and its CTERM codec use no libc I/O, stdio, terminal, wait or thread call"
else echo "FAIL: libc in the SET HOST path"; fail=1; fi
if noargv src/vmsdecnet/rtpad/rtpad.c; then echo "ok: RTPAD.EXE takes no argv (its command comes from CLI\$GET_VALUE)"
else echo "FAIL: RTPAD.EXE reads argv"; fail=1; fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
nc=0; total=0
for call in 'write(1, "x", 1);' 'pipe(fds);' 'poll(&p, 1, 0);' 'pthread_create(&t, 0, f, 0);' \
            'socket(2, 1, 0);' 'nanosleep(&ts, 0);' 'isatty(0);' 'printf("x");' 'tcsetattr(0, 0, &t);'; do
    total=$((total+1))
    cp src/vmsdecnet/rtpad/rtpad.c "$T/m.c"
    printf 'static void planted(void) { %s }\n' "$call" >> "$T/m.c"
    if scan "$T/m.c" >/dev/null; then echo "FAIL: negative control: planted '$call' was not caught"; fail=1; else nc=$((nc+1)); fi
done
total=$((total+1))
sed 's/^int main(void)$/int main(int argc, char **argv)/' src/vmsdecnet/rtpad/rtpad.c > "$T/a.c"
if noargv "$T/a.c"; then echo "FAIL: negative control: a main taking argv was not caught"; fail=1; else nc=$((nc+1)); fi
echo "ok: negative control caught $nc/$total planted violations"
[ "$nc" = "$total" ] || fail=1
[ "$fail" = 0 ] && echo RTPAD_NO_LIBC_OK || echo RTPAD_NO_LIBC_FAIL
exit $fail
