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
#include "rmsdef.h"
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
 *   SS$_NORMAL - Input read successfully
 *   RMS$_EOF   - End of input (Ctrl-Z / EOF)
 *   SS$_BADPARAM - Invalid descriptor
 *
 * Note: the OpenVMS RTL documents LIB$GET_INPUT as returning RMS$_EOF
 * (not SS$_ENDOFFILE) at end of file — the Eight-Cubed corpus programs
 * (lib_get_input.c, lib_lookup_key.c, ...) all test `== RMS$_EOF`, and
 * LIB$LOOKUP_KEY's read loop only terminates on that exact value.
 */
static uint32_t read_line_into(struct dsc$descriptor_s *result,
                               const struct dsc$descriptor_s *prompt,
                               uint16_t *result_len, FILE *stream) {
    if (!result || !result->dsc$a_pointer) return SS$_BADPARAM;

    /* Display prompt if provided */
    if (prompt && prompt->dsc$a_pointer && prompt->dsc$w_length > 0) {
        fwrite(prompt->dsc$a_pointer, 1, prompt->dsc$w_length, stdout);
        fflush(stdout);
    }

    /* Read a line from the stream */
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stream)) {
        return RMS$_EOF;
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

uint32_t lib$get_input(struct dsc$descriptor_s *result,
                       const struct dsc$descriptor_s *prompt,
                       uint16_t *result_len) {
    return read_line_into(result, prompt, result_len, stdin);
}

/*
 * lib$get_command - Read a line from SYS$COMMAND.
 *
 * Analogous to LIB$GET_INPUT, but on VMS the input source is SYS$COMMAND
 * (the original command stream) rather than SYS$INPUT. In this hosted
 * environment both map to the process's standard input. Returns RMS$_EOF
 * at end of file, matching the documented behavior.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$GET_COMMAND.
 */
uint32_t lib$get_command(struct dsc$descriptor_s *result,
                         const struct dsc$descriptor_s *prompt,
                         uint16_t *result_len) {
    return read_line_into(result, prompt, result_len, stdin);
}

/*
 * lib$get_foreign - Return the "foreign command" parameter line.
 *
 * When an image is invoked as a foreign command ($ FOO :== $dev:[dir]FOO),
 * LIB$GET_FOREIGN returns the text typed after the command verb. When no
 * such line is available it reads from SYS$INPUT exactly like
 * LIB$GET_INPUT (see the corpus lib_get_foreign.c header). OVMX images are
 * launched with an empty foreign line, so this falls through to reading a
 * line from standard input and returns RMS$_EOF at end of file.
 *
 * The optional fourth argument (flags) selects prompting behavior and is
 * accepted for source compatibility.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$GET_FOREIGN.
 */
uint32_t lib$get_foreign(struct dsc$descriptor_s *result,
                         const struct dsc$descriptor_s *prompt,
                         uint16_t *result_len,
                         ...) {
    return read_line_into(result, prompt, result_len, stdin);
}
