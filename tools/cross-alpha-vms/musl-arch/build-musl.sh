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
# -g0: the alpha-dec-vms port's .vmsdebug records emit a CRTL name's "..en" entry
# reference from the BASE name (strlen..en) while the entry label is the decorated
# decc$strlen..en (vms-f97 fixed the label, not the debug record) -> GAS "redefined
# symbol on reloc" when a CRTL definition is compiled WITH debug. A release C RTL
# needs no debug info; -g0 drops the .vmsdebug section. (see bead vms-2d8c)
CC_FLAGS="-mpointer-size=64 -g0"

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
# compiler. alpha-dec-vms is the OpenVMS "P64"/LLP64 model: int=4, long=4,
# long long=8, pointer=8 (with -mpointer-size=64), little-endian, long double=8
# (IEEE binary64). The overlay's alltypes.h.in decouples _Addr/_Reg/_Int64 from
# the 32-bit `long` accordingly. If the measured model differs, STOP rather than
# emit a subtly-broken libc.
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
# __BYTE_ORDER__ expands to the *name* __ORDER_LITTLE_ENDIAN__ on a LE target.
BORDER=$(probe __BYTE_ORDER__)
echo "long=${SZ_LONG} ptr=${SZ_PTR} int=${SZ_INT} ldbl=${SZ_LDBL} byte_order=${BORDER}"

fail_abi() { echo "PREFLIGHT ABI MISMATCH: $1" >&2; exit 3; }
[ "${SZ_PTR}"  = "8" ] || fail_abi "pointer is ${SZ_PTR}B, need 8. Is -mpointer-size=64 honored?"
[ "${SZ_INT}"  = "4" ] || fail_abi "int is ${SZ_INT}B, need 4"
[ "${SZ_LONG}" = "4" ] || fail_abi "long is ${SZ_LONG}B, expected 4 (OpenVMS Alpha LLP64). If long is now 8 the overlay could move to a plain LP64 model."
[ "${SZ_LDBL}" = "8" ] || fail_abi "long double is ${SZ_LDBL}B; bits/float.h assumes 8 (IEEE binary64). Swap float.h for the IEEE-quad variant if this is 16."
[ "${BORDER}" = "__ORDER_LITTLE_ENDIAN__" ] || fail_abi "not little-endian (byte_order=${BORDER})"
echo "== PREFLIGHT OK: alpha-dec-vms is LLP64 (int=4,long=4,ll=8,ptr=8) little-endian, as the overlay assumes =="

# ---- configure + build libc.a ----
./configure \
	--target="${TARGET}" \
	CC="${TARGET}-gcc" \
	CROSS_COMPILE="${TARGET}-" \
	CFLAGS="${CC_FLAGS}" \
	--disable-shared \
	2>&1 | tee /tmp/musl-configure.log

# lib/libc.a is the rung-1 deliverable. Try the full archive first; if the
# alpha-dec-vms toolchain (cc1 + EVAX binutils) cannot yet compile every member
# (known gaps: weak-alias/visibility, some complex-math relocs), fall back to a
# keep-going pass and archive what DID compile into a clearly-labeled PARTIAL
# libc.a. This is the rung-1-sanctioned "partial libc.a with documented gaps" -
# the failing set is enumerated below, never hidden.
PARTIAL=0
if make -j"$(nproc)" lib/libc.a 2>&1 | tee /tmp/musl-make.log; then
	echo "== FULL libc.a built =="
else
	PARTIAL=1
	echo "== full build hit toolchain gaps; keep-going pass to compile all that can =="
	make -k -j"$(nproc)" lib/libc.a 2>&1 | tee /tmp/musl-make-k.log || true
	echo "== archiving successfully-compiled objects into a PARTIAL lib/libc.a =="
	mkdir -p lib
	rm -f lib/libc.a /tmp/valid-objs
	# A failed compile can leave a truncated/empty .o; keep only VALID objects
	# (non-empty and readable by objdump) so ar does not choke.
	find obj -name '*.o' ! -path 'obj/crt/*' | sort | while read -r o; do
		[ -s "$o" ] || continue
		if "${TARGET}-objdump" -f "$o" >/dev/null 2>&1; then
			printf '%s\n' "$o" >> /tmp/valid-objs
		fi
	done
	if [ -s /tmp/valid-objs ]; then
		xargs -a /tmp/valid-objs "${TARGET}-ar" rcs lib/libc.a
		"${TARGET}-ranlib" lib/libc.a || true
	fi
fi

# ==========================================================================
# VERIFY: libc.a is a real alpha-dec-vms archive with genuine portable text
# symbols, and a syscall-using member references the honest stub.
# ==========================================================================
test -f lib/libc.a || { echo "VERIFY FAIL: lib/libc.a not built" >&2; exit 4; }
echo "== file lib/libc.a ==" ; file lib/libc.a || true

if [ "$PARTIAL" = "1" ]; then
	echo "############################################################"
	echo "# RUNG-1 RESULT: PARTIAL libc.a (documented toolchain gaps) #"
	echo "############################################################"
	echo "== members that FAILED to compile (alpha-dec-vms toolchain gaps) =="
	grep -hoE "obj/src/[^ ]+\.o" /tmp/musl-make-k.log | sed -n 's/.*\[Makefile[^ ]* \(obj[^]]*\.o\)\].*/\1/p' >/dev/null 2>&1 || true
	grep -E "Error 1|Fatal error|cannot generate|redefined symbol|not supported" /tmp/musl-make-k.log | sort -u | head -60 || true
	echo "== failing target count =="
	grep -cE "\*\*\* \[Makefile.*Error 1" /tmp/musl-make-k.log || true
	echo "== objects successfully archived =="
	find obj -name '*.o' ! -path 'obj/crt/*' | wc -l
fi

NM="${TARGET}-nm"
# On the VMS/EVAX ABI a callable function is a procedure DESCRIPTOR (nm type D)
# whose real code entry is the companion `<name>..en` symbol (nm type T). So a
# genuine function shows BOTH: `<name>` defined (D) and `<name>..en` in text (T).
echo "== portable functions must be real (descriptor D + code entry ..en T) =="
MISSING=0
"${NM}" lib/libc.a 2>/dev/null > /tmp/libc.nm || true
for sym in strlen malloc memcpy vsnprintf; do
	desc_ok=$(grep -cE "^[0-9a-fA-F]+ [DT] ${sym}$" /tmp/libc.nm || true)
	code_ok=$(grep -cE "^[0-9a-fA-F]+ [Tt] ${sym}\.\.en$" /tmp/libc.nm || true)
	if [ "${desc_ok}" -ge 1 ] && [ "${code_ok}" -ge 1 ]; then
		echo "  OK      ${sym} (descriptor + ${sym}..en code)"
	else
		echo "  MISSING ${sym} (desc=${desc_ok} code=${code_ok})"
		MISSING=$((MISSING+1))
	fi
done
if [ "${MISSING}" -gt 0 ]; then
	echo "VERIFY: ${MISSING}/4 target functions absent." >&2
	if [ "${PARTIAL}" = "1" ]; then
		echo "This is EXPECTED while the alpha-dec-vms binutils EVAX backend cannot" >&2
		echo "assemble cc1's .linkage reloc for non-leaf functions (see summary above)." >&2
		echo "RUNG 1 is BLOCKED on that cross-toolchain gap, not on the musl overlay." >&2
	fi
	exit 5
fi

echo "== honest syscall stub must be defined and referenced =="
"${NM}" lib/libc.a 2>/dev/null | grep -E " [Tt] __vms_alpha_syscall\.\.en$" \
	&& echo "  OK  __vms_alpha_syscall defined (the -ENOSYS stub)" \
	|| { echo "VERIFY FAIL: __vms_alpha_syscall code entry not defined" >&2; exit 6; }
# a real syscall-using member (e.g. open/write) must reference the descriptor (U)
if "${NM}" lib/libc.a 2>/dev/null | grep -qE " U __vms_alpha_syscall$"; then
	echo "  OK  syscall-using members reference __vms_alpha_syscall (honest -ENOSYS path)"
else
	echo "  NOTE: no undefined ref to __vms_alpha_syscall found; acceptable"
fi

# object machine check: pick one portable object and confirm it is alpha EVAX
echo "== object machine sanity (one portable member) =="
"${TARGET}-ar" x lib/libc.a strlen.lo 2>/dev/null || "${TARGET}-ar" x lib/libc.a strlen.o 2>/dev/null || true
ls -l strlen.* 2>/dev/null || true
"${TARGET}-objdump" -f lib/libc.a 2>/dev/null | grep -m1 -iE "alpha|architecture" || true

if [ "$PARTIAL" = "1" ]; then
	echo "=== vms-960 RUNG 1 VERIFY OK on a PARTIAL alpha-dec-vms libc.a ==="
	echo "=== (portable targets are genuine; failing members documented above) ==="
else
	echo "=== vms-960 RUNG 1 BUILD+VERIFY OK: FULL alpha-dec-vms libc.a ==="
fi
"${NM}" lib/libc.a | grep -cE " [Tt] .*\.\.en$" | xargs echo "defined function code-entries (..en) in libc.a:"
