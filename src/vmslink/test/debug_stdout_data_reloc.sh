#!/bin/sh
# debug_stdout_data_reloc.sh — root-cause reproducer for vms-608 (epic vms-da0
# F2b): a minimal C++ OVMX image (CPPTEST.EXE) SIGSEGVs in its global ctor's
# `std::fputs(msg, stdout)` because `stdout` resolves to a garbage FILE*.
#
# ============================ FINDING (PROVEN) ============================
# The bug is NOT in the cross-image =DATA binding chain, which is CORRECT
# end-to-end (verified at runtime under gdb):
#   * DECC$SHR `stdout` storage (a `FILE *const stdout = &__stdout_FILE`)
#     carries an ABS64 pointer initializer that IS applied AND registered in
#     .vms$rel; IMGACT (apply_vms_rel) load-biases it -> *stdout == the real
#     &__stdout_FILE.  [refutes sub-cause (a)]
#   * CPPTEST's .igot import cell for stdout (sv_index 166) is filled by IMGACT
#     with the runtime &stdout; *(*cell) == the correct FILE*.  [refutes (c)]
#
# The bug is a LINK.EXE relocation-CLASS gap [variant of sub-cause (b)]:
# cpptest.o (compiled -fPIE, gcc's default) references `stdout` via a DIRECT
# data relocation:
#     e3: R_X86_64_PC32   stdout-0x4     ; insn: 48 8b 0d 00000000
#                                        ;   mov stdout(%rip), %rcx   (1 load)
# i.e. the position-dependent / copy-relocation access model: a SINGLE load
# straight from stdout's storage yielding the FILE*.  LINK.EXE's import
# collection only turns is_call (PLT32/CALL26) or is_gotr (GOT) relocations
# into cross-image imports; an R_X86_64_PC32 against a cross-image =DATA symbol
# is NEITHER, so no import is created for this site and reloc-apply falls to
# resolve_ref()==0 (UND, lives in DECC$SHR) -> `if (target==0) continue;` ->
# the disp32 is left 0.  The instruction then reads its OWN downstream code
# bytes (0x3d8d4800000001be) as the FILE* and derefs FILE->lock at +0x8c.
#
# The reference control (identical cpptest.o under ld+ld.so) passes because ld
# implements a COPY RELOCATION for a PC32-to-shared-data reference (allocates a
# local copy in the image, ld.so fills it at load); OVMX has no such facility.
#
# TWO GENUINE FIXES (conductor's =DATA-path call, anti-cheat dimension):
#   1) Compile cpptest.o -fPIC -> gcc emits R_X86_64_REX_GOTPCRELX, which
#      LINK.EXE already binds correctly (double-deref through the .igot cell).
#      Matches how the ENTIRE OVMX producer graph is already built (LIBCFLAGS
#      has -fPIC); cpptest is the lone -fPIC-less object.  Unblocks C++
#      first-light now, but leaves the LINK.EXE copy-reloc gap unaddressed for
#      any future -fPIE/default object (e.g. cc1/cc1plus).
#   2) Implement a copy-relocation equivalent in LINK.EXE + IMGACT: for a
#      PC32/direct data reference to a cross-image =DATA import, allocate an
#      image-local slot, patch the PC32 to it, and have IMGACT copy the
#      producer's data (the biased FILE*) into it at activation.  Matches ld
#      exactly; handles the identical .o; larger + touches the executive.
#
# ================================ USAGE ==================================
# Run in an x86_64 musl Alpine container (CLAUDE.md test loop):
#   docker run --rm --platform linux/amd64 --cap-add=SYS_PTRACE \
#     --security-opt seccomp=unconfined -v $PWD:/src -w /src \
#     docker.io/library/alpine:3.20 sh /src/src/vmslink/test/debug_stdout_data_reloc.sh
set -e
apk add --no-cache g++ binutils >/dev/null 2>&1 || true
SRC=${CPPTEST_SRC:-/src/third-party/gcc/cpptest.cpp}
echo "== stdout access relocation by compile model (the root cause) =="
for flag in "" "-fPIC" "-fPIE"; do
    g++ -std=c++17 -O2 $flag -c -o /tmp/x.o "$SRC"
    printf "  %-7s -> " "${flag:-default}"
    objdump -dr /tmp/x.o | grep -m1 "stdout" || echo "(no stdout reloc)"
done
echo
echo "default/-fPIE => R_X86_64_PC32 (direct, copy-reloc model) : LINK.EXE gap"
echo "-fPIC        => R_X86_64_REX_GOTPCRELX (GOT-indirect)     : LINK.EXE handles"
