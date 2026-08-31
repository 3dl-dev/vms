#!/bin/sh
# build-ssh-harness.sh — the UNIFIED SSH proof harness, replacing the separate
# build-ssh-kex-harness.sh (client transport) and build-sshd-server-harness.sh
# (server transport). rd vms-0cd (sshd-over-BGn:), parent vms-843/vms-22a.
#
# ONE fetch + ONE libcrypto + ONE veneer archive, then TWO configured build trees
# — because the wrap TOPOLOGY is link-time but the sshd-session re-exec path is a
# configure-time --libexecdir baked into config.h, and the two proofs need
# DIFFERENT sshd-session binaries at that path (KEX = stock, server = wrapped).
# The expensive parts (fetch + libcrypto) are shared; only OpenSSH's own .o's
# compile twice (small, and layer-cached).
#
#   KEX tree   (--libexecdir=/ovmxssh/libexec):   WRAPPED ssh CLIENT (SSHLIBS
#     wrap) + STOCK sshd/sshd-session/sshd-auth. Proves the client transport over
#     BGn: against a byte-stock server (vms-9ac).
#   SERVER tree(--libexecdir=/ovmxsshsrv/libexec): STOCK ssh client + WRAPPED
#     sshd/sshd-session/sshd-auth (LIBS-global wrap: bind/listen/accept on the
#     listener + read/write/... on sshd-session, all via $(LIBS)). Proves the
#     server transport over BGn: (vms-0cd) — a stock client connects inbound.
#
# The OpenSSH source is UNMODIFIED in both; all dispatch is linker --wrap.
# Run inside the project's musl container (alpine:3.20), NEVER on the bare host.
#
# On success prints, on the LAST lines (consumed by tests/qemu/Dockerfile):
#   OVMX_KEX_SSH= OVMX_KEX_SSHD= OVMX_KEX_KEYGEN= OVMX_KEX_LIBEXEC= OVMX_KEX_SRCDIR=
#   OVMX_SRV_SSH= OVMX_SRV_SSHD= OVMX_SRV_LIBEXEC= OVMX_SRV_SRCDIR=
set -eu

WORK="${WORK:-/tmp/ovmx-ssh}"
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

# --- fetch tarball ONCE ---
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

# --- vendored static libcrypto ONCE (build-libcrypto.sh caches) ---
LC_OUT="$(sh "$ROOT/third-party/libcrypto/build-libcrypto.sh")"
LIBCRYPTO_A="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_A=//p' | tail -1)"
LIBCRYPTO_INC="$(echo "$LC_OUT" | sed -n 's/^OVMX_LIBCRYPTO_INCLUDE=//p' | tail -1)"
SSL="$WORK/ssl"; rm -rf "$SSL"; mkdir -p "$SSL/lib" "$SSL/include"
cp "$LIBCRYPTO_A" "$SSL/lib/"; cp -r "$LIBCRYPTO_INC/openssl" "$SSL/include/"

# --- veneer archive ONCE (topology-independent, -ffreestanding; carries both the
#     client ops and the server ops ovmx_bind/listen/accept) ---
VINC="-I$ROOT/src/libvms/include -I$ROOT/src/libvmssys -I$ROOT/src/vmstcpip/sockets -I$ROOT/src/kernel"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/vmstcpip/sockets/vms_bgsock.c"   -o "$WORK/ov_bgsock.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_kif.c"             -o "$WORK/ov_kif.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/kif_transport_linux.c" -o "$WORK/ov_xport.o"
"$CC" -O2 -ffreestanding -c $VINC "$ROOT/src/libvmssys/vms_string.c"          -o "$WORK/ov_string.o"
"$CC" -c "$ROOT/src/libvmssys/arch/x86_64/syscall.S" -o "$WORK/ov_syscall.o"
VENEER="$WORK/libovmxveneer.a"
ar rcs "$VENEER" "$WORK/ov_bgsock.o" "$WORK/ov_kif.o" "$WORK/ov_xport.o" "$WORK/ov_string.o" "$WORK/ov_syscall.o"

# configure + extract a tree; sets SRCDIR globals used by the caller. The
# --libexecdir (sshd-session re-exec path) and --with-privsep-path are baked at
# configure and must be the RUNTIME paths this tree's binaries are staged at.
extract_and_configure() {  # $1 = build dir, $2 = runtime libexecdir, $3 = runtime privsep dir
    _dir="$1"; _libexec="$2"; _priv="$3"
    rm -rf "$_dir"; mkdir -p "$_dir"; cd "$_dir"
    tar xzf "$WORK/$TARBALL"
    SRCDIR="$_dir/$OSSH_INNER"
    [ -d "$SRCDIR" ] || { echo "FAIL: inner dir $SRCDIR missing" >&2; exit 1; }
    mkdir -p "$SRCDIR/ovmx"
    cp "$ROOT/src/vmstcpip/sockets/vms_bgsock.h" "$SRCDIR/ovmx/"
    cp "$HERE/ovmx/ovmx_ssh_wrap.c" "$SRCDIR/ovmx/"
    # vms-0cd 3c: the sshd auth/session adapter TUs (only linked in the SERVER
    # tree, but copied into both -- harmless in the KEX tree, which does not
    # compile them).
    cp "$HERE/ovmx/ovmx_sshd_auth.c" "$HERE/ovmx/ovmx_sshd_session.c" \
       "$HERE/ovmx/ovmx_sshd_exec.c" "$SRCDIR/ovmx/"
    mkdir -p "$_libexec" "$_priv"
    cd "$SRCDIR"
    CC="$CC" CFLAGS="-O2" LDFLAGS="-static" \
      ./configure --host=x86_64-linux-musl \
        --with-ssl-dir="$SSL" --without-zlib --without-pam --without-selinux \
        --disable-strip --libexecdir="$_libexec" --with-privsep-path="$_priv" \
        >"$_dir/configure.log" 2>&1 || { echo "FAIL: configure ($_dir)"; tail -30 "$_dir/configure.log"; exit 1; }
    OSSH_CFLAGS=$(sed -n 's/^CFLAGS=[[:space:]]*//p' Makefile | head -1)
    OSSH_CPPFLAGS=$(sed -n 's/^CPPFLAGS=[[:space:]]*//p' Makefile | head -1 | sed "s#\$(PATHS)##; s#\$(srcdir)#$SRCDIR#g")
}

# ===================== KEX TREE: wrapped ssh CLIENT + stock sshd =====================
KEX_LIBEXEC="/ovmxssh/libexec"
extract_and_configure "$WORK/kex" "$KEX_LIBEXEC" "/ovmxssh/empty"
KEX_SRC="$SRCDIR"
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -DOVMX_WRAP -I"$KEX_SRC" -c "$KEX_SRC/ovmx/ovmx_ssh_wrap.c" -o "$WORK/ov_wrap_kex.o"
CLIENT_WRAP=""
for _s in socket connect read write close getpeername getsockname \
          setsockopt getsockopt shutdown fcntl poll ppoll; do
    CLIENT_WRAP="$CLIENT_WRAP -Wl,--wrap=$_s"
done
# CLIENT-only wrap via SSHLIBS (ssh link rule only); sshd stays STOCK on LIBS.
sed -i "s#^SSHLIBS=#SSHLIBS=$CLIENT_WRAP $WORK/ov_wrap_kex.o $VENEER #" Makefile
echo "== KEX: make ssh (wrapped) sshd sshd-session sshd-auth (stock) ssh-keygen =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh sshd sshd-session sshd-auth ssh-keygen \
    >"$WORK/kex/make.log" 2>&1 || { echo "FAIL: KEX make"; tail -50 "$WORK/kex/make.log"; exit 1; }
for b in ssh sshd sshd-session sshd-auth ssh-keygen; do
    [ -x "$KEX_SRC/$b" ] || { echo "FAIL: KEX $b not produced" >&2; exit 1; }
done
mkdir -p "$KEX_LIBEXEC"
[ "$KEX_LIBEXEC" = "$KEX_SRC" ] || cp "$KEX_SRC/sshd-session" "$KEX_SRC/sshd-auth" "$KEX_LIBEXEC/"

# ===================== SERVER TREE: stock ssh + wrapped sshd SERVER =====================
SRV_LIBEXEC="/ovmxsshsrv/libexec"
extract_and_configure "$WORK/srv" "$SRV_LIBEXEC" "/ovmxsshsrv/empty"
SRV_SRC="$SRCDIR"
# STOCK ssh + ssh-keygen FIRST, before any LIBS wrap mutation.
echo "== SERVER: make ssh ssh-keygen (STOCK client) =="
make -j"$(nproc 2>/dev/null || echo 2)" ssh ssh-keygen \
    >"$WORK/srv/make-client.log" 2>&1 || { echo "FAIL: SERVER client make"; tail -50 "$WORK/srv/make-client.log"; exit 1; }
[ -x "$SRV_SRC/ssh" ] || { echo "FAIL: SERVER stock ssh not produced" >&2; exit 1; }
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -DOVMX_WRAP -DOVMX_WRAP_SERVER -I"$SRV_SRC" \
    -c "$SRV_SRC/ovmx/ovmx_ssh_wrap.c" -o "$WORK/ov_wrap_srv.o"

# ===== vms-0cd RUNG-3 step 3c: SYSUAF password auth + LOGINOUT/DCL session =====
# The wrapped sshd now authenticates against the BINARY SYSUAF (Purdy) and runs
# a LOGINOUT->DCL session instead of a shell, via UNMODIFIED OpenSSH + two shims:
#   AUTH:    -DCUSTOM_SYS_AUTH_PASSWD selects our sys_auth_passwd (SYSUAF/Purdy)
#            and --wrap=getpwnam resolves the login account from SYSUAF (UIC ->
#            uid/gid, pw_shell = DCL) so OpenSSH's own permanently_set_uid drops
#            to the UIC.
#   SESSION: --wrap=permanently_set_uid runs the LOGINOUT pre-drop (executive
#            setident fail-closed + banner + accounting) while still root, and
#            --wrap=execve rewrites the DCL exec into `vmsdcl --login --lgicmd`.
# The VMS logic lives in src/vmsssh/{sshd_auth,sshd_session,ssh_ident}.c (the
# unit-tested, OpenSSH-free halves); the adapters bridge OpenSSH's call sites.
# -DCUSTOM_SYS_AUTH_PASSWD must reach auth-passwd.c too (it drops its default
# sys_auth_passwd under the define), so inject it into the tree's CPPFLAGS.
sed -i "s#^CPPFLAGS=#CPPFLAGS=-DCUSTOM_SYS_AUTH_PASSWD #" Makefile

# OVMX-free helper halves, hosted CFLAGS + the OVMX include set (mirrors the
# vmssshd CMake target's includes).
OVMXINC="-I$ROOT/src/vmsssh -I$ROOT/src/libvms/include -I$ROOT/src/vmsprocess/include -I$ROOT/src/vmsfs/include -I$ROOT/src/vmsrms/include -I$ROOT/src/vmslnm/include -I$ROOT/src/libvmssys"
"$CC" -O2 $OVMXINC -c "$ROOT/src/vmsssh/sshd_auth.c"    -o "$WORK/ov_sshd_auth.o"
"$CC" -O2 $OVMXINC -c "$ROOT/src/vmsssh/sshd_session.c" -o "$WORK/ov_sshd_session.o"
"$CC" -O2 $OVMXINC -c "$ROOT/src/vmsssh/ssh_ident.c"    -o "$WORK/ov_ssh_ident.o"
# The two OpenSSH adapters (macro-clean: only src/vmsssh on the OVMX side, so no
# OVMX header shadows an OpenSSH one). Auth adapter needs the CUSTOM define.
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -DCUSTOM_SYS_AUTH_PASSWD -I"$SRV_SRC" -I"$ROOT/src/vmsssh" \
    -c "$SRV_SRC/ovmx/ovmx_sshd_auth.c"    -o "$WORK/ov_adapt_auth.o"
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -I"$SRV_SRC" -I"$ROOT/src/vmsssh" \
    -c "$SRV_SRC/ovmx/ovmx_sshd_session.c" -o "$WORK/ov_adapt_session.o"
"$CC" $OSSH_CFLAGS $OSSH_CPPFLAGS -I"$SRV_SRC" -I"$ROOT/src/vmsssh" \
    -c "$SRV_SRC/ovmx/ovmx_sshd_exec.c"    -o "$WORK/ov_adapt_exec.o"

# The OVMX static archives the SYSUAF/RMS/executive stack links from -- built by
# the earlier build-static (OVMX_STATIC musl) stage this harness's Dockerfile
# runs first. The in-tree build names them by VMS convention (e.g. LIBVMS$SHR.EXE)
# even when they are static ar archives, so we DISCOVER every real `ar` archive
# under build-static by its `!<arch>` magic (name-independent, robust to a
# rename) and link them ALL in a --start-group: a static archive contributes
# objects only for still-unresolved symbols, so the unused ones cost nothing and
# the SYSUAF/RMS/accounting/purdy/kif objects resolve wherever they live. ABSENCE
# is fatal (a missing archive would leave sysuaf_lookup an unresolved symbol, not
# a silent no-op).
# NB: the archives are named by VMS convention with a literal '$' (e.g.
# LIBVMS$SHR.EXE). A '$' in a path injected into the Makefile LIBS= line is
# EATEN by make + shell variable expansion (LIBVMS$SHR.EXE -> LIBVMSHR.EXE -> ld
# "No such file"), so COPY each discovered archive to a '$'-free name under $WORK
# (which has none) and link the copies.
OVMXLIBS=""
_n=0
find "$ROOT/build-static" -type f \( -name '*.a' -o -name '*.EXE' \) 2>/dev/null > "$WORK/ovmx_arch_candidates"
while IFS= read -r _f; do
    [ "$(head -c 7 "$_f" 2>/dev/null)" = "!<arch>" ] || continue
    _n=$((_n + 1))
    cp "$_f" "$WORK/ovmxarch_$_n.a"
    OVMXLIBS="$OVMXLIBS $WORK/ovmxarch_$_n.a"
done < "$WORK/ovmx_arch_candidates"
[ -n "$OVMXLIBS" ] || { echo "FAIL: no OVMX static archives found under $ROOT/build-static (the OVMX_STATIC build-static stage must precede the SSH harness)"; exit 1; }
# The adapters + helpers go into an ARCHIVE (not force-linked .o's) so each
# server binary pulls ONLY the members it references: the sshd listener never
# calls permanently_set_uid (uidswap.o is not in its link), so __wrap_permanently_
# set_uid -- with its __real_permanently_set_uid reference -- must NOT be dragged
# into it; an archive leaves it out, while sshd-session (which links uidswap.o)
# pulls it. (__wrap_execve lives in its own object so pulling it for execve --
# referenced everywhere -- does not drag the permanently_set_uid wrap along.)
OVMX_SSHD_AR="$WORK/libovmxsshd.a"
rm -f "$OVMX_SSHD_AR"
ar rcs "$OVMX_SSHD_AR" \
    "$WORK/ov_adapt_auth.o" "$WORK/ov_adapt_session.o" "$WORK/ov_adapt_exec.o" \
    "$WORK/ov_sshd_auth.o" "$WORK/ov_sshd_session.o" "$WORK/ov_ssh_ident.o"
# --wrap adds the 3c auth/session interposers on top of the transport wraps.
OVMX_SSHD_WRAP="-Wl,--wrap=getpwnam -Wl,--wrap=getpwuid -Wl,--wrap=execve -Wl,--wrap=permanently_set_uid"

SERVER_WRAP=""
# dup2/dup: the session handoff (vms-0cd RUNG-3 completion). sshd dup2()s the
# accepted connection (a veneer handle) onto stdin/stdout before execv()'ing
# sshd-session; __wrap_dup2 materializes the handle as a real executive fd so the
# dup2 succeeds and the exec'd child reads/writes it natively (kernel fops -> the
# executive socket). Server-only -- the client link does not --wrap these.
for _s in socket connect bind listen accept accept4 dup dup2 read write close \
          getpeername getsockname setsockopt getsockopt shutdown fcntl poll ppoll; do
    SERVER_WRAP="$SERVER_WRAP -Wl,--wrap=$_s"
done
# SERVER wrap into GLOBAL LIBS so sshd (listener) AND sshd-session/sshd-auth (which
# do the connection I/O on the inherited veneer handle) ALL get the wraps -- every
# link rule pulls $(LIBS). ssh is already built (stock, above) and is not a dep, so
# it stays stock.
# The 3c objects/archives go BEFORE the veneer + OVMX archive group: the veneer
# (freestanding vms_kif/vms_string) satisfies those symbols first, so the same
# objects in libvmssys.a are not pulled (no duplicate-symbol clash); the OVMX
# archives (SYSUAF/RMS/accounting/purdy/...) resolve in a --start-group cycle.
# ov_wrap_srv.o stays force-linked (the listener's transport wraps, proven). The
# adapter archive + veneer + OVMX archives share ONE --start-group so on-demand
# members and their cross-references (adapter -> sysuaf -> kif) all resolve.
sed -i "s#^LIBS=#LIBS=$SERVER_WRAP $OVMX_SSHD_WRAP $WORK/ov_wrap_srv.o -Wl,--start-group $OVMX_SSHD_AR $VENEER $OVMXLIBS -Wl,--end-group -lm #" Makefile
echo "== SERVER: make sshd sshd-session sshd-auth (WRAPPED over BGn:) =="
make -j"$(nproc 2>/dev/null || echo 2)" sshd sshd-session sshd-auth \
    >"$WORK/srv/make-server.log" 2>&1 || { echo "FAIL: SERVER make"; tail -50 "$WORK/srv/make-server.log"; exit 1; }
for b in sshd sshd-session sshd-auth; do
    [ -x "$SRV_SRC/$b" ] || { echo "FAIL: SERVER $b not produced" >&2; exit 1; }
done
mkdir -p "$SRV_LIBEXEC"
[ "$SRV_LIBEXEC" = "$SRV_SRC" ] || cp "$SRV_SRC/sshd-session" "$SRV_SRC/sshd-auth" "$SRV_LIBEXEC/"

echo "OVMX_KEX_SSH=$KEX_SRC/ssh"
echo "OVMX_KEX_SSHD=$KEX_SRC/sshd"
echo "OVMX_KEX_KEYGEN=$KEX_SRC/ssh-keygen"
echo "OVMX_KEX_LIBEXEC=$KEX_LIBEXEC"
echo "OVMX_KEX_SRCDIR=$KEX_SRC"
echo "OVMX_SRV_SSH=$SRV_SRC/ssh"
echo "OVMX_SRV_SSHD=$SRV_SRC/sshd"
echo "OVMX_SRV_LIBEXEC=$SRV_LIBEXEC"
echo "OVMX_SRV_SRCDIR=$SRV_SRC"
