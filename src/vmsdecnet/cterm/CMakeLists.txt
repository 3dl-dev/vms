# vmsdecnet CTERM layer - DECnet Phase IV CTERM (Command Terminal) protocol, the
# terminal-service layered product behind $ SET HOST (rd vms-4d2, engine rung 3
# under epic vms-30e; north-star demo leg vms-e4dc).
#
# dnet_cterm: the CTERM message codec + the terminal-session state machine that
# rides on an established NSP logical link (dnet_link) -- Bind/Unbind, terminal-
# characteristics negotiation, and the Start Read/Read Data/Write/Out-of-Band
# messages that carry keystrokes up and screen output down. Like the NSP codec
# and the logical-link FSM below it, this is a PURE byte/state library: no
# socket, no allocation beyond memcpy/memset, no clock -- so it links equally
# into the daemon and the deterministic unit test, and presents $ SET HOST
# upward, never a raw frame (Rule 1). CLEAN-ROOM (Rule 8): CTERM is entirely
# SPEC-DERIVED -- no oracle specimen exists (the vms-3be capture never completed
# a logical link), so the message codes/layouts are OVMX-assigned from the public
# DNA CTERM functional description and proven only by round-trip. See
# docs/decnet-provenance-register.md sec 4.7.
add_library(vmsdecnet_cterm STATIC dnet_cterm.c)
target_include_directories(vmsdecnet_cterm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# vmsdecnet_cterm_host: the CTERM HOST SESSION (rd vms-f40) -- what an inbound
# $ SET HOST actually reaches. DELIBERATELY A SEPARATE TARGET from the pure
# codec above: this half calls VMS system services ($CREPRC) and the executive
# (the RTAn: mint), so keeping it out of vmsdecnet_cterm makes the codec's
# purity a build-enforced fact rather than a promise -- the unit test links the
# codec alone and would stop linking the moment a service call leaked into it.
#
# It closes the no-auth hole the first CTERM cut opened: an inbound object-42
# connect no longer openpty()s and fork()+execvp()s a shell, it mints an RTAn:
# through the executive and creates a process running LOGINOUT.EXE on it via
# $CREPRC PRC$M_INTER|PRC$M_LOGINOUT -- the SAME primitive the console login
# uses -- so the remote user is CHALLENGED. Oracle: docs/oracle/
# vax-sethost-cterm.* (rd vms-558) -- the carried node::user is proxy info,
# LOGINOUT authenticates fresh.
add_library(vmsdecnet_cterm_host STATIC dnet_cterm_host.c)
target_include_directories(vmsdecnet_cterm_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vmsdecnet_cterm_host PUBLIC vmsdecnet_cterm vms vmsfs vmssys)
