#!/bin/bash
# OVMX User Acceptance Test — Scripted VMS Session
# Prerequisites: sshpass installed, OVMX container running on SSH_PORT (default 2222)
set -euo pipefail

SSH_PORT="${SSH_PORT:-2222}"
SSH_HOST="${SSH_HOST:-localhost}"
SSH_USER="${SSH_USER:-system}"
SSH_PASS="${SSH_PASS:-MANAGER}"

# Wait for SSH to be ready.
#
# Do NOT probe by running a remote command (`ssh host 'echo ready'`):
# vmssshd ignores the exec command string and always starts an interactive
# DCL login session (see src/vmsssh/vmssshd.c — EXEC is handled like SHELL),
# so a command-mode probe never returns and hangs the whole loop until the
# outer timeout fires (exit 124 — this was the bug, vms-0b7). Instead just
# wait for the TCP port to accept connections, which cannot hang.
echo "Waiting for SSH on ${SSH_HOST}:${SSH_PORT}..."
ssh_ready=0
for _ in $(seq 1 45); do
    if (exec 3<>"/dev/tcp/${SSH_HOST}/${SSH_PORT}") 2>/dev/null; then
        exec 3>&- 2>/dev/null || true
        ssh_ready=1
        break
    fi
    sleep 2
done
if [ "$ssh_ready" -ne 1 ]; then
    echo "ERROR: SSH port ${SSH_HOST}:${SSH_PORT} never opened" >&2
    exit 1
fi

echo "SSH is ready. Running VMS session test..."

# Run the scripted session.
# Bound it with `timeout` and `|| true`: the session runs on a PTY and one
# of the piped commands (e.g. HELP's pager) could block, and LOGOUT may
# exit non-zero — under `set -euo pipefail` either would abort the script
# before validation. Cap the wall time and keep whatever output we got so
# the validation below produces a clear pass/fail instead of a silent hang.
OUTPUT=$(timeout 120 env SSHPASS="$SSH_PASS" sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -p $SSH_PORT $SSH_USER@$SSH_HOST 2>&1 <<'VMSEOF' || true
SHOW TIME
SHOW SYSTEM
SHOW MEMORY
SHOW DEFAULT
SET DEFAULT SYS$MANAGER
SHOW DEFAULT
DIRECTORY
SHOW PROCESS
SHOW PROCESS /PRIVILEGES
SHOW LOGICAL SYS$LOGIN
DEFINE UAT_TEST "session_test_passed"
SHOW LOGICAL UAT_TEST
DEASSIGN UAT_TEST
SHOW USERS
SHOW TERMINAL
HELP SHOW
LOGOUT
VMSEOF
)

echo "=== Session Output ==="
echo "$OUTPUT"
echo "=== End Session Output ==="

# Validation
PASS=0
FAIL=0
ERRORS=""

check_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should contain '$1'"
    fi
}

check_not_contains() {
    if echo "$OUTPUT" | grep -qi "$1"; then
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should NOT contain '$1'"
    else
        PASS=$((PASS + 1))
    fi
}

check_regex() {
    if echo "$OUTPUT" | grep -qiE "$1"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: output should match regex '$1'"
    fi
}

# VMS date format (DD-MMM-YYYY)
check_regex '[0-9]{1,2}-(JAN|FEB|MAR|APR|MAY|JUN|JUL|AUG|SEP|OCT|NOV|DEC)-[0-9]{4}'

# SHOW DEFAULT should show SYS$MANAGER after SET DEFAULT
check_contains 'SYS$MANAGER'

# SHOW LOGICAL UAT_TEST should show the value
check_contains 'session_test_passed'

# SHOW PROCESS should show process info
check_contains 'SYSTEM'

# Privilege names should appear
check_regex '(TMPMBX|NETMBX|OPER|PRIV)'

# SHOW TERMINAL should show terminal info
check_regex '(Terminal|Device|VT100|_[A-Z])'

# HELP should produce output
check_regex '(SHOW|Additional information)'

# Unix leak checks
check_not_contains '/bin/bash'
check_not_contains 'Permission denied'
check_not_contains 'No such file or directory'
check_not_contains 'Segmentation fault'
check_not_contains '/home/'
check_not_contains 'errno'

echo ""
echo "=== UAT Results ==="
echo "Passed: $PASS"
echo "Failed: $FAIL"
if [ -n "$ERRORS" ]; then
    echo -e "Errors:$ERRORS"
fi

if [ $FAIL -gt 0 ]; then
    exit 1
fi
echo "All checks passed!"
