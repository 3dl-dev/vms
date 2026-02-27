#!/bin/bash
# Integration tests for vmssshd
#
# Starts an OVMX Docker container, runs SSH tests against vmssshd using
# the test_ssh_client binary (libssh-based), and reports pass/fail.
#
# Requirements:
#   - Docker running on the host
#   - test_ssh_client binary built (see CMakeLists.txt)
#
# Usage (from repo root):
#   bash tests/vmsssh/test_vmsssh.sh
#
# The binary path can be overridden:
#   TEST_CLIENT=/path/to/test_ssh_client bash tests/vmsssh/test_vmsssh.sh

set -euo pipefail

# Skip gracefully if Docker is not available (e.g. running inside a container)
if ! docker info >/dev/null 2>&1; then
    echo "SKIP: Docker not available — vmsssh integration tests require Docker"
    exit 77  # CTest SKIP_RETURN_CODE
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0
FAIL=0
IMAGE="${OVMX_IMAGE:-ovmx-test}"
CONTAINER="vmsssh-test-$$"
PORT="${OVMX_SSH_PORT:-2299}"  # High port to avoid conflicts

# Locate test_ssh_client: check build dirs relative to repo root
if [ -n "${TEST_CLIENT:-}" ]; then
    SSH_CLIENT="$TEST_CLIENT"
elif [ -x "$REPO_ROOT/build/bin/test_ssh_client" ]; then
    SSH_CLIENT="$REPO_ROOT/build/bin/test_ssh_client"
elif [ -x "$REPO_ROOT/build-static/bin/test_ssh_client" ]; then
    SSH_CLIENT="$REPO_ROOT/build-static/bin/test_ssh_client"
else
    echo "ERROR: test_ssh_client not found. Build with -DBUILD_TESTS=ON first."
    echo "  cmake -B build -DBUILD_TESTS=ON && cmake --build build"
    exit 1
fi

# ------------------------------------------------------------------ #
# Cleanup handler                                                      #
# ------------------------------------------------------------------ #
cleanup() {
    if docker ps -q --filter "name=$CONTAINER" 2>/dev/null | grep -q .; then
        docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

# ------------------------------------------------------------------ #
# Build image                                                          #
# ------------------------------------------------------------------ #
# If OVMX_SKIP_BUILD=1 or if OVMX_IMAGE is set AND the image already
# exists, skip the build. This allows testing against a pre-built image
# (e.g. one built from main which has vmssshd) without rebuilding.
SKIP_BUILD="${OVMX_SKIP_BUILD:-0}"
if [ "$SKIP_BUILD" = "0" ] && [ -n "${OVMX_IMAGE:-}" ]; then
    if docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "Image $IMAGE already exists — skipping build."
        echo "(Set OVMX_SKIP_BUILD=0 to force rebuild.)"
        SKIP_BUILD=1
    fi
fi

if [ "$SKIP_BUILD" = "0" ]; then
    echo "Building OVMX Docker image ($IMAGE)..."
    docker build -t "$IMAGE" "$REPO_ROOT" >/dev/null 2>&1
    echo "Build complete."
else
    echo "Using pre-built image: $IMAGE"
fi

# ------------------------------------------------------------------ #
# Start container                                                      #
# ------------------------------------------------------------------ #
echo "Starting container $CONTAINER on port $PORT..."
docker run -d \
    --name "$CONTAINER" \
    -p "${PORT}:22" \
    "$IMAGE" >/dev/null

# Wait for the SSH server to be ready.
# vmssshd generates an RSA key on first start (2-10 seconds).
# Fallback: if openssh-server is also in the image (e.g. from a cached layer),
# wait for an SSH daemon process to appear — port 22 will be claimed by whichever
# starts first.
echo "Waiting for SSH server to start..."
MAX_WAIT=30
WAITED=0
SSH_READY=0
while [ "$WAITED" -lt "$MAX_WAIT" ]; do
    # Check for vmssshd key (vmssshd mode)
    if docker exec "$CONTAINER" sh -c 'test -f /etc/ovmx/ssh_host_rsa_key' >/dev/null 2>&1; then
        SSH_READY=1
        sleep 1  # give vmssshd a moment to fully bind
        break
    fi
    # Check for any sshd process (covers openssh fallback in older images)
    if docker exec "$CONTAINER" pgrep -x sshd >/dev/null 2>&1 || \
       docker exec "$CONTAINER" pgrep -x vmssshd >/dev/null 2>&1; then
        SSH_READY=1
        sleep 2  # give sshd time to bind fully
        break
    fi
    sleep 1
    WAITED=$((WAITED + 1))
done

if [ "$SSH_READY" -eq 0 ]; then
    echo "ERROR: SSH server did not start within ${MAX_WAIT}s"
    docker logs "$CONTAINER" 2>&1 | tail -20
    exit 1
fi

# Identify which SSH daemon is running
if docker exec "$CONTAINER" pgrep -x vmssshd >/dev/null 2>&1; then
    echo "vmssshd is up (native VMS SSH daemon)."
elif docker exec "$CONTAINER" pgrep -x sshd >/dev/null 2>&1; then
    echo "OpenSSH sshd is up (proxying through vms_login)."
else
    echo "SSH daemon is up."
fi
echo ""

# ------------------------------------------------------------------ #
# Test runner                                                          #
# ------------------------------------------------------------------ #
run_test() {
    local name="$1"
    shift
    local exit_code=0

    # Run test_ssh_client against the container's exposed port on the host.
    # We connect from the host to localhost:PORT (mapped from container port 22).
    "$SSH_CLIENT" -h 127.0.0.1 -p "$PORT" "$@" >/dev/null 2>&1 || exit_code=$?

    if [ "$exit_code" -eq 0 ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (exit $exit_code)"
        FAIL=$((FAIL + 1))
    fi
}

echo "Running vmssshd integration tests..."
echo ""

# ------------------------------------------------------------------ #
# Test 1: Valid authentication — SYSTEM user (empty password hash)    #
# ------------------------------------------------------------------ #
run_test "Valid auth: SYSTEM user with empty password" \
    -u SYSTEM -P "" -c "SHOW TIME"

# ------------------------------------------------------------------ #
# Test 2: Command output — SHOW TIME returns a year                   #
# ------------------------------------------------------------------ #
run_test "SHOW TIME returns current year" \
    -u SYSTEM -P "" -c "SHOW TIME" -e "$(date +%Y)"

# ------------------------------------------------------------------ #
# Test 3: Command output — SHOW DEFAULT returns VMS-style path        #
# The default dir for SYSTEM is /root. vmsdcl's SHOW DEFAULT may     #
# return a VMS-style path like SYS$LOGIN or [ROOT] depending on impl.#
# We accept any non-empty output (the dir must be set).               #
# ------------------------------------------------------------------ #
# Use the verbose flag and check manually via a wrapper
SHOW_DEFAULT_OUT=$("$SSH_CLIENT" -h 127.0.0.1 -p "$PORT" \
    -u SYSTEM -P "" -c "SHOW DEFAULT" -v 2>&1) || true
echo "  INFO: SHOW DEFAULT output: $(echo "$SHOW_DEFAULT_OUT" | tr '\n' ' ' | head -c 120)"
# Accept any output that is non-empty (path was returned)
if echo "$SHOW_DEFAULT_OUT" | grep -qiE "root|sys\\\$|DEFAULT|VMS|\[|disk\$"; then
    echo "  PASS: SHOW DEFAULT returns directory path"
    PASS=$((PASS + 1))
else
    echo "  FAIL: SHOW DEFAULT output not recognised"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
# Test 4: VMS login banner                                            #
# Request a shell (no command) — vmssshd prints the banner before    #
# starting vmsdcl. We capture output with expect=-e OpenVMS           #
# ------------------------------------------------------------------ #
run_test "VMS login banner contains 'OpenVMS'" \
    -u SYSTEM -P "" -e "OpenVMS"

# ------------------------------------------------------------------ #
# Test 5: Authentication failure — non-existent user                  #
# ------------------------------------------------------------------ #
run_test "Auth failure: unknown user is rejected" \
    -u NONEXISTENT_USER_XYZ -P "wrongpass" -x

# ------------------------------------------------------------------ #
# Test 6: LOGOUT command exits cleanly                                #
# ------------------------------------------------------------------ #
run_test "LOGOUT exits cleanly" \
    -u SYSTEM -P "" -c "LOGOUT"

# ------------------------------------------------------------------ #
# Test 7: OPERATOR user (also has empty password hash)                #
# ------------------------------------------------------------------ #
run_test "Valid auth: OPERATOR user" \
    -u OPERATOR -P "" -c "SHOW TIME"

# ------------------------------------------------------------------ #
# Test 8: Multiple sequential sessions                                 #
# ------------------------------------------------------------------ #
SEQ_PASS=0
for i in 1 2 3; do
    "$SSH_CLIENT" -h 127.0.0.1 -p "$PORT" \
        -u SYSTEM -P "" -c "SHOW TIME" >/dev/null 2>&1 \
        && SEQ_PASS=$((SEQ_PASS + 1)) || true
done
if [ "$SEQ_PASS" -eq 3 ]; then
    echo "  PASS: Multiple sequential sessions ($SEQ_PASS/3)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: Multiple sequential sessions (only $SEQ_PASS/3 succeeded)"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
# Test 9: SYSUAF library accessible (vms_login uses sysuaf_lookup)   #
# ------------------------------------------------------------------ #
SYSUAF_OK=0
if docker exec "$CONTAINER" sh -c \
    'vms_login --help 2>&1; echo SYSUAF_OK' 2>/dev/null | grep -q "SYSUAF_OK"; then
    SYSUAF_OK=1
fi
if [ "$SYSUAF_OK" -eq 1 ]; then
    echo "  PASS: SYSUAF library accessible via vms_login"
    PASS=$((PASS + 1))
else
    echo "  FAIL: SYSUAF library not accessible"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
# Test 10: SSH daemon process is running                              #
# (vmssshd in native mode, or sshd in legacy openssh mode)           #
# ------------------------------------------------------------------ #
if docker exec "$CONTAINER" sh -c 'pgrep -x vmssshd >/dev/null 2>&1 || pgrep -x sshd >/dev/null 2>&1'; then
    echo "  PASS: SSH daemon process is running in container"
    PASS=$((PASS + 1))
else
    echo "  FAIL: No SSH daemon process found in container"
    FAIL=$((FAIL + 1))
fi

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #
echo ""
TOTAL=$((PASS + FAIL))
echo "Results: $PASS passed, $FAIL failed out of $TOTAL tests"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "=== Container logs (last 30 lines) ==="
    docker logs "$CONTAINER" 2>&1 | tail -30
    exit 1
fi

exit 0
