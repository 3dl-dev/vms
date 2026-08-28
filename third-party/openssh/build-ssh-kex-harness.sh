#!/bin/sh
# build-ssh-kex-harness.sh — build the SSH KEX proof harness: the veneer-linked
# `ssh` CLIENT (transport over the OVMX BGn: veneer) plus a STOCK OpenSSH `sshd`
# server (+ sshd-session / sshd-auth, its per-connection workers) and ssh-keygen,
# all from the one SHA-pinned OpenSSH tree, static against musl + the vendored
# libcrypto. Item vms-22a (parent vms-843), docs/design-openssh-port-ovmx.md §7.
#
# WHY ONE BUILD FOR BOTH ENDS (vms-9ac full de-veneer). The OpenSSH source is
# UNMODIFIED: the client's socket()/connect()/read()/write()/... reach the
# executive purely via the linker's --wrap dispatch (ovmx_ssh_wrap.o), and that
# --wrap set is applied ONLY to the `ssh` link (via SSHLIBS), NOT to `sshd`. So
# `ssh` = the OVMX client whose transport rides BGn: → the executive over a
# genuine veneer handle (no AF_UNIX socketpair, no pump), and `sshd` = a STOCK
# server on real host sockets (its socket()/connect() are never wrapped). The
# real key exchange therefore happens OVER the executive socket seam, with zero
# fabrication.
#
# OpenSSH >= 9.8 splits the daemon: `sshd` (listener) re-execs `sshd-session`
# (per-connection) and `sshd-auth` from the COMPILED libexecdir, so this builds
# and stages both, and configure's --libexecdir must be the RUNTIME path.
#
# Run inside the project's musl container (alpine:3.20), NEVER on the bare host.
#
# Inputs (env):
#   WORK        build/scratch dir (default /tmp/ovmx-ssh-kex)
#   CC          compiler (default gcc)
#   OSSH_LIBEXECDIR   runtime libexecdir sshd re-execs from (default $SRCDIR)
#   OSSH_PRIVSEP      runtime privilege-separation dir (default $WORK/empty)
# On success prints, on the LAST lines:
#   OVMX_KEX_SSH=<abs ssh>  OVMX_KEX_SSHD=<abs sshd>  OVMX_KEX_KEYGEN=<abs ssh-keygen>
#   OVMX_KEX_LIBEXEC=<abs libexecdir with sshd-session/sshd-auth>
#   OVMX_KEX_SRCDIR=<abs source tree>
set -eu

WORK="${WORK:-/tmp/ovmx-ssh-kex}"
CC="${CC:-gcc}"
HERE="$(cd "$(dirname "$0")" && pwd)"        # third-party/openssh
ROOT="$(cd "$HERE/../.." && pwd)"            # repo root

OSSH_VERSION="${OSSH_VERSION:-10.0p1}"
OSSH_SHA256="${OSSH_SHA256:-021a2e709a0edf4250b1256bd5a9e500411a90dddabea830ed59cef90eb9d85c}"
OSSH_INNER="${OSSH_INNER:-openssh-10.0p1}"
TARBALL="openssh-${OSSH_VERSION}.tar.gz"
URL_PRIMARY="${OVMX_OPENSSH_URL:-https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}}"
URL_MIRROR="https://ftp.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}"

mkdir -p "$WORK"; cd "$WORK"
SRCDIR="$WORK/${OSSH_INNER}"
OSSH_LIBEXECDIR="${OSSH_LIBEXECDIR:-$SRCDIR}"
OSSH_PRIVSEP="${OSSH_PRIVSEP:-$WORK/empty}"

fetch() {
    if command -v curl >/dev/null 2>&1; then curl -fsSL -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"
    else echo "FAIL: no curl/wget" >&2; return 1; fi
}
if [ ! -f "$TARBALL" ]; then
    fetch "$URL_PRIMARY" "$TARBALL" || fetch "$URL_MIRROR" "$TARBALL" || {
        echo "FAIL: could not fetch $TARBALL" >&2; exit 1; }
fi
echo "${OSSH_SHA256}  ${TARBALL}" | sha256sum -c - >/dev/null || {
    echo "FAIL: SHA256 mismatch for $TARBALL" >&2; exit 1; }

# --- vendored static libcrypto ---
LC_OUT="$(sh "$ROOT/third-party/libcrypto/build-libcrypto.sh")"
LIBCRYPTO_A="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)"
LIBCRYPTO_INC="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)"
SSL="$WORK/ssl"; rm -rf "$SSL"; mkdir -p "$SSL/lib" "$SSL/include"
cp "$LIBCRYPTO_A" "$SSL/lib/"; cp -r "$LIBCRYPTO_INC/openssl" "$SSL/include/"

# --- extract; NO source patch (vms-9ac full de-veneer: --wrap dispatch only) ---
rm -rf "$SRCDIR"; tar xzf "$TARBALL"
[ -d "$SRCDIR" ] || { echo "FAIL: inner dir $SRCDIR missing" >&2; exit 1; }
mkdir -p "$SRCDIR/ovmx"
cp "$ROOT/src/vmstcpip/sockets/vms_bgsock.h" "$SRCDIR/ovmx/"
cp "$HERE/ovmx/ovmx_ssh_wrap.c" "$SRCDIR/ovmx/"

# --- configure (static musl; runtime libexec/privsep paths baked in) ---
mkdir -p "$OSSH_PRIVSEP"
cd "$SRCDIR"
CC="$CC" CFLAGS="-O2" LDFLAGS="-static" \
  ./configure --host=x86_64-linux-musl \
    --with-ssl-dir="$SSL" --without-zlib --without-pam --without-selinux \
    --disable-strip --libexecdir="$OSSH_LIBEXECDIR" --with-privsep-path="$OSSH_PRIVSEP" \
    >"$WORK/kex-configure.log" 2>&1 || { echo "FAIL: configure"; tail -30 "$WORK/kex-configure.log"; exit 1; }

# --- veneer object set (self-contained via __vms_syscallN; no musl collision) ---
VINC="-I$ROOT/src/libvms/include -I$ROOT/src/libvmssys -I$ROOT/src/vmstcpip/sockets -I$ROOT/src/kernel"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/vmstcpip/sockets/vms_bgsock.c"   -o "$WORK/ov_bgsock.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_kif.c"             -o "$WORK/ov_kif.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/kif_transport_linux.c" -o "$WORK/ov_xport.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_string.c"          -o "$WORK/ov_string.o"
"$CC" -c "$ROOT/src/libvmssys/arch/x86_64/syscall.S" -o "$WORK/ov_syscall.o"
ar rcs "$WORK/libovmxveneer.a" "$WORK/ov_bgsock.o" "$WORK/ov_kif.o" "$WORK/ov_xport.o" "$WORK/ov_string.o" "$WORK/ov_syscall.o"

OSSH_CFLAGS=$(sed -n 's/^CFLAGS=[[:space:]]*//p' Makefile | head -1)
OSSH_CPPFLAGS=$(sed -n 's/^CPPFLAGS=[[:space:]]*//p' Makefile | head -1 | sed "s#\$(PATHS)##; s#\$(srcdir)#$SRCDIR#g")
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -DOVMX_WRAP -I"$SRCDIR" -c "$SRCDIR/ovmx/ovmx_ssh_wrap.c" -o "$WORK/ov_wrap.o"
OVMX_WRAP_LDFLAGS=""
for _s in socket connect read write close getpeername getsockname \
          setsockopt getsockopt shutdown fcntl poll ppoll; do
    OVMX_WRAP_LDFLAGS="$OVMX_WRAP_LDFLAGS -Wl,--wrap=$_s"
done

# CLIENT-ONLY wrap: SSHLIBS is on the `ssh` link rule ONLY (not sshd), so the
# forked sshd stays a STOCK server on a REAL socket that the wrapped ssh client
# reaches over the executive (BGn:). Wrapping sshd too would route its listen
# socket through the executive and break the test's stock-server topology.
sed -i "s#^SSHLIBS=#SSHLIBS=$OVMX_WRAP_LDFLAGS $WORK/ov_wrap.o $WORK/libovmxveneer.a #" Makefile

echo "== make ssh sshd sshd-session sshd-auth ssh-keygen =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh sshd sshd-session sshd-auth ssh-keygen \
    >"$WORK/kex-make.log" 2>&1 || { echo "FAIL: make"; tail -50 "$WORK/kex-make.log"; exit 1; }
for b in ssh sshd sshd-session sshd-auth ssh-keygen; do
    [ -x "$SRCDIR/$b" ] || { echo "FAIL: $b not produced" >&2; exit 1; }
done
# Place the per-connection workers where the compiled sshd re-execs them.
mkdir -p "$OSSH_LIBEXECDIR"
if [ "$OSSH_LIBEXECDIR" != "$SRCDIR" ]; then
    cp "$SRCDIR/sshd-session" "$SRCDIR/sshd-auth" "$OSSH_LIBEXECDIR/"
fi

echo "OVMX_KEX_SSH=$SRCDIR/ssh"
echo "OVMX_KEX_SSHD=$SRCDIR/sshd"
echo "OVMX_KEX_KEYGEN=$SRCDIR/ssh-keygen"
echo "OVMX_KEX_LIBEXEC=$OSSH_LIBEXECDIR"
echo "OVMX_KEX_SRCDIR=$SRCDIR"
