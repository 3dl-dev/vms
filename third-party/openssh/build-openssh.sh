#!/bin/sh
# build-openssh.sh — fetch the pinned OpenSSH-portable tarball (SHA256-verified)
# and cross-build the STATIC `ssh` client against musl + the vendored static
# libcrypto (third-party/libcrypto), entirely out-of-tree.
#
# OpenSSH CLIENTS port (item vms-22a, parent vms-843,
# docs/design-openssh-port-ovmx.md). Vendoring model = PINNED FETCH, same as
# third-party/libcrypto (see VENDOR-REV for the pin, license, and the p2/p1
# mirror note). The tarball is verified against a pinned SHA256 (reproducible;
# mismatch = hard fail) and built to $WORK (default /tmp) — NOTHING is installed
# on the host and no product lands in the repo. Run inside the project's musl
# container (alpine:3.20), NEVER on the bare host (shared-host rule).
#
# This proves the make-or-break for the whole port: OpenSSH + LibreSSL + musl
# links a fully-static `ssh`. The OVMX TRANSPORT does NOT live in this build:
# ssh reaches TCP through STANDARD BSD sockets, and OVMX supplies those via the
# BSD-sockets RTL veneer over BGn: (a separate prerequisite; see the design doc)
# — the app never speaks $QIO/TCPIP$DEVICE:. Building ssh as the OVMX IMGACT
# image linked against that veneer is the follow-on integration.
#
# Usage:
#   sh build-openssh.sh              # static ssh build
#   WORK=/tmp/foo sh build-openssh.sh
#
# On success prints, on the LAST lines, the paths downstream consumers need:
#   OVMX_SSH_BIN=<abs path to the static ssh binary>
#   OVMX_SSH_SRCDIR=<abs path to the extracted OpenSSH source tree>
set -eu

OSSH_VERSION="${OSSH_VERSION:-10.0p1}"
OSSH_SHA256="${OSSH_SHA256:-021a2e709a0edf4250b1256bd5a9e500411a90dddabea830ed59cef90eb9d85c}"
OSSH_INNER="${OSSH_INNER:-openssh-10.0p1}"
TARBALL="openssh-${OSSH_VERSION}.tar.gz"
URL_PRIMARY="${OVMX_OPENSSH_URL:-https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}}"
URL_MIRROR="https://ftp.openbsd.org/pub/OpenBSD/OpenSSH/portable/${TARBALL}"

WORK="${WORK:-/tmp/ovmx-openssh}"
CC="${CC:-gcc}"

HERE="$(cd "$(dirname "$0")" && pwd)"        # third-party/openssh
ROOT="$(cd "$HERE/../.." && pwd)"            # repo root

mkdir -p "$WORK"
cd "$WORK"

fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        echo "FAIL: neither curl nor wget available to fetch $1" >&2
        return 1
    fi
}

if [ ! -f "$TARBALL" ]; then
    echo "== fetch $TARBALL =="
    fetch "$URL_PRIMARY" "$TARBALL" || fetch "$URL_MIRROR" "$TARBALL" || {
        echo "FAIL: could not fetch $TARBALL from primary or mirror" >&2
        exit 1
    }
fi

echo "== verify SHA256 (pinned, reproducible) =="
echo "${OSSH_SHA256}  ${TARBALL}" | sha256sum -c - || {
    echo "FAIL: SHA256 mismatch for ${TARBALL} — refusing to build an unpinned tree" >&2
    echo "  expected ${OSSH_SHA256}" >&2
    echo "  got      $(sha256sum "${TARBALL}" | awk '{print $1}')" >&2
    exit 1
}

# ---- vendored static libcrypto (reuse the libcrypto build) ----------------
echo "== build the vendored static libcrypto =="
LC_OUT="$(sh "$ROOT/third-party/libcrypto/build-libcrypto.sh")"
echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO/  &/p'
LIBCRYPTO_A="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)"
LIBCRYPTO_INC="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)"
[ -f "$LIBCRYPTO_A" ] || { echo "FAIL: libcrypto.a not reported by build-libcrypto.sh" >&2; exit 1; }

# Stage an --with-ssl-dir prefix OpenSSH's configure understands (lib/ + include/).
SSL="$WORK/ssl"
rm -rf "$SSL"; mkdir -p "$SSL/lib" "$SSL/include"
cp "$LIBCRYPTO_A" "$SSL/lib/"
cp -r "$LIBCRYPTO_INC/openssl" "$SSL/include/"

# ---- extract + configure + build the static ssh client --------------------
SRCDIR="$WORK/${OSSH_INNER}"
rm -rf "$SRCDIR"
echo "== extract =="
tar xzf "$TARBALL"
[ -d "$SRCDIR" ] || { echo "FAIL: expected inner dir $SRCDIR not found after extract" >&2; exit 1; }

if [ "${OVMX_VENEER:-0}" = "1" ]; then
    echo "== apply OVMX veneer transport+poll substitution (sshconnect + packet) =="
    ( cd "$SRCDIR" && patch -p1 < "$HERE/ovmx/sshconnect-veneer.patch" \
                   && patch -p1 < "$HERE/ovmx/packet-veneer.patch" )
    # Stage the veneer header + glue so the vendored source's #include "ovmx/..."
    # resolves off -I. (srcdir).
    mkdir -p "$SRCDIR/ovmx"
    cp "$ROOT/src/vmstcpip/sockets/vms_bgsock.h" "$SRCDIR/ovmx/"
    cp "$HERE/ovmx/ovmx_ssh_glue.h" "$SRCDIR/ovmx/"
    cp "$HERE/ovmx/ovmx_ssh_glue.c" "$SRCDIR/ovmx/"
    # OVMX_WRAP (vms-4bf de-veneer): the linker-`--wrap` dispatch TU that routes
    # every fd syscall on the veneer handle to $QIO, retiring the socketpair.
    cp "$HERE/ovmx/ovmx_ssh_wrap.c" "$SRCDIR/ovmx/"
fi

cd "$SRCDIR"
echo "== configure (static, musl, vendored libcrypto) =="
CC="$CC" CFLAGS="-O2" LDFLAGS="-static" \
  ./configure --host=x86_64-linux-musl \
    --with-ssl-dir="$SSL" \
    --without-zlib \
    --without-pam \
    --without-selinux \
    --disable-strip \
    >"$WORK/openssh-configure.log" 2>&1 || {
        echo "FAIL: configure failed; tail of log:" >&2
        tail -40 "$WORK/openssh-configure.log" >&2
        exit 1
    }

if [ "${OVMX_VENEER:-0}" = "1" ]; then
    echo "== build the OVMX veneer object set (freestanding libvmssys subset) =="
    # The veneer reaches the executive over its own direct syscalls
    # (__vms_syscallN, syscall.S) -- no glibc/musl socket path -- so this set is
    # self-contained and links into the hosted-musl ssh without collision.
    VINC="-I$ROOT/src/libvms/include -I$ROOT/src/libvmssys -I$ROOT/src/vmstcpip/sockets -I$ROOT/src/kernel"
    "$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/vmstcpip/sockets/vms_bgsock.c"   -o "$WORK/ovmx_vms_bgsock.o"
    "$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_kif.c"             -o "$WORK/ovmx_vms_kif.o"
    "$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/kif_transport_linux.c" -o "$WORK/ovmx_kif_xport.o"
    "$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_string.c"          -o "$WORK/ovmx_vms_string.o"
    "$CC" -c "$ROOT/src/libvmssys/arch/x86_64/syscall.S" -o "$WORK/ovmx_syscall.o"
    ar rcs "$WORK/libovmxveneer.a" \
        "$WORK/ovmx_vms_bgsock.o" "$WORK/ovmx_vms_kif.o" "$WORK/ovmx_kif_xport.o" \
        "$WORK/ovmx_vms_string.o" "$WORK/ovmx_syscall.o"

    echo "== compile the OpenSSH<->veneer glue with OpenSSH's own flags =="
    # Reuse the exact CFLAGS/CPPFLAGS configure baked into the Makefile so the
    # glue sees config.h + sshbuf.h, plus -DOVMX_VENEER and the srcdir include.
    # OVMX_WRAP=1 additionally builds the --wrap dispatch layer (vms-4bf): the
    # glue returns the veneer HANDLE directly (no socketpair/pump), and every fd
    # syscall on it is routed to $QIO by ovmx_ssh_wrap.o via linker --wrap.
    OSSH_CFLAGS=$(sed -n 's/^CFLAGS=[[:space:]]*//p' Makefile | head -1)
    OSSH_CPPFLAGS=$(sed -n 's/^CPPFLAGS=[[:space:]]*//p' Makefile | head -1 | sed "s#\$(PATHS)##; s#\$(srcdir)#$SRCDIR#g")
    OVMX_DEFS="-DOVMX_VENEER"
    OVMX_WRAP_OBJ=""
    OVMX_WRAP_LDFLAGS=""
    if [ "${OVMX_WRAP:-0}" = "1" ]; then
        OVMX_DEFS="-DOVMX_VENEER -DOVMX_WRAP"
        "$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS $OVMX_DEFS -I"$SRCDIR" \
            -c "$SRCDIR/ovmx/ovmx_ssh_wrap.c" -o "$WORK/ovmx_ssh_wrap.o"
        OVMX_WRAP_OBJ="$WORK/ovmx_ssh_wrap.o"
        # One --wrap per fd syscall unmodified OpenSSH issues on the connection.
        for _s in read write close getpeername getsockname setsockopt getsockopt \
                  shutdown fcntl poll ppoll; do
            OVMX_WRAP_LDFLAGS="$OVMX_WRAP_LDFLAGS -Wl,--wrap=$_s"
        done
    fi
    "$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS $OVMX_DEFS -I"$SRCDIR" \
        -c "$SRCDIR/ovmx/ovmx_ssh_glue.c" -o "$WORK/ovmx_ssh_glue.o"

    echo "== wire the OVMX branch + objects into the ssh link =="
    # -DOVMX_VENEER activates the patched branches; the glue object + veneer
    # archive (+ the wrap object under OVMX_WRAP) are appended to LIBS so they
    # resolve at the final ssh link. The --wrap flags rewrite the fd-syscall
    # symbol references at link time.
    sed -i "s/^CPPFLAGS=/CPPFLAGS=$OVMX_DEFS /" Makefile
    sed -i "s#^LIBS=#LIBS=$OVMX_WRAP_LDFLAGS $WORK/ovmx_ssh_glue.o $OVMX_WRAP_OBJ $WORK/libovmxveneer.a #" Makefile
fi

echo "== make ssh (static) =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh >"$WORK/openssh-make.log" 2>&1 || {
    echo "FAIL: make ssh failed; tail of log:" >&2
    tail -60 "$WORK/openssh-make.log" >&2
    exit 1
}
[ -x "$SRCDIR/ssh" ] || { echo "FAIL: ssh binary not produced" >&2; exit 1; }

echo "== built ssh =="
ls -l "$SRCDIR/ssh"
echo "OVMX_SSH_BIN=$SRCDIR/ssh"
echo "OVMX_SSH_SRCDIR=$SRCDIR"
