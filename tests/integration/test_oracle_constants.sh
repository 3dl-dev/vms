#!/bin/sh
#
# test_oracle_constants.sh - header constants vs the OpenVMS VAX V7.3 oracle (vms-619).
# A ratchet: new wrong values and stale known-mismatch entries both fail; the list
# docs/oracle/constants-known-mismatch.txt only shrinks. See
# tools/compat/check_oracle_constants.py. Negative control: test_oracle_constants_negctl.sh.
set -eu
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
exec python3 "$SRC/tools/compat/check_oracle_constants.py" --root="$SRC"
