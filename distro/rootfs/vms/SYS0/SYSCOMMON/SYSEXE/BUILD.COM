$ ! ===========================================================================
$ ! BUILD.COM -- OVMX self-hosting S3.2 (bead vms-615): compile + link + run a
$ ! C program end-to-end from a DCL session, driving the OVMX-NATIVE toolchain
$ ! (TCC.EXE + LINK.EXE, both OVMX-native images in SYS$SYSTEM) as DCL foreign
$ ! commands -- NO bash / no host tool anywhere in the build path. This is the
$ ! DCL command procedure that replaces the run_*.sh shell build drivers.
$ !
$ !     @BUILD name
$ !
$ !   P1 (name)   program base name, no extension.  name.C is compiled to
$ !               name.OBJ, linked to name.EXE, then activated.  The build runs
$ !               in the current default directory; the LINK --use producer
$ !               shareables are taken from the standard OVMX SYS$LIBRARY.
$ !
$ ! CONTROL FLOW: each foreign command sets $STATUS / $SEVERITY (vms-96e); the
$ ! ON ERROR handler jumps to BUILD_FAILED on any failing step and the procedure
$ ! exits with an error severity, so the caller (and the CI harness) sees it.
$ !
$ ! NOTES (OVMX deviations, all filed on vms-615):
$ !  - The OVMX DCL `LINK` command is a builtin that shells out to the host
$ !    linker, so -- exactly as on real VMS, where a foreign-command symbol
$ !    cannot shadow a DCL command -- the native LINK.EXE is invoked here under
$ !    the distinct foreign name LNK.
$ !  - OVMX DCL does not yet substitute symbols inside quoted strings ('' form)
$ !    nor evaluate a bare symbol in IF / WRITE, so this procedure keeps its
$ !    status messages literal and drives control flow through ON ERROR only.
$ ! ===========================================================================
$ TCC :== $SYS$SYSTEM:TCC.EXE
$ LNK :== $SYS$SYSTEM:LINK.EXE
$ ON ERROR THEN GOTO BUILD_FAILED
$ ON SEVERE_ERROR THEN GOTO BUILD_FAILED
$ !
$ WRITE SYS$OUTPUT "%BUILD-I-COMPILE, compiling name.C with the OVMX-native TCC.EXE"
$ ! -x c forces the C language: OVMX/VMS filenames are upper-case ("HELLO.C"),
$ ! but tcc keys the source language off a case-SENSITIVE extension and treats
$ ! ".C" as C++ (unsupported) -- so name the language explicitly (vms-615).
$ TCC -x c -c 'P1'.C -o 'P1'.OBJ
$ !
$ WRITE SYS$OUTPUT "%BUILD-I-LINK, linking name.EXE with the OVMX-native LINK.EXE"
$ LNK --executable -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/DECC$SHR.EXE -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMS$SHR.EXE -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMSPROCESS$SHR.EXE -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMSFS$SHR.EXE -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMSLNM$SHR.EXE -
	--use /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMSRMS$SHR.EXE -
	-o 'P1'.EXE 'P1'.OBJ
$ !
$ WRITE SYS$OUTPUT "%BUILD-I-RUN, activating the OVMX-native image name.EXE"
$ RUN 'P1'.EXE
$ WRITE SYS$OUTPUT "%BUILD-S-OK, compile + link + run complete"
$ EXIT 1
$ !
$ BUILD_FAILED:
$ WRITE SYS$OUTPUT "%BUILD-F-FAILED, build failed"
$ EXIT %X10000012
