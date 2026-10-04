export PATH=/opt/cross-alpha-vms/bin:$PATH
cd /b/build2/alpha-dec-vms/libgcc && rm -f libgcc.a libgcc_eh.a
make -k AR=ar AR_FLAGS=rcS RANLIB=true CFLAGS="-g0 -O2 -mpointer-size=64 -fno-function-sections -fno-data-sections" > /w/s2c.log 2>&1; echo RC=$?
ls -la libgcc.a libgcc_eh.a crt0.o 2>&1
ar t libgcc.a | grep -c . ; ar t libgcc.a | grep -E 'unwind|shell|emutls' ; ar t libgcc_eh.a 2>/dev/null | head
grep -E 'error|Error' /w/s2c.log | head
