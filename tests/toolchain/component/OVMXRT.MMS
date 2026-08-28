! =============================================================================
! OVMXRT.MMS  --  MMK (MadGoat Make) description file for the OVMX freestanding
!                 runtime component.  Self-host spine #5 (bead vms-fe4).
!
! Builds a REAL multi-translation-unit OVMX component -- the OVMX freestanding
! string / format / math runtime, the actual src/libvmssys TUs vms_string.c,
! vms_snprintf.c and vms_math.c (CLAUDE.md Rule 3: -ffreestanding -fno-builtin,
! no glibc dependency) -- end to end through the OVMX-native toolchain, DRIVEN BY
! MMK, with ZERO bash in the build path:
!
!     TCC.EXE       compiles each of the three runtime TUs (+ a small driver)
!                   to an .OBJ;
!     LIBRARIAN.EXE archives the three runtime objects into OVMXRT.OLB
!                   (an OVMX-labelled `ar` container, Rule 8 --
!                   docs/design-self-host-mmk-spine.md sec 3);
!     LINK.EXE      links the driver object against the .OLB into a runnable
!                   image, pulling only the library members that resolve.
!
! The dependency graph (image <- driver.OBJ + .OLB <- 3 runtime .OBJ <- 3 .C)
! makes MMK order the whole build itself: compile every TU, THEN archive, THEN
! link -- the plan a self-hosting `make` must produce.
!
! -----------------------------------------------------------------------------
! SYNTAX (clean-room, CLAUDE.md Rule 8).  This file uses only documented
! MMS/MMK description-file syntax:
!   * comments introduced by "!";
!   * macro definitions  NAME = value  and references  $(NAME);
!   * the automatic (special) macros  $(MMS$TARGET) / $(MMS$SOURCE);
!   * target : dependency-list rules with TAB-indented DCL action bodies.
! Grounded in the MadGoat MMK manual (tests/corpus/tier3-mmk/mmk_doc.sdml) and
! the public HP/VSI "MMS Reference Manual"; no VSI/HPE source or byte layout is
! used.
!
! The toolchain verbs TCC / LIBR / LNK are OVMX foreign commands defined by the
! build environment (see the test harness), an OVMX design choice (Rule 8).  LNK
! -- not LINK -- names the OVMX-native LINK.EXE because a DCL foreign-command
! symbol cannot shadow the built-in DCL LINK verb, exactly as on OpenVMS.  This
! descrip.mms is the self-host build driver: the shell-free DCL BUILD.COM it
! replaced was retired (vms-e49) once the ODS-2 flip landed.
! =============================================================================

! ---- macros ----------------------------------------------------------------
CFLAGS  = -x c -c -ffreestanding -fno-builtin
INCS    = -I OVMX$INCLUDE:
RTLIBS  = --use SYS$LIBRARY:DECC$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSPROCESS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSFS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSLNM$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSRMS$SHR.EXE
OBJS    = VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ

! ---- final image: the driver linked against the runtime library ------------
OVMXRT.EXE : OVMXRTDRV.OBJ OVMXRT.OLB
	LNK --executable $(RTLIBS) -o $(MMS$TARGET) OVMXRTDRV.OBJ OVMXRT.OLB

! ---- the runtime library: archive the three freestanding objects -----------
OVMXRT.OLB : $(OBJS)
	LIBR /CREATE $(MMS$TARGET) $(OBJS)

! ---- compile each translation unit (TCC.EXE) -------------------------------
OVMXRTDRV.OBJ : OVMXRTDRV.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)

VMS_MATH.OBJ : VMS_MATH.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)

VMS_STRING.OBJ : VMS_STRING.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)

VMS_SNPRINTF.OBJ : VMS_SNPRINTF.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)
