/*
 * lib_output.c - LIB$PUT_OUTPUT / LIB$GET_INPUT
 *
 * VMS Runtime Library console I/O routines. On VMS these write to
 * SYS$OUTPUT and read from SYS$INPUT; here they use stdout/stdin.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ssdef.h"
#include "rmsdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "str$routines.h"

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
 * lib$get_foreign - Return the "foreign command" line that activated the image.
 *
 * On OpenVMS, when an image is activated as a foreign command
 * ($ FOO :== $dev:[dir]FOO ; then $ FOO /qual target), DCL hands the whole
 * raw command tail ("/qual target") to the image, and LIB$GET_FOREIGN asks
 * the CLI for that text. Per the VSI OpenVMS RTL Library (LIB$) Manual:
 *   - the FIRST call returns the CLI's foreign command line if one is present;
 *   - if there is NO command line (an ordinary RUN, or a foreign command with
 *     an empty tail), and on SUBSEQUENT calls, it prompts on SYS$INPUT exactly
 *     like LIB$GET_INPUT — the corpus lib_get_foreign.c documents this "behaves
 *     like lib_get_input" fall-through;
 *   - end of file on the SYS$INPUT path returns RMS$_EOF;
 *   - if the command line is longer than the resultant-string descriptor the
 *     text is truncated and LIB$_INPSTRTRU is returned.
 *
 * OVMX plumbing (this is the fix — it previously ALWAYS read stdin, which is
 * the LARP: an image invoked as `FOO /qual target` could never see its own
 * arguments). DCL's foreign-command dispatch (dcl_exec_foreign_command in
 * src/vmsdcl/dcl_cmd_process.c) publishes the raw command tail — cmd->raw_tail,
 * the exact untokenized text after the verb — in the VMS_FOREIGN_CMD
 * environment variable before activating the image, following the same
 * env-channel convention DCL already uses to pass VMS process context
 * (VMS_DEFAULT_DIR, VMS_PRIVILEGES, ...). The channel works for BOTH activation
 * models: in-process activation (the image runs in DCL's process and reads the
 * var directly) and the fork()+execve() fallback (the child inherits DCL's
 * environment). RUN and DCL-driven utilities do NOT set the variable, so they
 * correctly fall through to the SYS$INPUT path.
 *
 * One-shot semantics: on VMS the CLI foreign line is consumed on first
 * retrieval, so a second call falls through to SYS$INPUT. We model that
 * faithfully by CLEARING VMS_FOREIGN_CMD once it has been returned. Modelling
 * consumption in the environment (rather than in a static flag) is deliberate:
 * under in-process activation the LIB$ code lives in the resident producer
 * shareable and its statics persist ACROSS successive image activations, so a
 * static "already consumed" flag would leak from one image into the next; the
 * env var is per-activation state that DCL re-establishes for each foreign
 * command, so keying consumption on it is correct for both activation models.
 *
 * The optional fourth argument (flags, an unsigned longword by reference that
 * the manual sets to 1 after the first call to steer a prompt loop) is accepted
 * for source compatibility. It is NOT dereferenced: with the "..." prototype
 * its PRESENCE cannot be detected safely, and every in-tree caller (mmk.c,
 * corpus lib_get_foreign.c) omits it, so the implementation follows the
 * documented flags-omitted behavior — first call returns the CLI line,
 * subsequent calls prompt — which the env-var consumption already provides.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$GET_FOREIGN.
 */
uint32_t lib$get_foreign(struct dsc$descriptor_s *result,
                         const struct dsc$descriptor_s *prompt,
                         uint16_t *result_len,
                         ...) {
    /* A dynamic (class D) descriptor legitimately starts with a NULL pointer
     * and zero length — str$copy_dx allocates it below.  Only a static
     * descriptor must already point at a buffer. */
    if (!result) return SS$_BADPARAM;
    if (result->dsc$b_class != DSC$K_CLASS_D && !result->dsc$a_pointer)
        return SS$_BADPARAM;

    /* First call with a foreign command line present: return the raw tail
     * DCL published, then consume it so a second call reads SYS$INPUT. An
     * empty/absent value is "no command line" -> fall through to SYS$INPUT. */
    const char *tail = getenv("VMS_FOREIGN_CMD");
    if (tail && tail[0] != '\0') {
        size_t len = strlen(tail);

        /* Dynamic (class D) result descriptor: allocate/resize to hold the whole
         * tail via str$copy_dx, matching VMS LIB$GET_FOREIGN, which accepts a
         * dynamic string descriptor and returns the full command line.  (Callers
         * such as MMK pass an INIT_DYNDESC descriptor whose dsc$w_length starts
         * at 0; the static path below would otherwise copy zero bytes.) */
        if (result->dsc$b_class == DSC$K_CLASS_D) {
            struct dsc$descriptor_s src;
            src.dsc$w_length  = (uint16_t)len;
            src.dsc$b_dtype   = DSC$K_DTYPE_T;
            src.dsc$b_class   = DSC$K_CLASS_S;
            src.dsc$a_pointer = (char *)tail;
            uint32_t st = str$copy_dx(result, &src);
            if (result_len) *result_len = (uint16_t)len;
            unsetenv("VMS_FOREIGN_CMD");
            return (st & 1) ? SS$_NORMAL : st;
        }

        uint16_t copylen = (uint16_t)len;
        int truncated = 0;
        if (len > result->dsc$w_length) {
            copylen = result->dsc$w_length;
            truncated = 1;
        }
        memcpy(result->dsc$a_pointer, tail, copylen);

        /* Pad with spaces for static (class S) descriptors, matching the
         * LIB$GET_INPUT convention above. */
        if (copylen < result->dsc$w_length) {
            memset(result->dsc$a_pointer + copylen, ' ',
                   result->dsc$w_length - copylen);
        }

        if (result_len) *result_len = copylen;

        /* Consume the CLI foreign line: subsequent calls read SYS$INPUT. */
        unsetenv("VMS_FOREIGN_CMD");

        return truncated ? LIB$_INPSTRTRU : SS$_NORMAL;
    }

    /* No foreign command line (RUN, an empty tail, or an already-consumed
     * line): behave exactly like LIB$GET_INPUT, prompting on SYS$INPUT and
     * returning RMS$_EOF at end of file. */
    return read_line_into(result, prompt, result_len, stdin);
}
