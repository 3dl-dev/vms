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

#include <stdint.h>
#include <stdio.h>

/* Print the listing (or the failure, in VMS's words) to `out`. Returns the
 * command's VMS status; a failure has already been reported. */
uint32_t dcl_remote_directory(FILE *out, const char *spec);
uint32_t dcl_remote_type(FILE *out, const char *spec);

/* COPY with a node:: input or output: $SEARCH the input, then per file $OPEN
 * + $GET into $CREATE + $PUT. `src` / `dst` are the effective specs (the
 * caller completes a local side with the process default directory).
 * `log`: COPY/LOG. */
uint32_t dcl_remote_copy(FILE *out, const char *src, const char *dst, int log);

#endif /* DCL_REMOTE_H */
