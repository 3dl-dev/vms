#!/bin/bash
# build-host-link.sh -- OVMX's own LINK.EXE (src/vmslink/link.c) built FOR the
# alpha-dec-vms host: a 32-bit DEC C program over OVMX's C RTL, linked by the
# bootstrap LINK.EXE through the stage-2 toolchain's `ld` (ovmx-ld). It is the
# linker the GCC driver's collect2 runs after cc1 and as (vms-e40): the same C
# source the x86_64/aarch64 LINK.EXE images are built from, unmodified. Runs
# INSIDE the ovmx-cross-alpha-vms toolchain image:
#
#   docker run --network none -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-link.sh
#
# Output: /w/host-link/out/link.exe; progress and any failure go to stdout.
set -euo pipefail
T=alpha-dec-vms
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
O=/w/host-link; mkdir -p $O/out
cd $O
trap 'echo "build-host-link: FAIL at line $LINENO: $BASH_COMMAND"' ERR
echo "== LINK.EXE (host=$T)"
# link.c is one translation unit (it includes the EVAX reader); the default pointer
# size is the DEC C one (32-bit), as for the host GCC and gas.
$T-gcc -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I/src/src/vmslink/include -I/src/src/vmslink \
    -o $O/out/link.exe /src/src/vmslink/link.c > build.log 2>&1 \
    || { grep -nE ' error:|LINK-[FE]|undefined' build.log | head -40; tail -20 build.log; exit 1; }
ls -la $O/out/link.exe
echo "HOST-LINK: OK"
