/*
 * lib_output.c - LIB$PUT_OUTPUT / LIB$GET_INPUT
 *
 * VMS Runtime Library console I/O routines. On VMS these write to
 * SYS$OUTPUT and read from SYS$INPUT; here they use stdout/stdin.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

/*
 * lib$put_output - Write descriptor contents to stdout with a newline.
 *
 * Extracts the string from the descriptor, writes it to stdout,
 * appends a newline, and flushes the output.
 *
 * Parameters:
 *   message - Descriptor containing the string to output
 *
 * Returns:
 *   SS$_NORMAL   - Output successful
 *   SS$_BADPARAM - NULL or invalid descriptor
 */
uint32_t lib$put_output(const struct dsc$descriptor_s *message) {
    if (!message) return SS$_BADPARAM;
    if (!message->dsc$a_pointer) return SS$_BADPARAM;

    fwrite(message->dsc$a_pointer, 1, message->dsc$w_length, stdout);
    fputc('\n', stdout);
    fflush(stdout);

    return SS$_NORMAL;
}

/*
 * lib$get_input - Read a line from stdin into a descriptor.
 *
 * If prompt is non-null, writes the prompt string to stdout first.
 * Reads a line from stdin (stripping the newline) into the result
 * descriptor. For static descriptors, pads with spaces. Sets
 * result_len to the actual number of characters read.
 *
 * Parameters:
 *   result      - Descriptor to receive the input string
 *   prompt      - Optional prompt string descriptor (or NULL)
 *   result_len  - Receives the actual length of input (or NULL)
 *
 * Returns:
 *   SS$_NORMAL    - Input read successfully
 *   SS$_ENDOFFILE - End of input (Ctrl-D)
 *   SS$_BADPARAM  - Invalid descriptor
 */
uint32_t lib$get_input(struct dsc$descriptor_s *result,
                       const struct dsc$descriptor_s *prompt,
                       uint16_t *result_len) {
    if (!result || !result->dsc$a_pointer) return SS$_BADPARAM;

    /* Display prompt if provided */
    if (prompt && prompt->dsc$a_pointer && prompt->dsc$w_length > 0) {
        fwrite(prompt->dsc$a_pointer, 1, prompt->dsc$w_length, stdout);
        fflush(stdout);
    }

    /* Read a line from stdin */
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return SS$_ENDOFFILE;
    }

    /* Remove trailing newline */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
    }

    /* Copy to descriptor */
    uint16_t copylen = (uint16_t)len;
    if (copylen > result->dsc$w_length) copylen = result->dsc$w_length;
    memcpy(result->dsc$a_pointer, buf, copylen);

    /* Pad with spaces for static descriptors (VMS convention) */
    if (copylen < result->dsc$w_length) {
        memset(result->dsc$a_pointer + copylen, ' ',
               result->dsc$w_length - copylen);
    }

    if (result_len) *result_len = copylen;

    return SS$_NORMAL;
}
