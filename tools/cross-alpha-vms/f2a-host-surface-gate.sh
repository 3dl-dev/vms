set -e
# F2a rung-3 (vms-e52): build the alpha-dec-vms port GCC on a MUSL host (Alpine =
# musl 1.2.5, = OVMX's host CRTL surface), then ACTUALLY RUN it to compile a
# minimal multi-TU C program -> real target EVAX ELF, and inspect the ELF.
# Anti-LARP: a real compiler RUN + inspected target ELF, on OOM-safe non-alpha
# hardware (the worker). build-toolchain.sh has no --host flag, so on Alpine
# host=build=x86_64-alpine-linux-musl automatically; GMP/MPFR/MPC come from
# alpine's musl-built -dev packages (the rung-2 host-provided-libs scenario).
docker run --rm -v "$PWD/tools/cross-alpha-vms:/srcro:ro" alpine:3.20 sh -c '
set -e
apk add -q build-base gmp-dev mpfr-dev mpc1-dev texinfo bison flex zlib-dev xz wget patch bash coreutils
cp -r /srcro /src && cd /src
echo "=== building the port GCC on a MUSL host (build-toolchain.sh) ==="
bash build-toolchain.sh 2>&1 | tail -25
echo "=== BUILD DONE — compile-test: REAL RUN of the musl-hosted port GCC ==="
export PATH=/opt/cross-alpha-vms/bin:$PATH
echo "--- proof the driver is a musl host binary (runs on worker Linux) ---"
file "$(command -v alpha-dec-vms-gcc)"
printf "extern int add(int,int);\nint main(void){return add(2,3);}\n" > /tmp/a.c
printf "int add(int a,int b){return a+b;}\n" > /tmp/b.c
echo "--- multi-TU compile -> target EVAX objects ---"
alpha-dec-vms-gcc -mpointer-size=64 -c /tmp/a.c -o /tmp/a.obj && echo "OK compiled a.obj"
alpha-dec-vms-gcc -mpointer-size=64 -c /tmp/b.c -o /tmp/b.obj && echo "OK compiled b.obj"
echo "=== VERIFY target ELF is real EVAX (EM_ALPHA) ==="
alpha-dec-vms-objdump -f /tmp/a.obj 2>/dev/null | head -6 || true
ls -l /tmp/a.obj /tmp/b.obj
echo "=== DONE rung-3 gate ==="
'
