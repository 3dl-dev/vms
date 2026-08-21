#!/usr/bin/env bash
#
# build-alpha-bootimage.sh -- assemble a bootable Linux/Alpha OVMX system image
# carrying the FULL OVMX userland + executive, mirroring the x86_64 QEMU
# bootable path (distro/Dockerfile.bootable) as far as the Alpha cross-build
# reaches.  rd vms-989, epic vms-989 (OVMX-on-Alpha), rung A5a -- the
# dispatchable front-half of boot-to-DCL.
#
# WHAT THIS PRODUCES (all under $WORK, default /tmp/ovmx-alpha-boot):
#   ovmx-distrib-alpha.img  -- a GENUINE Files-11 ODS-2 system disk built from
#                              the Alpha cross-built /vms tree (STARTUP/PROVISION/
#                              JOB_CONTROL/LOGINOUT/DCL + the RTL, all EM_ALPHA),
#                              mastered by the Alpha vmsfs_master --ods2 run under
#                              qemu-alpha -- the SAME genuine ODS-2 disk the
#                              x86_64 path masters (distro/Dockerfile.bootable
#                              `vmsfs_master --ods2 master ... 128`), just with
#                              Alpha images inside.  MUST be ODS-2: the atomic
#                              ACP flip (vms-5f0/vms-208) mounts DKA0: through the
#                              Files-11 ODS-2 executive and rejects a bespoke
#                              VMFS volume (acp_validate_ods2 requires DECFILE11B
#                              home block + BITMAP.SYS header + SCB).
#   vmlinux-boot            -- the Linux/Alpha kernel (6.6.52) with a
#                              bootstrap initramfs baked in (clipper's -initrd
#                              is unreliable, so we bake it, same as
#                              boot-vmsko-qemu-alpha.sh).  The initramfs is the
#                              bootstrap-only slim image: /init = STARTUP.EXE,
#                              vms.ko + vmsfs.ko, minimal SYSMGR/SYSUAF config,
#                              vms_mount_helper -- exactly Dockerfile.bootable's
#                              initramfs-slim, cross-built for Alpha.
#   imgact-proof/           -- IMGACT.EXE(alpha) + a VMS-native ET_DYN Alpha
#                              image (PT_INTERP=IMGACT.EXE, DT_NEEDED shareable)
#                              staged so boot-alpha-image.sh can prove IMGACT
#                              activates a real image UNDER THE BOOTED KERNEL.
#
# DEPENDS ON (built by the sibling scripts, cached in their own $WORK):
#   * build-vmsko-alpha.sh  -> /tmp/ovmx-vmsko-alpha/{linux-6.6.52, vms.ko,
#                              vmsfs.ko}  (kernel tree + executive modules)
#   * the Alpha userland cross-build -> $USERLAND/bin/*.EXE.  If absent this
#     script builds it (cmake alpha toolchain, static, tools ON).
#
# Rule 9: BUILD/TEST tooling only, all containerized.  It assembles and (via
# boot-alpha-image.sh) boots the REAL runtime; it is never itself a runtime.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMG="ovmx-cross-alpha"
KV="${KV:-6.6.52}"
VMSKO_WORK="${VMSKO_WORK:-/tmp/ovmx-vmsko-alpha}"
USERLAND="${USERLAND:-/tmp/ovmx-alpha-boot-build}"
WORK="${WORK:-/tmp/ovmx-alpha-boot}"

echo "== build cross/emulation image ($IMG) =="
docker build -t "$IMG" "$HERE" >/dev/null

# FORCE_BUILD=1 must force a FULL rebuild (kernel modules + userland), not just
# the boot-artifact re-assembly: run-boot-alpha.sh's ensure_artifacts checks
# FORCE_BUILD only for the disk/kernel-bake step, but the vms.ko and userland
# cross-builds below are otherwise guarded solely by "is the cached output
# present". A stale cache (e.g. a userland built from an earlier checkout) would
# then be silently reused under FORCE_BUILD=1, booting old binaries on a fresh
# disk and giving a false "green on this tree" (caught once: a V0.4-6 login
# banner on a V0.5 tree). So OR FORCE_BUILD into both guards below -- the caches
# are root-owned (built by the in-container docker user), so we cannot rm them
# host-side; instead we re-enter the build, whose cmake/make run as root in the
# container and overwrite the root-owned outputs from THIS tree.
_force="${FORCE_BUILD:-0}"

# ---- 0. prerequisites: executive modules + kernel tree ----
if [ "$_force" = "1" ] || [ ! -f "$VMSKO_WORK/vms.ko" ] || [ ! -f "$VMSKO_WORK/vmsfs.ko" ] || [ ! -d "$VMSKO_WORK/linux-$KV" ]; then
    echo "== executive modules/kernel not cached (or FORCE_BUILD) -- running build-vmsko-alpha.sh =="
    FORCE_BUILD="$_force" WORK="$VMSKO_WORK" KV="$KV" "$HERE/build-vmsko-alpha.sh"
fi

# ---- 1. Alpha userland (STARTUP/PROVISION/JOB_CONTROL/LOGINOUT/DCL + RTL) ----
if [ "$_force" = "1" ] || [ ! -x "$USERLAND/bin/STARTUP.EXE" ]; then
    echo "== Alpha userland not cached (or FORCE_BUILD) -- cross-building (static, tools ON) =="
    mkdir -p "$USERLAND"
    docker run --rm -v "$REPO":/src:ro -v "$USERLAND":/b "$IMG" bash -euo pipefail -c '
        cmake -S /src -B /b \
          -DCMAKE_TOOLCHAIN_FILE=/src/tools/cross-alpha/toolchain-alpha-linux.cmake \
          -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON -DOVMX_STATIC=ON >/b/configure.log 2>&1
        cmake --build /b -j"$(nproc)" >/b/build-all.log 2>&1
    '
fi

mkdir -p "$WORK"

echo "== assemble the Alpha boot image (disk master + initramfs + kernel bake) =="
docker run --rm --memory=8g --cpus="$(nproc)" \
  -v "$REPO":/repo:ro \
  -v "$HERE":/tools:ro \
  -v "$VMSKO_WORK":/vmsko \
  -v "$USERLAND":/userland:ro \
  -v "$WORK":/work "$IMG" bash -euo pipefail -c '
    KV="'"$KV"'"
    export ARCH=alpha CROSS_COMPILE=alpha-linux-gnu-
    export QEMU_LD_PREFIX=/usr/alpha-linux-gnu
    BIN=/userland/bin

    #########################################################################
    # 1. Stage the /vms system tree (Alpha images), mirroring
    #    distro/Dockerfile.bootable "System tree staging".
    #########################################################################
    echo "-- stage /vms system tree (Alpha) --"
    ST=/work/system-stage
    rm -rf "$ST"; mkdir -p \
        "$ST/vms/SYS0/SYSCOMMON/SYSEXE" \
        "$ST/vms/SYS0/SYSCOMMON/SYSLIB" \
        "$ST/vms/SYS0/SYSCOMMON/SYSMGR" \
        "$ST/vms/SYS0/SYSCOMMON/SYSHLP" \
        "$ST/vms/SYS0/SYSCOMMON/SYSUPD" \
        "$ST/vms/SYS0/SYSCOMMON/SYS\$STARTUP" \
        "$ST/vms/USERS" "$ST/vms/SYSTMP"

    SYSEXE="$ST/vms/SYS0/SYSCOMMON/SYSEXE"
    # Core boot + login chain images (all EM_ALPHA static EXEC -- the Alpha
    # cross-build produces no VMS-native LINK.EXE graph, see build log
    # "OVMX_LINK_NATIVE off ... alpha-linux-gnu is not aarch64/x86_64-musl").
    for e in PROVISION.EXE JOB_CONTROL.EXE LOGINOUT.EXE DCL.EXE HELP.EXE \
             AUTHORIZE.EXE MAIL.EXE MONITOR.EXE INITIALIZE.EXE INSTALL.EXE \
             SYSGEN.EXE SCSD.EXE PRODUCT.EXE LIBRARIAN.EXE ANALYZE.EXE \
             SYSMAN.EXE; do
        [ -f "$BIN/$e" ] && cp "$BIN/$e" "$SYSEXE/" || echo "   (no $e)"
    done
    # IMGACT.EXE (alpha) is the FIRST of the five mandatory first-hop images the
    # boot-image staging bridge reads off the ODS-2 volume over the ACP
    # (ovmx_init.c stage_boot_images(): "the PT_INTERP the kernel opens for each
    # execve").  Absent it, PID 1 halts %OVMX-F-SYSINIT boot-image staging failed
    # / SS$_NOSUCHFILE right after the mount.  IMGACT.EXE is NOT in $BIN --
    # OVMX_LINK_NATIVE is off for alpha-linux-gnu, so the userland cmake build
    # omits the VMS-native toolchain graph -- so build it here from the standalone
    # src/imgact Makefile (a SEPARATE src copy from the step-5 imgact-proof, so the
    # two do not collide) and stage it on the volume.  NOTE: no apostrophes in this
    # block -- it lives inside the assemble docker `bash -c '...'` single-quote.
    cp -a /repo/src /work/imgact-boot-src
    ( cd /work/imgact-boot-src/imgact && make ARCH=alpha CC=alpha-linux-gnu-gcc >/work/imgact-boot-build.log 2>&1 )
    [ -f /work/imgact-boot-src/imgact/IMGACT.EXE ] \
        || { echo "FAIL: IMGACT.EXE(alpha) did not build -- see /work/imgact-boot-build.log"; tail -20 /work/imgact-boot-build.log; exit 1; }
    cp /work/imgact-boot-src/imgact/IMGACT.EXE "$SYSEXE/IMGACT.EXE"
    cp "$BIN/STARTUP.EXE" "$SYSEXE/"   # STARTUP is also /init, but keep on disk
    # Config: SYSUAF/RIGHTSLIST + SYSMGR command files + SYS$STARTUP data
    cp /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT "$SYSEXE/"
    cp /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT "$SYSEXE/" 2>/dev/null || true
    cp /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR "$SYSEXE/" 2>/dev/null || true
    cp -r /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/* "$ST/vms/SYS0/SYSCOMMON/SYSMGR/"
    # ALPHA STATIC-BOOTSTRAP OVERLAY (vms-3f6): the Linux SYSTARTUP_VMS.COM
    # INSTALL ADDs 7 OVMX shareables (DECC$SHR etc.) that DO NOT EXIST on a
    # static Alpha volume -- the Alpha boot/login chain is static EM_ALPHA with
    # no VMS-native LINK.EXE shareable graph -- so every INSTALL ADD failed
    # %INSTALL-E-FILNOTFND (non-fatal under SET NOON, but 7 lines of misleading
    # noise). Replace it with the static-bootstrap variant that omits the
    # INSTALL block, mirroring the netbsd-vax Decision-A variant
    # (distro/rootfs-vax, vms-42d/vms-d9c).
    cp /repo/distro/rootfs-alpha/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM \
       "$ST/vms/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM"
    cp -r "/repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/." "$ST/vms/SYS0/SYSCOMMON/SYS\$STARTUP/" 2>/dev/null || true
    cp -r /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSHLP/. "$ST/vms/SYS0/SYSCOMMON/SYSHLP/" 2>/dev/null || true

    echo "   staged $(ls "$SYSEXE" | wc -l) files in SYS\$SYSTEM:"

    #########################################################################
    # 2. Master a GENUINE Files-11 ODS-2 system disk from that tree, using the
    #    Alpha vmsfs_master run under qemu-alpha (the ODS-2 on-disk format is
    #    fixed-width little-endian and endian-independent -- ods2_writer.c:43,
    #    ods2_reader.c:14-16 -- and the .EXE payloads are copied verbatim, so
    #    the Alpha binary under qemu produces byte-identical volume bytes to
    #    the host x86_64 tool).  This MUST match the x86_64 path
    #    (distro/Dockerfile.bootable: `vmsfs_master --ods2 master ... 128`):
    #    the atomic ACP flip (vms-5f0/vms-208) mounts DKA0: through the
    #    Files-11 ODS-2 executive ($MOUNT -> acp_validate_ods2, which requires
    #    a DECFILE11B home block + BITMAP.SYS header + SCB), so a bespoke-VMFS
    #    volume is rejected SS$_DEVNOTMOUNT and PID 1 halts "not an installed
    #    genuine system disk".  --ods2 + 128 MB is what makes it a real
    #    installed system disk the flip mounts.
    #########################################################################
    echo "-- master ovmx-distrib-alpha.img (genuine ODS-2, 128 MB) --"
    timeout 300 qemu-alpha "$BIN/vmsfs_master" --ods2 master \
        /work/ovmx-distrib-alpha.img OVMXSYS "$ST/vms" 128
    # Verify: the ODS-2 reader has no `extract`; presence-gate via `--ods2 list`
    # exactly as the x86_64 Dockerfile.bootable path does.
    echo "-- verify: --ods2 list carries the login chain --"
    timeout 300 qemu-alpha "$BIN/vmsfs_master" --ods2 list /work/ovmx-distrib-alpha.img > /work/distrib-list.txt
    head -40 /work/distrib-list.txt
    # IMGACT.EXE + JOB_CONTROL.EXE included: the boot-image staging bridge
    # (ovmx_init.c stage_boot_images) requires all five first-hop images on the
    # volume, so verify them here rather than discover a miss only at boot.
    for name in IMGACT.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE SYSUAF.DAT; do
        grep -qi "$name" /work/distrib-list.txt \
            || { echo "FAIL: mastered ODS-2 image missing SYS\$SYSTEM:$name"; exit 1; }
        echo "   OK: ovmx-distrib-alpha.img (ODS-2) carries SYS\$SYSTEM:$name"
    done
    rm -f /work/distrib-list.txt

    #########################################################################
    # 3. Bootstrap initramfs -- Dockerfile.bootable initramfs-slim for Alpha:
    #    /init = STARTUP.EXE, /lib/modules/{vms.ko,vmsfs.ko}, /sbin/
    #    vms_mount_helper, minimal SYSUAF + SYSMGR config.  The full system
    #    (DCL/LOGINOUT/IMGACT/SYSLIB) lives on the mounted disk, not here.
    #########################################################################
    echo "-- assemble bootstrap initramfs list --"
    IR=/work/initramfs
    rm -rf "$IR"; mkdir -p \
        "$IR/dev" "$IR/proc" "$IR/sys" "$IR/tmp" "$IR/var" "$IR/mnt" \
        "$IR/vms" "$IR/sbin" "$IR/etc" "$IR/lib/modules" \
        "$IR/vms/SYS0/SYSCOMMON/SYSEXE" "$IR/vms/SYS0/SYSCOMMON/SYSMGR"
    cp "$BIN/STARTUP.EXE"        "$IR/init"
    cp "$BIN/vms_mount_helper"   "$IR/sbin/vms_mount_helper"
    chmod 4755 "$IR/sbin/vms_mount_helper"
    cp /vmsko/vms.ko             "$IR/lib/modules/vms.ko"
    cp /vmsko/vmsfs.ko           "$IR/lib/modules/vmsfs.ko"
    cp /repo/distro/rootfs/etc/os-release "$IR/etc/os-release" 2>/dev/null || true
    cp /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT "$IR/vms/SYS0/SYSCOMMON/SYSEXE/"
    cp -r /repo/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/* "$IR/vms/SYS0/SYSCOMMON/SYSMGR/"

    # Build a gen_init_cpio spec so the kernel bakes it in (INITRAMFS_SOURCE).
    # (clipper -initrd is unreliable -- boot-vmsko-qemu-alpha.sh proves the
    # baked path; we reuse it.)
    {
      echo "dir /dev 755 0 0"
      echo "nod /dev/console 644 0 0 c 5 1"
      echo "nod /dev/null 666 0 0 c 1 3"
      echo "dir /proc 755 0 0"
      echo "dir /sys 755 0 0"
      echo "dir /tmp 1777 0 0"
      echo "dir /var 755 0 0"
      echo "dir /mnt 755 0 0"
      echo "dir /vms 755 0 0"
      echo "dir /sbin 755 0 0"
      echo "dir /etc 755 0 0"
      echo "dir /lib 755 0 0"
      echo "dir /lib/modules 755 0 0"
      echo "file /init /work/initramfs/init 755 0 0"
      echo "file /sbin/vms_mount_helper /work/initramfs/sbin/vms_mount_helper 4755 0 0"
      echo "file /lib/modules/vms.ko /work/initramfs/lib/modules/vms.ko 644 0 0"
      echo "file /lib/modules/vmsfs.ko /work/initramfs/lib/modules/vmsfs.ko 644 0 0"
      [ -f "$IR/etc/os-release" ] && echo "file /etc/os-release /work/initramfs/etc/os-release 644 0 0"
      # VMS config tree (walk the staged initramfs vms/ subtree)
      while IFS= read -r d; do
          rel="${d#/work/initramfs}"
          echo "dir $rel 755 0 0"
      done < <(find "$IR/vms" -type d | sort)
      while IFS= read -r f; do
          rel="${f#/work/initramfs}"
          echo "file $rel $f 644 0 0"
      done < <(find "$IR/vms" -type f | sort)
    } > /work/alpha-boot.list
    echo "   initramfs list: $(wc -l < /work/alpha-boot.list) entries"

    #########################################################################
    # 4. Bake the initramfs into vmlinux (INITRAMFS_SOURCE) + rebuild.
    #########################################################################
    echo "-- rebuild vmlinux with the boot initramfs baked in --"
    cd /vmsko/linux-$KV
    # OVMX kernel patches (rd vms-80d): idempotently ensure they are applied to
    # this (possibly stale-cached) tree before make -- build-vmsko-alpha.sh only
    # runs them when the modules are absent, so a pre-built cache could bake an
    # unpatched vmlinux-boot otherwise. `patch --dry-run --forward` no-ops an
    # already-patched tree.
    for p in /repo/tools/cross-alpha/patches/*.patch; do
        [ -e "$p" ] || continue
        if patch -p1 --dry-run --forward <"$p" >/dev/null 2>&1; then
            echo "   applying kernel patch: $(basename "$p")"
            patch -p1 --forward <"$p"
        fi
    done
    ./scripts/config --enable BLK_DEV_INITRD --set-str INITRAMFS_SOURCE /work/alpha-boot.list
    # Make sure virtio-blk-pci (-> /dev/vda) + serial console are in (they are
    # from build-vmsko-alpha.sh, re-assert idempotently).
    ./scripts/config \
        --enable VIRTIO --enable VIRTIO_PCI --enable VIRTIO_BLK \
        --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
        --enable DEVTMPFS --enable DEVTMPFS_MOUNT --enable TMPFS \
        --enable BINFMT_ELF
    make olddefconfig >/dev/null 2>&1
    rm -f usr/initramfs_data.cpio* usr/.initramfs_data.cpio* 2>/dev/null || true
    make -j"$(nproc)" vmlinux >/work/kbuild.log 2>&1
    alpha-linux-gnu-strip -o /work/vmlinux-boot vmlinux
    echo "   vmlinux-boot: $(ls -lh /work/vmlinux-boot | awk "{print \$5}")"

    #########################################################################
    # 5. IMGACT-under-booted-kernel proof staging (A2 EM_ALPHA activator).
    #    The Alpha login chain is static (no LINK.EXE graph yet), so IMGACT is
    #    not in the static boot chain -- we prove IMGACT activates a real
    #    VMS-native Alpha image under the BOOTED kernel separately, honestly
    #    labelled, using the A2 proof image.
    #########################################################################
    echo "-- build IMGACT.EXE(alpha) + a VMS-native Alpha image for the boot proof --"
    PROOF=/work/imgact-proof
    rm -rf "$PROOF"; mkdir -p "$PROOF"
    cp -a /repo/src /work/imgact-src
    ( cd /work/imgact-src/imgact && make ARCH=alpha CC=alpha-linux-gnu-gcc >/work/imgact-build.log 2>&1 )
    cp /work/imgact-src/imgact/IMGACT.EXE "$PROOF/IMGACT.EXE"
    CC=alpha-linux-gnu-gcc
    LIB="LIBTEST\$SHR.EXE"
    # VMS-native shareable (General Dynamic TLS -- Alpha has no TLSDESC)
    $CC -std=gnu11 -O2 -Wall -shared -fPIC -nostdlib \
        -Wl,--hash-style=sysv -Wl,-z,norelro -Wl,-soname,"$LIB" \
        -o "$PROOF/$LIB" /work/imgact-src/imgact/test/test_lib.c
    # VMS-native executable: PT_INTERP=/imgact-proof/IMGACT.EXE, DT_NEEDED=$LIB
    $CC -std=gnu11 -O2 -Wall -no-pie -nostdlib -ffreestanding -fno-stack-protector \
        -Wl,--dynamic-linker="$PROOF/IMGACT.EXE" -Wl,--hash-style=sysv \
        -Wl,-z,norelro -Wl,--allow-shlib-undefined -Wl,-e,_start \
        -o "$PROOF/test_prog_alpha" /work/imgact-src/imgact/test/test_prog.c \
        -L"$PROOF" -l:"$LIB"
    alpha-linux-gnu-readelf -hl "$PROOF/test_prog_alpha" | grep -E "Type:|interpreter" | sed "s/^/   /"
    echo "   imgact-proof staged."

    echo
    echo "== BUILD COMPLETE =="
    ls -la /work/vmlinux-boot /work/ovmx-distrib-alpha.img
  '
echo "== done; artifacts in $WORK =="
