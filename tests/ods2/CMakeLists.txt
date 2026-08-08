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
target_compile_definitions(test_ods2_real PRIVATE
    ODS2_REAL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/real_vax_ods2.dsk")

add_test(NAME ods2_real_image COMMAND test_ods2_real)
set_tests_properties(ods2_real_image PROPERTIES
    LABELS "unit;ods2;vmsfs"
    TIMEOUT 30
)
