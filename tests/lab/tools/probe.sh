#!/bin/bash
# probe.sh <node#> <tag> <cmd:sleep> ...   -- drive a VAX console, capture between markers
N=$1; TAG=$2; shift 2
T=/tmp/clean-vax1-test
F=$T/vax$N.log.in
say(){ printf '%s\r' "$1" > "$F"; sleep "${2:-2}"; }
START=$(wc -c < "$T/vax$N.log")
say "WRITE SYS\$OUTPUT \"===$TAG-BEGIN===\"" 2
for spec in "$@"; do
  cmd="${spec%%::*}"; slp="${spec##*::}"
  say "$cmd" "$slp"
done
say "WRITE SYS\$OUTPUT \"===$TAG-END===\"" 2
tail -c +$START "$T/vax$N.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
