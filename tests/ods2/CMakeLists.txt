# tests/ods2 - Unit tests for genuine ODS-2 (Files-11 level 2) structures + reader.

add_executable(test_ods2 test_ods2.c)
target_include_directories(test_ods2 PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2 PRIVATE ods2)

add_test(NAME ods2_unit COMMAND test_ods2)
set_tests_properties(ods2_unit PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# Increment 2: drive the reader over a REAL OpenVMS VAX V7.3 volume image
# (tests/ods2/real_vax_ods2.dsk). See PROVENANCE-real_vax_ods2.md for how it
# was made and what OpenVMS itself reported for the same volume.
add_executable(test_ods2_real test_ods2_real.c)
target_include_directories(test_ods2_real PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_real PRIVATE ods2)

# The fixture path is passed via the test's ENVIRONMENT rather than a quoted
# -D string compile-definition: a string macro forces CMake to emit
# backslash-escaped quotes into compile_commands.json, which the
# kif_caller_census authenticity gate rejects (it parses that file naively).
add_test(NAME ods2_real_image COMMAND test_ods2_real)
set_tests_properties(ods2_real_image PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
    ENVIRONMENT "ODS2_REAL_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/real_vax_ods2.dsk"
)

# Increment 3: the ODS-2 WRITER. Builds a volume with ods2_volume_format() +
# ods2_wvolume_create_dir()/create_file()/dir_insert(), then reads it back
# with the SAME increment-1/2 reader already validated against a real
# OpenVMS VAX volume (test_ods2_real.c) -- proving writer/reader agreement.
# ODS2_WRITE_DUMP is an optional env var (not set here, so unused by ctest)
# a human can export when re-running this binary by hand to dump the built
# image to disk for a real-VAX lab-2 MOUNT cross-check.
add_executable(test_ods2_write test_ods2_write.c)
target_include_directories(test_ods2_write PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_write PRIVATE ods2)

add_test(NAME ods2_write COMMAND test_ods2_write)
set_tests_properties(ods2_write PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# Increment 4: SECURITY.SYS VBN1 data-block writer/reader
# (ods2_security_build()/ods2_security_parse()). Offline round-trip plus a
# regression pin against the real lab-2 checksum values that resolved the
# increment-3 %MOUNT-F-BADSECSYS failure -- see ods2.h's WRITER [F6].
add_executable(test_ods2_security test_ods2_security.c)
target_include_directories(test_ods2_security PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_security PRIVATE ods2)

add_test(NAME ods2_security COMMAND test_ods2_security)
set_tests_properties(ods2_security PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-6cb (R1 of the real-ODS-2-runtime epic vms-5eb): the BLOCK-BACKED reader.
# Builds a genuine volume with the writer, lays it into a real loop-image fd,
# and drives ods2_bdev_open/_read_block/_read_header/_list_dir over it --
# proving the block-backed reader returns byte-for-byte identical results to
# the in-memory reader (home block + both checksums + DECFILE11B, MFD FID
# (4,4,0), INDEXF.SYS FID 1, BITMAP.SYS FID 2, and a versioned FID chain).
add_executable(test_ods2_bdev test_ods2_bdev.c)
target_include_directories(test_ods2_bdev PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_bdev PRIVATE ods2)

add_test(NAME ods2_bdev COMMAND test_ods2_bdev)
set_tests_properties(ods2_bdev PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-6d3b (R2 of the real-ODS-2-runtime epic vms-5eb): the BLOCK-DEVICE-
# BACKED WRITER -- the WRITE counterpart to vms-6cb's block-backed reader
# above. Builds a genuine volume DIRECTLY against a real loop-image fd via
# ods2_wvolume_format_bdev()/create_dir()/create_file()/dir_insert() (pwrite,
# never an in-memory whole-image buffer), reads it back with the EXISTING
# block-backed reader (ods2_bdev_*) over the SAME fd, decodes a created
# file's VAR records byte-exact, and cross-checks the output against the
# in-memory writer (ods2_volume_format()) for byte-identity.
add_executable(test_ods2_write_bdev test_ods2_write_bdev.c)
target_include_directories(test_ods2_write_bdev PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_write_bdev PRIVATE ods2)

add_test(NAME ods2_write_bdev COMMAND test_ods2_write_bdev)
set_tests_properties(ods2_write_bdev PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-1bd (R8 of the real-ODS-2-runtime epic vms-5eb): MULTI-BLOCK DIRECTORY
# GROWTH ([F17]). Inserts more files into one directory than fit in a single
# 512-byte directory block, forcing ods2_wvolume_dir_insert() to grow the
# directory across blocks + extend its FH2 map/recattr, then proves BOTH the
# in-memory reader and R1's block-backed reader (ods2_bdev_*) list every
# entry and that a created file's content round-trips through the VAR framing.
add_executable(test_ods2_dirgrow test_ods2_dirgrow.c)
target_include_directories(test_ods2_dirgrow PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_dirgrow PRIVATE ods2)

add_test(NAME ods2_dirgrow COMMAND test_ods2_dirgrow)
set_tests_properties(ods2_dirgrow PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-6ef (R4 of the real-ODS-2-runtime epic vms-5eb): INITIALIZE writes a
# GENUINE ODS-2 volume. Proves the ODS-2 method INITIALIZE uses produces a
# real Files-11 volume the block-backed reader (R1) parses clean + that the
# on-disk bytes are byte-identical to the writer's known-good output. When
# BUILD_TOOLS is on, INIT_EXE points the test at the actual shipped
# INITIALIZE.EXE so the binary itself is exercised, not just the library.
add_executable(test_ods2_initialize test_ods2_initialize.c)
target_include_directories(test_ods2_initialize PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_initialize PRIVATE ods2)

add_test(NAME ods2_initialize COMMAND test_ods2_initialize)
set_tests_properties(ods2_initialize PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)
if(BUILD_TOOLS)
    # Generator expression -- resolved at generate time, so it is valid even
    # though tools/ is add_subdirectory'd after tests/ods2. Exercises the real
    # binary; add_dependencies makes the tool build before this test's target.
    set_property(TEST ods2_initialize APPEND PROPERTY
        ENVIRONMENT "INIT_EXE=$<TARGET_FILE:vms_initialize>")
    add_dependencies(test_ods2_initialize vms_initialize)
endif()

# vms-5eb (R2/R3/R5 read-path foundation): the block-backed PATH-RESOLUTION +
# CONTENT-READ layer (src/vmsfs/ods2/ods2_path.c). Builds a genuine ODS-2
# volume carrying the real system-disk hierarchy shape
# ([000000]->[SYS0]->[SYS0.SYSCOMMON]->[SYS0.SYSCOMMON.SYSEXE] with files),
# lays it on a loop-image fd, and drives ods2_bdev_resolve_dir/_resolve_file/
# _dir_find/_read_file/_read_file_text over it -- proving the live RMS/DCL/
# MOUNT path can reach SYS$SYSTEM files through a real FID chain + FM2 extents,
# never POSIX opendir/open. Also the shape the R6 boot-master builder uses.
add_executable(test_ods2_path test_ods2_path.c)
target_include_directories(test_ods2_path PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_path PRIVATE ods2)

add_test(NAME ods2_path COMMAND test_ods2_path)
set_tests_properties(ods2_path PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-351 (epic vms-5eb "B2"): the userspace MOUNTED-VOLUME table
# (src/vmsfs/vmsfs_volume.c) -- device name -> (open fd + cached ods2_bdev_t).
# The additive substrate PID 1 registers the boot device into at boot. Builds a
# genuine ODS-2 loop image, drives the SAME vmsfs_volume_register() call PID 1
# makes, and resolves [SYS0.SYSCOMMON.SYSEXE]LOGIN.COM through the cached handle
# -- plus the fail-honest paths (non-ODS-2 / unopenable backing store register
# nothing). Links vmsfs_volume (which links the ods2 reader sublibrary).
add_executable(test_ods2_volume test_ods2_volume.c)
target_include_directories(test_ods2_volume PRIVATE ${VMS_FS_INCLUDE_DIR} ${VMS_INCLUDE_DIR})
target_link_libraries(test_ods2_volume PRIVATE vmsfs_volume)

add_test(NAME ods2_volume COMMAND test_ods2_volume)
set_tests_properties(ods2_volume PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-5eb (ODS-2 runtime flip): the SYS$DISK read adapter
# (src/vmsfs/ods2_sysdisk.c) -- the "/vms/A/B/.../NAME.EXT;ver" -> ods2_bdev
# path bridge every flip consumer (RMS rms_impl_open, DCL cmd_directory/$SEARCH,
# MOUNT) reroutes onto. Builds a genuine ODS-2 loop image with the system
# hierarchy, registers it as DKA0: (as PID 1 does), and drives resolve/read/list
# through "/vms/..." paths -- byte-identical verbatim read-back, version
# selection, directory listing, and the fail-honest paths (no volume ->
# SS$_DEVNOTMOUNT, non-/vms -> SS$_BADPARAM). Links vmsfs_volume.
add_executable(test_ods2_sysdisk test_ods2_sysdisk.c)
target_include_directories(test_ods2_sysdisk PRIVATE ${VMS_FS_INCLUDE_DIR} ${VMS_INCLUDE_DIR})
target_link_libraries(test_ods2_sysdisk PRIVATE vmsfs_volume)

add_test(NAME ods2_sysdisk COMMAND test_ods2_sysdisk)
set_tests_properties(ods2_sysdisk PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-02e (epic vms-5eb, the WRITE half of the ODS-2 runtime flip): the
# SYS$DISK ODS-2 WRITE adapter (src/vmsfs/ods2_sysdisk.c create/append/mkdir
# + ods2_writer.c ods2_wvolume_open_bdev/_append_file). Builds a genuine ODS-2
# loop image with the system hierarchy, registers it as DKA0: (O_RDWR, as PID 1
# does), and through "/vms/..." paths: creates a verbatim file (byte-identical
# read-back), APPENDS to an existing file repeatedly across contiguous AND
# non-contiguous extent growth (the novel path -- read-back concatenation
# byte-exact), mkdir + creates a file inside it, and exercises the fail-honest
# edges (no volume -> SS$_DEVNOTMOUNT, non-/vms -> SS$_BADPARAM, absent file ->
# SS$_NOSUCHFILE, write over an O_RDONLY volume fails honestly -- INV-6).
# Additive: flips no live path. Links vmsfs_volume.
add_executable(test_ods2_write_adapter test_ods2_write_adapter.c)
target_include_directories(test_ods2_write_adapter PRIVATE ${VMS_FS_INCLUDE_DIR} ${VMS_INCLUDE_DIR})
target_link_libraries(test_ods2_write_adapter PRIVATE vmsfs_volume)

add_test(NAME ods2_write_adapter COMMAND test_ods2_write_adapter)
set_tests_properties(ods2_write_adapter PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-88d (R6-build unblocker for the ODS-2 flip vms-5eb): the WRITER's
# long-filename IDENT-area encoding. The real distro tree contains names
# like "LIBVMSPROCESS$SHR.EXE;1" (23 chars) that the writer's old 20-char-only
# fi2_filename cap rejected outright. Builds a genuine volume via the
# block-device-backed writer, inspects the raw on-disk IDENT area
# (ods2_fh2_ident()) to prove the >20-char split across fi2_filename[20] +
# fi2_filenamext[66] is byte-exact, resolves + reads the long-named file back
# through ods2_bdev_resolve_file()/_read_file() (the same path DCL/RMS use),
# and regression-guards that <=20-char names (including the exact 20-char
# boundary) are byte-identical to the pre-fix encoding.
add_executable(test_ods2_longname test_ods2_longname.c)
target_include_directories(test_ods2_longname PRIVATE ${VMS_FS_INCLUDE_DIR})
target_link_libraries(test_ods2_longname PRIVATE ods2)

add_test(NAME ods2_longname COMMAND test_ods2_longname)
set_tests_properties(ods2_longname PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)

# vms-5eb R6-build: the vmsfs_master BINARY masters a host source tree onto a
# GENUINE ODS-2 (DECFILE11B) volume via --ods2 and reads it back over a real
# block device (ods2_bdev_*). Exercises the actual shipped boot-master tool
# end-to-end -- the R6-build primitive the atomic RMS/DCL/MOUNT flip consumes --
# plus the fail-honest path (list --ods2 rejects a non-ODS-2 VMFS volume,
# INV-6). Gated on BUILD_TOOLS since it drives the real binary.
if(BUILD_TOOLS)
    add_test(NAME ods2_master
             COMMAND ${CMAKE_COMMAND} -E env
                     VMSFS_MASTER=$<TARGET_FILE:vmsfs_master>
                     sh ${CMAKE_CURRENT_SOURCE_DIR}/test_ods2_master.sh)
    set_tests_properties(ods2_master PROPERTIES
        LABELS "integration;ods2;vmsfs;tools"
        TIMEOUT 60
    )
endif()
