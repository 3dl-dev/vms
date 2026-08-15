# src/vmsfs/ods2 - GENUINE ODS-2 (Files-11 level 2) structures + reader/writer.
#
# A standalone, dependency-free static library: reads AND writes byte-genuine
# ODS-2 volume images (home block, INDEXF.SYS headers, FM2 retrieval pointers,
# directory records, storage bitmap). Kept separate from the ODS-2-INSPIRED
# src/kernel/vmsfs on-disk format, which is not byte-genuine. See vmsfs/ods2.h
# for provenance (reader: increments 1-2; writer: increment 3).
#
# Standalone netbsd-vax cross (vms-64a, epic vms-f10, unified cross-build Rung
# B): INITIALIZE.EXE (tools/vms_initialize.c) links this library directly
# (tools/CMakeLists.txt: target_link_libraries(vms_initialize PRIVATE vmssys
# ods2)) for its --ods2 genuine-volume writer path. Mirrors the standalone-
# detection guard src/vmsfs/CMakeLists.txt and src/vmsprocess/CMakeLists.txt
# already use: when configured directly (no enclosing project()), declare one
# and default VMS_FS_INCLUDE_DIR to the sibling include/ dir the parent build
# normally injects.
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ods2_standalone C)
    if(NOT DEFINED VMS_FS_INCLUDE_DIR)
        set(VMS_FS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    endif()
endif()

add_library(ods2 STATIC ods2_reader.c ods2_writer.c ods2_bdev.c ods2_path.c)
target_include_directories(ods2 PUBLIC ${VMS_FS_INCLUDE_DIR})
set_target_properties(ods2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
