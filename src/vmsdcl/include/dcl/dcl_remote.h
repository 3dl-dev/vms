/*
 * dcl_remote.h - DIRECTORY and TYPE of a node:: filespec (rd vms-0a7 S1).
 *
 * Both are ordinary RMS clients: $PARSE + $SEARCH list the remote files, and
 * $OPEN + $GET read them. RMS does the network access (src/vmsrms/
 * rms_remote.c). These routines only lay the results out the way a VAX V7.3
 * DIRECTORY / TYPE prints them for a remote directory.
 */
#ifndef DCL_REMOTE_H
#define DCL_REMOTE_H

#include <stddef.h>
#include <stdint.h>

/* Print the listing (or the failure, in VMS's words) on SYS$OUTPUT. Returns the
 * command's VMS status; a failure has already been reported. */
uint32_t dcl_remote_directory(const char *spec);
uint32_t dcl_remote_type(const char *spec);
/* DIRECTORY/FULL of a node:: spec: RMS asks the FAL for each file's attributes,
 * dates and protection with the listing (one DIRECTORY LIST), and the answer is
 * laid out as a VAX V7.3 prints it for a remote file. */
uint32_t dcl_remote_directory_full(const char *spec);

/* The DIRECTORY/FULL texts, shared with the local DIRECTORY/FULL. */
void dcl_fmt_recfmt(uint8_t rfm, uint16_t mrs, uint16_t lrl, uint8_t fsz,
                    char *out, size_t cap);
void dcl_fmt_recattr(uint8_t rat, char *out, size_t cap);
void dcl_fmt_prot_full(uint16_t prot, char *buf, size_t bufsize);

/* COPY with a node:: input or output: $SEARCH the input, then per file $OPEN
 * + $GET into $CREATE + $PUT. `src` / `dst` are the effective specs (the
 * caller completes a local side with the process default directory).
 * `log`: COPY/LOG. */
uint32_t dcl_remote_copy(const char *src, const char *dst, int log);

#endif /* DCL_REMOTE_H */
