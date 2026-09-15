# vmsdecnet FAL layer - the DECnet FILE ACCESS LISTENER (Session Control object
# 17) server + the COPY node:: client (rd vms-8c2, epic vms-30e; north-star demo
# leg vms-e4dc). The file-transfer layered product behind
# `$ COPY node"user pw"::file localfile`.
#
# vmsdecnet_fal: the FAL server + COPY client. DELIBERATELY SEPARATE from the
# pure DAP codec (vmsdecnet_dap) and the pure NSP/CTERM codecs: this half
# AUTHENTICATES (sysuaf_lookup + sysuaf_authenticate Purdy, in libvms) and does
# real FILE I/O (rms_textfile_* -- RMS over the ODS-2 ACP, in libvms), so
# keeping it out of the codecs makes their purity a build-enforced fact. It
# reuses the bounded FAL access-control decoder dnet_fal_access_decode
# (vmsdecnet_cterm) for the connect-carried credentials.
#
# CLEAN-ROOM (Rule 8): the DAP message SEQUENCE + the credential/object
# semantics are docs/oracle/vax-copy-fal-dap.* (rd vms-cd3); the DAP field
# framing is the public spec (vmsdecnet_dap). Security: an inbound FAL connect
# is authenticated before any file is served (INV-6, the no-auth-FAL analogue of
# the CTERM hole); a bad password is refused by real Purdy. NO
# fork/exec/openpty/dup2, NO raw-fd/raw-termios file mechanics.
add_library(vmsdecnet_fal STATIC dnet_fal.c dnet_nodespec.c)
target_include_directories(vmsdecnet_fal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsdecnet_fal PUBLIC
    vmsdecnet_dap vmsdecnet_cterm vms vmsfs vmssys)
