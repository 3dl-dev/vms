#!/bin/sh
# test_transport_seam.sh - the executive transport seam gate (rd vms-bbde).
#
# POSITIVE: the tree passes tools/ci/check_transport_seam.py -- no C code in src/
# outside libvmssys and the executive names a VMS_IOCTL_* request or spells the
# "/dev/vms" node. NEGATIVE CONTROLS: a fixture that opens /dev/vms itself, and
# one that issues a VMS_IOCTL_* request itself, must each FAIL the same gate; a
# fixture that only mentions both in a comment and a message string must PASS
# (the gate reads code, not prose).
set -u
ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$ROOT/tools/ci/check_transport_seam.py"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0

if python3 "$GATE" "$ROOT"; then
    echo "  PASS: the tree reaches the executive only through the transport seam"
else
    echo "  FAIL: the tree reaches the executive around the transport seam"; fail=1
fi

printf 'int f(void) { return open("/dev/vms", 2); }\n' > "$WORK/opens.c"
if python3 "$GATE" --file "$WORK/opens.c" >/dev/null; then
    echo "  FAIL: NEGCTL a direct open of /dev/vms passes the gate"; fail=1
else
    echo "  PASS: NEGCTL a direct open of /dev/vms is refused"
fi

printf 'long g(int h, void *a) { return ioctl(h, VMS_IOCTL_REGISTER, a); }\n' > "$WORK/ioctls.c"
if python3 "$GATE" --file "$WORK/ioctls.c" >/dev/null; then
    echo "  FAIL: NEGCTL a raw VMS_IOCTL_* request passes the gate"; fail=1
else
    echo "  PASS: NEGCTL a raw VMS_IOCTL_* request is refused"
fi

printf '/* opens "/dev/vms" with VMS_IOCTL_REGISTER */\nconst char *m = "via VMS_IOCTL_L2_OPEN";\n' > "$WORK/prose.c"
if python3 "$GATE" --file "$WORK/prose.c" >/dev/null; then
    echo "  PASS: prose (a comment, a message string) is not a transport use"
else
    echo "  FAIL: the gate reads prose as code"; fail=1
fi

[ "$fail" -eq 0 ] && echo "transport seam gate: PASS" || echo "transport seam gate: FAIL"
exit "$fail"
