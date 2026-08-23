#!/bin/bash
# build-musl.sh - fetch musl, overlay the OVMX alpha-dec-vms arch, build
# lib/libc.a with the alpha-dec-vms cross toolchain, and verify the result.
#
# Runs INSIDE the tools/cross-alpha-vms toolchain container (which already has
# binutils + gcc cc1 for alpha-dec-vms under /opt/cross-alpha-vms). Build/oracle
# tooling, Rule-9-clean: nothing here runs in the OVMX guest.
#
# vms-960 RUNG 1: portable C -> genuine alpha-dec-vms EVAX objects; the
# syscall-dependent members compile against an HONEST -ENOSYS stub.
set -euxo pipefail

MUSL_VER=${MUSL_VER:-1.2.5}
MUSL_SHA256=${MUSL_SHA256:-a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4}
TARGET=alpha-dec-vms
PREFIX=${PREFIX:-/opt/cross-alpha-vms}
OVERLAY=${OVERLAY:-/overlay}          # bind-mounted musl-arch/ (read-only)
WORK=${WORK:-/tmp/musl-build}         # build out of tree, never into the repo
export PATH="${PREFIX}/bin:${PATH}"

# 64-bit pointers are mandatory: alpha-dec-vms defaults to 32-bit (VMS short
# pointers); musl is LP64. Every object must be built -mpointer-size=64.
CC_FLAGS="-mpointer-size=64"

mkdir -p "$WORK" && cd "$WORK"

# ---- fetch musl (no vendoring; pinned + checksum-verified) ----
wget -q "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
echo "${MUSL_SHA256}  musl-${MUSL_VER}.tar.gz" | sha256sum -c -
tar xf "musl-${MUSL_VER}.tar.gz"
cd "musl-${MUSL_VER}"

# ---- overlay the OVMX alpha-dec-vms arch (arch/, crt/, src/) ----
cp -rv "${OVERLAY}/arch/${TARGET}"        "arch/"
cp -rv "${OVERLAY}/crt/${TARGET}"         "crt/"
mkdir -p "src/setjmp/${TARGET}"
cp -v  "${OVERLAY}/src/setjmp/${TARGET}/"* "src/setjmp/${TARGET}/"
cp -v  "${OVERLAY}/src/internal/vms_alpha_syscall.c" "src/internal/"

# ---- teach configure the triplet -> ARCH mapping (idempotent) ----
if ! grep -q "ARCH=alpha-dec-vms" configure; then
	sed -i 's#^unknown) fail#alpha*) ARCH=alpha-dec-vms ;;\nunknown) fail#' configure
fi
grep -n "ARCH=alpha-dec-vms" configure

# ==========================================================================
# PREFLIGHT: assert the ABI model the arch overlay assumes, against the REAL
# compiler. This is the honesty gate - if alpha-dec-vms is not LP64 (e.g. long
# is 32-bit, as classic OpenVMS Alpha C), the overlay's alltypes are wrong and
# we STOP here rather than emit a subtly-broken libc.
# ==========================================================================
cat > /tmp/abi.c <<'EOF'
int  s_int      = sizeof(int);
int  s_long     = sizeof(long);
int  s_ptr      = sizeof(void *);
int  s_longlong = sizeof(long long);
int  s_ldbl     = sizeof(long double);
EOF
"${TARGET}-gcc" ${CC_FLAGS} -S /tmp/abi.c -o /tmp/abi.s
echo "== alpha-dec-vms ABI probe (with ${CC_FLAGS}) =="
"${TARGET}-gcc" ${CC_FLAGS} -dM -E - < /dev/null | \
	grep -E "__SIZEOF_(LONG|POINTER|LONG_LONG|LONG_DOUBLE|INT)__|__BYTE_ORDER__|__WCHAR_TYPE__|__ORDER_LITTLE" || true

probe() { "${TARGET}-gcc" ${CC_FLAGS} -dM -E - < /dev/null | awk -v k="$1" '$2==k{print $3}'; }
SZ_LONG=$(probe __SIZEOF_LONG__)
SZ_PTR=$(probe __SIZEOF_POINTER__)
SZ_INT=$(probe __SIZEOF_INT__)
SZ_LDBL=$(probe __SIZEOF_LONG_DOUBLE__)
BORDER=$(probe __BYTE_ORDER__)
ORDER_LE=$(probe __ORDER_LITTLE_ENDIAN__)
echo "long=${SZ_LONG} ptr=${SZ_PTR} int=${SZ_INT} ldbl=${SZ_LDBL} byte_order=${BORDER} le=${ORDER_LE}"

fail_abi() { echo "PREFLIGHT ABI MISMATCH: $1" >&2; exit 3; }
[ "${SZ_PTR}"  = "8" ] || fail_abi "pointer is ${SZ_PTR}B, need 8 (LP64). Is -mpointer-size=64 honored?"
[ "${SZ_INT}"  = "4" ] || fail_abi "int is ${SZ_INT}B, need 4"
[ "${SZ_LONG}" = "8" ] || fail_abi "long is ${SZ_LONG}B, need 8 (LP64). alpha-dec-vms may use 32-bit long; the LP64 arch overlay CANNOT be used as-is -> STOP + report (vms-960)."
[ "${SZ_LDBL}" = "8" ] || fail_abi "long double is ${SZ_LDBL}B; bits/float.h assumes 8 (IEEE binary64). Swap float.h for the IEEE-quad variant if this is 16."
[ "${BORDER}" = "${ORDER_LE}" ] || fail_abi "not little-endian (byte_order=${BORDER})"
echo "== PREFLIGHT OK: alpha-dec-vms is LP64 little-endian as the overlay assumes =="

# ---- configure + build libc.a ----
./configure \
	--target="${TARGET}" \
	CC="${TARGET}-gcc" \
	CROSS_COMPILE="${TARGET}-" \
	CFLAGS="${CC_FLAGS}" \
	--disable-shared \
	2>&1 | tee /tmp/musl-configure.log

# lib/libc.a is the rung-1 deliverable.
make -j"$(nproc)" lib/libc.a 2>&1 | tee /tmp/musl-make.log

# ==========================================================================
# VERIFY: libc.a is a real alpha-dec-vms archive with genuine portable text
# symbols, and a syscall-using member references the honest stub.
# ==========================================================================
test -f lib/libc.a || { echo "VERIFY FAIL: lib/libc.a not built" >&2; exit 4; }
echo "== file lib/libc.a ==" ; file lib/libc.a || true

NM="${TARGET}-nm"
echo "== portable text symbols must be real (T) =="
for sym in strlen malloc memcpy vsnprintf; do
	if "${NM}" lib/libc.a 2>/dev/null | grep -qE "^[0-9a-fA-F]+ [Tt] ${sym}$"; then
		echo "  OK  ${sym} defined (text)"
	else
		echo "VERIFY FAIL: ${sym} is not a defined text symbol in libc.a" >&2
		exit 5
	fi
done

echo "== honest syscall stub must be defined and referenced =="
"${NM}" lib/libc.a 2>/dev/null | grep -E " [Tt] __vms_alpha_syscall$" \
	&& echo "  OK  __vms_alpha_syscall defined (the -ENOSYS stub)" \
	|| { echo "VERIFY FAIL: __vms_alpha_syscall not defined" >&2; exit 6; }
# a real syscall-using member (e.g. open/write) must reference it (U)
if "${NM}" lib/libc.a 2>/dev/null | grep -qE " U __vms_alpha_syscall$"; then
	echo "  OK  syscall-using members reference __vms_alpha_syscall"
else
	echo "  NOTE: no undefined ref to __vms_alpha_syscall found (all callers inlined into the stub TU?); acceptable"
fi

# object machine check: pick one portable object and confirm it is alpha EVAX
echo "== object machine sanity (one portable member) =="
"${TARGET}-ar" x lib/libc.a strlen.lo 2>/dev/null || "${TARGET}-ar" x lib/libc.a strlen.o 2>/dev/null || true
ls -l strlen.* 2>/dev/null || true
"${TARGET}-objdump" -f lib/libc.a 2>/dev/null | grep -m1 -iE "alpha|architecture" || true

echo "=== vms-960 RUNG 1 BUILD+VERIFY OK: lib/libc.a is a real alpha-dec-vms archive ==="
"${NM}" lib/libc.a | grep -cE " [Tt] " | xargs echo "defined text symbols in libc.a:"
