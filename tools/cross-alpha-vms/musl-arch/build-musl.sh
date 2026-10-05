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
#
# gap-4 (vms-5f9): musl's ./configure auto-detects and adds
# `-ffunction-sections -fdata-sections` to CFLAGS_AUTO. On alpha-dec-vms that is
# actively harmful: each function lands in its own `.text.<name>` section which
# the alpha-dec-vms `as` mis-classifies as DATA, so the cc1-emitted `.align 3`
# prologue gap is ZERO-filled (0x00000000 = a halt/SIGILL) instead of fnop-filled.
# Under whole-archive linking (LINK.EXE pulls EVERY member) function-sections buys
# nothing anyway, so force it off. User CFLAGS is applied last by musl's Makefile,
# so `-fno-*` here overrides configure's CFLAGS_AUTO.
CC_FLAGS="-mpointer-size=64 -fno-function-sections -fno-data-sections"
# MUSL_EXTRA_CFLAGS (vms-7b96): appended to every compile. The primary path
# leaves DST on (genuine .vmsdebug; LINK.EXE's EVAX reader skips it). Pass
# MUSL_EXTRA_CFLAGS=-g0 to suppress DST so GNU nm/ar can read the objects — the
# ONLY way to ENUMERATE the port's decorated decc$* definitions for the DECC$SHR
# symbol vector (mk_decc_shr.sh), since the GNU vms-alpha reader cannot parse DST
# and objcopy/strip cannot remove it. The resulting DECC$SHR is byte-identical
# either way: LINK.EXE skips EDBG/ETBT, so no DST reaches the linked image.
CC_FLAGS="${CC_FLAGS} ${MUSL_EXTRA_CFLAGS:-}"
# vms-28d: the C RTL's own build sees the plain POSIX declarations; the DEC C
# forms patched into the public headers below are for client code only.
CC_FLAGS="${CC_FLAGS} -D__OVMX_LIBC_BUILD"

mkdir -p "$WORK" && cd "$WORK"

# ---- musl source (pinned + checksum-verified) ----
# Resolution order: (1) a copy already staged in $WORK; (2) the VENDORED tarball
# shipped in the overlay (musl-arch/musl-<ver>.tar.gz), so the CI build is
# HERMETIC — musl.libc.org has been persistently unreachable from GitHub runners
# (vms-244d); (3) a network fetch as a last resort for a dev tree without the
# vendored blob. The pinned checksum is enforced in EVERY case, so even the
# vendored tarball is integrity-verified (a stale/corrupt copy fails the build).
if [ ! -f "musl-${MUSL_VER}.tar.gz" ] && [ -f "${OVERLAY}/musl-${MUSL_VER}.tar.gz" ]; then
	cp "${OVERLAY}/musl-${MUSL_VER}.tar.gz" .
fi
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
# vms-032: one heap. Replace generic lite_malloc.c (a second, brk-sharing bump
# allocator reached through section-relative self-binds the linker cannot
# redirect) with forwarders that name mallocng's __libc_malloc_impl. See the
# header of src/malloc/alpha-dec-vms/lite_malloc.c.
mkdir -p "src/malloc/${TARGET}"
cp -v  "${OVERLAY}/src/malloc/${TARGET}/"* "src/malloc/${TARGET}/"
# vms-122: VMS P0 placement. An address-less mmap (the heap's, and mremap's
# moves) is placed in the P0 region below 0x40000000, as the VMS heap grows P0,
# so every malloc result is a valid 32-bit pointer. See
# src/mman/alpha-dec-vms/p0_region.h.
mkdir -p "src/mman/${TARGET}"
cp -v  "${OVERLAY}/src/mman/${TARGET}/"* "src/mman/${TARGET}/"
# vms-430: LLP64 syscall RETURN-leg width fix. syscall_ret.c is a full overlay
# (widened __syscall_ret to long long / unsigned long long). Its declaration in
# syscall.h and the one truncating local in mmap.c are one-line widenings patched
# below — stock musl is pinned + checksum-verified, so exact-text sed is safe and
# reviewable (same idiom as the configure ARCH sed).
cp -v  "${OVERLAY}/src/internal/syscall_ret.c" "src/internal/"

# ---- teach configure the triplet -> ARCH mapping (idempotent) ----
if ! grep -q "ARCH=alpha-dec-vms" configure; then
	sed -i 's#^unknown) fail#alpha*) ARCH=alpha-dec-vms ;;\nunknown) fail#' configure
fi
grep -n "ARCH=alpha-dec-vms" configure

# ==========================================================================
# vms-430: widen the syscall RETURN leg to 64-bit (LLP64 return-leg fix).
# ==========================================================================
# On this LLP64 port `long` is 32 bits but pointers / the Linux-Alpha result
# register are 64 bits. The syscall-ARGUMENT width was fixed in syscall_arch.h
# (__scc / syscall_arg_t == long long); this is the RETURN counterpart. The raw
# __syscallN return was widened in the syscall_arch.h overlay and __syscall_ret
# in the syscall_ret.c overlay copied above; here we widen the two stock-musl
# sites that would otherwise re-narrow a 64-bit pointer result to `long`(32):
#   (1) the __syscall_ret DECLARATION in src/internal/syscall.h (must match the
#       widened overlay definition, and widen every call site's return/arg), and
#   (2) mmap()'s `long ret` local, which holds the raw 64-bit mmap address before
#       the (void*)__syscall_ret cast.
# Both patches are idempotent (guarded on the post-state) and hard-fail if the
# pinned musl text ever drifts out from under them.
if ! grep -q 'long long __syscall_ret' src/internal/syscall.h; then
	sed -i 's/hidden long __syscall_ret(unsigned long),/hidden long long __syscall_ret(unsigned long long);\nhidden long/' src/internal/syscall.h
fi
grep -q 'long long __syscall_ret(unsigned long long)' src/internal/syscall.h || {
	echo "vms-430 PATCH FAIL: could not widen __syscall_ret decl in syscall.h" >&2; exit 7; }

if ! grep -q 'long long ret;' src/mman/mmap.c; then
	sed -i 's/\tlong ret;/\tlong long ret;/' src/mman/mmap.c
fi
grep -q 'long long ret;' src/mman/mmap.c || {
	echo "vms-430 PATCH FAIL: could not widen 'long ret' in mmap.c" >&2; exit 7; }
echo "== vms-430 return-leg widening applied (syscall.h decl + mmap.c local) =="

# ==========================================================================
# vms-537: the DEC C data model for size_t. On OpenVMS Alpha size_t is 32-bit
# even with 64-bit pointers -- the port compiler says so itself
# (gcc/config/vms/vms.h: SIZE_TYPE "unsigned int", "Always a 32 bit type";
# PTRDIFF_TYPE is long long under -mpointer-size=64). The CRTL's ::size_t must
# equal the compiler's __SIZE_TYPE__ or C++ (libstdc++'s std::size_t) and the
# GCC port's own sources stop compiling. So: size_t/ssize_t 32-bit, while
# uintptr_t/intptr_t/ptrdiff_t stay pointer-width (_Addr, 64-bit).
#
# The Linux-Alpha kernel still reads 64-bit lengths in the structs it is handed,
# so the two kernel-shaped structs that carry a length get a 64-bit field rather
# than a 32-bit size_t with 4 bytes of uninitialized padding above it:
# struct iovec (readv/writev -- stdio writes through writev) here, and
# stack_t.ss_size in the arch bits/signal.h overlay.
# Patched with the same guarded exact-text idiom as vms-430; a drift hard-fails.
# ==========================================================================
sed -i 's/^TYPEDEF unsigned _Addr size_t;$/TYPEDEF unsigned int size_t;/' include/alltypes.h.in
sed -i 's/^TYPEDEF _Addr ssize_t;$/TYPEDEF int ssize_t;/' include/alltypes.h.in
sed -i 's/^STRUCT iovec { void \*iov_base; size_t iov_len; };$/STRUCT iovec { void *iov_base; unsigned long long iov_len; };/' include/alltypes.h.in
grep -q '^TYPEDEF unsigned int size_t;$' include/alltypes.h.in \
 && grep -q '^TYPEDEF int ssize_t;$' include/alltypes.h.in \
 && grep -q '^STRUCT iovec { void \*iov_base; unsigned long long iov_len; };$' include/alltypes.h.in \
 || { echo "vms-537 PATCH FAIL: size_t/ssize_t/iovec in include/alltypes.h.in" >&2; exit 7; }
# vms-ce6: ptrdiff_t follows the client's pointer size (_Ptrdiff, arch
# bits/alltypes.h.in), as the port compiler's __PTRDIFF_TYPE__ does.
sed -i 's/^TYPEDEF _Addr ptrdiff_t;$/TYPEDEF _Ptrdiff ptrdiff_t;/' include/alltypes.h.in
grep -q '^TYPEDEF _Ptrdiff ptrdiff_t;$' include/alltypes.h.in \
 || { echo "vms-ce6 PATCH FAIL: ptrdiff_t in include/alltypes.h.in" >&2; exit 7; }
# mprotect() rounds the ADDRESS through size_t; with a 32-bit size_t that
# truncates a 64-bit pointer. Round through uintptr_t instead.
grep -q '^#include <stdint.h>$' src/mman/mprotect.c || sed -i 's/^#include <sys\/mman.h>$/#include <sys\/mman.h>\n#include <stdint.h>/' src/mman/mprotect.c
sed -i 's/^\tsize_t start, end;$/\tuintptr_t start, end;/; s/(size_t)addr/(uintptr_t)addr/; s/end = (size_t)(/end = (uintptr_t)(/' src/mman/mprotect.c
grep -q 'uintptr_t start, end;' src/mman/mprotect.c && grep -q '^#include <stdint.h>$' src/mman/mprotect.c && ! grep -q '(size_t)' src/mman/mprotect.c \
 || { echo "vms-537 PATCH FAIL: src/mman/mprotect.c" >&2; exit 7; }
echo "== vms-537 DEC C size_t model applied (size_t/ssize_t 32-bit, iovec + mprotect kernel-width) =="

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

# ==========================================================================
# vms-28d: DEC C header forms the GCC port's own host sources use under
# `#ifdef VMS` (measured: libiberty/xstrerror.c, getpwd.c, strsignal.c), for
# client code compiled for alpha-dec-vms (__VMS) -- never for the C RTL's own
# build (__OVMX_LIBC_BUILD, set in CC_FLAGS above).
#   <errno.h>  vaxc$errno: DEC C's second per-thread error cell (the VMS
#              condition value), reached through get_vms_errno_addr(), which
#              DECC$SHR exports (src/vmslink/ovmx_decc_crtl.c).
#   <unistd.h> getcwd(buf, size, ...): DEC C's optional third argument selects
#              the result format; OVMX returns the UNIX format (0, what the GCC
#              port passes) for every call -- the VMS-format result is not
#              implemented (tracked on vms-28d).
#   <signal.h> psignal/psiginfo are not part of the DEC C RTL and DECC$SHR does
#              not export them; a client that declares its own (libiberty
#              strsignal.c, after configure finds none) must not collide.
# Exact-text, guarded, idempotent; a drift in the pinned musl text hard-fails.
if ! grep -q 'vaxc\$errno' include/errno.h; then
	perl -0pi -e 's/(#define errno \(\*__errno_location\(\)\)\n)/$1\n#if defined(__VMS) \&\& !defined(__OVMX_LIBC_BUILD)\nint *get_vms_errno_addr(void);\n#define vaxc\$errno (*get_vms_errno_addr())\n#endif\n/' include/errno.h
fi
grep -q 'define vaxc\$errno' include/errno.h || { echo "vms-28d PATCH FAIL: vaxc\$errno in include/errno.h" >&2; exit 7; }
if ! grep -q 'getcwd(char \*, size_t, \.\.\.)' include/unistd.h; then
	perl -0pi -e 's/^char \*getcwd\(char \*, size_t\);\n/#if defined(__VMS) \&\& !defined(__OVMX_LIBC_BUILD)\nchar *getcwd(char *, size_t, ...);\n#else\nchar *getcwd(char *, size_t);\n#endif\n/m' include/unistd.h
fi
grep -q 'getcwd(char \*, size_t, \.\.\.)' include/unistd.h || { echo "vms-28d PATCH FAIL: getcwd in include/unistd.h" >&2; exit 7; }
if ! grep -q 'vms-28d: psignal' include/signal.h; then
	perl -0pi -e 's/^void psiginfo\(const siginfo_t \*, const char \*\);\nvoid psignal\(int, const char \*\);\n/#if !defined(__VMS) || defined(__OVMX_LIBC_BUILD) \/* vms-28d: psignal not in the DEC C RTL *\/\nvoid psiginfo(const siginfo_t *, const char *);\nvoid psignal(int, const char *);\n#endif\n/m' include/signal.h
fi
grep -q 'vms-28d: psignal' include/signal.h || { echo "vms-28d PATCH FAIL: psignal in include/signal.h" >&2; exit 7; }
echo "== vms-28d DEC C header forms applied (vaxc\$errno, getcwd 3-arg, psignal) =="
# vms-fe03: the headers declare, for alpha-dec-vms clients, only what DECC$SHR
# exports -- every function the port's CRTL name map covers (decc-crtl-names.txt);
# the rest bind bare names no link can reach, so a configure probe would find a
# declaration without a definition (GCC's strsignal cascade). See the script.
perl "${OVERLAY}/filter-headers.pl" "${OVERLAY}/decc-crtl-names.txt" include \
	|| { echo "vms-fe03 FAIL: header filter" >&2; exit 7; }

# ---- configure + build libc.a ----
./configure \
	--target="${TARGET}" \
	CC="${TARGET}-gcc" \
	CROSS_COMPILE="${TARGET}-" \
	CFLAGS="${CC_FLAGS}" \
	--disable-shared \
	2>&1 | tee /tmp/musl-configure.log

# vms-4d0: MUSL_HEADERS_ONLY=<dir> installs this alpha-dec-vms CRTL header set
# (every overlay + patch above applied: the DEC C size_t model, kernel-shaped
# iovec, ...) under <dir>/usr/include and stops -- the C RTL headers of the
# OVMX sysroot the stage-2 C/C++ toolchain is configured against.
if [ -n "${MUSL_HEADERS_ONLY:-}" ]; then
	make install-headers DESTDIR="${MUSL_HEADERS_ONLY}" prefix=/usr includedir=/usr/include >/tmp/musl-headers.log 2>&1 \
		|| { cat /tmp/musl-headers.log >&2; exit 1; }
	echo "== musl alpha-dec-vms headers installed in ${MUSL_HEADERS_ONLY}/usr/include =="
	exit 0
fi

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

# --------------------------------------------------------------------------
# PER-SYMBOL READER PROOF (vms-7b96 FIX). Previously blocked: these members are
# built with DST on (primary path), and GNU binutils' vms-alpha reader choked
# on the .vmsdebug/DST -> nm/objdump "file format not recognized". The
# 0005-vms-7b96-defer-dst-at-scan.patch (in tools/cross-alpha-vms/patches,
# baked into this very toolchain image) defers the DST slurp at object-scan
# time, so the GNU reader now loads these DST-carrying members and enumerates
# their EGSD symbols. Assert it on a real global-bearing member: this is the
# per-symbol reader verification that used to be skipped (Rule 7), and it
# guards the DST-reader fix against regression. (Statics are not in the EGSD by
# design, so pick a member that defines a global; the assertion is nm exit 0 +
# >=1 symbol, NOT a specific name.)
# --------------------------------------------------------------------------
NM="${TARGET}-nm"
LIBCA="$(pwd)/lib/libc.a"
MEMBER=$(grep -m1 -E '(^|/)(strlen|memcpy|strcmp|memset|strchr)\.' /tmp/libc.members \
         || head -1 /tmp/libc.members)
echo "== per-symbol reader proof (vms-7b96): ${NM} reads DST-carrying member '${MEMBER}' =="
rm -rf /tmp/nmck && mkdir -p /tmp/nmck
( cd /tmp/nmck && ar x "${LIBCA}" "${MEMBER}" ) 2>/dev/null || true
if [ ! -s "/tmp/nmck/${MEMBER}" ]; then
	echo "VERIFY FAIL: could not extract member '${MEMBER}' from libc.a" >&2
	exit 6
fi
if ! "${NM}" "/tmp/nmck/${MEMBER}" >/tmp/nm.out 2>&1; then
	echo "VERIFY FAIL (vms-7b96 regressed): ${NM} cannot read DST-carrying member '${MEMBER}':" >&2
	cat /tmp/nm.out >&2
	exit 6
fi
NSYMS=$(grep -cE '[^[:space:]]' /tmp/nm.out || true)
[ "${NSYMS}" -ge 1 ] || {
	echo "VERIFY FAIL: ${NM} read '${MEMBER}' but enumerated 0 symbols" >&2
	exit 6
}
echo "  OK      ${NM} read DST member '${MEMBER}' (DST on): ${NSYMS} symbol(s) enumerated"

# ==========================================================================
# PAGE-SIZE GATE (vms-c5d). alpha-dec-vms is a FIXED 8192-byte-page arch. musl's
# mallocng derives its meta-area mmap/mprotect extent and per-slot placement from
# PGSZ (= PAGE_SIZE). Before this fix the overlay shipped NO arch bits/limits.h,
# so the build fell back to arch/generic/bits/limits.h (empty) -> PAGESIZE
# undefined -> src/internal/libc.h defines PAGE_SIZE = libc.page_size, a RUNTIME
# value set from the ELF auxv AT_PAGESZ. OVMX/VMS starts images via IMGACT/STARTUP
# with no Linux auxv, so libc.page_size is 0, which malloc.c clamps to 4096. On
# alpha's real 8KB pages that mis-sizing lands a struct-meta zero-init (memset,
# ~sizeof(struct meta)) straddling an 8KB page boundary into an unmapped page ->
# SIGSEGV on the FIRST small alloc (the crtl_rms veneer's ~1KB handle). Captured
# fault frame: user PC=memset, dst=0x20001003fe0 fill=0 count=0x38, page bound
# 0x20001004000 unmapped (vms-c5d note 2026-09-10T17:19Z).
#
# arch/alpha-dec-vms/bits/limits.h now pins `#define PAGESIZE 8192` — the
# musl-canonical fixed-page-arch mechanism (cf. arch/or1k, the only other 8KB
# musl arch). Assert the WHOLE header-resolution chain yields a COMPILE-TIME
# 8192: a _Static_assert on the runtime libc.page_size lvalue would NOT compile,
# which is precisely the pre-fix state, so this gate genuinely fails without the
# fix. Uses the target compiler with musl's own include set (-nostdinc, mirroring
# CFLAGS_ALL) in -fsyntax-only mode (no codegen -> no DST/EVAX emission).
# ==========================================================================
# ==========================================================================
# DEC C HEADER-FORMS GATE (vms-28d). A client TU in the shape of the GCC port's
# host sources (libiberty strsignal.c defines its own psignal; getpwd.c calls
# getcwd(buf, len, 0); xstrerror.c reads vaxc$errno) must compile against the
# installed public headers at BOTH pointer sizes, with ptrdiff_t/intptr_t/
# uintptr_t (vms-ce6) equal to the compiler's own types at each size. Control: the SAME TU compiled
# as the C RTL's own build (__OVMX_LIBC_BUILD: plain POSIX forms) must FAIL, so
# the forms demonstrably come from the patched __VMS client declarations.
# ==========================================================================
echo "== DEC C header-forms gate (vms-28d) =="
cat > /tmp/decc_forms.c <<'EOF'
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stddef.h>
void psignal(int signo, char *message) { (void)signo; (void)message; }
/* vms-fe03: strsignal is not in the DEC C RTL; GCC's system.h declares its own
 * when configure finds none -- that must not conflict with <string.h>. */
#include <string.h>
const char *strsignal(int);
size_t forms_len(const char *s) { return strlen(s); }   /* DEC C surface stays declared */
char *forms_pwd(char *b, size_t n) { return getcwd(b, n, 0); }
int forms_vms_status(void) { return vaxc$errno; }
/* vms-ce6: the pointer-width types match the compiler's at this pointer size */
#include <stdint.h>
_Static_assert(__builtin_types_compatible_p(ptrdiff_t, __PTRDIFF_TYPE__), "ptrdiff_t is the compiler's");
_Static_assert(__builtin_types_compatible_p(intptr_t, __INTPTR_TYPE__), "intptr_t is the compiler's");
_Static_assert(__builtin_types_compatible_p(uintptr_t, __UINTPTR_TYPE__), "uintptr_t is the compiler's");
_Static_assert(INTPTR_MAX == __INTPTR_MAX__ && PTRDIFF_MAX == __PTRDIFF_MAX__, "pointer-width limits");
EOF
for ps in "" "-mpointer-size=64"; do
	if "${TARGET}-gcc" ${ps} -Werror=implicit-function-declaration -nostdinc -Iarch/${TARGET} -Iarch/generic -Iobj/include -Iinclude \
		-fsyntax-only /tmp/decc_forms.c 2>/tmp/decc_forms.err; then
		echo "  OK      client TU compiles (pointer size: ${ps:-default 32})"
	else
		echo "VERIFY FAIL (vms-28d): DEC C client forms do not compile (pointer size: ${ps:-default 32}):" >&2
		cat /tmp/decc_forms.err >&2; exit 7
	fi
done
if "${TARGET}-gcc" -mpointer-size=64 -D__OVMX_LIBC_BUILD -nostdinc -Iarch/${TARGET} -Iarch/generic \
	-Iobj/include -Iinclude -fsyntax-only /tmp/decc_forms.c 2>/dev/null; then
	echo "VERIFY FAIL (vms-28d): control -- the client TU compiled under __OVMX_LIBC_BUILD, so the gate proves nothing" >&2
	exit 7
fi
echo "  OK      control: the same TU is rejected under the C RTL's own POSIX forms"

# vms-fe03: a header with no DEC C RTL function (sys/prctl.h) is refused whole
# for a client, and still compiles for the C RTL's own build.
printf '#include <sys/prctl.h>\nint fe03_x;\n' > /tmp/fe03_prctl.c
if "${TARGET}-gcc" -nostdinc -Iarch/${TARGET} -Iarch/generic -Iobj/include -Iinclude \
	-fsyntax-only /tmp/fe03_prctl.c 2>/dev/null; then
	echo "VERIFY FAIL (vms-fe03): <sys/prctl.h> (no DECC\$SHR entry point) was accepted for an alpha-dec-vms client" >&2
	exit 7
fi
"${TARGET}-gcc" -mpointer-size=64 -D__OVMX_LIBC_BUILD -nostdinc -Iarch/${TARGET} -Iarch/generic \
	-Iobj/include -Iinclude -fsyntax-only /tmp/fe03_prctl.c \
	|| { echo "VERIFY FAIL (vms-fe03): <sys/prctl.h> rejected for the C RTL's own build" >&2; exit 7; }
echo "  OK      a header with no DEC C RTL function is refused for clients, kept for the RTL build"

echo "== page-size gate (vms-c5d): PAGE_SIZE must be a compile-time 8192 on alpha-dec-vms =="
PGSZ_HDR="arch/${TARGET}/bits/limits.h"
if ! grep -qE '^[[:space:]]*#[[:space:]]*define[[:space:]]+PAGESIZE[[:space:]]+8192' "${PGSZ_HDR}"; then
	echo "VERIFY FAIL (vms-c5d): ${PGSZ_HDR} does not pin '#define PAGESIZE 8192'." >&2
	echo "  Without it mallocng uses the runtime libc.page_size path (=0 on OVMX, clamped 4096)" >&2
	echo "  and straddles alpha's 8KB pages -> SIGSEGV on the first small alloc." >&2
	exit 7
fi
cat > /tmp/pgsz.c <<'EOF'
#include <limits.h>
#ifndef PAGE_SIZE
#error "PAGE_SIZE is not a compile-time constant: bits/limits.h did not pin PAGESIZE, so mallocng falls to the runtime libc.page_size path (=0 on OVMX -> clamped 4096) and straddles 8KB pages (vms-c5d)"
#endif
_Static_assert(PAGE_SIZE == 8192, "alpha-dec-vms is a fixed 8KB-page arch; PAGE_SIZE must be 8192 (vms-c5d)");
int __vms_c5d_pgsz_ok = PAGE_SIZE;
EOF
if "${TARGET}-gcc" ${CC_FLAGS} -nostdinc -D_XOPEN_SOURCE=700 \
	-Iarch/${TARGET} -Iarch/generic -Iobj/src/internal \
	-Isrc/include -Isrc/internal -Iobj/include -Iinclude \
	-fsyntax-only /tmp/pgsz.c 2>/tmp/pgsz.err; then
	echo "  OK      PAGE_SIZE == 8192 (compile-time) — mallocng meta placement matches alpha's 8KB pages"
else
	echo "VERIFY FAIL (vms-c5d): PAGE_SIZE is not a compile-time 8192 through musl's own header set:" >&2
	cat /tmp/pgsz.err >&2
	exit 7
fi

# --------------------------------------------------------------------------
# ONE-HEAP GATE (vms-032): lite_malloc.o must be the alpha-dec-vms override --
# it may only REFERENCE __libc_malloc_impl (mallocng's), never define it. A
# local (weak) definition is what the back end self-binds section-relatively,
# splitting DECC$SHR into two allocators over one brk.
# --------------------------------------------------------------------------
echo "== one-heap gate (vms-032): lite_malloc.o references mallocng, defines no allocator =="
rm -rf /tmp/lmck && mkdir -p /tmp/lmck
LMEMB=$(grep -m1 -E '(^|/)lite_malloc\.' /tmp/libc.members || true)
[ -n "${LMEMB}" ] || { echo "VERIFY FAIL (vms-032): no lite_malloc member in libc.a" >&2; exit 8; }
( cd /tmp/lmck && ar x "${LIBCA}" "${LMEMB}" ) 2>/dev/null || true
"${NM}" "/tmp/lmck/${LMEMB}" >/tmp/lmck/nm.out 2>&1 || { cat /tmp/lmck/nm.out >&2; exit 8; }
if grep -qE '^[0-9a-fA-F]+[[:space:]]+[A-TV-Za-tv-z][[:space:]]+(__libc_malloc_impl|__simple_malloc)$' /tmp/lmck/nm.out; then
	echo "VERIFY FAIL (vms-032): lite_malloc.o DEFINES an allocator (generic lite_malloc.c, not the alpha-dec-vms override):" >&2
	cat /tmp/lmck/nm.out >&2
	exit 8
fi
if ! grep -qE '[[:space:]]U[[:space:]]+__libc_malloc_impl$' /tmp/lmck/nm.out; then
	echo "VERIFY FAIL (vms-032): lite_malloc.o does not reference __libc_malloc_impl by name:" >&2
	cat /tmp/lmck/nm.out >&2
	exit 8
fi
echo "  OK      lite_malloc.o: U __libc_malloc_impl, no local allocator -> one heap (mallocng)"

# --------------------------------------------------------------------------
# SIZE_T MODEL GATE (vms-537): through musl's own installed header set, ::size_t
# must BE the compiler's __SIZE_TYPE__ (32-bit, DEC C), ssize_t 32-bit, pointers
# and uintptr_t/ptrdiff_t 64-bit, and the kernel-shaped iovec / stack_t keep a
# 64-bit length. Compile-only (no codegen), so it runs in the light leg.
# --------------------------------------------------------------------------
echo "== size_t model gate (vms-537): CRTL size_t == compiler __SIZE_TYPE__ =="
cat > /tmp/szt.c <<'SZTEOF'
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <signal.h>
_Static_assert(__builtin_types_compatible_p(size_t, __typeof__(sizeof 0)), "::size_t must be the compiler's __SIZE_TYPE__ (vms-537)");
_Static_assert(sizeof(size_t) == 4 && sizeof(ssize_t) == 4, "size_t/ssize_t are 32-bit on OpenVMS Alpha (vms-537)");
_Static_assert(sizeof(void *) == 8 && sizeof(uintptr_t) == 8 && sizeof(ptrdiff_t) == 8, "64-bit pointers keep pointer-width uintptr_t/ptrdiff_t");
_Static_assert(SIZE_MAX == 0xffffffffu, "SIZE_MAX tracks the 32-bit size_t");
_Static_assert(sizeof(struct iovec) == 16 && offsetof(struct iovec, iov_len) == 8 && sizeof(((struct iovec *)0)->iov_len) == 8, "iovec is kernel-shaped");
_Static_assert(offsetof(stack_t, ss_size) == 16 && sizeof(((stack_t *)0)->ss_size) == 8, "stack_t.ss_size is kernel-shaped");
int __vms_537_sizet_ok;
SZTEOF
if "${TARGET}-gcc" ${CC_FLAGS} -nostdinc -D_GNU_SOURCE \
	-Iarch/${TARGET} -Iarch/generic -Iobj/src/internal \
	-Isrc/include -Isrc/internal -Iobj/include -Iinclude \
	-fsyntax-only /tmp/szt.c 2>/tmp/szt.err; then
	echo "  OK      size_t == __SIZE_TYPE__ (32-bit), pointers/uintptr_t/ptrdiff_t 64-bit, iovec/stack_t kernel-shaped"
else
	echo "VERIFY FAIL (vms-537): CRTL size_t model does not match the port compiler:" >&2
	cat /tmp/szt.err >&2
	exit 9
fi

if [ "$PARTIAL" = "1" ]; then
	echo "=== vms-960 RUNG 1 VERIFY OK on a PARTIAL alpha-dec-vms libc.a (${NMEMB} members) ==="
	echo "=== (failing members documented above; symbol-level proof is LINK.EXE) ==="
else
	echo "=== vms-960 RUNG 1 BUILD+VERIFY OK: FULL alpha-dec-vms libc.a (${NMEMB} members) ==="
fi
echo "NOTE: per-symbol nm/objdump verification is blocked by the GNU binutils"
echo "      vms-alpha DST reader gap (vms-7b96); the real symbol proof is"
echo "      LINK.EXE consuming these members to emit DECC\$SHR (mk_decc_shr.sh)."
