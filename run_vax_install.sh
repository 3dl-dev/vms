#!/bin/bash
set -o pipefail
cd /home/baron/projects/vms/.claude/worktrees/agent-a763c7d4b77da0bb7
export CACHE_DIR=/home/baron/projects/vms/.boot-cache/lab-vax
export FORCE_CROSS_BUILD=1
export FORCE_VAX_IMAGES=1
export FORCE_OS_KIT=1
LOG=/home/baron/projects/vms/.claude/worktrees/agent-a763c7d4b77da0bb7/vax-install.log
: > "$LOG"
timeout 5400 tests/lab-vax/run-boot.sh install >> "$LOG" 2>&1
echo "EXITCODE=$?" >> "$LOG"
