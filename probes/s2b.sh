export PATH=/opt/cross-alpha-vms/bin:$PATH
cp -r /repo/src/libvms/include/* /sysroot/usr/include/
cd /b/build2
make -k all-target-libgcc -j"$(nproc)" CFLAGS_FOR_TARGET="-g0 -O2 -mpointer-size=64 -fno-function-sections -fno-data-sections" > /w/s2-libgcc.log 2>&1; echo LIBGCC_RC=$?
cd alpha-dec-vms/libgcc && ls -la unwind-dw2.o unwind-dw2-fde.o vms-gcc_shell_handler.o vms-ucrt0.o libgcc.a libgcc_eh.a crt0.o 2>&1
grep -E 'error:' /w/s2-libgcc.log | sort | uniq -c | sort -rn | head -20
