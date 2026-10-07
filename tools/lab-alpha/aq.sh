#!/bin/bash
# usage: aq.sh 'DCL command' [wait]  -> prints console output after the command
M="@@$RANDOM"
$(dirname "$0")/asend.sh "WRITE SYS\$OUTPUT \"$M\"" >/dev/null
sleep 2
$(dirname "$0")/asend.sh "$1" >/dev/null
sleep ${2:-4}
$(dirname "$0")/asend.sh "WRITE SYS\$OUTPUT \"END$M\"" >/dev/null
sleep 2
kubectl -n ovmx-lab exec ${POD:-corpusalpha-1} -- sh -c "tr -d '\000' < /lab/k8s-labs/${POD:-corpusalpha-1}/${NODE:-alpha1}/logs/${NODE:-alpha1}.log | awk '/$M/{c++} c>=2{print}' " | sed -n '1,400p'
