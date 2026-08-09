/*
 * LIB$ROUTINES.H - VMS Library (LIB$) Routine Prototypes
 *
 * OpenVMX compatibility layer - Declares the LIB$ run-time library
 * routines.  In VMS, these routines provide general-purpose services
 * including memory management, string formatting, terminal I/O,
 * date/time operations, process information, and condition handling.
 *
 * Calling conventions: All routines follow the VMS calling standard.
 * Most parameters are passed by reference.  Descriptors are used
 * for string parameters.  Return values are 32-bit condition codes.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#ifndef __LIB_ROUTINES_H
#define __LIB_ROUTINES_H

#include <stdint.h>
#include <stdarg.h>
#include "descrip.h"
/* gen64def.h supplies the __int64 / unsigned __int64 aliases that ported
 * VMS (DECC) sources use as builtin 64-bit integer types, plus GENERIC_64.
 * DECC provides __int64 as a compiler builtin; gcc does not, so pulling it
 * in here makes ported sources that only #include <lib$routines.h> (directly
 * or via errchk.h) compile without each having to include gen64def.h. */
#include "gen64def.h"
/* libdef.h is the single source of truth for the LIB$_ condition values
 * (LIB$_NORMAL, LIB$_NOTFOU, LIB$_KEYNOTFOU, LIB$_ONEENTQUE, ...).  Pull
 * it in so anything that includes lib$routines.h sees those codes with a
 * consistent value regardless of include order. */
#include "libdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration - full definition in lnmdef.h. Only used here
 * as a pointer type (lib$set_logical's optional item list), so a
 * forward declaration avoids requiring every lib$routines.h includer
 * to also pull in lnmdef.h. */
struct item_list_3;

/* Forward declaration - full definition in gen64def.h. Only used here
 * as a pointer type (lib$sys_asctim's time argument, which corpus call
 * sites pass as "&binary_time" where binary_time is GENERIC_64 - see
 * gen64def.h), so a forward declaration avoids requiring every
 * lib$routines.h includer to also pull in gen64def.h. */
struct _generic_64;

/* ================================================================
 * Memory Management Routines
 * ================================================================ */

/**
 * lib$get_vm - Allocate virtual memory
 *
 * @param num_bytes  Pointer to longword containing number of bytes to allocate
 * @param base_adr   Pointer to receive address of allocated memory
 * @param ...        Optional: pointer to zone identifier (NULL for default zone)
 *
 * @return  SS$_NORMAL on success, SS$_INSFMEM if insufficient memory
 *
 * Allocates a block of contiguous virtual memory from the specified
 * zone (or the default zone if zone_id is NULL or 0).
 */
uint32_t lib$get_vm(
    const uint32_t *num_bytes,
    void **base_adr,
    ...  /* optional: const uint32_t *zone_id */
);

/**
 * lib$free_vm - Free virtual memory
 *
 * @param num_bytes  Pointer to longword containing number of bytes to free
 * @param base_adr   Pointer to address of memory to free
 * @param ...        Optional: pointer to zone identifier (NULL for default zone)
 *
 * @return  SS$_NORMAL on success
 *
 * Returns a block of memory previously allocated by lib$get_vm
 * to the specified zone.
 */
uint32_t lib$free_vm(
    const uint32_t *num_bytes,
    void **base_adr,
    ...  /* optional: const uint32_t *zone_id */
);

/**
 * lib$get_vm_page - Allocate virtual memory in page units
 *
 * @param num_pages  Pointer to number of pages to allocate
 * @param base_adr   Pointer to receive address of allocated pages
 *
 * @return  SS$_NORMAL on success, SS$_INSFMEM if insufficient memory
 */
uint32_t lib$get_vm_page(
    const uint32_t *num_pages,
    void **base_adr
);

/**
 * lib$free_vm_page - Free virtual memory allocated in page units
 *
 * @param num_pages  Pointer to number of pages to free
 * @param base_adr   Pointer to address of pages to free
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$free_vm_page(
    const uint32_t *num_pages,
    void **base_adr
);

/**
 * lib$create_vm_zone - Create a virtual memory zone
 *
 * @param zone_id    Pointer to receive the zone identifier
 * @param ...        Optional zone creation parameters
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$create_vm_zone(
    uint32_t *zone_id,
    ...
);

/**
 * lib$delete_vm_zone - Delete a virtual memory zone
 *
 * @param zone_id    Pointer to zone identifier to delete
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$delete_vm_zone(
    const uint32_t *zone_id
);

/* ================================================================
 * Terminal I/O Routines
 * ================================================================ */

/**
 * lib$put_output - Write a line to SYS$OUTPUT
 *
 * @param message_string  Pointer to descriptor of message to write
 *
 * @return  SS$_NORMAL on success
 *
 * Writes the string described by message_string to the current
 * SYS$OUTPUT device, followed by a newline.
 */
uint32_t lib$put_output(
    const struct dsc$descriptor_s *message_string
);

/**
 * lib$get_input - Read a line from SYS$INPUT
 *
 * @param resultant_string  Pointer to descriptor to receive input
 * @param prompt_string     Optional pointer to descriptor of prompt
 * @param resultant_length  Optional pointer to receive actual length read
 *
 * @return  SS$_NORMAL on success, SS$_ENDOFFILE on EOF
 *
 * Reads a line of input from SYS$INPUT.  If prompt_string is
 * provided, it is displayed before reading.
 */
uint32_t lib$get_input(
    struct dsc$descriptor_s *resultant_string,
    const struct dsc$descriptor_s *prompt_string,
    uint16_t *resultant_length
);

/**
 * lib$put_common - Write record to process common area
 *
 * @param string  Pointer to descriptor of string to write
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$put_common(
    const struct dsc$descriptor_s *string
);

/**
 * lib$get_common - Read record from process common area
 *
 * @param resultant_string  Pointer to descriptor to receive string
 * @param resultant_length  Optional pointer to receive length
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$get_common(
    struct dsc$descriptor_s *resultant_string,
    uint16_t *resultant_length
);

/* ================================================================
 * Condition Handling Routines
 * ================================================================ */

/**
 * lib$signal - Signal a condition
 *
 * @param condition_value  The condition code to signal
 * @param ...              Optional FAO arguments for the message
 *
 * @return  Does not return if condition is severe;
 *          returns condition value otherwise
 *
 * Generates a signal that invokes the condition handler call
 * chain.  If no handler claims the condition, the default
 * handler issues the corresponding message.
 */
uint32_t lib$signal(
    uint32_t condition_value,
    ...
);

/**
 * lib$stop - Signal a condition and force image exit
 *
 * @param condition_value  The condition code to signal
 * @param ...              Optional FAO arguments
 *
 * @return  Does not return
 *
 * Like lib$signal, but forces image exit after handler processing.
 */
uint32_t lib$stop(
    uint32_t condition_value,
    ...
);

/**
 * lib$sig_to_ret - Convert signal to return status
 *
 * @param signal_args    Pointer to signal argument vector
 * @param mechanism_args Pointer to mechanism argument vector
 *
 * @return  SS$_RESIGNAL or SS$_CONTINUE
 *
 * A condition handler that converts a signaled condition into
 * a return status.  Typically established as a handler to allow
 * a routine to return the condition value to its caller.
 */
uint32_t lib$sig_to_ret(
    void *signal_args,
    void *mechanism_args
);

/**
 * lib$establish - Establish a condition handler
 *
 * @param handler  Pointer to handler routine
 *
 * @return  Address of previously established handler (or NULL)
 */
void *lib$establish(
    void *handler
);

/**
 * lib$revert - Revert to previous condition handler
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$revert(void);

/* ================================================================
 * String Formatting Routines
 * ================================================================ */

/**
 * lib$sys_fao - Formatted ASCII output (system)
 *
 * @param ctrstr   Pointer to descriptor of control string
 * @param outlen   Optional pointer to receive output length
 * @param outbuf   Pointer to descriptor of output buffer
 * @param ...      FAO directive arguments
 *
 * @return  SS$_NORMAL on success, SS$_BUFFEROVF if output truncated
 *
 * Formats output according to the FAO control string.
 * FAO directives include:
 *   !AS  - ASCII string (descriptor pointer)
 *   !AD  - ASCII descriptor with length (!AD takes count, address)
 *   !SL  - Signed longword
 *   !UL  - Unsigned longword
 *   !SW  - Signed word
 *   !UW  - Unsigned word
 *   !SB  - Signed byte
 *   !UB  - Unsigned byte
 *   !XL  - Hexadecimal longword
 *   !XW  - Hexadecimal word
 *   !XB  - Hexadecimal byte
 *   !OL  - Octal longword
 *   !ZL  - Zero-filled longword
 *   !/   - Newline
 *   !_   - Tab
 *   !!   - Literal exclamation point
 *   !n*c - Repeat character c, n times
 *   !n<  - Left justify in field of n
 *   !n>  - Right justify in field of n
 */
uint32_t lib$sys_fao(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    ...
);

/**
 * lib$sys_faol - Formatted ASCII output with argument list
 *
 * Like lib$sys_fao but takes an explicit argument list pointer
 * instead of variable arguments.
 */
uint32_t lib$sys_faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    const uint32_t *prmlst
);

/* ================================================================
 * Date/Time Routines
 * ================================================================ */

/**
 * lib$day - Get day number
 *
 * @param days      Pointer to receive day number (days since VMS base date)
 * @param timadr    Optional pointer to quadword time (NULL = current time)
 * @param day_time  Optional pointer to receive time within day (10us units)
 *
 * @return  SS$_NORMAL on success
 *
 * Returns the number of days since the VMS system base date
 * (November 17, 1858 - the Smithsonian base date).
 */
uint32_t lib$day(
    int32_t *days,
    const void *timadr,
    int32_t *day_time
);

/**
 * lib$day_of_week - Get day of week
 *
 * @param timadr         Optional pointer to quadword time (NULL = current time)
 * @param day_of_week    Pointer to receive day (1=Monday .. 7=Sunday)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$day_of_week(
    const void *timadr,
    int32_t *day_of_week
);

/**
 * lib$date_time - Get current date and time as a string
 *
 * @param date_time_string  Pointer to descriptor to receive date/time string
 *
 * @return  SS$_NORMAL on success
 *
 * Returns the current date and time in the standard VMS format:
 *   "dd-MMM-yyyy hh:mm:ss.cc"
 */
uint32_t lib$date_time(
    struct dsc$descriptor_s *date_time_string
);

/**
 * lib$sub_times - Subtract two quadword times
 *
 * @param time1     Pointer to first quadword time
 * @param time2     Pointer to second quadword time
 * @param result    Pointer to receive result (time1 - time2)
 *
 * @return  SS$_NORMAL on success, LIB$_NEGTIM if result is negative
 */
uint32_t lib$sub_times(
    const void *time1,
    const void *time2,
    void *result
);

/**
 * lib$add_times - Add two quadword times
 *
 * @param time1     Pointer to first quadword time
 * @param time2     Pointer to second quadword time
 * @param result    Pointer to receive result
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$add_times(
    const void *time1,
    const void *time2,
    void *result
);

/**
 * lib$mult_delta_time - Multiply delta time by scalar
 *
 * @param multiplier  Pointer to longword multiplier
 * @param timadr      Pointer to quadword delta time (modified in place)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$mult_delta_time(
    const int32_t *multiplier,
    void *timadr
);

/**
 * lib$cvt_from_internal_time - Convert from internal time format
 *
 * @param operation  Pointer to operation code
 * @param result     Pointer to receive result
 * @param time       Pointer to quadword time value
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$cvt_from_internal_time(
    const uint32_t *operation,
    uint32_t *result,
    const void *time
);

/* ================================================================
 * Conversion Routines
 * ================================================================ */

/**
 * lib$cvt_dtb - Convert decimal text to binary
 *
 * @param ndigits  Number of digits to convert
 * @param string   Pointer to ASCII digit string
 * @param value    Pointer to receive binary value
 *
 * @return  SS$_NORMAL on success, LIB$_INVARG on invalid character
 */
uint32_t lib$cvt_dtb(
    int32_t ndigits,
    const char *string,
    int32_t *value
);

/**
 * lib$cvt_htb - Convert hexadecimal text to binary
 *
 * @param ndigits  Number of hex digits to convert
 * @param string   Pointer to ASCII hex string
 * @param value    Pointer to receive binary value
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$cvt_htb(
    int32_t ndigits,
    const char *string,
    int32_t *value
);

/**
 * lib$cvt_otb - Convert octal text to binary
 *
 * @param ndigits  Number of octal digits to convert
 * @param string   Pointer to ASCII octal string
 * @param value    Pointer to receive binary value
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$cvt_otb(
    int32_t ndigits,
    const char *string,
    int32_t *value
);

/* ================================================================
 * Process and System Information Routines
 * ================================================================ */

/**
 * lib$getjpi - Get job/process information
 *
 * @param item_code         Pointer to JPI item code
 * @param pid               Optional pointer to process ID (NULL = current)
 * @param prcnam            Optional pointer to descriptor of process name
 * @param resultant_value   Optional pointer to receive longword result
 * @param resultant_string  Optional pointer to descriptor to receive string
 * @param resultant_length  Optional pointer to receive string length
 *
 * @return  SS$_NORMAL on success, SS$_NONEXPR if process not found
 *
 * Retrieves a single item of process information, similar to
 * SYS$GETJPI but simpler for single-item queries.
 */
uint32_t lib$getjpi(
    const uint32_t *item_code,
    const uint32_t *pid,
    const struct dsc$descriptor_s *prcnam,
    void *resultant_value,
    struct dsc$descriptor_s *resultant_string,
    uint16_t *resultant_length
);

/**
 * lib$getsyi - Get system information
 *
 * @param item_code         Pointer to SYI item code
 * @param resultant_value   Optional pointer to receive longword result
 * @param resultant_string  Optional pointer to descriptor to receive string
 * @param resultant_length  Optional pointer to receive string length
 * @param cluster_id        Optional pointer to cluster system ID
 * @param node_name         Optional pointer to descriptor of node name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$getsyi(
    const uint32_t *item_code,
    void *resultant_value,
    struct dsc$descriptor_s *resultant_string,
    uint16_t *resultant_length,
    const uint32_t *cluster_id,
    const struct dsc$descriptor_s *node_name
);

/* ================================================================
 * Symbol and CLI Routines
 * ================================================================ */

/**
 * lib$get_symbol - Get value of a CLI symbol
 *
 * @param symbol       Pointer to descriptor of symbol name
 * @param value        Pointer to descriptor to receive value
 * @param value_len    Optional pointer to receive value length
 * @param table_type   Optional pointer to receive table type indicator
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$get_symbol(
    const struct dsc$descriptor_s *symbol,
    struct dsc$descriptor_s *value,
    uint16_t *value_len,
    uint32_t *table_type
);

/**
 * lib$set_symbol - Set value of a CLI symbol
 *
 * @param symbol       Pointer to descriptor of symbol name
 * @param value        Pointer to descriptor of new value
 * @param table_type   Optional pointer to table type indicator
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$set_symbol(
    const struct dsc$descriptor_s *symbol,
    const struct dsc$descriptor_s *value,
    const uint32_t *table_type
);

/**
 * lib$delete_symbol - Delete a CLI symbol
 *
 * @param symbol       Pointer to descriptor of symbol name
 * @param table_type   Optional pointer to table type indicator
 *                      (LIB$K_CLI_LOCAL_SYM or LIB$K_CLI_GLOBAL_SYM;
 *                      defaults to LOCAL if not supplied)
 *
 * @return  SS$_NORMAL on success, LIB$_NOSUCHSYM if not found
 */
uint32_t lib$delete_symbol(
    const struct dsc$descriptor_s *symbol,
    const uint32_t *table_type
);

/* ================================================================
 * Logical Name Routines (simplified interface)
 * ================================================================ */

/**
 * lib$set_logical - Define a logical name (simplified interface)
 *
 * @param lognam   Pointer to descriptor of the logical name
 * @param eqvnam   Optional pointer to descriptor of the equivalence
 *                 string (used to build a one-entry item list when
 *                 itmlst is not supplied)
 * @param tabnam   Optional pointer to descriptor of the table name
 *                 (defaults to LNM$PROCESS_TABLE)
 * @param attr     Optional pointer to attribute flags
 * @param itmlst   Optional pointer to an item list, passed through
 *                 to SYS$CRELNM verbatim if supplied
 *
 * @return  SS$_NORMAL or SS$_SUPERSEDE on success
 */
uint32_t lib$set_logical(
    const struct dsc$descriptor_s *lognam,
    const struct dsc$descriptor_s *eqvnam,
    const struct dsc$descriptor_s *tabnam,
    const uint32_t *attr,
    const struct item_list_3 *itmlst
);

/**
 * lib$delete_logical - Delete a logical name (simplified interface)
 *
 * @param lognam  Pointer to descriptor of the logical name
 * @param tabnam  Optional pointer to descriptor of the table name
 *                (defaults to LNM$PROCESS_TABLE)
 *
 * @return  SS$_NORMAL on success, SS$_NOLOGNAM if not found
 */
uint32_t lib$delete_logical(
    const struct dsc$descriptor_s *lognam,
    const struct dsc$descriptor_s *tabnam
);

/* ================================================================
 * Dynamic String Descriptor Routines
 * ================================================================ */

/**
 * lib$sget1_dd - Allocate one dynamic string descriptor
 *
 * @param len     Pointer to longword requested length in bytes
 * @param dyndsc  Pointer to the dynamic descriptor to initialize
 *
 * @return  SS$_NORMAL on success, SS$_INSFMEM if allocation fails
 */
uint32_t lib$sget1_dd(
    const uint32_t *len,
    struct dsc$descriptor_d *dyndsc
);

/**
 * lib$sfree1_dd - Free one dynamic string descriptor
 *
 * @param dyndsc  Pointer to the dynamic descriptor to free (declared
 *                as a raw 64-bit pointer to match the
 *                "(unsigned __int64 *)&desc" cast used at call sites;
 *                reinterpreted internally as
 *                struct dsc$descriptor_d *)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$sfree1_dd(
    uint64_t *dyndsc
);

/**
 * lib$sfreen_dd - Free N dynamic string descriptors in one call
 *
 * @param n            Pointer to longword count of descriptors
 * @param dyndsc_array  Pointer to first element of an array of
 *                       dynamic descriptors
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$sfreen_dd(
    const uint32_t *n,
    struct dsc$descriptor_d *dyndsc_array
);

/* ================================================================
 * Subprocess and File Routines
 * ================================================================ */

/**
 * lib$spawn - Spawn a subprocess
 *
 * @param command_string  Optional pointer to descriptor of command
 * @param input_file      Optional pointer to descriptor of input file
 * @param output_file     Optional pointer to descriptor of output file
 * @param flags           Optional pointer to flags longword
 * @param process_name    Optional pointer to descriptor of process name
 * @param process_id      Optional pointer to receive process ID
 * @param completion_code Optional pointer to receive completion status
 * @param event_flag      Optional pointer to event flag number
 * @param ast_routine     Optional AST routine address
 * @param ast_argument    Optional AST argument
 * @param prompt_string   Optional pointer to descriptor of prompt
 * @param cli_name        Optional pointer to descriptor of CLI name
 * @param table_name      Optional pointer to descriptor of CLI table name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$spawn(
    const struct dsc$descriptor_s *command_string,
    const struct dsc$descriptor_s *input_file,
    const struct dsc$descriptor_s *output_file,
    const uint32_t *flags,
    const struct dsc$descriptor_s *process_name,
    uint32_t *process_id,
    uint32_t *completion_code,
    const uint32_t *event_flag,
    void *ast_routine,
    void *ast_argument,
    const struct dsc$descriptor_s *prompt_string,
    const struct dsc$descriptor_s *cli_name,
    const struct dsc$descriptor_s *table_name
);

/**
 * lib$find_file - Find file matching wildcard specification
 *
 * @param filespec   Pointer to descriptor of file specification (may contain wildcards)
 * @param resultspec Pointer to descriptor to receive found file specification
 * @param context    Pointer to context value (must be 0 on first call)
 *
 * @return  RMS$_NORMAL on success, RMS$_NMF when no more files
 */
uint32_t lib$find_file(
    const struct dsc$descriptor_s *filespec,
    struct dsc$descriptor_s *resultspec,
    uint32_t *context
);

/**
 * lib$find_file_end - End find file sequence
 *
 * @param context  Pointer to context value from lib$find_file
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$find_file_end(
    uint32_t *context
);

/**
 * lib$rename_file - Rename a file
 *
 * @param old_filespec  Pointer to descriptor of existing file specification
 * @param new_filespec  Pointer to descriptor of new file specification
 *
 * @return  RMS$_NORMAL on success
 */
uint32_t lib$rename_file(
    const struct dsc$descriptor_s *old_filespec,
    const struct dsc$descriptor_s *new_filespec
);

/**
 * lib$delete_file - Delete a file
 *
 * @param filespec  Pointer to descriptor of file specification
 *
 * @return  RMS$_NORMAL on success
 */
uint32_t lib$delete_file(
    const struct dsc$descriptor_s *filespec
);

/* ================================================================
 * Table-driven Parser
 * ================================================================ */

/**
 * lib$tparse - Table-driven finite-state parser
 *
 * @param tparse_block  Pointer to TPARSE argument block
 * @param state_table   Pointer to state transition table
 * @param key_table     Pointer to keyword table
 *
 * @return  SS$_NORMAL on success, LIB$_SYNTAXERR on parse failure
 */
uint32_t lib$tparse(
    void *tparse_block,
    const void *state_table,
    const void *key_table
);

/* ================================================================
 * LIB$ condition value definitions
 *
 * The LIB$_ condition values live in <libdef.h> (facility 21, the
 * VMS-correct encoding).  This header formerly carried a SECOND, private
 * copy of them with a wrong facility number (128 / 0x0080xxxx); every
 * source that included both headers then saw a redefinition warning and,
 * worse, a value that depended on include order.  The duplicate is
 * deleted and libdef.h (included above) is now the single source of
 * truth, so LIB$_NOTFOU / LIB$_KEYNOTFOU / ... are consistent everywhere.
 * ================================================================ */

/* Symbol table type codes for lib$get_symbol / lib$set_symbol */
#define LIB$K_CLI_LOCAL_SYM     1   /* Local symbol */
#define LIB$K_CLI_GLOBAL_SYM    2   /* Global symbol */

/* ================================================================
 * Bit and Arithmetic Interlocked Routines
 * ================================================================ */

/**
 * lib$adawi - Add aligned word interlocked
 *
 * @param add   Pointer to signed word addend
 * @param sum   Pointer to signed word accumulator (updated in place)
 * @param sign  Pointer to signed word receiving sign of result (-1/0/1)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$adawi(
    const int16_t *add,
    int16_t *sum,
    int16_t *sign
);

/**
 * lib$bbcci - Branch on Bit Clear and Clear Interlocked
 *
 * @param bit_position  Pointer to longword bit position
 * @param base_address  Pointer to start of bit array
 *
 * @return  Old value of the bit (1 if was set, 0 if was clear)
 */
uint32_t lib$bbcci(
    const int32_t *bit_position,
    void *base_address
);

/**
 * lib$bbssi - Branch on Bit Set and Set Interlocked
 *
 * @param bit_position  Pointer to longword bit position
 * @param base_address  Pointer to start of bit array
 *
 * @return  Old value of the bit (1 if was set, 0 if was clear)
 */
uint32_t lib$bbssi(
    const int32_t *bit_position,
    void *base_address
);

/**
 * lib$extv - Extract signed bit field
 *
 * @param pos           Pointer to longword bit position (0-based)
 * @param size          Pointer to byte field size (0-32 bits)
 * @param base_address  Pointer to start of bit array
 *
 * @return  Sign-extended 32-bit field value
 */
int32_t lib$extv(
    const int32_t *pos,
    const uint8_t *size,
    const void *base_address
);

/**
 * lib$extzv - Extract zero-extended (unsigned) bit field
 *
 * @param pos           Pointer to longword bit position (0-based)
 * @param size          Pointer to byte field size (0-32 bits)
 * @param base_address  Pointer to start of bit array
 *
 * @return  Zero-extended 32-bit field value
 */
uint32_t lib$extzv(
    const int32_t *pos,
    const uint8_t *size,
    const void *base_address
);

/**
 * lib$insv - Insert bit field
 *
 * @param source        Pointer to longword whose low bits are inserted
 * @param pos           Pointer to longword bit position (0-based)
 * @param size          Pointer to byte field size (0-32 bits)
 * @param base_address  Pointer to start of bit array (modified in place)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$insv(
    const int32_t *source,
    const int32_t *pos,
    const uint8_t *size,
    void *base_address
);

/* ================================================================
 * Event Flag Routines
 * ================================================================ */

/**
 * lib$get_ef - Allocate a free local event flag
 *
 * @param efn  Pointer to receive allocated event flag number (24-63)
 *
 * @return  SS$_NORMAL on success, LIB$_INSEF if no flags available
 */
uint32_t lib$get_ef(
    uint32_t *efn
);

/**
 * lib$free_ef - Release a previously allocated event flag
 *
 * @param efn  Pointer to event flag number to release
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$free_ef(
    const uint32_t *efn
);

/* ================================================================
 * String and Character Operation Routines
 * ================================================================ */

/**
 * lib$ichar - Integer value of first character
 *
 * @param str  Pointer to string descriptor
 *
 * @return  ASCII value of first character (not a status code)
 */
uint32_t lib$ichar(
    const struct dsc$descriptor_s *str
);

/**
 * lib$index - Find position of substring in string
 *
 * @param str  Pointer to descriptor of string to search
 * @param sub  Pointer to descriptor of substring to find
 *
 * @return  1-based position of first match, or 0 if not found
 */
uint32_t lib$index(
    const struct dsc$descriptor_s *str,
    const struct dsc$descriptor_s *sub
);

/**
 * lib$len - Length of string excluding trailing spaces
 *
 * @param str  Pointer to string descriptor
 *
 * @return  Length without trailing spaces (not a status code)
 */
uint32_t lib$len(
    const struct dsc$descriptor_s *str
);

/**
 * lib$locc - Locate character in string
 *
 * @param char_to_find  Descriptor whose first char is searched for
 * @param str           Descriptor of string to search
 *
 * @return  1-based position of first match, or 0 if not found
 */
uint32_t lib$locc(
    const struct dsc$descriptor_s *char_to_find,
    const struct dsc$descriptor_s *str
);

/**
 * lib$lp_lines - Lines per page for listing output
 *
 * @return  Default lines per page (66)
 */
uint32_t lib$lp_lines(void);

/**
 * lib$matchc - Match characters (substring search)
 *
 * @param sub  Descriptor of substring to search for
 * @param str  Descriptor of string to search
 *
 * @return  1-based position after match end, or 0 if not found
 */
uint32_t lib$matchc(
    const struct dsc$descriptor_s *sub,
    const struct dsc$descriptor_s *str
);

/**
 * lib$movc3 - Move (copy) characters, 3-argument form
 *
 * @param len  Pointer to word containing number of bytes to copy
 * @param src  Pointer to source buffer
 * @param dst  Pointer to destination buffer
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$movc3(
    const uint16_t *len,
    const void *src,
    void *dst
);

/**
 * lib$movc5 - Move characters with fill, 5-argument form
 *
 * @param src_len    Pointer to word containing source length
 * @param src        Pointer to source buffer
 * @param fill_char  Pointer to fill character
 * @param dst_len    Pointer to word containing destination length
 * @param dst        Pointer to destination buffer
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$movc5(
    const uint16_t *src_len,
    const void *src,
    const char *fill_char,
    const uint16_t *dst_len,
    void *dst
);

/**
 * lib$spanc - Span characters matching a table and mask
 *
 * @param str    Pointer to string descriptor
 * @param table  Pointer to 256-byte translation table
 * @param mask   Pointer to byte mask for table lookup
 *
 * @return  1-based position of first non-matching char, or 0 if all match
 */
uint32_t lib$spanc(
    const struct dsc$descriptor_s *str,
    const unsigned char *table,
    const unsigned char *mask
);

/* ================================================================
 * Date/Time Routines (continued)
 * ================================================================ */

/**
 * lib$sys_asctim - Convert binary time to ASCII string (RTL entry point)
 *
 * Functionally identical to sys$asctim (see starlet.h); documented as
 * a separate RTL procedure entry point in the OpenVMS RTL Library
 * (LIB$) Manual for callers that prefer the LIB$ calling interface.
 * OVMX implements it as a thin wrapper around sys$asctim.
 *
 * @param timlen  Optional pointer to receive string length
 * @param timbuf  Pointer to descriptor of output buffer
 * @param timadr  Optional pointer to a 64-bit time value (NULL = current
 *                time). Declared as GENERIC_64* to match corpus call
 *                sites that pass "&binary_time" where binary_time is
 *                GENERIC_64 (see gen64def.h) - functionally the same
 *                64-bit quadword sys$asctim accepts as uint64_t*.
 * @param cvtflg  Conversion flags (0 = full, 1 = date only)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t lib$sys_asctim(
    uint16_t *timlen,
    struct dsc$descriptor_s *timbuf,
    const struct _generic_64 *timadr,
    uint32_t cvtflg
);

/* ================================================================
 * Device Information Routines
 * ================================================================ */

/**
 * lib$getdvi - Get Device/Volume Information (simplified wrapper)
 *
 * Provides a simpler calling interface to sys$getdviw for retrieving
 * a single DVI$_ item, mirroring lib$getjpi/lib$getsyi in lib_misc.c.
 *
 * @param item_code    Pointer to longword containing the DVI$_ item code
 * @param chan         Optional I/O channel number, passed by value (0 if
 *                      using devnam) - matches sys$getdvi's chan argument
 *                      in starlet.h, which is likewise passed by value
 * @param devnam       Optional descriptor of device name
 * @param resultval    Optional address of longword to receive the item
 *                      value (for numeric items)
 * @param resultstring Optional descriptor to receive the item value as
 *                      a string
 * @param string_length Optional address of word to receive the length
 *                      written to resultstring
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if item_code is missing,
 *          SS$_NOSUCHDEV if the device cannot be identified
 */
uint32_t lib$getdvi(
    const uint32_t *item_code,
    uint16_t chan,
    const struct dsc$descriptor_s *devnam,
    void *resultval,
    struct dsc$descriptor_s *resultstring,
    uint16_t *string_length
);

/* ================================================================
 * Extended (multiple-precision) arithmetic routines
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$ADDX, LIB$SUBX,
 *            LIB$EMUL, LIB$EDIV; OpenVMS VAX Architecture Reference
 *            Manual (the ADAWI/EMUL/EDIV instruction semantics these
 *            jacket).
 * ================================================================ */

/**
 * lib$addx - Add two multiple-precision binary numbers.
 *
 * @param addend        First operand: array of longwords, low order first
 * @param augend        Second operand: array of longwords, low order first
 * @param sum           Result: addend + augend (may alias an input)
 * @param array_length  Optional pointer to the number of longwords in each
 *                      operand.  If omitted (NULL), the operands are a
 *                      single quadword (2 longwords).
 *
 * @return  SS$_NORMAL
 */
uint32_t lib$addx(
    const uint32_t *addend,
    const uint32_t *augend,
    uint32_t *sum,
    const int32_t *array_length
);

/**
 * lib$subx - Subtract two multiple-precision binary numbers (sum = minuend
 *            - subtrahend).
 *
 * @param minuend       Array of longwords, low order first
 * @param subtrahend    Array of longwords, low order first
 * @param difference    Result: minuend - subtrahend
 * @param array_length  Optional longword count (default 2 = one quadword)
 *
 * @return  SS$_NORMAL
 */
uint32_t lib$subx(
    const uint32_t *minuend,
    const uint32_t *subtrahend,
    uint32_t *difference,
    const int32_t *array_length
);

/**
 * lib$emul - Extended multiply: product(64) = multiplier*multiplicand +
 *            addend, using signed 32-bit operands.
 *
 * @param multiplier    Pointer to signed longword multiplier
 * @param multiplicand  Pointer to signed longword multiplicand
 * @param addend        Pointer to signed longword addend
 * @param product       Pointer to signed quadword receiving the result
 *
 * @return  SS$_NORMAL
 */
uint32_t lib$emul(
    const int32_t *multiplier,
    const int32_t *multiplicand,
    const int32_t *addend,
    long long *product
);

/**
 * lib$ediv - Extended divide: quotient = dividend/divisor,
 *            remainder = dividend - (quotient*divisor).  The remainder
 *            carries the sign of the dividend.
 *
 * @param divisor    Pointer to signed longword divisor
 * @param dividend   Pointer to signed quadword dividend
 * @param quotient   Pointer to signed longword receiving the quotient
 * @param remainder  Pointer to signed longword receiving the remainder
 *
 * @return  SS$_NORMAL, or LIB$_INVARG if divisor is zero
 */
uint32_t lib$ediv(
    const int32_t *divisor,
    const long long *dividend,
    int32_t *quotient,
    int32_t *remainder
);

/* ================================================================
 * Bit-scan routines (find first set / clear bit in a bit field)
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$FFS, LIB$FFC.
 * ================================================================ */

/**
 * lib$ffs - Find the first set bit in a bit field.
 *
 * @param start_position  Pointer to the longword start bit position
 * @param size            Pointer to the byte field size (0..32 bits)
 * @param base_address    Base of the bit field
 * @param find_position   Receives the bit position (relative to
 *                        base_address) of the first set bit
 *
 * @return  SS$_NORMAL if a set bit was found, LIB$_NOTFOU otherwise
 */
uint32_t lib$ffs(
    const int32_t *start_position,
    const uint8_t *size,
    const void *base_address,
    int32_t *find_position
);

/**
 * lib$ffc - Find the first clear bit in a bit field.
 *
 * @param start_position  Pointer to the longword start bit position
 * @param size            Pointer to the byte field size (0..32 bits)
 * @param base_address    Base of the bit field
 * @param find_position   Receives the bit position (relative to
 *                        base_address) of the first clear bit
 *
 * @return  SS$_NORMAL if a clear bit was found, LIB$_NOTFOU otherwise
 */
uint32_t lib$ffc(
    const int32_t *start_position,
    const uint8_t *size,
    const void *base_address,
    int32_t *find_position
);

/* ================================================================
 * Cyclic redundancy check
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$CRC_TABLE, LIB$CRC.
 * ================================================================ */

/**
 * lib$crc_table - Build the 16-longword CRC table used by lib$crc.
 *
 * @param polynomial  Pointer to the longword CRC polynomial
 * @param table       16-longword array to receive the table
 */
void lib$crc_table(
    const uint32_t *polynomial,
    uint32_t *table
);

/**
 * lib$crc - Compute a cyclic redundancy check over a string, nibble at a
 *           time, using a table built by lib$crc_table.
 *
 * @param table       The 16-longword table from lib$crc_table
 * @param initial     Pointer to the longword initial CRC value
 * @param string      Descriptor of the data to checksum
 *
 * @return  The computed CRC (a function value, not a status code)
 */
uint32_t lib$crc(
    const uint32_t *table,
    const uint32_t *initial,
    const struct dsc$descriptor_s *string
);

/* ================================================================
 * Character scan / classify / convert
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$SCANC, LIB$SKPC,
 *            LIB$CHAR, LIB$ANALYZE_SDESC.
 * ================================================================ */

/**
 * lib$scanc - Scan a string for a character whose 256-byte table entry,
 *             ANDed with mask, is non-zero.
 *
 * @param string  Descriptor of the string to scan
 * @param table   256-byte classification table
 * @param mask    Pointer to the byte mask
 *
 * @return  The 1-based position of the first matching character, or 0 if
 *          none matched.
 */
uint32_t lib$scanc(
    const struct dsc$descriptor_s *string,
    const uint8_t *table,
    const uint8_t *mask
);

/**
 * lib$skpc - Skip equal characters: find the first character of a string
 *            that is not equal to a given character.
 *
 * @param character  Descriptor whose first byte is the character to skip
 * @param string     Descriptor of the string to scan
 *
 * @return  The 1-based position of the first non-matching character, or 0
 *          if every character matched.
 */
uint32_t lib$skpc(
    const struct dsc$descriptor_s *character,
    const struct dsc$descriptor_s *string
);

/**
 * lib$char - Convert a byte value to a one-character string.
 *
 * @param destination  Destination string descriptor
 * @param ascii_code   Pointer to the byte value to convert
 *
 * @return  SS$_NORMAL, or LIB$_STRTRU if the destination is zero length
 */
uint32_t lib$char(
    struct dsc$descriptor_s *destination,
    const uint8_t *ascii_code
);

/**
 * lib$analyze_sdesc - Return the length and data address described by any
 *                     class of string descriptor.
 *
 * @param input_descriptor  The descriptor to analyze
 * @param data_length       Receives the data length (word)
 * @param data_address      Receives the data address
 *
 * @return  SS$_NORMAL
 */
uint32_t lib$analyze_sdesc(
    const void *input_descriptor,
    uint16_t *data_length,
    void **data_address
);

/**
 * lib$analyze_sdesc_64 - 64-bit form of lib$analyze_sdesc: returns a
 *                        quadword length and a 64-bit data address.
 *
 * @param input_descriptor  The descriptor to analyze
 * @param data_length       Receives the data length (quadword)
 * @param data_address      Receives the data address
 *
 * @return  SS$_NORMAL
 */
uint32_t lib$analyze_sdesc_64(
    const void *input_descriptor,
    uint64_t *data_length,
    void **data_address
);

/* ================================================================
 * Self-relative interlocked queue routines
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$INSQHI, LIB$INSQTI,
 *            LIB$REMQHI, LIB$REMQTI; OpenVMS VAX Architecture Reference
 *            Manual (self-relative queue format: quadword header of two
 *            longword self-relative offsets).
 * ================================================================ */

/**
 * lib$insqhi - Insert an entry at the head of a self-relative queue.
 *
 * @param entry        The queue entry to insert
 * @param header       The quadword queue header
 * @param retry_count  Optional retry count for the secondary interlock
 *
 * @return  LIB$_ONEENTQUE if the queue was empty (now one entry),
 *          else SS$_NORMAL
 */
uint32_t lib$insqhi(void *entry, void *header, const uint32_t *retry_count);

/**
 * lib$insqti - Insert an entry at the tail of a self-relative queue.
 * @see lib$insqhi
 */
uint32_t lib$insqti(void *entry, void *header, const uint32_t *retry_count);

/**
 * lib$remqhi - Remove the entry at the head of a self-relative queue.
 *
 * @param header       The quadword queue header
 * @param addr         Receives the address of the removed entry
 * @param retry_count  Optional retry count for the secondary interlock
 *
 * @return  LIB$_QUEWASEMP if the queue was empty, LIB$_ONEENTQUE if the
 *          removed entry was the last one (queue now empty), else SS$_NORMAL
 */
uint32_t lib$remqhi(void *header, void *addr, const uint32_t *retry_count);

/**
 * lib$remqti - Remove the entry at the tail of a self-relative queue.
 * @see lib$remqhi
 */
uint32_t lib$remqti(void *header, void *addr, const uint32_t *retry_count);

/* ================================================================
 * Balanced binary tree routines
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$INSERT_TREE,
 *            LIB$LOOKUP_TREE, LIB$TRAVERSE_TREE.
 * ================================================================ */

/**
 * lib$insert_tree - Insert a node into a binary tree, calling user
 *                   compare and allocate routines.
 *
 * @param treehead      Pointer to the tree-head cell (a node pointer)
 * @param symbol        The key to insert (passed through to the callbacks)
 * @param flags         Control flags (bit 0 = no duplicates)
 * @param compare_rtn   int compare(symbol, node) -> <0/0/>0
 * @param allocate_rtn  int allocate(symbol, &newnode, user_data) -> status
 * @param newnode       Receives the matched or newly-allocated node
 * @param user_data     Opaque value passed to allocate_rtn
 *
 * @return  SS$_NORMAL on insert, LIB$_KEYALRINS if the key already exists,
 *          or an allocate_rtn error status
 */
uint32_t lib$insert_tree(
    void *treehead,
    void *symbol,
    const uint32_t *flags,
    int (*compare_rtn)(void),
    int (*allocate_rtn)(void),
    void *newnode,
    void *user_data
);

/**
 * lib$lookup_tree - Look up a node in a binary tree by key.
 *
 * @param treehead     Pointer to the tree-head cell
 * @param symbol       The key to find
 * @param compare_rtn  int compare(symbol, node) -> <0/0/>0
 * @param node         Receives the matching node
 *
 * @return  SS$_NORMAL if found, LIB$_KEYNOTFOU otherwise
 */
uint32_t lib$lookup_tree(
    void *treehead,
    void *symbol,
    int (*compare_rtn)(void),
    void *node
);

/**
 * lib$traverse_tree - Visit every node of a binary tree in key order,
 *                     calling a user action routine for each.
 *
 * @param treehead   Pointer to the tree-head cell
 * @param action_rtn int action(node, user_data) -> status (odd to continue)
 * @param user_data  Opaque value passed to action_rtn
 *
 * @return  SS$_NORMAL, or the first non-success status from action_rtn
 */
uint32_t lib$traverse_tree(
    void *treehead,
    int (*action_rtn)(void),
    void *user_data
);

#ifdef __cplusplus
}
#endif

#endif /* __LIB_ROUTINES_H */
