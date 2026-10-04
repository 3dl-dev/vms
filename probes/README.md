# vms-4d0 rail probes (2026-10-04) -- exploration scripts, not product code

Ran on the k3s rail (tools/k3s/run-on-rail.sh --keep --dind) inside the
alpha-dec-vms toolchain images. Order and what each established:

1. gxx-phase1.sh  -- add C++ (cc1plus, alpha-dec-vms-g++) to the cached toolchain
   (pushed: 192.168.2.43:30500/ovmx-cross-alpha-vms-decc:vms4d0-gxx).
2. ovmx-ld        -- collect2 `ld` -> OVMX LINK.EXE (+DECC$SHR/LIBOTS/LIBVMSRMS,
   port crt0), STRICT; truthful autoconf link tests. Use via gcc -B<dir>/.
3. lsc-phase2.sh + lsc4.sh -- libstdc++-v3 for alpha-dec-vms over the OVMX CRTL
   headers (needs vms-537 32-bit size_t, no function-sections, -fpermissive
   on src/c++11/debug.cc).
4. stage2.sh + s2b.sh + s2c.sh -- stage-2 toolchain with --with-sysroot (musl
   headers + src/libvms/include STARLET surface): libgcc with the EH unwinder
   and the port's crt0/crtbegin/crtend; archive with host `ar rcS`.
5. cxxlink4.sh + cxx1.cc -- C++ link with LINK.EXE --library (PR #1362):
   stops at %LINK-F-UNDEF SYS$GL_CALL_HANDL (rd vms-bfd03).
