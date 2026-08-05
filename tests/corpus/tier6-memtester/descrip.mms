!
! 64-bit memtester for OpenVMS
!

.IFDEF MMSALPHA
ARCH_FLAGS=/ARCH=HOST/FLOAT=IEEE/IEEE=DENORM
.ENDIF

CFLAGS=$(ARCH_FLAGS)/OPT=(LEV=5)/POI=LONG/MAIN=POSIX_EXIT/DEFINE=(_POSIX_EXIT, __UNIX_PUTC)

memtester.exe :     memtester.obj, output.obj, tests.obj
         link $(MMS$SOURCE_LIST)

memtester.obj :     memtester.c, memtester.h, sizes.h, tests.h, types.h, output.h

output.obj :        output.c, output.h

tests.obj :         tests.c, memtester.h, sizes.h, tests.h, types.h, output.h

clean :
         delete/nolog memtester.exe;*, *.obj;*
