# src/vmsfs/ods2 - GENUINE ODS-2 (Files-11 level 2) structures + reader/writer.
#
# A standalone, dependency-free static library: reads AND writes byte-genuine
# ODS-2 volume images (home block, INDEXF.SYS headers, FM2 retrieval pointers,
# directory records, storage bitmap). Kept separate from the ODS-2-INSPIRED
# src/kernel/vmsfs on-disk format, which is not byte-genuine. See vmsfs/ods2.h
# for provenance (reader: increments 1-2; writer: increment 3).

add_library(ods2 STATIC ods2_reader.c ods2_writer.c ods2_bdev.c ods2_path.c)
target_include_directories(ods2 PUBLIC ${VMS_FS_INCLUDE_DIR})
set_target_properties(ods2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
