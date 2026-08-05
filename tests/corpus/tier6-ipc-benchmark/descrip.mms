! MMS/MMK build file
! Use MMS/EXT if you build with MMS.

TARGET = pipe.exe, fpipe.exe, socketpair.exe, uds.exe, tcp.exe, udp.exe, shm.exe
CRTL_INIT = vms_crtl_init.obj, vms_crtl_values.obj

.ifdef MMSALPHA
CFLAGS = $(CFLAGS)/ARCH=HOST
.endif

CFLAGS = $(CFLAGS)/MAIN=POSIX_EXIT/DEFINE=(_USE_STD_STAT,_POSIX_EXIT,-
         __UNIX_PUTC,_XOPEN_SOURCE_EXTENDED)/FL=IEEE/IEEE=DENORM-
         /OPT=(LEV=5)

all : $(TARGET)

! Compile pipe
pipe.exe : pipe.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile fpipe
fpipe.exe : fpipe.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile socketpair
socketpair.exe : socketpair.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile unix domain socket
uds.exe : uds.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile tcp
tcp.exe : tcp.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile udp
udp.exe : udp.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Compile shared memory
shm.exe : shm.obj, $(CRTL_INIT)
    $(LINK) $(LINKFLAGS) $(MMS$SOURCE_LIST)

! Clean build artifacts
clean :
	del/log *.exe;*,*.obj;*
