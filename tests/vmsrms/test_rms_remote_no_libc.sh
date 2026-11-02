#!/bin/bash
# rd vms-0a7 (S1): RMS remote file access reaches the network only through
# VMS services -- $ASSIGN _NET:, $QIOW IO$_ACCESS / IO$_WRITEVBLK /
# IO$_READVBLK / IO$_DEACCESS, $DASSGN -- never Linux libc I/O, a thread, a
# pipe or a poll (Baron 2026-11-02: "don't drop to linux LIBC"). This scans the
# RMS remote module, the DAP codec it compiles in, and the DCL layout over it.
#
# NEGATIVE CONTROL: the same scan over a copy of rms_remote.c with one libc
# call planted (each forbidden call in turn) must FAIL, so the gate has teeth.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FILES="src/vmsrms/rms_remote.c src/vmsrms/rms_dap.c src/vmsdecnet/dap/dnet_dap.c src/vmsdcl/dcl_remote_file.c"
# libc / raw-syscall I/O, process, thread and wait entry points.
FORBID='\b(read|write|open|openat|close|pipe|pipe2|poll|ppoll|select|pselect|socket|connect|bind|listen|accept|send|recv|sendto|recvfrom|ioctl|fcntl|dup|dup2|fork|vfork|execv|execve|execvp|system|popen|nanosleep|usleep|sleep|syscall|pthread_create|fopen|fdopen)\s*\('

scan() {   # scan FILE... -> prints offending lines; returns 1 if any
    local bad=0 f hits
    for f in "$@"; do
        # strip comments and string literals before matching
        hits=$(sed -e 's://.*$::' "$f" | awk '
            { line=$0
              while (1) {
                if (inc) { e=index(line,"*/"); if (e==0) { line=""; break } line=substr(line,e+2); inc=0 }
                s=index(line,"/*"); if (s==0) break
                pre=substr(line,1,s-1); rest=substr(line,s+2); e=index(rest,"*/")
                if (e==0) { line=pre; inc=1; break } line=pre substr(rest,e+2)
              }
              gsub(/"([^"\\]|\\.)*"/, "\"\"", line); print NR": "line }' \
            | grep -E "$FORBID" | grep -vE 'sys\$|\.(send|recv|open|close)\b|->(send|recv|open|close)\b')
        if [ -n "$hits" ]; then echo "$f:"; echo "$hits" | sed 's/^/    /'; bad=1; fi
    done
    return $bad
}

cd "$ROOT"
fail=0
if scan $FILES; then echo "ok: RMS remote access + DAP codec + DCL layout use no libc I/O"; else echo "FAIL: libc I/O in the RMS remote path"; fail=1; fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
nc=0
for call in 'write(1, "x", 1);' 'pipe(fds);' 'poll(&p, 1, 0);' 'pthread_create(&t, 0, f, 0);' 'socket(2, 1, 0);' 'nanosleep(&ts, 0);'; do
    cp src/vmsrms/rms_remote.c "$T/m.c"
    printf 'static void planted(void) { %s }\n' "$call" >> "$T/m.c"
    if scan "$T/m.c" >/dev/null; then echo "FAIL: negative control: planted '$call' was not caught"; fail=1; else nc=$((nc+1)); fi
done
echo "ok: negative control caught $nc/6 planted libc calls"
[ "$nc" = 6 ] || fail=1
[ "$fail" = 0 ] && echo RMS_REMOTE_NO_LIBC_OK || echo RMS_REMOTE_NO_LIBC_FAIL
exit $fail
