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
# If you are here because this failed: do NOT add an allowlist entry. Move the
# work to the QEMU path, where a real executive exists.
#
# NOTE, 2026-07-29 -- this header used to end "...or fail honestly when
# /dev/vms is absent (SS$_NOSUCHDEV, as sys_lock.c does)". THAT ADVICE IS
# SUPERSEDED and was itself the defect. Governing rule: what would VMS do?
# SYSBOOT loads the executive before any process exists, so VMS is NEVER in
# the state where a running system has no executive. "Fail honestly with
# SS$_NOSUCHDEV" encoded "OVMX runs, minus the executive" -- a system that
# does not exist. A handled-but-impossible-on-VMS state is the same class of
# lie as a fake that reports success; it just fails more politely.
#
# The rule now is a GUARANTEE, not an error path: PID 1 refuses to boot
# without the executive and pins it open for the life of the system
# (src/ovmx_init/ovmx_init.c, executive_attach), so no caller can observe its
# absence. Per-call fallbacks are DELETED, not corrected. SS$_NOSUCHDEV keeps
# its real meaning -- a caller named a VMS device that does not exist.

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
# Comment lines are excluded: documenting the policy is exactly right and must
# not trip the gate. A real fallback is executable code, which does not start
# with a comment marker.
#
# THE SS$_NOSUCHDEV EXEMPTION IS GONE (2026-07-29). This check used to skip any
# file that merely CONTAINED the string "SS$_NOSUCHDEV" anywhere -- so a file
# could carry a genuine fallback and be waved through because some unrelated
# line elsewhere in it mentioned the constant. Worse, the exemption encoded the
# superseded rule: "fail honestly with SS$_NOSUCHDEV" is no longer the correct
# behaviour for an absent executive (see the header). There is nothing left to
# exempt.
hits=$(grep -rnE '(no|without|missing|absent).{0,24}/dev/vms|/dev/vms.{0,40}(fall ?back|fallback|emulate|in-process|userspace fake)' \
        --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
       | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' || true)
if [ -n "$hits" ]; then
    echo "FAIL: possible silent userspace fallback for an executive facility:"
    echo "$hits" | sed 's/^/  /'
    echo "  -> the executive is integral; make its absence unreachable, do not"
    echo "     handle it. See CLAUDE.md Rule 9 and docs/design-executive-retrofit.md."
    status=1
else
    echo "  OK: no silent /dev/vms fallback path detected"
fi

# --- 3b. The executive must be INTEGRAL, not optional -------------------
# The guarantee that makes check 3 enforceable: PID 1 refuses to bring the
# system up unless the executive is reachable, and pins it for the life of
# the system. If this erodes, every per-call fallback we just deleted becomes
# necessary again -- so the gate holds the guarantee itself, not just its
# consequences.
init_c="$SRC_ROOT/src/ovmx_init/ovmx_init.c"
if [ ! -f "$init_c" ]; then
    echo "FAIL: $init_c is missing -- the boot-time executive guarantee lives there"
    status=1
else
    missing=""
    grep -q 'executive_attach' "$init_c" || missing="$missing executive_attach()"
    grep -q 'execinit_halt' "$init_c"    || missing="$missing execinit_halt()"
    grep -q '"/dev/vms"' "$init_c"       || missing="$missing open(/dev/vms)"
    if [ -n "$missing" ]; then
        echo "FAIL: ovmx_init.c no longer establishes the executive guarantee:$missing"
        echo "  -> PID 1 must refuse to boot without /dev/vms and hold it open"
        echo "     for the life of the system (pins vms.ko: rmmod -> EBUSY)."
        status=1
    else
        echo "  OK: PID 1 still refuses to boot without the executive"
    fi
fi

# --- 3c. No per-call "is the executive there?" test --------------------
# Opening /dev/vms is unconditional; branching on whether it succeeded is the
# reintroduction of the deleted fallback, whatever status it returns. This is
# the precise shape that was removed from sys_lock.c's ensure_kif_open():
#     return vms_kif_open() >= 0 ? 0 : -1;     <- flagged
#     (void)vms_kif_open();                    <- fine
probe=$(grep -rnE 'vms_kif_open[[:space:]]*\([[:space:]]*\)[[:space:]]*(<|>|=|!|\?)|(if|while)[[:space:]]*\([^)]*vms_kif_open' \
         --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' || true)
if [ -n "$probe" ]; then
    echo "FAIL: code branches on whether the executive could be opened:"
    echo "$probe" | sed 's/^/  /'
    echo "  -> that condition is unreachable by construction (PID 1 guarantees"
    echo "     it). Do not handle it; open unconditionally."
    status=1
else
    echo "  OK: no per-call executive-presence test"
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
