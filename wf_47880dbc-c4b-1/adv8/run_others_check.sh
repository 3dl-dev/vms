#!/bin/sh
set -e
apk add --no-cache gcc musl-dev binutils make linux-headers >/dev/null
export CC=gcc
for s in run_tcc_rms.sh run_tcc_native.sh run_tcc_selfhost.sh run_vmsrms_native.sh run_tcc_object_native.sh; do
    echo "############ $s ############"
    export WORK=/tmp/wk-$s
    rm -rf "$WORK"
    if sh /src/src/imgact/test/$s > /tmp/out-$s.log 2>&1; then
        echo "PASS: $s"
    else
        echo "FAIL: $s (exit $?)"
    fi
    tail -20 /tmp/out-$s.log
    echo
done
