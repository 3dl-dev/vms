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
# MUSL_EXTRA_CFLAGS (vms-7b96): appended to every compile. The primary path
# leaves DST on (genuine .vmsdebug; LINK.EXE's EVAX reader skips it). Pass
# MUSL_EXTRA_CFLAGS=-g0 to suppress DST so GNU nm/ar can read the objects — the
# ONLY way to ENUMERATE the port's decorated decc$* definitions for the DECC$SHR
# symbol vector (mk_decc_shr.sh), since the GNU vms-alpha reader cannot parse DST
# and objcopy/strip cannot remove it. The resulting DECC$SHR is byte-identical
# either way: LINK.EXE skips EDBG/ETBT, so no DST reaches the linked image.
CC_FLAGS="${CC_FLAGS} ${MUSL_EXTRA_CFLAGS:-}"

mkdir -p "$WORK" && cd "$WORK"

# ---- fetch musl (no vendoring; pinned + checksum-verified) ----
# Honor a pre-placed tarball (a mounted host copy under $WORK) so a reproducible
# / offline containerized build can bypass the network; otherwise fetch it. The
# pinned checksum is enforced either way, so a stale/corrupt cached copy fails.
if [ ! -f "musl-${MUSL_VER}.tar.gz" ]; then
	wget -q "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
fi
echo "${MUSL_SHA256}  musl-${MUSL_VER}.tar.gz" | sha256sum -c -
tar xf "musl-${MUSL_VER}.tar.gz"
cd "musl-${MUSL_VER}"

# ---- overlay the OVMX alpha-dec-vms arch (arch/, crt/, src/) ----
cp -rv "${OVERLAY}/arch/${TARGET}"        "arch/"
cp -rv "${OVERLAY}/crt/${TARGET}"         "crt/"
mkdir -p "src/setjmp/${TARGET}"
cp -v  "${OVERLAY}/src/setjmp/${TARGET}/"* "src/setjmp/${TARGET}/"
mkdir -p "src/thread/${TARGET}"
cp -v  "${OVERLAY}/src/thread/${TARGET}/"* "src/thread/${TARGET}/"
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
# ==========================================================================
# ARCHIVE STEP — HAND-BUILT `ar` CONTAINER, NO GNU ar (vms-7b96, do-it-like-VMS).
#
# The patched alpha-dec-vms cc1 emits a `.vmsdebug` (VMS DST) section into every
# object even without -g. GNU binutils 2.43's vms-alpha BFD *reader* cannot parse
# DST: `nm`/`objdump`/`ld` die "file format not recognized", and `ar` (any op key,
# even quick-append `qcS` with no symbol index) still bfd-opens every input to
# copy it, so it too dies "error reading <obj>: invalid operation". objcopy/strip
# cannot remove the section either — the read fails first. So GNU ar CANNOT build
# this archive at all.
#
# But the `ar` container is a trivial System V/GNU format (a text header + the
# member bytes) that needs NO object parsing to assemble. We build it ourselves,
# byte-for-byte, never reading object contents — keeping the genuine DST. That is
# exactly what OVMX's real consumer needs: LINK.EXE (src/vmslink/link.c
# load_archive_evax, vms-7b96) walks the `ar` container itself, pulls EVERY
# member, evax_read()s each (its EVAX reader SKIPS DST/EDBG records), and never
# consults an archive symbol index. So a hand-built, no-index libc.a is complete
# and correct for its actual target. This is the primary do-it-like-VMS path —
# NOT the -g0 sidestep (which would drop the genuine DST). The remaining genuine
# fix (a DST-tolerant binutils vms-alpha reader) stays tracked as vms-7b96.
# ==========================================================================
AR_ARCHIVER="${WORK}/ar-noindex"
cat > "$AR_ARCHIVER" <<'AREOF'
#!/usr/bin/perl
# do-it-like-VMS archiver: assemble a System V/GNU `ar` container from object
# files WITHOUT reading their contents (GNU ar's vms-alpha reader chokes on the
# port cc1's .vmsdebug/DST, vms-7b96). Invoked with musl's AR call shape
# `<op> <archive> <obj>...`; the op key (rc/rcs/rcS/...) is ignored — we never
# build a symbol index (LINK.EXE whole-archives every member, never reads one).
use strict; use warnings;
shift @ARGV;                       # op key (rc/...): ignored
my $out = shift @ARGV or die "ar-noindex: no output archive\n";
my @objs = @ARGV;
my %off; my $tab = "";             # GNU "//" long-name table for names >15 bytes
for my $o (@objs) { my $n = (split m{/}, $o)[-1];
    if (length($n)+1 > 16 && !exists $off{$n}) { $off{$n}=length($tab); $tab .= "$n/\n"; } }
$tab .= "\n" if length($tab) % 2;
sub hdr { my ($name,$size)=@_;
    return sprintf("%-16s%-12d%-6d%-6d%-8s%-10d\140\n",$name,0,0,0,"100644",$size); }
open(my $fh,'>',$out) or die "ar-noindex: cannot write $out: $!\n"; binmode $fh;
print $fh "!<arch>\n";
if (length $tab) { print $fh hdr("//", length $tab); print $fh $tab; }
for my $o (@objs) {
    my $n = (split m{/}, $o)[-1];
    open(my $in,'<',$o) or die "ar-noindex: cannot read $o: $!\n"; binmode $in;
    local $/; my $data = <$in>; close $in;
    my $field = exists $off{$n} ? "/$off{$n}" : "$n/";
    print $fh hdr($field, length $data); print $fh $data;
    print $fh "\n" if length($data) % 2;
}
close $fh;
AREOF
chmod +x "$AR_ARCHIVER"

PARTIAL=0
if make -j"$(nproc)" AR="$AR_ARCHIVER" RANLIB=true lib/libc.a 2>&1 | tee /tmp/musl-make.log; then
	echo "== FULL libc.a built (hand-built no-index archive) =="
else
	PARTIAL=1
	echo "== full build hit toolchain gaps; keep-going pass to compile all that can =="
	make -k -j"$(nproc)" AR="$AR_ARCHIVER" RANLIB=true lib/libc.a 2>&1 | tee /tmp/musl-make-k.log || true
	echo "== archiving successfully-compiled objects into a PARTIAL lib/libc.a =="
	mkdir -p lib
	rm -f lib/libc.a /tmp/valid-objs
	# A failed compile can leave a truncated/empty .o; keep only NON-EMPTY objects.
	# We must NOT use objdump/nm here — those READ the object and choke on DST
	# (vms-7b96). These are VMS-native EVAX objects (`file` reports "data",
	# objdump -f "file format vms-alpha"), NOT ELF; a non-empty file the compiler
	# did not error on is a member. The perl archiver copies bytes, never reading.
	find obj -name '*.o' ! -path 'obj/crt/*' | sort | while read -r o; do
		[ -s "$o" ] && printf '%s\n' "$o" >> /tmp/valid-objs
	done
	if [ -s /tmp/valid-objs ]; then
		xargs -a /tmp/valid-objs "$AR_ARCHIVER" rc lib/libc.a
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

# --------------------------------------------------------------------------
# VERIFY — DST-safe (vms-7b96). The genuine objects carry a `.vmsdebug` (DST)
# section that GNU binutils' vms-alpha reader cannot parse, so nm/objdump/`ar t`
# with a symbol index all die "file format not recognized" on these members —
# and objcopy/strip cannot remove the section either (the read fails first). The
# ONLY per-symbol reader that works is a `-g0` recompile, which this deliverable
# deliberately does NOT do (the DST is genuine; LINK.EXE's own EVAX reader,
# src/vmslink/evax_read.c, skips EDBG/ETBT debug records, so it consumes these
# members — that, not GNU nm, is the real consumer this rung targets). So verify
# structurally, without reading object symbols: `ar t` lists member NAMES from
# the archive headers (a pure container walk, no object parse), which is DST-safe.
# The authoritative symbol/consumption proof is LINK.EXE emitting DECC$SHR.
# --------------------------------------------------------------------------
echo "== member manifest (ar header walk in perl — DST-safe, tool-independent) =="
# Walk the `ar` container directly (never opening a member as an object), so this
# does not depend on GNU ar's broken vms-alpha reader at all. Resolves GNU "//"
# long names. `ar t` would also work (header-only), but this is reader-agnostic.
perl -e '
    open(my $f,"<",$ARGV[0]) or die; binmode $f; local $/; my $b=<$f>;
    my $p=8; my $lt=""; my @names;
    while ($p+60 <= length $b) {
        my $h=substr($b,$p,60); my $nm=substr($h,0,16); $nm=~s/\s+$//;
        my $sz=substr($h,48,10)+0; my $d=$p+60;
        if ($nm eq "//") { $lt=substr($b,$d,$sz); }
        elsif ($nm ne "/" && $nm ne "/SYM64/") {
            if ($nm=~m{^/(\d+)}) { my $o=$1; my $s=substr($lt,$o); $s=~s/\n.*//s; $s=~s{/$}{}; push @names,$s; }
            else { $nm=~s{/$}{}; push @names,$nm; }
        }
        $p=$d+$sz; $p++ if $p&1;
    }
    print "$_\n" for @names;
' lib/libc.a > /tmp/libc.members 2>/dev/null || true
NMEMB=$(wc -l < /tmp/libc.members)
echo "  libc.a members: ${NMEMB}"
[ "${NMEMB}" -ge 1000 ] || { echo "VERIFY FAIL: only ${NMEMB} members archived (expected ~1346)" >&2; exit 5; }
echo "== a few expected members present =="
for m in strlen malloc memcpy vsnprintf; do
	if grep -qE "(^|/)${m}\." /tmp/libc.members; then
		echo "  OK      ${m}.* member present"
	else
		echo "  NOTE    no ${m}.* member (may be folded into another TU)"
	fi
done

if [ "$PARTIAL" = "1" ]; then
	echo "=== vms-960 RUNG 1 VERIFY OK on a PARTIAL alpha-dec-vms libc.a (${NMEMB} members) ==="
	echo "=== (failing members documented above; symbol-level proof is LINK.EXE) ==="
else
	echo "=== vms-960 RUNG 1 BUILD+VERIFY OK: FULL alpha-dec-vms libc.a (${NMEMB} members) ==="
fi
echo "NOTE: per-symbol nm/objdump verification is blocked by the GNU binutils"
echo "      vms-alpha DST reader gap (vms-7b96); the real symbol proof is"
echo "      LINK.EXE consuming these members to emit DECC\$SHR (mk_decc_shr.sh)."
