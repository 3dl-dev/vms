$ ! ===========================================================================
$ ! BUILD.COM -- OVMX self-hosting (beads vms-615 S3.2 + vms-62b S4): compile N
$ ! source TUs and link them into one image, end-to-end from a DCL session,
$ ! driving the OVMX-NATIVE toolchain (TCC.EXE + LINK.EXE, both OVMX-native
$ ! images in SYS$SYSTEM) as DCL foreign commands -- NO bash / no host tool
$ ! anywhere in the build path.  This is the DCL command procedure that replaces
$ ! the run_*.sh shell build drivers.
$ !
$ !     @BUILD name                       ! single TU: name.C -> name.EXE, RUN it
$ !     @BUILD out src1 [src2 ... src7]   ! multi TU: srcN.C -> out.EXE
$ !
$ !   P1 (out/name)  output image base name, no extension -> out.EXE.
$ !   P2..P8 (srcN)  source base names (no extension); each srcN.C is compiled
$ !                  to srcN.OBJ and all objects are linked into out.EXE.  With
$ !                  no P2 the single-TU form is used: P1 is BOTH the source and
$ !                  the output (P1.C -> P1.EXE), preserving @BUILD HELLO.
$ !
$ !   CFLAGS  (optional symbol)  extra flags inserted ahead of the TCC compile
$ !                  (e.g. "-DOVMX_RMS_IO -I/path/include"); each whitespace-
$ !                  separated token becomes a separate argv element.
$ !   NORUN   (optional symbol)  if defined non-empty, the linked image is NOT
$ !                  activated after the link (a tool such as LINK.EXE cannot be
$ !                  RUN with no arguments).  Default: activate it (the S3.2
$ !                  HELLO smoke).
$ !
$ ! CONTROL FLOW: each foreign command sets $STATUS / $SEVERITY (vms-96e); the
$ ! ON ERROR handler jumps to BUILD_FAILED on any failing step and the procedure
$ ! exits with an error severity, so the caller (and the CI harness) sees it.
$ !
$ ! DCL note: the multi-TU loop uses genuine DCL symbol evaluation -- SRC = P'N'
$ ! (parameter indexing), IF SRC .EQS. "" (bare-symbol comparison), OBJLIST
$ ! accumulation, and ''SRC' substitution inside quoted WRITE strings -- all
$ ! fixed in vms-5c1.  The OVMX DCL `LINK` builtin still shells to the host
$ ! linker, so -- exactly as on real VMS, where a foreign-command symbol cannot
$ ! shadow a DCL command -- the native LINK.EXE is invoked here under the
$ ! distinct foreign name LNK.
$ ! ===========================================================================
$ TCC :== $SYS$SYSTEM:TCC.EXE
$ LNK :== $SYS$SYSTEM:LINK.EXE
$ ON ERROR THEN GOTO BUILD_FAILED
$ ON SEVERE_ERROR THEN GOTO BUILD_FAILED
$ IF F$TYPE(CFLAGS) .EQS. "" THEN CFLAGS = ""
$ IF F$TYPE(NORUN) .EQS. "" THEN NORUN = ""
$ !
$ OUTNAME = P1
$ OBJLIST = ""
$ ! A DCL command procedure has only P1..P8, capping the param form at 7 source
$ ! TUs (P2..P8) -- too few for a real OVMX component (libvmssys is 10 TUs). The
$ ! SOURCES symbol lifts that: a space-separated list of source base names,
$ ! walked with F$ELEMENT (no cap), so @BUILD scales to a whole component while
$ ! the P2..P8 param form stays intact for the HELLO/LINK.EXE smokes. (vms-678)
$ IF F$TYPE(SOURCES) .EQS. "" THEN SOURCES = ""
$ IF SOURCES .NES. "" THEN GOTO COMPILE_LIST
$ IF P2 .EQS. "" THEN GOTO COMPILE_SINGLE
$ !
$ ! -- multi-TU: compile each of P2..P8 that is supplied --
$ NPARAM = 2
$ COMPILE_LOOP:
$   SRC = P'NPARAM'
$   IF SRC .EQS. "" THEN GOTO LINK_STEP
$   WRITE SYS$OUTPUT "%BUILD-I-COMPILE, compiling ''SRC'.C with the OVMX-native TCC.EXE"
$   TCC 'CFLAGS' -x c -c 'SRC'.C -o 'SRC'.OBJ
$   OBJLIST = OBJLIST + " " + SRC + ".OBJ"
$   NPARAM = NPARAM + 1
$   IF NPARAM .LE. 8 THEN GOTO COMPILE_LOOP
$   GOTO LINK_STEP
$ !
$ ! -- source-list: compile every base name in the SOURCES symbol (no cap).
$ !    F$ELEMENT returns the delimiter itself once the list is exhausted, so a
$ !    lone " " is the loop terminator. --
$ COMPILE_LIST:
$ ISRC = 0
$ LIST_LOOP:
$   SRC = F$ELEMENT(ISRC, " ", "''SOURCES'")
$   IF SRC .EQS. " " THEN GOTO LINK_STEP
$   IF SRC .EQS. "" THEN GOTO LIST_NEXT
$   WRITE SYS$OUTPUT "%BUILD-I-COMPILE, compiling ''SRC'.C with the OVMX-native TCC.EXE"
$   TCC 'CFLAGS' -x c -c 'SRC'.C -o 'SRC'.OBJ
$   OBJLIST = OBJLIST + " " + SRC + ".OBJ"
$ LIST_NEXT:
$   ISRC = ISRC + 1
$   GOTO LIST_LOOP
$ !
$ ! -- single-TU: P1 is both the source and the output --
$ COMPILE_SINGLE:
$ WRITE SYS$OUTPUT "%BUILD-I-COMPILE, compiling ''P1'.C with the OVMX-native TCC.EXE"
$ ! -x c forces the C language: OVMX/VMS filenames are upper-case ("HELLO.C"),
$ ! but tcc keys the source language off a case-SENSITIVE extension and treats
$ ! ".C" as C++ (unsupported) -- so name the language explicitly (vms-615).
$ TCC 'CFLAGS' -x c -c 'P1'.C -o 'P1'.OBJ
$ OBJLIST = P1 + ".OBJ"
$ !
$ LINK_STEP:
$ WRITE SYS$OUTPUT "%BUILD-I-LINK, linking ''OUTNAME'.EXE with the OVMX-native LINK.EXE"
$ ! The C run-time + OVMX shareables the executable binds are named by VMS
$ ! logical spec (SYS$SHARE:, the OpenVMS home of installed shareable images) --
$ ! NOT a /vms POSIX path (the atomic-flip-retired passthrough, vms-104). The
$ ! OVMX-native LINK.EXE translates a SYS$SHARE:/SYS$LIBRARY:/SYS$SYSTEM: --use
$ ! producer to the boot-staged copy ovmx_init read off the ODS-2 volume over the
$ ! Files-11 ACP -- so the producer bytes come from the volume, never /vms.
$ LNK --executable -
	--use SYS$SHARE:DECC$SHR.EXE -
	--use SYS$SHARE:LIBVMS$SHR.EXE -
	--use SYS$SHARE:LIBVMSPROCESS$SHR.EXE -
	--use SYS$SHARE:LIBVMSFS$SHR.EXE -
	--use SYS$SHARE:LIBVMSLNM$SHR.EXE -
	--use SYS$SHARE:LIBVMSRMS$SHR.EXE -
	-o 'OUTNAME'.EXE 'OBJLIST'
$ !
$ IF NORUN .NES. "" THEN GOTO SKIP_RUN
$ WRITE SYS$OUTPUT "%BUILD-I-RUN, activating the OVMX-native image ''OUTNAME'.EXE"
$ RUN 'OUTNAME'.EXE
$ SKIP_RUN:
$ WRITE SYS$OUTPUT "%BUILD-S-OK, compile + link complete"
$ EXIT 1
$ !
$ BUILD_FAILED:
$ WRITE SYS$OUTPUT "%BUILD-F-FAILED, build failed"
$ EXIT %X10000012
