#!/bin/bash
# states.sh <console.log> -- the ordered, deduplicated sequence of connectivity
# states this node's own SHOW CLUSTER showed for each system. This is what tells
# a p. 7-24 DISCONNECT tombstone (the rd vms-dfe wedge) apart from p. 7-25's
# deallocate-and-rebuild (a block that comes back NEW, or vanishes and returns).
set -u
tr -d '\r' < "$1" | grep -E '^\| [A-Z0-9]+ +\|' | awk -F'|' '
{
    node=$2; st=$5;
    gsub(/^ +| +$/,"",node); gsub(/^ +| +$/,"",st);
    if (st == "") next;
    if (st != last[node]) { seq[node] = seq[node] " -> " st; last[node]=st }
}
END { for (n in seq) printf "  %-7s%s\n", n, substr(seq[n],5) }' | sort
