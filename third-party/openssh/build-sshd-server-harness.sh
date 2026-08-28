#!/bin/sh
# build-sshd-server-harness.sh — build the SSH SERVER-transport proof harness: a
# WRAPPED OpenSSH `sshd` (+ sshd-session / sshd-auth) whose listen/accept AND the
# per-connection I/O ride the OVMX BGn: veneer via the linker's --wrap, PLUS a
# STOCK `ssh` client (real sockets) + ssh-keygen to drive it. Item vms-0cd (the
# OpenSSH sshd-over-BGn: server port), parent vms-843; the inverse topology of
# build-ssh-kex-harness.sh (which wraps the CLIENT and leaves sshd stock).
#
# WHY THIS INVERTS THE KEX HARNESS. The OpenSSH source is UNMODIFIED. Here the
# SERVER is wrapped: sshd's socket()/bind()/listen()/accept()/accept4() and
# sshd-session's read()/write()/... dispatch to the executive BGn: veneer, so the
# inbound TCP connection is an executive-resident socket. sshd's master accepts a
# veneer handle, then fork()+exec()s sshd-session; the accepted connection reaches
# the session child because the executive inherits the channel by number (#815)
# AND the veneer handle is self-describing (#822). NO AF_UNIX socketpair, NO pump.
# The STOCK `ssh` client uses REAL host sockets to reach the port the executive
# actually bound (the IP stack is the host's), so an ordinary client connects in.
#
# HOW THE WRAP IS SCOPED. OpenSSH >= 9.8 splits the daemon: `sshd` (listener, links
# with global $(LIBS)), `sshd-session` / `sshd-auth` (per-connection). The listener
# uses global LIBS, so we cannot scope the wrap to sshd via a sshd-only var the way
# the client build uses SSHLIBS. Instead we build the STOCK `ssh`/`ssh-keygen`
# FIRST (LIBS unmodified), THEN add the wrap set to global LIBS and `make sshd
# sshd-session sshd-auth` only — `ssh` is already built and is not a dep of sshd,
# so it stays stock. ovmx_ssh_wrap.c is compiled with -DOVMX_WRAP_SERVER to
# activate __wrap_bind/listen/accept/accept4 (gated so the client link never sees
# the undefined __real_bind &c.).
#
# Run inside the project's musl container (alpine:3.20), NEVER on the bare host.
#
# Inputs (env): WORK, CC, OSSH_LIBEXECDIR, OSSH_PRIVSEP (as the KEX harness).
# On success prints, on the LAST lines:
#   OVMX_SRV_SSH=<abs stock ssh>  OVMX_SRV_SSHD=<abs wrapped sshd>
#   OVMX_SRV_KEYGEN=<abs ssh-keygen>  OVMX_SRV_LIBEXEC=<abs libexecdir>
#   OVMX_SRV_SRCDIR=<abs source tree>
set -eu

WORK="${WORK:-/tmp/ovmx-ssh-server}"
CC="${CC:-gcc}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

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

LC_OUT="$(sh "$ROOT/third-party/libcrypto/build-libcrypto.sh")"
LIBCRYPTO_A="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)"
LIBCRYPTO_INC="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)"
SSL="$WORK/ssl"; rm -rf "$SSL"; mkdir -p "$SSL/lib" "$SSL/include"
cp "$LIBCRYPTO_A" "$SSL/lib/"; cp -r "$LIBCRYPTO_INC/openssl" "$SSL/include/"

# --- extract; NO source patch (--wrap dispatch only) ---
rm -rf "$SRCDIR"; tar xzf "$TARBALL"
[ -d "$SRCDIR" ] || { echo "FAIL: inner dir $SRCDIR missing" >&2; exit 1; }
mkdir -p "$SRCDIR/ovmx"
cp "$ROOT/src/vmstcpip/sockets/vms_bgsock.h" "$SRCDIR/ovmx/"
cp "$HERE/ovmx/ovmx_ssh_wrap.c" "$SRCDIR/ovmx/"

mkdir -p "$OSSH_PRIVSEP"
cd "$SRCDIR"
CC="$CC" CFLAGS="-O2" LDFLAGS="-static" \
  ./configure --host=x86_64-linux-musl \
    --with-ssl-dir="$SSL" --without-zlib --without-pam --without-selinux \
    --disable-strip --libexecdir="$OSSH_LIBEXECDIR" --with-privsep-path="$OSSH_PRIVSEP" \
    >"$WORK/srv-configure.log" 2>&1 || { echo "FAIL: configure"; tail -30 "$WORK/srv-configure.log"; exit 1; }

# --- veneer object set (carries the SERVER ops ovmx_bind/listen/accept too) ---
VINC="-I$ROOT/src/libvms/include -I$ROOT/src/libvmssys -I$ROOT/src/vmstcpip/sockets -I$ROOT/src/kernel"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/vmstcpip/sockets/vms_bgsock.c"   -o "$WORK/ov_bgsock.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_kif.c"             -o "$WORK/ov_kif.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/kif_transport_linux.c" -o "$WORK/ov_xport.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_string.c"          -o "$WORK/ov_string.o"
"$CC" -c "$ROOT/src/libvmssys/arch/x86_64/syscall.S" -o "$WORK/ov_syscall.o"
ar rcs "$WORK/libovmxveneer.a" "$WORK/ov_bgsock.o" "$WORK/ov_kif.o" "$WORK/ov_xport.o" "$WORK/ov_string.o" "$WORK/ov_syscall.o"

# --- STOCK ssh + ssh-keygen FIRST, before any LIBS wrap mutation ---
echo "== make ssh ssh-keygen (STOCK client) =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh ssh-keygen \
    >"$WORK/srv-make-client.log" 2>&1 || { echo "FAIL: make client"; tail -50 "$WORK/srv-make-client.log"; exit 1; }
[ -x "$SRCDIR/ssh" ] && [ -x "$SRCDIR/ssh-keygen" ] || { echo "FAIL: stock ssh/ssh-keygen not produced" >&2; exit 1; }

# --- the --wrap dispatch object, SERVER variant (bind/listen/accept enabled) ---
OSSH_CFLAGS=$(sed -n 's/^CFLAGS=[[:space:]]*//p' Makefile | head -1)
OSSH_CPPFLAGS=$(sed -n 's/^CPPFLAGS=[[:space:]]*//p' Makefile | head -1 | sed "s#\$(PATHS)##; s#\$(srcdir)#$SRCDIR#g")
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -DOVMX_WRAP -DOVMX_WRAP_SERVER -I"$SRCDIR" \
    -c "$SRCDIR/ovmx/ovmx_ssh_wrap.c" -o "$WORK/ov_wrap.o"
OVMX_WRAP_LDFLAGS=""
for _s in socket connect bind listen accept accept4 read write close \
          getpeername getsockname setsockopt getsockopt shutdown fcntl poll ppoll; do
    OVMX_WRAP_LDFLAGS="$OVMX_WRAP_LDFLAGS -Wl,--wrap=$_s"
done

# SERVER wrap into GLOBAL LIBS: sshd (listener) links with $(LIBS), and
# sshd-session/sshd-auth pick it up too. `ssh` is ALREADY built (above) and is not
# a dep of the sshd targets, so it stays STOCK -- only sshd/sshd-session/sshd-auth
# (made below) get the wrap.
sed -i "s#^LIBS=#LIBS=$OVMX_WRAP_LDFLAGS $WORK/ov_wrap.o $WORK/libovmxveneer.a #" Makefile

echo "== make sshd sshd-session sshd-auth (WRAPPED server over BGn:) =="
make -j"$(nproc 2>/dev/null || echo 2)" sshd sshd-session sshd-auth \
    >"$WORK/srv-make-server.log" 2>&1 || { echo "FAIL: make server"; tail -50 "$WORK/srv-make-server.log"; exit 1; }
for b in sshd sshd-session sshd-auth; do
    [ -x "$SRCDIR/$b" ] || { echo "FAIL: $b not produced" >&2; exit 1; }
done
mkdir -p "$OSSH_LIBEXECDIR"
if [ "$OSSH_LIBEXECDIR" != "$SRCDIR" ]; then
    cp "$SRCDIR/sshd-session" "$SRCDIR/sshd-auth" "$OSSH_LIBEXECDIR/"
fi

echo "OVMX_SRV_SSH=$SRCDIR/ssh"
echo "OVMX_SRV_SSHD=$SRCDIR/sshd"
echo "OVMX_SRV_KEYGEN=$SRCDIR/ssh-keygen"
echo "OVMX_SRV_LIBEXEC=$OSSH_LIBEXECDIR"
echo "OVMX_SRV_SRCDIR=$SRCDIR"
