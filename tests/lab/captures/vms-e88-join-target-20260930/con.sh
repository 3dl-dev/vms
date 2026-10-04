#!/bin/bash
# con.sh <n> <line>  -- send a line to vax<n>'s console
printf '%s\r' "$2" > /lab/k8s-labs/e88lab/logs/vax$1.log.in
