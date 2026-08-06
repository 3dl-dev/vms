# lib_build_graph.sh — shared VMS-native producer-graph build steps, factored
# out of run_dcl_native.sh for vms-c39 so that a second executable consumer
# (LOGINOUT.EXE, run_login_native.sh) does not carry a second, independently
# maintained copy of "how do you build IMGACT.EXE, LINK.EXE and the six-
# library shareable graph in the arm64 musl container" — the exact kind of
# drift CLAUDE.md's libvms LIST comment (mk_libvms_shr.sh) warns is otherwise
# unenforced and silent until a downstream link job fails.
#
# NOT a standalone script: `. lib_build_graph.sh` then call build_producer_graph.
# Callers set CC/WORK/SYSEXE/SYSLIB and the *_DIR path variables (see either
# run_dcl_native.sh or run_login_native.sh for the expected set) before
# sourcing this file, and may set SYS_VEC_EXTRA to append extra PROCEDURE
# universals to LIBVMSSYS$SHR's symbol vector beyond the DCL-only set below
# (vms-c39 appends vms_kif_setident for LOGINOUT's identity-establishment
# path; DCL never calls it, so run_dcl_native.sh leaves SYS_VEC_EXTRA unset).
#
# Requires (set by caller before sourcing/calling):
#   CC, WORK, IMGACT_DIR, LINK_DIR, LIBVMSSYS_DIR, VMSPROC_DIR, VMSLNM_DIR,
#   VMSFS_DIR, LIBVMS_DIR, VMSRMS_DIR, LIBVMS_INC, LNM_INC, VMSFS_INC,
#   RMS_INC, SYSEXE, SYSLIB, LIBC, LIBGCC
# Optional: ARCH (default aarch64; also supports x86_64, vms-cb5f — the
#   x86_64 analog of this proof reuses this same graph builder, it is NOT a
#   forked copy). ARCH selects the libvmssys arch/<ARCH>/syscall.S and the
#   one target-specific codegen flag each producer's CFLAGS needs:
#   -mno-outline-atomics on aarch64 (no __aarch64_* outline-atomic helper
#   calls DECC$SHR lacks) vs -mtls-dialect=gnu2 on x86_64 (TLSDESC codegen,
#   the standing precedent from docs/design-link-x86_64-relocs.md /
#   run_test_x86_64.sh -- x86_64's default TLS dialect is the GD model, not
#   TLSDESC). The mk_*_shr.sh recipes read this through CFLAGS (env-
#   overridable, vms-cb5f); IMGACT.EXE's own Makefile reads ARCH directly.

build_producer_graph() {
    ARCH=${ARCH:-aarch64}
    case "$ARCH" in
        aarch64) ARCHFLAG="-mno-outline-atomics" ;;
        x86_64)  ARCHFLAG="-mtls-dialect=gnu2" ;;
        *) echo "build_producer_graph: unsupported ARCH=$ARCH (expected aarch64 or x86_64)"; exit 1 ;;
    esac
    LIBCFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector $ARCHFLAG"

    echo "== build IMGACT.EXE + LINK.EXE ($ARCH) =="
    ( cd "$IMGACT_DIR" && make CC="$CC" ARCH="$ARCH" clean >/dev/null 2>&1 || true; make CC="$CC" ARCH="$ARCH" ) >/dev/null 2>&1
    cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
    $CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

    echo "== DECC\$SHR.EXE (whole-archive musl) =="
    sh "$LINK_DIR/mk_decc_shr.sh" "$WORK/LINK.EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
    readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: DECC\$SHR no symbol vector"; exit 1; }

    echo "== LIBVMSSYS\$SHR.EXE =="
    SYSCFLAGS="$LIBCFLAGS -I$LIBVMSSYS_DIR"
    SYSOBJS=""
    for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif; do
        $CC $SYSCFLAGS -c -o "$WORK/sys_$c.o" "$LIBVMSSYS_DIR/$c.c"
        SYSOBJS="$SYSOBJS $WORK/sys_$c.o"
    done
    $CC -fPIC -c -o "$WORK/sys_syscall.o" "$LIBVMSSYS_DIR/arch/$ARCH/syscall.S"
    SYSOBJS="$SYSOBJS $WORK/sys_syscall.o"
    SYS_VEC="vms_strlen=PROCEDURE,vms_kif_open=PROCEDURE,vms_kif_enq=PROCEDURE,vms_kif_deq=PROCEDURE,vms_kif_convert=PROCEDURE,vms_kif_assign=PROCEDURE,vms_kif_dassgn=PROCEDURE,vms_kif_getdvi_chan=PROCEDURE,vms_kif_setprn=PROCEDURE,vms_kif_getjpi_self=PROCEDURE,vms_kif_getjpi_pid=PROCEDURE,vms_kif_getjpi_prcnam=PROCEDURE,vms_kif_procscan=PROCEDURE,vms_kif_setef=PROCEDURE,vms_kif_clref=PROCEDURE,vms_kif_readef=PROCEDURE,vms_kif_waitfr=PROCEDURE,vms_kif_wflor=PROCEDURE,vms_kif_wfland=PROCEDURE,vms_kif_ascefc=PROCEDURE,vms_kif_dacefc=PROCEDURE,vms_kif_dlcefc=PROCEDURE,vms_kif_devscan=PROCEDURE,vms_kif_getdvi_devnam=PROCEDURE"
    if [ -n "${SYS_VEC_EXTRA:-}" ]; then
        SYS_VEC="$SYS_VEC,$SYS_VEC_EXTRA"
    fi
    "$WORK/LINK.EXE" --shareable \
        --symbol-vector "$SYS_VEC" \
        --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $SYSOBJS

    echo "== LIBVMSPROCESS\$SHR.EXE =="
    CC="$CC" CFLAGS="$LIBCFLAGS" sh "$LINK_DIR/mk_vmsprocess_shr.sh" \
        "$WORK/LINK.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "" "$VMSPROC_DIR" "$LIBVMS_INC"

    echo "== LIBVMSLNM\$SHR.EXE =="
    CC="$CC" CFLAGS="$LIBCFLAGS" sh "$LINK_DIR/mk_vmslnm_shr.sh" \
        "$WORK/LINK.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "$VMSLNM_DIR" "$LIBVMS_INC"

    echo "== LIBVMSFS\$SHR.EXE =="
    CC="$CC" CFLAGS="$LIBCFLAGS" sh "$LINK_DIR/mk_vmsfs_shr.sh" \
        "$WORK/LINK.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" \
        "$VMSFS_DIR" "$LIBVMS_INC" "$LNM_INC"

    echo "== LIBVMS\$SHR.EXE =="
    CC="$CC" CFLAGS="$LIBCFLAGS" sh "$LINK_DIR/mk_libvms_shr.sh" \
        "$WORK/LINK.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
        "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
        "$LIBVMS_DIR"

    echo "== LIBVMSRMS\$SHR.EXE =="
    CC="$CC" CFLAGS="$LIBCFLAGS" sh "$LINK_DIR/mk_vmsrms_shr.sh" \
        "$WORK/LINK.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
        "$VMSRMS_DIR" "$LIBVMS_INC" "$VMSFS_INC"
    echo "-- full six-library producer graph linked VMS-native ($ARCH) --"
}
