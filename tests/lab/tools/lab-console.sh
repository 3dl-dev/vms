#!/bin/sh
# lab-console.sh — the one way to talk to an ovmx-lab emulator console (the repo copy of
# the maintainer's ovmx-lab-console; see tests/lab/README.md for the lab layout).
# Baron-approved scope (2026-10-08 / 2026-10-11): VMS console input (incl. lab
# test-account logins) and SIMH emulator control (^E, EXAMINE, SHOW, SET,
# ATTACH/DETACH of xq/rq units, CONTINUE). ovmx-lab namespace only.
#
#   lab-console.sh send <pod> <node> <line>    send one line (CR appended)
#   lab-console.sh ctrl <pod> <node> <letter>  send ^<letter> (e.g. E for sim>)
#   lab-console.sh read <pod> <node> [lines]   print the console tail (default 40)
#
# Lines travel base64-encoded so DCL '$' and '"' survive the shell layers.
set -eu
[ $# -ge 3 ] || { sed -n '2,12p' "$0"; exit 2; }
op=$1 pod=$2 node=$3; shift 3
case "$pod" in *[!A-Za-z0-9-]*|"") echo "bad pod name" >&2; exit 2;; esac
case "$node" in *[!A-Za-z0-9_-]*|"") echo "bad node name" >&2; exit 2;; esac

# Console log/FIFO location differs between lab generations; resolve inside the pod.
locate='for d in /lab/k8s-labs/'"$pod"'/'"$node"'/logs /data/training/vax/k8s-labs/'"$pod"'/logs /lab/k8s-labs/'"$pod"'/logs; do
  [ -e "$d/'"$node"'.log" ] && { echo "$d/'"$node"'.log"; exit 0; }; done; exit 1'

k() { kubectl -n ovmx-lab exec "$pod" -- sh -c "$1"; }

case "$op" in
  send)
    [ $# -ge 1 ] || { echo "send needs a line" >&2; exit 2; }
    b=$(printf '%s\r' "$*" | base64 -w0)
    k "L=\$( $locate ) || { echo 'console not found' >&2; exit 1; }; echo $b | base64 -d > \"\$L.in\"" ;;
  ctrl)
    [ $# -eq 1 ] || { echo "ctrl needs one letter" >&2; exit 2; }
    case "$1" in [A-Za-z]) ;; *) echo "ctrl takes a single letter" >&2; exit 2;; esac
    code=$(printf '%d' "'$(printf '%s' "$1" | tr a-z A-Z)"); code=$((code - 64))
    b=$(printf "\\$(printf '%03o' "$code")" | base64 -w0)
    k "L=\$( $locate ) || { echo 'console not found' >&2; exit 1; }; echo $b | base64 -d > \"\$L.in\"" ;;
  read)
    n=${1:-40}; case "$n" in *[!0-9]*) echo "lines must be a number" >&2; exit 2;; esac
    k "L=\$( $locate ) || { echo 'console not found' >&2; exit 1; }; tr -d '\\000\\r' < \"\$L\" | tail -n $n" ;;
  *) echo "unknown op: $op" >&2; exit 2 ;;
esac
