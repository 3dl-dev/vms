# vmsscs - SCS Datalink daemon (SCSD)
#
# Greenfield AF_PACKET raw-socket datalink for the VMScluster wire protocol
# (docs/design-cluster-node.md sec 3.1). vms-294 gave this the receive +
# length-classify skeleton. vms-b62 adds the transmit path: scs_hello.c
# builds a spec-valid multicast HELLO (docs/cluster-protocol-spec.md sec
# 4a/4b) and scsd.c gains an --emit-hello beacon mode.
#
# No dependency on any other OVMX library for the classify/hello builders
# (plain POSIX + stdint); scsd_exe additionally pulls in src/libvms/include
# header-only readers (sysgen_params.h, cluster_authorize.h) for node/group
# identity -- no link dependency, they are static inline.

add_library(vmsscs_classify STATIC scs_classify.c)
target_include_directories(vmsscs_classify PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(vmsscs_hello STATIC scs_hello.c)
target_include_directories(vmsscs_hello PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vms-5fe - VMS$VAXcluster SCS connect handshake (directed HELLO reply lives
# in vmsscs_hello; the connect-request/response builder + parser is here).
add_library(vmsscs_connect STATIC scs_connect.c)
target_include_directories(vmsscs_connect PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(scsd_exe scsd.c)
target_include_directories(scsd_exe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/libvms/include
)
target_link_libraries(scsd_exe PRIVATE vmsscs_classify vmsscs_hello vmsscs_connect)
set_target_properties(scsd_exe PROPERTIES
    OUTPUT_NAME "SCSD"
    SUFFIX ".EXE"
)
install(TARGETS scsd_exe RUNTIME DESTINATION ${VMS_SYSTEM_DIR})
