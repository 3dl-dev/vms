export PATH=/opt/cross-alpha-vms/bin:$PATH AR=ar AR_FLAGS=crS RANLIB=true
cd /b/lsc
make -k clean >/dev/null 2>&1
grep -rl -- "-ffunction-sections -fdata-sections" --include=Makefile . | xargs sed -i "s/-ffunction-sections -fdata-sections//g"
make -C include SECTION_FLAGS= > /w/lsc4.log 2>&1 || true
make -C src/c++11 debug.lo SECTION_FLAGS= CXXFLAGS="-g -O2 -fpermissive" >> /w/lsc4.log 2>&1 || true
make -k -j"$(nproc)" SECTION_FLAGS= >> /w/lsc4.log 2>&1; echo MAKE_RC=$?
grep -c -- '-ffunction-sections' /w/lsc4.log
grep -E 'error:|Internal error' /w/lsc4.log | sort | uniq -c | sort -rn | head
ls -la src/.libs/*.a libsupc++/.libs/*.a
