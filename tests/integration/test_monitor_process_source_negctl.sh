#!/bin/sh
#
# test_monitor_process_source_negctl.sh - negative controls for the vms-c840
# gate (tests/integration/test_monitor_process_source.sh).
#
# A gate that cannot go red is not a gate (CLAUDE.md Rule 7/8). The gate proves
# MONITOR and SHOW SYSTEM share the executive process table as their one source.
# This control BUILDS a mutant MONITOR that reintroduces the exact vms-c840
# defect -- an independent /proc process scan -- and requires the gate to catch
# it, both structurally (the /proc scan is back in the source) and behaviourally
# (MONITOR then lists Linux tasks and never opens /dev/vms, so its set diverges
# from SHOW SYSTEM's and property 3 fails). The mutation is applied in a sandbox
# copy of the tree and the vms_monitor target is rebuilt there; a sandbox
# configure + build of one tool target is cheap.
#
# POSITIVE CONTROL first: the unmutated sandbox MONITOR must PASS the gate, so a
# red below is attributable to the mutation and not to a broken sandbox.
#
# Usage: test_monitor_process_source_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_monitor_process_source.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-c840 negative controls: a MONITOR with its own /proc scan must turn the gate RED"

command -v cmake >/dev/null 2>&1 || {
    echo "FAIL: cmake is not available, so no mutant can be built"
    echo "  -> this control cannot be evaluated; reported as FAILED, never skipped (Rule 10)"
    exit 1
}

# A DCL.EXE is needed for the gate's consistency comparison. Prefer one passed
# in by the caller ($2, e.g. CMake's $<TARGET_FILE:vmsdcl>), else an already
# built one from the caller's tree; the gate only reads its output.
DCL="${2:-}"
if [ -z "$DCL" ] || [ ! -x "$DCL" ]; then
    DCL=""
    for cand in "$SRC_ROOT/build/bin/DCL.EXE" "$SRC_ROOT/build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

ROOT="$WORK/tree"
mkdir -p "$ROOT"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp -a "$SRC_ROOT/tools" "$ROOT/tools"
cp -a "$SRC_ROOT/tests" "$ROOT/tests"
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"

MON_C="$ROOT/tools/vms_monitor.c"
cp "$MON_C" "$WORK/vms_monitor.c.orig"
restore() { cp "$WORK/vms_monitor.c.orig" "$MON_C"; }

BUILD="$WORK/build"
build_monitor() {
    cmake -B "$BUILD" -S "$ROOT" \
        -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON \
        >"$WORK/cmake.log" 2>&1 || return 1
    cmake --build "$BUILD" --target vms_monitor >"$WORK/build.log" 2>&1 || return 1
    [ -x "$BUILD/bin/MONITOR.EXE" ]
}

# The gate needs a DCL to compare against. If the caller built one, reuse it;
# otherwise build it once in the sandbox too.
ensure_dcl() {
    if [ -n "$DCL" ] && [ -x "$DCL" ]; then return 0; fi
    cmake --build "$BUILD" --target vmsdcl >"$WORK/dcl_build.log" 2>&1 || return 1
    if [ -x "$BUILD/bin/DCL.EXE" ]; then DCL="$BUILD/bin/DCL.EXE"; return 0; fi
    return 1
}

# The mutant replaces read_processes()'s body with the pre-fix /proc walk: a
# real second, divergent source. Anchored on the function signature so a
# reformatted tree makes the mutation FAIL to apply (reported), never silently
# no-op into a false pass.
mutate_proc_scan() {
    awk '
        /^static int read_processes\(proc_info_t \*procs, int max_procs\)$/ { sig = 1 }
        sig && /^\{$/ && !done {
            print "{"
            print "    DIR *d = opendir(\"/proc\");"
            print "    if (!d) return 0;"
            print "    int count = 0;"
            print "    struct dirent *e;"
            print "    while ((e = readdir(d)) != NULL && count < max_procs) {"
            print "        if (!isdigit((unsigned char)e->d_name[0])) continue;"
            print "        int pid = atoi(e->d_name);"
            print "        if (pid <= 0) continue;"
            print "        char path[40];"
            print "        snprintf(path, sizeof(path), \"/proc/%d/stat\", pid);"
            print "        FILE *fp = fopen(path, \"r\");"
            print "        if (!fp) continue;"
            print "        char buf[1024];"
            print "        if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); continue; }"
            print "        fclose(fp);"
            print "        char *s = strchr(buf, (int)0x28), *en = strrchr(buf, (int)0x29);"
            print "        if (!s || !en || en <= s) continue;"
            print "        proc_info_t *p = &procs[count++];"
            print "        memset(p, 0, sizeof(*p));"
            print "        p->pid = pid;"
            print "        size_t ln = (size_t)(en - s - 1);"
            print "        if (ln >= sizeof(p->name)) ln = sizeof(p->name) - 1;"
            print "        size_t k; for (k = 0; k < ln; k++) p->name[k] = (char)toupper((unsigned char)s[1+k]);"
            print "        p->name[ln] = 0;"
            print "    }"
            print "    closedir(d);"
            print "    return count;"
            print "}"
            # swallow the original body up to and including its closing brace
            depth = 1
            while ((getline line) > 0) {
                n = gsub(/\{/, "{", line); m = gsub(/\}/, "}", line);
                depth += n - m;
                if (depth <= 0) break;
            }
            done = 1; sig = 0; next
        }
        { print }
    ' "$WORK/vms_monitor.c.orig" > "$MON_C"

    # The reinjected /proc scan needs <dirent.h> (DIR / opendir / readdir /
    # struct dirent). The FIXED base file no longer includes it -- it has no
    # /proc scan -- so the mutation must supply the header itself, or the
    # mutant would not COMPILE and a mutant that cannot build is not a valid
    # negative control. Insert it ahead of <signal.h> (present in the base).
    if ! grep -q '#include <dirent.h>' "$MON_C"; then
        sed -i 's|#include <signal.h>|#include <dirent.h>\n#include <signal.h>|' "$MON_C"
    fi

    if ! grep -q 'opendir("/proc")' "$MON_C" || ! grep -q '#include <dirent.h>' "$MON_C"; then
        echo "  FAIL: mutation could not be applied -- read_processes()'s signature"
        echo "        moved, so this control tested NOTHING"
        failed=$((failed + 1)); status=1; restore; return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# POSITIVE CONTROL
# ---------------------------------------------------------------------------
if build_monitor && ensure_dcl && \
   sh "$GATE" "$BUILD/bin/MONITOR.EXE" "$DCL" >"$WORK/pos.log" 2>&1; then
    echo "  PASS: positive control - unmutated sandbox MONITOR passes the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - the unmutated sandbox does not pass, so no"
    echo "        control below can attribute its RED to a mutation"
    sed 's/^/          | /' "$WORK/pos.log" 2>/dev/null | tail -30
    tail -20 "$WORK/build.log" 2>/dev/null | sed 's/^/          B /'
    failed=$((failed + 1)); status=1
fi

# ---------------------------------------------------------------------------
# MUTANT: MONITOR walks /proc again (the vms-c840 defect, restored)
# ---------------------------------------------------------------------------
if mutate_proc_scan; then
    if ! build_monitor; then
        echo "  FAIL: /proc-scan mutant did not build"
        tail -20 "$WORK/build.log" | sed 's/^/          | /'
        failed=$((failed + 1)); status=1
    else
        if sh "$GATE" "$BUILD/bin/MONITOR.EXE" "$DCL" >"$WORK/mut.log" 2>&1; then
            echo "  FAIL: the gate PASSED a MONITOR that scans /proc -- it does not"
            echo "        catch the vms-c840 defect it exists to catch"
            sed 's/^/          | /' "$WORK/mut.log" | tail -20
            failed=$((failed + 1)); status=1
        else
            echo "  PASS: the /proc-scan mutant drove the gate RED (as required)"
            passed=$((passed + 1))
            # Attribute the red: the gate must have flagged the structural
            # /proc scan and/or the executive-read/consistency property.
            if grep -q 'still enumerates processes from /proc' "$WORK/mut.log" \
               || grep -q 'did not open /dev/vms' "$WORK/mut.log" \
               || grep -q 'DIFFERENT process sets' "$WORK/mut.log"; then
                echo "  PASS: the red is attributed to the reintroduced /proc scan"
                passed=$((passed + 1))
            else
                echo "  FAIL: the gate went red but not for the /proc-scan reason"
                sed 's/^/          | /' "$WORK/mut.log" | tail -20
                failed=$((failed + 1)); status=1
            fi
        fi
    fi
    restore
fi

echo "vms-c840 negative controls: $passed passed, $failed failed"
if [ "$status" -eq 0 ]; then
    echo "vms-c840 negative controls: PASS"
else
    echo "vms-c840 negative controls: FAIL"
fi
exit $status
