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
