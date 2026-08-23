#!/bin/bash
# build-toolchain.sh — build the alpha-dec-vms (OpenVMS/Alpha, VMS ABI) cross
# toolchain: GNU binutils (as/ld/objdump for EVAX objects) + GCC cc1 (the
# compiler proper). Runs INSIDE the tools/cross-alpha-vms Dockerfile container.
#
# WHY THIS EXISTS (build/oracle tooling, Rule-9-clean): this compiler emits
# genuine alpha-dec-vms EVAX objects, so OVMX's LINK.EXE can be gap-probed
# against REAL GCC-port output instead of reasoning to (sometimes false) gaps.
# It NEVER runs inside the OVMX guest — it is the oracle-side cross compiler on
# the build host. Note the OVMX Alpha *runtime* lane is OVMX/Linux-Alpha (the
# Linux ABI on Alpha), a DIFFERENT target; this is the VMS ABI the GCC port
# compiles for, which no Linux distro packages — hence a from-source build.
#
# cc1-only: `make all-gcc` builds the compiler proper (emits .s); no target
# libc/headers are needed to produce assembly, so this stays small and fast.
set -euxo pipefail

BINUTILS_VER=${BINUTILS_VER:-2.43}
GCC_VER=${GCC_VER:-14.2.0}
TARGET=${TARGET:-alpha-dec-vms}
PREFIX=${PREFIX:-/opt/cross-alpha-vms}
JOBS=$(nproc)

cd /src

# ---- patch routing helper (vms-52c1) ----
# Patches under tools/cross-alpha-vms/patches/ target EITHER the GCC tree OR the
# binutils tree; route each by the paths its diff touches. A binutils patch must
# be applied BEFORE binutils is configured/built (below); GCC patches are applied
# in the GCC section further down. Keep the two loops in sync with this predicate.
is_binutils_patch() {
  grep -qE '^(---|\+\+\+) [ab]/(bfd|gas|ld|opcodes|binutils|libctf|gprof)/' "$1"
}

# ---- binutils: target assembler/linker/objdump for EVAX ----
wget -q "https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VER}.tar.xz"
tar xf "binutils-${BINUTILS_VER}.tar.xz"

# ---- apply checked-in alpha-dec-vms port patches to binutils (vms-52c1) ----
# e.g. 0004: emit weak/aliased procedure DEFINITIONS in the vms-alpha back end
# (gas/bfd) so musl's weak_alias'd decc$* universals (decc$_malloc64, ...) reach
# the object symbol table. Applied to the FETCHED binutils source (we patch,
# never vendor).
if [ -d /src/patches ]; then
  for p in /src/patches/*.patch; do
    [ -e "$p" ] || continue
    is_binutils_patch "$p" || continue
    echo "== applying binutils port patch: $(basename "$p") =="
    patch -p1 -d "/src/binutils-${BINUTILS_VER}" < "$p"
  done
fi

mkdir -p build-binutils && cd build-binutils
"/src/binutils-${BINUTILS_VER}/configure" \
    --target="${TARGET}" --prefix="${PREFIX}" --disable-nls --disable-werror
make -j"${JOBS}"
make install
export PATH="${PREFIX}/bin:${PATH}"
cd /src

# ---- gcc cc1 (compiler proper) ----
wget -q "https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VER}/gcc-${GCC_VER}.tar.xz"
tar xf "gcc-${GCC_VER}.tar.xz"

# ---- apply checked-in alpha-dec-vms port patches (vms-f97) ----
# Minimal codegen-consistency fixes to the FETCHED GCC source (we patch, never
# vendor the whole tree). Each patch is a plain `patch -p1` unified diff under
# tools/cross-alpha-vms/patches/, COPYed into /src/patches by the Dockerfile.
if [ -d /src/patches ]; then
  for p in /src/patches/*.patch; do
    [ -e "$p" ] || continue
    is_binutils_patch "$p" && continue   # binutils patches applied above
    echo "== applying gcc port patch: $(basename "$p") =="
    patch -p1 -d "/src/gcc-${GCC_VER}" < "$p"
  done
fi

mkdir -p build-gcc && cd build-gcc
"/src/gcc-${GCC_VER}/configure" \
    --target="${TARGET}" --prefix="${PREFIX}" \
    --enable-languages=c --disable-bootstrap --disable-multilib \
    --disable-libssp --disable-shared --disable-nls \
    --disable-fixincludes \
    --without-headers --with-gnu-as --with-gnu-ld
# WRINKLE 1 (--disable-fixincludes above): a cross `make all-gcc` otherwise tries
# to build stmp-fixinc, which needs the BUILD-side fixincludes/fixinc.sh that a
# cross build never produces -> "No rule to make target .../fixinc.sh".
#
# WRINKLE 2 (mkdir below): all-gcc generates per-frontend target-hooks headers
# for EVERY frontend (d/, rust/, ...) even with --enable-languages=c, but only
# the enabled languages' build subdirs exist, so the genhooks `mv` fails
# ("cannot move tmp-d-target-hooks-def.h to d/"). Pre-create the subdirs.
mkdir -p gcc/{c,cp,c-family,common,objc,d,rust,go,fortran,ada,lto,jit,m2,analyzer}
make all-gcc -j"${JOBS}"
make install-gcc

# ---- libgcc.a: the compiler runtime (vms-7b96, RUNG-1) --------------------
# The alpha-dec-vms C-RTL shareable (DECC$SHR) whole-archives libgcc.a alongside
# musl's libc.a (mk_decc_shr.sh) — the compiler support routines musl references
# internally. Build it here so the image carries it.
#
# -g0 (no .vmsdebug): UNLIKE libc.a, libgcc carries no genuine debug value we
# need to preserve, and building it -g0 means no DST -> GNU `ar`/`ranlib` archive
# it normally (the vms-7b96 DST reader gap only bites objects that carry DST).
# -mpointer-size=64 matches musl's LP64/P64 objects so the two archives share one
# ABI. --without-headers configured the tree, so libgcc builds in inhibit_libc
# mode (the soft-float / integer / __clear_cache helpers, no libc-dependent bits).
# KEEP-GOING (-k): the alpha/vms libgcc config pulls VMS condition-handling EH
# glue (config/alpha/vms-gcc_shell_handler.c) that #includes VMS SDK headers we
# don't have (`vms/chfdef.h`) — that file, and any other VMS-header-dependent
# LIB2ADD extra, cannot build without the VMS system headers (vms-7b96 follow-up).
# They are NOT the compiler runtime musl references (integer/soft-float helpers,
# __clear_cache); on alpha integer divide is even OTS$DIV_I (VMS OTS), not libgcc.
# So build every libgcc object that CAN build (-k), then hand-archive the core
# objects into libgcc.a. -g0 -> no DST -> normal GNU ar reads them fine.
make -k all-target-libgcc -j"${JOBS}" \
    CFLAGS_FOR_TARGET='-g0 -O2 -mpointer-size=64' \
    || echo "== all-target-libgcc kept going past VMS-EH gaps (expected) =="
LIBGCC_BUILD="/src/build-gcc/${TARGET}/libgcc"
mkdir -p "${PREFIX}/lib"

# ARCHIVE STEP — HAND-BUILT no-index System V `ar` CONTAINER, NOT the cross ar
# (vms-a90f, do-it-like-VMS; identical archiver to musl-arch/build-musl.sh).
#
# WHY NOT `${TARGET}-ar`: the binutils vms-alpha `ar` writes a VMS *LBR librarian*
# container (magic 0x0207 "GNU ar ...", i.e. a `.olb`), NOT the System V `!<arch>\n`
# format. OVMX LINK.EXE's EVAX front end (link.c load_archive_evax) walks a System V
# `ar` container and evax_read()s each member; it does not parse an LBR. Fed an LBR
# libgcc.a it dies "not an ar archive", and even host `ar`/`nm` report "file format
# not recognized" (they cannot read vms-alpha at all — confirmed empirically), so the
# LBR could not be repacked after the fact either. The cross ar was therefore the
# wrong tool: libgcc.a must be a System V container from the start.
#
# The System V `ar` container is a trivial text-header + member-bytes format that
# needs NO object parsing to assemble — build it byte-for-byte, never reading a
# member. This is also robust to the port cc1's `.vmsdebug`/DST (which makes GNU ar
# choke on read) regardless of whether -g0 fully suppresses it. LINK.EXE never reads
# an archive symbol index, so a no-index archive is complete and correct.
AR_NOINDEX="$(mktemp)"
cat > "$AR_NOINDEX" <<'AREOF'
#!/usr/bin/perl
# no-index System V/GNU `ar` archiver: assemble a container from object files
# WITHOUT reading their contents. Call shape `<op> <archive> <obj>...`; the op
# key is ignored (we never build a symbol index — LINK.EXE whole-archives every
# member and never reads one). Shared logic with musl-arch/build-musl.sh.
use strict; use warnings;
shift @ARGV;                       # op key (rc/rcs/...): ignored
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

# WHICH objects belong in the C-RTL libgcc.a (vms-a90f): the compiler ARITHMETIC
# / soft-float / integer / complex / bit support builtins ONLY — the routines a
# musl C run-time references internally (on alpha-dec-vms that is __muldc3/__mulsc3
# for libc's complex math; the integer/float runtime is VMS OTS$/MATH$, a separate
# shareable — vms-bfd6). NOT everything that happens to sit in the libgcc build
# dir. The old blind `*.o` glob swept in whole categories that are NOT part of a
# C-RTL libgcc, each excluded below by category (this mirrors the x86_64 recipe's
# own filter_tls_members / __*_chk filtering — a deliberate C-RTL composition):
#   _gcov*                     profiling -> gcc packages these as a SEPARATE
#                              libgcov.a; never a libgcc.a member.
#   crtbegin*/crtend*          startup crt objects -> standalone files installed
#                              beside the compiler; never archive members. (They
#                              also carry a REFLONG into a placed ctor/frame table
#                              that LINK.EXE's EVAX path rightly refuses — a 32-bit
#                              slot cannot hold a 64-bit load-biased address.)
#   unwind*/vms-dwarf2*        exception-handling / DWARF-unwind runtime -> OVMX's
#                              DECC$SHR is a NO-EH C-RTL (the EH glue is the
#                              vms-7b96 follow-up; LIB2ADDEH= intent). Their
#                              _Unwind_* refs would otherwise dangle.
#   emutls*                    GCC emulated-TLS -> musl uses NATIVE TLS; dead here.
#   _eprintf*                  the glibc-fortify __eprintf assert helper -> dead
#                              code the x86_64 recipe already filters out.
# Everything else (the __*di3/__*sf*/__*df*/__*tf*/__mul?c3/__fix*/__float*/bit
# builtins + the small trampoline/hardening helpers) is kept. Result on
# alpha-dec-vms: 85 members; whole-archiving them resolves libc's __muldc3/__mulsc3
# and adds only OTS$REM_UL to the (expected, vms-bfd6) OTS$/MATH$ deferred residual.
LGOBJS=""
for o in "${LIBGCC_BUILD}"/*.o; do
  b=$(basename "$o")
  case "$b" in
    _gcov*|crtbegin*|crtend*|unwind*|vms-dwarf2*|emutls*|_eprintf*) continue;;
  esac
  LGOBJS="$LGOBJS $o"
done

if [ -n "$LGOBJS" ]; then
  # shellcheck disable=SC2086
  perl "$AR_NOINDEX" rc "${PREFIX}/lib/libgcc.a" $LGOBJS
  echo "== libgcc.a hand-assembled (System V no-index) at ${PREFIX}/lib/libgcc.a ($(echo $LGOBJS | wc -w) C-RTL builtin objects; gcov/crt/unwind/emutls/_eprintf excluded) =="
  # Sanity: it must be a System V ar (`!<arch>`), the container LINK.EXE walks.
  head -c 8 "${PREFIX}/lib/libgcc.a" | grep -q '!<arch>' \
    && echo "== libgcc.a is a System V ar container (LINK.EXE-readable) ==" \
    || { echo "== FAIL: libgcc.a is not a System V ar container ==" >&2; exit 5; }
else
  echo "== WARNING: no libgcc objects built; emitting an EMPTY libgcc.a placeholder =="
  # An empty archive is valid; whole-archive pulls 0 members. If musl-alpha ends
  # up referencing a libgcc helper, the strict DECC$SHR link names it (not faked).
  printf '!<arch>\n' > "${PREFIX}/lib/libgcc.a"
fi

# ---- smoke test: the compiler emits genuine VMS/Alpha asm ----
echo 'int main(void){ return 0; }' > /tmp/t.c
"${PREFIX}/bin/${TARGET}-gcc" -S -mpointer-size=64 /tmp/t.c -o /tmp/t.s || \
  "${PREFIX}/libexec/gcc/${TARGET}/${GCC_VER}/cc1" /tmp/t.c -o /tmp/t.s -quiet -mpointer-size=64
grep -q "__gcc_main_flags = 3" /tmp/t.s \
  && grep -qE "\.ent|\.pdesc" /tmp/t.s \
  && echo "SMOKE OK: emits __gcc_main_flags + VMS procedure descriptors"

echo "=== alpha-dec-vms cross toolchain built under ${PREFIX} ==="
"${PREFIX}/bin/${TARGET}-gcc" -dumpmachine
