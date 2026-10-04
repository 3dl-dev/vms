#!/bin/bash
# con.sh <n> <line>  -- send a line to vax<n>'s console
printf '%s\r' "$2" > /lab/k8s-labs/eb3lab/logs/vax$1.log.in
