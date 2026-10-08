# vmsdecnet MAIL layer (rd vms-47fd, epic vms-30e): INBOUND DECnet MAIL-11 --
# Session Control object 27, what a VMS `MAIL> SEND` to `NODE::USER` speaks.
#
# vmsdecnet_mail11: the PURE MAIL-11 receiver state machine (no I/O; recipient
# check + delivery are callbacks), so the host replay test drives this exact
# code with the real VAX V7.3 oracle segments
# (tests/lab/captures/decnet-mail11-20261008/).
add_library(vmsdecnet_mail11 STATIC dnet_mail11.c)
target_include_directories(vmsdecnet_mail11 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# vmsdecnet_mail: NETACP's half of the MAIL server process -- the MAIL object's
# persona (SYSUAF MAIL$SERVER, else DEFAULT, + SYSPRV) and the $CREPRC of
# SYS$SYSTEM:MAIL_SERVER.EXE over the FAL server-process mailbox link.
add_library(vmsdecnet_mail STATIC dnet_mail_proc.c)
target_include_directories(vmsdecnet_mail PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(vmsdecnet_mail PUBLIC vmsdecnet_mail11 vmsdecnet_fal vms)

# MAIL_SERVER.EXE itself is defined in tools/CMakeLists.txt beside MAIL.EXE:
# it is built from the same mail store sources (tools/mail_*.c) and, like every
# shipped image, only when BUILD_TOOLS adds tools/.
