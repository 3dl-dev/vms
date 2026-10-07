#!/bin/bash
# usage: asend.sh 'line'   (sends base64-safe line to ${POD:-corpusalpha-1} console)
B=$(printf '%s' "$1" | base64 -w0)
kubectl -n ovmx-lab exec ${POD:-corpusalpha-1} -- sh -c "{ echo $B | base64 -d; echo; } > /lab/k8s-labs/${POD:-corpusalpha-1}/${NODE:-alpha1}/logs/${NODE:-alpha1}.log.in"
