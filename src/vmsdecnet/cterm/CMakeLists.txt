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
