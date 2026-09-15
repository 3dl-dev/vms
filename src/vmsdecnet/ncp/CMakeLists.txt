# vmsdecnet NCP layer (rd vms-1e9, epic vms-30e): the DECnet Phase IV
# CONFIGURATION surface -- the "configuration" pillar of the layered product.
#
# dnet_nodedb: the persisted remote-node database (the NCP node table) +
# name<->address resolution the SET HOST / NODE:: paths need. Pure table logic
# + a documented plain-text on-disk format; no socket, no engine, unit-testable
# in isolation. The NCP command grammar + SHOW layout are public (DECnet for
# OpenVMS Networking Manual); the on-disk format is an OVMX choice (labelled).
add_library(vmsdecnet_nodedb STATIC dnet_nodedb.c)
target_include_directories(vmsdecnet_nodedb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/vmsdecnet/routing/include   # dnet_hello.h: area/node helpers
)

# dnet_objectdb (rd vms-f52): the persisted DECnet OBJECT database -- the
# Session Control objects this node offers (42=CTERM, 17=FAL, ...), managed by
# NCP SET/DEFINE/SHOW/CLEAR/PURGE OBJECT. Same footing as vmsdecnet_nodedb: pure
# table logic + a documented plain-text file, no socket/engine, unit-testable.
add_library(vmsdecnet_objectdb STATIC dnet_objectdb.c)
target_include_directories(vmsdecnet_objectdb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# NCP.EXE -- the Network Control Program utility that drives the database +
# executor config (SET/DEFINE/SHOW/CLEAR NODE + OBJECT, SET/SHOW EXECUTOR).
add_executable(ncp_exe ncp.c)
target_link_libraries(ncp_exe PRIVATE vmsdecnet_nodedb vmsdecnet_objectdb)
set_target_properties(ncp_exe PROPERTIES OUTPUT_NAME "NCP" SUFFIX ".EXE")
install(TARGETS ncp_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
