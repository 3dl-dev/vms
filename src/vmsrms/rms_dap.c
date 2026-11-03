/*
 * rms_dap.c - the DAP message codec, as part of RMS (rd vms-0a7).
 *
 * RMS speaks DAP to a remote FAL for a node:: filespec (rms_remote.c), so
 * LIBVMSRMS$SHR carries the codec itself. The codec is the one pure byte
 * library src/vmsdecnet/dap/dnet_dap.c (no I/O, no allocation); this unit
 * compiles that same source into RMS, so every RMS build recipe (CMake, the
 * LINK.EXE shareable, the Alpha and netbsd-vax crosses) lists one more RMS
 * unit and there is still exactly one codec.
 */
#include "../vmsdecnet/dap/dnet_dap.c"
