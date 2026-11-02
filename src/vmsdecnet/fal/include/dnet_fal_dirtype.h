/*
 * dnet_fal_dirtype.h - `$ DIRECTORY node::spec` and `$ TYPE node::spec`, the
 * FAL client side of the two commands (rd vms-6a4).
 *
 * What a VMS client does, as the real-VAX captures show it
 * (tests/lab/captures/decnet-fal-verbs-20261008/, vax-to-vax-sys-login/):
 *
 *   DIRECTORY  one link: a DIRECTORY LIST access of the spec. The FAL answers
 *              a NAME for the volume, a NAME for the directory, then a NAME per
 *              file, then ACCESS COMPLETE. The listing header is
 *              "Directory NODE::<volume><directory>", the names run four to a
 *              line in 20-column cells, then "Total of N files.". A search that
 *              matched nothing ends in STATUS FNF: %DIRECT-W-NOFILES.
 *   TYPE       a DIRECTORY LIST of the spec, then on ONE further link a
 *              CONFIGURATION + OPEN + GET of each file by its full resultant
 *              name. A wildcard prints " ", the file's name and " " before each
 *              file's records; a file the FAL refuses is
 *              %TYPE-W-OPENIN ... as input + the RMS reason, and a search that
 *              matched nothing %TYPE-W-SEARCHFAIL + the reason.
 *
 * The node prefix in every message is the one the user typed, with VMS's own
 * masking of the password: NODE"user password"::.
 *
 * The transport is abstracted (struct dnet_fal_linker), so the same code runs
 * over a NETACP _NET: link (DECNETD.EXE --directory / --type) and over a
 * replayed real-VAX capture in the host test. Output goes to a stdio stream.
 *
 * CLEAN-ROOM (Rule 8): formats and message choice are from the captured
 * console output of a real VAX; nothing here is from VMS source or binaries.
 */
#ifndef DNET_FAL_DIRTYPE_H
#define DNET_FAL_DIRTYPE_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include "dnet_fal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens / closes one logical link to the remote FAL (object 17). open()
 * returns the DAP transport of a connected link, or NULL with the reason the
 * connect failed in *st (SS$_INVLOGIN, SS$_NOSUCHNODE, ...). */
struct dnet_fal_linker {
    struct dnet_dap_transport *(*open)(void *ctx, uint32_t *st);
    void (*close)(void *ctx, struct dnet_dap_transport *t);
    void *ctx;
};

/* Verb for dnet_fal_default_spec. */
enum { DNET_FAL_VERB_DIRECTORY = 1, DNET_FAL_VERB_TYPE = 2 };

/*
 * The defaults the verb applies to a node-stripped remote spec before it goes
 * on the wire. DIRECTORY: an empty name is *.*, a missing type .*, a missing
 * version ;* (the VAX sent SYS$LOGIN:BRK*.TXT;* for DIRECTORY ...BRK*.TXT).
 * TYPE: a missing version is ";" (it sent ...BRK*.TXT;). 0 ok, -1 too long.
 */
int dnet_fal_default_spec(const char *spec, int verb, char *out, size_t cap);

/*
 * The node prefix VMS prints in a message or header: NODE::, NODE"user"::, or
 * NODE"user password"::. VMS never echoes the password; it prints the word
 * "password" in its place (vax1-console.txt).
 */
void dnet_fal_display_prefix(const char *node, const char *user, int has_access,
                             int has_password, char *out, size_t cap);

/*
 * The RMS condition VMS reports for a DAP STATUS (the MICCODE names the RMS
 * error: FNF, PRV, DNF, ACC). Anything else is the STV when one came, else
 * `fallback`.
 */
uint32_t dnet_fal_dap_rms(uint16_t stscode, uint64_t stv, uint32_t fallback);

/* Run the command and write what VMS prints to `out`. `nodepfx` is from
 * dnet_fal_display_prefix; `spec` is the node-stripped spec as typed (the
 * verb's defaults are applied here). Returns the command's VMS status. */
uint32_t dnet_fal_directory(FILE *out, const struct dnet_fal_linker *lk,
                            const char *nodepfx, const char *spec);
uint32_t dnet_fal_type(FILE *out, const struct dnet_fal_linker *lk,
                       const char *nodepfx, const char *spec);

/* Most file NAMEs one listing keeps (a bound on what a remote can make this
 * client allocate). Past it the listing ends with what it holds. */
#define DNET_FAL_DIR_MAXFILES 65536

#ifdef __cplusplus
}
#endif

#endif /* DNET_FAL_DIRTYPE_H */
