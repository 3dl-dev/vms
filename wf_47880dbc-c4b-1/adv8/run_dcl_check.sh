#!/bin/sh
set -e
apk add --no-cache gcc musl-dev binutils make linux-headers >/dev/null
export CC=gcc WORK=/tmp/dcl-native
sh /src/src/imgact/test/run_dcl_native.sh
