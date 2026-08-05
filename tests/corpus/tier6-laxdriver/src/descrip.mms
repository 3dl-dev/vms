! Simple build script for MMK or MMS.

.ifdef DEBUG
debugopts = /DEBUG/NOOPTIMIZE/DEFINE=DEBUG
.endif

warnopts = /WARN=(ENABLE=(defunct,obsolescent,questcode,unusedtop),-
	    DISABLE=(boolexprconst,unreachcode))

all : laxdriver.exe test-lax-driver.exe test-lav-driver.exe
	@ write sys$output "Build complete."	! so MMS doesn't complain

clean :
    DEL *.exe;*,*.obj;*,*.lis;*,*.stb;*,*.map;*,*.dsf;*

laxdriver.obj : laxdriver.c
    CC/FLOAT=IEEE/EXTERN=STRICT/POINTER_SIZE=32-
        $(debugopts)$(warnopts)-
        /LIS=LAXDRIVER/MACHINE_CODE-
        /OBJ=LAXDRIVER LAXDRIVER -
	+SYS$LIBRARY:SYS$LIB_C.TLB/LIBRARY

laxdriver.exe : laxdriver.obj laxdriver.opt
    LINK/USERLIB=PROC/NATIVE_ONLY/BPAGE=14/SECTION/REPLACE-
        /NODEMAND_ZERO/NOTRACEBACK/SYSEXE/NOSYSSHR-
        /SHARE=LAXDRIVER.EXE-		! Driver image
        /DSF=LAXDRIVER.DSF-		! Debug symbol file
        /SYMBOL=LAXDRIVER.STB-		! Symbol table
        /MAP=LAXDRIVER.MAP/FULL/CROSS -	! Map listing
	LAXDRIVER.OPT/OPTIONS

! Compile with IEEE floating-point.
test-lax-driver.obj : test-lax-driver.c
    CC/LIS/FLOAT=IEEE$(debugopts)$(warnops) test-lax-driver.c

test-lax-driver.exe : test-lax-driver.obj
    LINK test-lax-driver

! Compile with VAX floating-point.
test-lav-driver.obj : test-lax-driver.c
    CC/LIS/FLOAT=G_FLOAT$(debugopts)$(warnops)/OBJ=test-lav-driver.obj-
	/LIS=test-lav-driver test-lax-driver.c

test-lav-driver.exe : test-lav-driver.obj
    LINK test-lav-driver
