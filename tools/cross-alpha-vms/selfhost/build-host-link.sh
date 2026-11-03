#!/bin/bash
# build-host-link.sh -- OVMX's LINK.EXE (src/vmslink/link.c, with the EVAX object
# front end) built FOR the alpha-dec-vms host: the stage-2 alpha-dec-vms compiler
# compiles the unmodified linker as a 32-bit DEC C program over the build's C RTL,
# so it is an OpenVMS Alpha image that runs on OVMX/Alpha (vms-e40). Sibling of
# build-host-gcc.sh / build-host-as.sh; runs INSIDE the ovmx-cross-alpha-vms image:
#
#   docker run --network none -v <repo>:/src:ro -v <cxxtc>:/out:ro -v <joint>:/joint:ro \
#       -v <work>:/w ovmx-cross-alpha-vms bash /src/tools/cross-alpha-vms/selfhost/build-host-link.sh
#
# Output: /w/host-link/link.exe; progress and any failure go to stdout.
set -euo pipefail
X=/out/cxx
export PATH=$X/bin:/opt/cross-alpha-vms/bin:$PATH
export OVMX_ALPHA_SYSROOT=/joint
O=/w/host-link; mkdir -p $O
trap 'echo "build-host-link: FAIL at line $LINENO: $BASH_COMMAND"' ERR
cd $O
# the PT_INTERP every image carries: where PID 1 stages IMGACT.EXE at run time, the same
# value build-joint-image.sh bakes into the images the cross LINK.EXE writes (vms-430)
IMGACT_INTERP_PATH="${IMGACT_INTERP_PATH:-/run/ovmx-boot/IMGACT.EXE}"
echo "== link.c (host alpha-dec-vms, DEC C default 32-bit pointers)"
# rd vms-3cb1: the shipped SYS$SYSTEM:LINK.EXE takes the VMS LINK command,
# parsed by CLI$DCL_PARSE against its LINK table (OVMX_LINK_IMAGE); the CLI$
# routines are compiled in, at this image's pointer size (OVMX_LINK_CLI_ARGV:
# the command arrives as the C RTL's main() words).
alpha-dec-vms-gcc -std=gnu11 -O1 -mpointer-size=no -DIMGACT_INTERP_PATH=$IMGACT_INTERP_PATH \
    -DOVMX_LINK_IMAGE -DOVMX_LINK_CLI_ARGV \
    -I/src/src/vmslink/include -I/src/src/libvms/include/hostabi -iquote /src/src/libvms/include/hostabi \
    -I/src/src/libvms/include \
    -o $O/link.exe /src/src/vmslink/link.c /src/src/libvms/rtl/lib_cli.c > build.log 2>&1 || { grep -nE 'error|LINK-[FE]' build.log | head -40; tail -20 build.log; exit 1; }
ls -la $O/link.exe
echo "HOST-LINK: OK"
