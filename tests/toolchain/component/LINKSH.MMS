! =============================================================================
! LINKSH.MMS  --  MMK (MadGoat Make) description file for the OVMX self-host S4
!                 FIXPOINT: the OVMX linker LINK.EXE built from WITHIN OVMX.
!                 Ports the multi-TU LINK.EXE fixpoint from BUILD.COM (the DCL
!                 driver distro/.../SYS0/SYSCOMMON/SYSEXE/BUILD.COM +
!                 src/imgact/test/run_link_selfhost_native.sh) to MMK (bead
!                 vms-89d, the vms-678 Build-native 1.0 gate).
!
! Builds the REAL two-translation-unit OVMX linker -- the actual
! src/vmslink/link.c (LINK.C) + src/vmslink/ovmx_link_rms_io.c
! (OVMX_LINK_RMS_IO.C, the RMS-backed object-read / image-write shim) -- end to
! end through the OVMX-native toolchain, DRIVEN BY MMK, with ZERO bash in the
! build path:
!
!     TCC.EXE   compiles LINK.C and OVMX_LINK_RMS_IO.C to objects;
!     LINK.EXE  --executable links the two objects against the C run-time
!               shareable DECC$SHR.EXE + the five OVMX runtime shareables into a
!               runnable OVMX linker image, LINKSH.EXE (PT_INTERP=IMGACT.EXE).
!
! THE FIXPOINT (what the harness that drives this file asserts -- vms-89d,
! mirroring the BUILD.COM path run_link_selfhost_native.sh proves today):
!
!   gen1  a host-bootstrap-built LINK.EXE links LINK.OBJ + OVMX_LINK_RMS_IO.OBJ
!         (the SAME objects this file compiles) into LINKSH.EXE = gen2 -- the
!         first OVMX-BUILT linker;
!   gen2  is installed AS the linker (SYS$SYSTEM:LINK.EXE) and MMK drives THIS
!         SAME file again: the OVMX-built gen2 links the same objects into
!         LINKSH.EXE = gen3 -- the OVMX-built linker linking itself;
!   gen2 == gen3, byte-for-byte.  A linker that reproduces itself under its own
!         successor is a true self-hosting fixpoint (the tcc gen2==gen3 pattern,
!         now for LINK.EXE and driven by MMK instead of BUILD.COM).
!
! The dependency graph (image <- LINK.OBJ + OVMX_LINK_RMS_IO.OBJ <- 2 .C) makes
! MMK order the build itself: compile every TU, THEN link -- the plan a
! self-hosting `make` must produce.  link.c / imgact.c / tcc are the toolchain
! and are OUT of scope here (do NOT edit them); this file is the MMK driver that
! replaces BUILD.COM's multi-TU LINK.EXE fixpoint (the destructive retirement of
! BUILD.COM itself is a later vms-89d rung, deferred behind the ODS-2 flip).
!
! -----------------------------------------------------------------------------
! SYNTAX (clean-room, CLAUDE.md Rule 8).  This file uses only documented
! MMS/MMK description-file syntax:
!   * comments introduced by "!";
!   * macro definitions  NAME = value  and references  $(NAME);
!   * the automatic (special) macros  $(MMS$TARGET) / $(MMS$SOURCE);
!   * target : dependency-list rules with TAB-indented DCL action bodies;
!   * line continuation with a trailing "-".
! Grounded in the MadGoat MMK manual (tests/corpus/tier3-mmk/mmk_doc.sdml) and
! the public HP/VSI "MMS Reference Manual"; no VSI/HPE source or byte layout is
! used.
!
! The toolchain verbs TCC / LNK are OVMX foreign commands the build environment
! defines (see the harness).  LNK -- not LINK -- names the OVMX-native LINK.EXE
! because a DCL foreign-command symbol cannot shadow the built-in DCL LINK verb,
! exactly as on OpenVMS (this mirrors BUILD.COM's own LNK :== $...LINK.EXE, and
! OVMXRT.MMS documents the same trap).
!
! CFLAGS/INCS mirror run_link_selfhost_native.sh's LC_CFLAGS (the same -D/-I set
! mk_link.sh compiles link.c with): -DOVMX_RMS_IO selects link.c's RMS I/O seams;
! the include roots are logical names the harness defines (VMSLINK$SRC -> the
! staged sources' dir, holding ovmx_link_rms_io.h; OVMX$INCLUDE -> the staged
! ovmx_image.h / ovmx_symvec.h; RMS$INCLUDE -> rms/rms.h).  tcc ignores gcc-only
! codegen flags, so only the -D/-I set matters.
! =============================================================================

! ---- macros ----------------------------------------------------------------
CFLAGS  = -x c -c -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO
INCS    = -I VMSLINK$SRC: -I OVMX$INCLUDE: -I RMS$INCLUDE:
RTLIBS  = --use SYS$LIBRARY:DECC$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSPROCESS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSFS$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSLNM$SHR.EXE -
	  --use SYS$LIBRARY:LIBVMSRMS$SHR.EXE
OBJS    = LINK.OBJ OVMX_LINK_RMS_IO.OBJ

! ---- final image: the OVMX linker, linked from its two objects --------------
LINKSH.EXE : $(OBJS)
	LNK --executable $(RTLIBS) -o $(MMS$TARGET) $(OBJS)

! ---- compile each translation unit (TCC.EXE) -------------------------------
LINK.OBJ : LINK.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)

OVMX_LINK_RMS_IO.OBJ : OVMX_LINK_RMS_IO.C
	TCC $(CFLAGS) $(INCS) $(MMS$SOURCE) -o $(MMS$TARGET)
