#!/bin/sh
#
# test_runtime_target.sh - Rule 9 standing gate: ONE runtime target
#
# Operator ruling 2026-07-28 (CLAUDE.md Project-Specific Rule 9): the Docker
# RUNTIME layer is dead. OVMX has exactly one runtime -- the real-kernel/QEMU
# path, where vms.ko provides the VMS executive via /dev/vms.
#
# WHY THIS GATE EXISTS. This ruling was made, recorded in prose, and then
# forgotten -- an agent re-derived "Docker is a live dev/CI target" from the
# repo (the files are still there) and designed an executive around Docker's
# lack of /dev/vms. Prose did not hold. A gate does.
#
# The deeper trap it guards: because CI runs in Docker and Docker has no
# /dev/vms, the kernel executive is unprovable in CI -- so OVMX grew
# per-process userspace fakes (logical names, process table, event flags,
# mailboxes) that report success while sharing nothing. THE ARCHITECTURE
# DRIFTED TO FIT THE TEST HARNESS. This gate makes that drift loud.
#
# If you are here because this failed: do NOT add an allowlist entry. Either
# fail honestly when /dev/vms is absent (SS$_NOSUCHDEV, as sys_lock.c does),
# or move the work to the QEMU path.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "Rule 9 gate: one runtime target (kernel/QEMU); Docker is not a runtime"

# --- 1. The dead-legacy runtime files must stay marked ------------------
# They are retained only until CI migrates off them. While they exist they
# must announce that they are not a runtime, so nobody re-derives otherwise.
for f in "$SRC_ROOT/Dockerfile" "$SRC_ROOT/docker-compose.yml"; do
    [ -f "$f" ] || continue          # deleted == migration finished == fine
    if grep -q "DEAD LEGACY" "$f"; then
        echo "  OK: $(basename "$f") is marked dead legacy"
    else
        echo "FAIL: $(basename "$f") exists without the DEAD LEGACY marker"
        echo "  -> it is not a runtime target (Rule 9); mark it or delete it."
        status=1
    fi
done

# --- 2. No NEW Docker-runtime entry points -----------------------------
# Exactly one compose file may exist, and only as marked legacy.
extra=$(find "$SRC_ROOT" -maxdepth 2 -name "docker-compose*.y*ml" \
          -not -path "*/.claude/*" -not -path "*/build*" 2>/dev/null \
        | grep -v "^$SRC_ROOT/docker-compose.yml$" || true)
if [ -n "$extra" ]; then
    echo "FAIL: additional docker-compose entry point(s) — Docker is not a runtime:"
    echo "$extra" | sed 's/^/  /'
    status=1
else
    echo "  OK: no new docker-compose runtime entry points"
fi

# --- 3. No silent userspace fallback for executive facilities ----------
# The executive lives in vms.ko behind /dev/vms. When it is absent the
# correct behavior is an honest error, never a per-process fake that
# reports success. Flag code that branches on /dev/vms absence toward a
# "local"/"fallback"/"emulate" path.
# Comment lines are excluded: documenting the policy ("Docker has no /dev/vms,
# so $ENQ returns SS$_NOSUCHDEV") is exactly right and must not trip the gate.
# A real fallback is executable code, which does not start with a comment
# marker. Files that fail honestly (SS$_NOSUCHDEV present) are also exempt.
hits=$(grep -rnE '(no|without|missing|absent).{0,24}/dev/vms|/dev/vms.{0,40}(fall ?back|fallback|emulate|in-process|userspace fake)' \
        --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
       | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' \
       | while IFS=: read -r f rest; do
             grep -q 'SS\$_NOSUCHDEV' "$f" 2>/dev/null || echo "$f:$rest"
         done)
if [ -n "$hits" ]; then
    echo "FAIL: possible silent userspace fallback for an executive facility:"
    echo "$hits" | sed 's/^/  /'
    echo "  -> fail honestly (SS\$_NOSUCHDEV) instead; see CLAUDE.md Rule 9."
    status=1
else
    echo "  OK: no silent /dev/vms fallback path detected"
fi

# --- 4. Rule 9 must still be in CLAUDE.md ------------------------------
# The gate enforces the mechanics; CLAUDE.md carries the reasoning. If the
# rule is deleted, the gate is cargo cult -- fail loudly rather than drift.
if grep -q "One runtime target: the kernel/QEMU path" "$SRC_ROOT/CLAUDE.md" 2>/dev/null; then
    echo "  OK: CLAUDE.md still carries Rule 9"
else
    echo "FAIL: CLAUDE.md no longer carries Rule 9 (one runtime target)"
    echo "  -> this gate enforces a rule that must stay written down."
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "Rule 9 gate: PASS"
else
    echo "Rule 9 gate: FAIL"
fi
exit "$status"
