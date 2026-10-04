export PATH=/opt/cross-alpha-vms/bin:$PATH
cd /tmp
/w/sysroot/LINK2.EXE --transfer __main --use '/w/sysroot/DECC$SHR.EXE' --use /w/sysroot/LIBOTS_SHR.EXE -o /w/cxx1.exe /w/sysroot/crt0.obj /w/cxx1.obj --library /w/libstdc++.a --library /b/build2/alpha-dec-vms/libgcc/libgcc.a > /w/cxxlink4.log 2>&1; echo LINK_RC=$?
grep -E 'LINK-I-LIBRARY|LINK-F|LINK-S|LINK-W' /w/cxxlink4.log | head -20
