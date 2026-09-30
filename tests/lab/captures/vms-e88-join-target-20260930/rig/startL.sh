#!/bin/bash
# The LIVE-SHAPED stall (rd vms-8c54). The M matrix armed on the REAL VAX's own
# commit of the joiner's addition, which lands BEFORE the joiner has learned it
# is a member -- so every M arm took the join's "lost the connection before this
# node was admitted" path and the MEMBER reconnect ladder was never entered.
#
# Baron's live console is the other shape: "Node OVMXA ... this node is now a
# VAXcluster member" FIRST, then the VAX loses its connection, then CNXMGRERR.
# So this matrix arms on the OVMX node's OWN membership line and stalls it
# INSIDE the real VAX's reconnect window (RECNXINTERVAL 20 s, PE listen timeout
# ~8 s): the VAX closes the virtual circuit and keeps the CSB, and the node wakes
# while the VAX has NOT given it up.
set -u
export MARK_LOG=/lab/run-e88/OVMXB.console.log
export MARK="this node is now a VAXcluster member"
export TRIG_DELAY="${TRIG_DELAY:-2}"
exec bash /lab/run-e88/matrix.sh "$@"
