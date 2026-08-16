# CMake toolchain file for cross-compiling OVMX to alpha-linux-gnu (glibc).
#
# Used INSIDE the ovmx-cross-alpha container (tools/cross-alpha/Dockerfile),
# which installs Debian's gcc-alpha-linux-gnu + libc6-dev-alpha-cross (a glibc
# Alpha sysroot under /usr/alpha-linux-gnu). Unlike the netbsd-vax toolchain
# (a link-libc NetBSD substrate), this is the RAW-FREESTANDING Linux substrate
# on a 64-bit LP64 arch: src/libvmssys/CMakeLists.txt takes its VMSSYS_ARCH=
# alpha branch (hand-written crt0/syscall/softdiv/softmath, no glibc), while the
# higher RTL libraries (vmsprocess/libvms/...) are ordinary glibc-hosted objects
# that link libvmssys for the /dev/vms kif transport.
#
# Rung A3 of the OVMX-on-Alpha epic (rd vms-fed): prove the full RTL stack
# cross-builds LP64-clean and its unit tests pass under qemu-alpha.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR alpha)

set(CROSS_TARGET "alpha-linux-gnu")
set(CMAKE_C_COMPILER   "${CROSS_TARGET}-gcc")
set(CMAKE_CXX_COMPILER "${CROSS_TARGET}-g++")
set(CMAKE_ASM_COMPILER "${CROSS_TARGET}-gcc")
set(CMAKE_AR           "${CROSS_TARGET}-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${CROSS_TARGET}-ranlib" CACHE FILEPATH "")

# The Debian cross package keeps the Alpha glibc sysroot here; point qemu-user
# at the same prefix (QEMU_LD_PREFIX) when running the built binaries.
set(CMAKE_SYSROOT "/usr/alpha-linux-gnu")
set(CMAKE_FIND_ROOT_PATH "/usr/alpha-linux-gnu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Compiler works; cannot execute Alpha binaries directly on the amd64 host
# (they run under qemu-alpha, which CMake's try-run cannot use here).
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CROSSCOMPILING 1)
