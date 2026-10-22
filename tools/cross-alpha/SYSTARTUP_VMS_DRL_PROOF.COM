$! SYSTARTUP_VMS.COM - VMS-hosted xgcc compile + link proof variant (vms-65a;
$! harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the DRVLNK proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:XGCC.EXE is the GCC 14.2 driver for alpha-dec-vms; CC1.EXE, AS.EXE,
$! LD.EXE (the port's wrapper around the VMS LINK command) and LINK.EXE sit beside it,
$! all built --host=alpha-dec-vms over OVMX's C RTL. DCL writes the C source;
$! SYS$SYSTEM:JOINT_E2E runs `xgcc ... -o HELLO.EXE` (vfork + execv, wait); the driver
$! runs cc1, as and ld, and ld runs DCL LINK. DCL then DUMPs and RUNs HELLO.EXE.
$!
$ SET NOON
$ SET DEFAULT VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "DRL-PROOF: === DCL writes the C source ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "static int sq (int x) { return x * x; }"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""hello from cc1 on OVMX %d\n"", sq (7));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]HELLO2.C
$ WRITE HC "#include <stdio.h>"
$ WRITE HC "#include <stdlib.h>"
$ WRITE HC "#include <string.h>"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  char *p = malloc (16);"
$ WRITE HC "  strcpy (p, ""headers"");"
$ WRITE HC "  printf (""hello2 %s %d\n"", p, (int) strlen (p));"
$ WRITE HC "  free (p);"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$! the system headers and the compiler's own, where the driver looks by default
$! (/usr/include -> USR:[INCLUDE], /gnu/lib/gcc/... -> GNU:[LIB.GCC...]): concealed rooted logicals
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED USR SYS$COMMON:[USR.]
$ DEFINE/SYSTEM/TRANSLATION_ATTRIBUTES=CONCEALED GNU SYS$COMMON:[GNU.]
$ SHOW LOGICAL USR
$ SHOW LOGICAL GNU
$ DIRECTORY USR:[INCLUDE]STDIO.H
$ DIRECTORY USR:[INCLUDE.SYS]TYPES.H
$ DIRECTORY GNU:[LIB.GCC.ALPHA-DEC-VMS.14_2_0.INCLUDE]STDDEF.H
$ WRITE SYS$OUTPUT "DRL-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === MILESTONE: RUN JOINT_E2E (vfork+execv XGCC.EXE -o HELLO.EXE) ==="
$ RUN SYS$SYSTEM:JOINT_E2E
$ WRITE SYS$OUTPUT "DRL-PROOF: RUN-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === DIRECTORY (what the driver left behind) ==="
$ DIRECTORY/SIZE VDA0:[SYSTMP]
$ WRITE SYS$OUTPUT "DRL-PROOF: === the options file ld wrote ==="
$ TYPE VDA0:[SYSTMP]HELLO.OPT_TMPFILE
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.EXE ==="
$ DUMP VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE LINKED PROGRAM: RUN HELLO.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: HELLO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP HELLO2.EXE ==="
$ DUMP VDA0:[SYSTMP]HELLO2.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE HEADER PROGRAM: RUN HELLO2.EXE ==="
$ RUN VDA0:[SYSTMP]HELLO2.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: HELLO2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$! A native BUILD: the compiler and the archiver as DCL foreign commands, the way a
$! command procedure (or MMK) runs them -- two sources compiled, an object library made
$! with the port's ar wrapper (which runs LIBRARY), a program linked against it.
$ WRITE SYS$OUTPUT "DRL-PROOF: === native build: GCC and AR as foreign commands ==="
$ GCC := $SYS$SYSTEM:XGCC.EXE
$ AR := $SYS$SYSTEM:AR.EXE
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOA.C
$ WRITE HC "int two_a (int x) { return x + 3; }"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOB.C
$ WRITE HC "int two_b (int x) { return x * 4; }"
$ CLOSE HC
$ OPEN/WRITE HC VDA0:[SYSTMP]TWOMAIN.C
$ WRITE HC "int printf (const char *, ...);"
$ WRITE HC "int two_a (int);"
$ WRITE HC "int two_b (int);"
$ WRITE HC "int main (void)"
$ WRITE HC "{"
$ WRITE HC "  printf (""two %d %d\n"", two_a (4), two_b (4));"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ GCC -BSYS$SYSTEM: -nostdinc -x c -c VDA0:[SYSTMP]TWOA.C -o VDA0:[SYSTMP]TWOA.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-CC1-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ GCC -BSYS$SYSTEM: -nostdinc -x c -c VDA0:[SYSTMP]TWOB.C -o VDA0:[SYSTMP]TWOB.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-CC2-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$! the port's ar (config/vms/vms-ar.c) takes key letters ("rc", the AR_FLAGS a Makefile passes) or separate -r -c; it rejects a joined "-rc"
$ AR rc VDA0:[SYSTMP]LIBTWO.A VDA0:[SYSTMP]TWOA.O VDA0:[SYSTMP]TWOB.O
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-AR-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ LIBRARY/LIST VDA0:[SYSTMP]LIBTWO.A
$ GCC -BSYS$SYSTEM: -L/SYS$SYSTEM -nostdinc -x c VDA0:[SYSTMP]TWOMAIN.C -o VDA0:[SYSTMP]TWO.EXE -x none VDA0:[SYSTMP]LIBTWO.A
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-LINK-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP TWO.EXE ==="
$ DUMP VDA0:[SYSTMP]TWO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP3-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE BUILT PROGRAM: RUN TWO.EXE ==="
$ RUN VDA0:[SYSTMP]TWO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: TWO-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$!
$! vms-5b3: C++. GCC 14 is written in C++, so cc1 built on OVMX needs cc1plus and libstdc++
$! here. The g++ driver (xg++, GXX.EXE: ODS-2 names have no '+') runs CC1PLUS.EXE, as and
$! ld; LINK searches SYS$SYSTEM:LIBSTDCXX.A, the 32-bit libstdc++. The C++ headers are on
$! ODS-2 names under GNU:[CXX]: -remap has the preprocessor read each directory's
$! HEADER.GCC, which pairs a name the source includes (bits/c++config.h) with the name the
$! file has (CXXCONFIG.H) -- GCC's own mechanism for file systems that restrict names.
$! -nostdlib++ and the library named last: the -lstdc++ the driver adds would look for
$! libstdc++.a, a name ODS-2 cannot hold.
$ WRITE SYS$OUTPUT "DRL-PROOF: === C++: the native g++ compiles a C++ program ==="
$ OPEN/WRITE HC VDA0:[SYSTMP]CXXHELLO.CC
$ WRITE HC "#include <cstdio>"
$ WRITE HC "#include <string>"
$ WRITE HC "#include <vector>"
$ WRITE HC "#include <stdexcept>"
$ WRITE HC "struct B { virtual int f () const { return 1; } virtual ~B () {} };"
$ WRITE HC "struct D : B { int f () const { return 7; } };"
$ WRITE HC "static int ctor_ran = 0;"
$ WRITE HC "struct Init { Init () { ctor_ran = 42; } };"
$ WRITE HC "static Init g_init;"
$ WRITE HC "__attribute__ ((noinline)) static int thrower (int x)"
$ WRITE HC "{"
$ WRITE HC "  if (x > 0)"
$ WRITE HC "    throw std::runtime_error (""ovmx"");"
$ WRITE HC "  return x;"
$ WRITE HC "}"
$ WRITE HC "int main (int argc, char **argv)"
$ WRITE HC "{"
$ WRITE HC "  B *b = new D;"
$ WRITE HC "  std::vector<std::string> v;"
$ WRITE HC "  v.push_back (""OVMX"");"
$ WRITE HC "  v.push_back (std::string (""C++"") + ""/libstdc++"");"
$ WRITE HC "  int caught = 0;"
$ WRITE HC "  try { thrower (argc); }"
$ WRITE HC "  catch (const std::runtime_error e) { caught = std::string (e.what ()) == ""ovmx""; }"
$ WRITE HC "  std::printf (""cxx ctor=%d virt=%d vec=%s,%s caught=%d\n"", ctor_ran, b->f (), v[0].c_str (), v[1].c_str (), caught);"
$ WRITE HC "  delete b;"
$ WRITE HC "  return 0;"
$ WRITE HC "}"
$ CLOSE HC
$ DIRECTORY GNU:[CXX]STRING.,GNU:[CXX.ALPHA-DEC-VMS.BITS]CXXCONFIG.H,GNU:[CXX.ALPHA-DEC-VMS.BITS]HEADER.GCC
$ GXX := $SYS$SYSTEM:GXX.EXE
$ GXX -BSYS$SYSTEM: -L/SYS$SYSTEM -remap -nostdinc++ -isystem /GNU/CXX -isystem /GNU/CXX/ALPHA-DEC-VMS -nostdlib++ -x c++ VDA0:[SYSTMP]CXXHELLO.CC -o VDA0:[SYSTMP]CXXHELLO.EXE -x none SYS$SYSTEM:LIBSTDCXX.A
$ WRITE SYS$OUTPUT "DRL-PROOF: BUILD-CXX-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === INDEPENDENT READER: DCL DUMP CXXHELLO.EXE ==="
$ DUMP VDA0:[SYSTMP]CXXHELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: DUMP4-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === RUN THE C++ PROGRAM: RUN CXXHELLO.EXE ==="
$ RUN VDA0:[SYSTMP]CXXHELLO.EXE
$ WRITE SYS$OUTPUT "DRL-PROOF: CXX-STATUS=''$STATUS' SEVERITY=''$SEVERITY'"
$ WRITE SYS$OUTPUT "DRL-PROOF: === END ==="
$!
$ EXIT
