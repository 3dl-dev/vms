# OVMX Run-Time Library (RTL) API Reference

This document covers all implemented RTL routines in `src/libvms/rtl/`. Routines are grouped by prefix: LIB$, STR$, MTH$, and OTS$.

All parameters follow the VMS calling convention: passed by reference (pointer) unless noted otherwise. String parameters use VMS descriptors (`struct dsc$descriptor_s`). Return values are 32-bit VMS condition codes (odd = success, even = error) unless documented otherwise.

**Headers:**
- `lib$routines.h` -- LIB$ routine prototypes and condition values
- `str$routines.h` -- STR$ routine prototypes and condition values
- `mth$routines.h` -- MTH$ routine prototypes and condition values
- `ots$routines.h` -- OTS$ routine prototypes and condition values

---

## LIB$ -- General-Purpose Library Routines

### Memory Management

#### lib$get_vm -- Allocate Virtual Memory

```c
uint32_t lib$get_vm(const uint32_t *num_bytes, void **base_adr, ...);
```

Allocates contiguous virtual memory from a zone. Uses the default zone (zone 0) if the optional zone_id is NULL or points to 0. Zone 0 uses quick-fit with lookaside lists for small allocations (8-2048 bytes); larger allocations get their own mmap region.

| Parameter | Type | Description |
|-----------|------|-------------|
| num_bytes | `const uint32_t *` | Number of bytes to allocate |
| base_adr | `void **` | Receives address of allocated memory |
| zone_id | `const uint32_t *` | (Optional) Zone identifier |

**Returns:** SS$_NORMAL, SS$_INSFMEM, SS$_BADPARAM, LIB$_BADZONE

**Status:** Fully implemented with zone support, lookaside lists, and mmap-backed extents.

#### lib$free_vm -- Free Virtual Memory

```c
uint32_t lib$free_vm(const uint32_t *num_bytes, void **base_adr, ...);
```

Returns memory to a zone's lookaside list (small blocks) or marks it freed (large blocks, reclaimed on zone deletion). Sets `*base_adr` to NULL on success.

| Parameter | Type | Description |
|-----------|------|-------------|
| num_bytes | `const uint32_t *` | Number of bytes (used for validation only) |
| base_adr | `void **` | Address of memory to free (set to NULL on return) |
| zone_id | `const uint32_t *` | (Optional) Zone identifier |

**Returns:** SS$_NORMAL, SS$_BADPARAM, LIB$_BADZONE

**Status:** Fully implemented.

#### lib$get_vm_page -- Allocate in Page Units

```c
uint32_t lib$get_vm_page(const uint32_t *num_pages, void **base_adr);
```

Allocates memory in VMS page units (512 bytes each). Wrapper around lib$get_vm.

| Parameter | Type | Description |
|-----------|------|-------------|
| num_pages | `const uint32_t *` | Number of 512-byte pages |
| base_adr | `void **` | Receives address of allocated pages |

**Returns:** SS$_NORMAL, SS$_INSFMEM, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$free_vm_page -- Free Page-Unit Allocation

```c
uint32_t lib$free_vm_page(const uint32_t *num_pages, void **base_adr);
```

Frees memory allocated by lib$get_vm_page. Wrapper around lib$free_vm.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$create_vm_zone -- Create a Memory Zone

```c
uint32_t lib$create_vm_zone(uint32_t *zone_id, ...);
```

Creates an isolated memory zone. Up to 64 zones supported (zone 0 is the default, always active). All zones use quick-fit allocation.

| Parameter | Type | Description |
|-----------|------|-------------|
| zone_id | `uint32_t *` | Receives the zone identifier |

**Returns:** SS$_NORMAL, SS$_INSFMEM, SS$_BADPARAM

**Status:** Fully implemented. Optional VMS zone creation parameters (algorithm, flags) are accepted but ignored.

#### lib$delete_vm_zone -- Delete a Memory Zone

```c
uint32_t lib$delete_vm_zone(const uint32_t *zone_id);
```

Bulk-frees all memory in a zone by munmap'ing all extents. Zone 0 (default) cannot be deleted.

| Parameter | Type | Description |
|-----------|------|-------------|
| zone_id | `const uint32_t *` | Zone identifier to delete |

**Returns:** SS$_NORMAL, SS$_BADPARAM, LIB$_BADZONE

**Status:** Fully implemented.

---

### Terminal I/O

#### lib$put_output -- Write to SYS$OUTPUT

```c
uint32_t lib$put_output(const struct dsc$descriptor_s *message_string);
```

Writes the descriptor string to stdout followed by a newline.

| Parameter | Type | Description |
|-----------|------|-------------|
| message_string | `const struct dsc$descriptor_s *` | String to output |

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$get_input -- Read from SYS$INPUT

```c
uint32_t lib$get_input(struct dsc$descriptor_s *resultant_string,
                       const struct dsc$descriptor_s *prompt_string,
                       uint16_t *resultant_length);
```

Reads a line from stdin. If prompt_string is provided, displays it first. Static descriptors are space-padded to their full length.

| Parameter | Type | Description |
|-----------|------|-------------|
| resultant_string | `struct dsc$descriptor_s *` | Receives input string |
| prompt_string | `const struct dsc$descriptor_s *` | (Optional) Prompt to display |
| resultant_length | `uint16_t *` | (Optional) Receives actual length read |

**Returns:** SS$_NORMAL, SS$_ENDOFFILE, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$put_common -- Write to Process Common

```c
uint32_t lib$put_common(const struct dsc$descriptor_s *string);
```

Writes a record to the process common area.

**Status:** Declared in header. Not implemented in current RTL sources.

#### lib$get_common -- Read from Process Common

```c
uint32_t lib$get_common(struct dsc$descriptor_s *resultant_string,
                        uint16_t *resultant_length);
```

Reads a record from the process common area.

**Status:** Declared in header. Not implemented in current RTL sources.

---

### Condition Handling

#### lib$signal -- Signal a Condition

```c
uint32_t lib$signal(uint32_t condition_value, ...);
```

Signals a condition by walking the thread-local handler chain. If no handler claims the condition (SS$_CONTINUE), the default handler prints the message. Severe conditions cause process exit; other severities continue execution.

| Parameter | Type | Description |
|-----------|------|-------------|
| condition_value | `uint32_t` | VMS condition code |
| ... | | Optional FAO arguments |

**Returns:** SS$_NORMAL if a handler claimed it. Does not return for unclaimed SEVERE conditions.

**Status:** Fully implemented with thread-local handler chain (max 64 handlers).

#### lib$stop -- Signal and Force Exit

```c
uint32_t lib$stop(uint32_t condition_value, ...);
```

Like lib$signal but always exits the process if no handler claims the condition, regardless of severity.

**Returns:** SS$_NORMAL if a handler claimed it. Does not return otherwise.

**Status:** Fully implemented.

#### lib$sig_to_ret -- Convert Signal to Return Status

```c
uint32_t lib$sig_to_ret(void *signal_args, void *mechanism_args);
```

A condition handler that converts a signaled condition into a return status. Stores the condition value in the mechanism array's saved R0 field and returns SS$_CONTINUE.

**Usage:**
```c
lib$establish(lib$sig_to_ret);
status = some_function();  /* signals convert to return value */
lib$revert();
```

**Returns:** SS$_CONTINUE (always)

**Status:** Fully implemented.

#### lib$establish -- Establish a Condition Handler

```c
void *lib$establish(void *handler);
```

Pushes a handler onto the thread-local handler stack.

| Parameter | Type | Description |
|-----------|------|-------------|
| handler | `void *` | Pointer to handler function |

**Returns:** Address of previously established handler, or NULL.

**Status:** Fully implemented.

#### lib$revert -- Revert Condition Handler

```c
uint32_t lib$revert(void);
```

Pops the most recent handler from the stack.

**Returns:** SS$_NORMAL

**Status:** Fully implemented.

---

### String Formatting

#### lib$sys_fao -- Formatted ASCII Output

```c
uint32_t lib$sys_fao(const struct dsc$descriptor_s *ctrstr,
                     uint16_t *outlen,
                     struct dsc$descriptor_s *outbuf, ...);
```

Formats output using VMS FAO directives. Wrapper around sys$faol.

Supported directives: `!AS`, `!AD`, `!SL`, `!UL`, `!SW`, `!UW`, `!SB`, `!UB`, `!XL`, `!XW`, `!XB`, `!OL`, `!ZL`, `!/`, `!_`, `!!`, `!n*c`, `!n<`, `!n>`.

| Parameter | Type | Description |
|-----------|------|-------------|
| ctrstr | `const struct dsc$descriptor_s *` | FAO control string |
| outlen | `uint16_t *` | (Optional) Receives output length |
| outbuf | `struct dsc$descriptor_s *` | Output buffer descriptor |
| ... | | Directive arguments |

**Returns:** SS$_NORMAL, SS$_BUFFEROVF

**Status:** Fully implemented (delegates to sys$faol).

#### lib$sys_faol -- Formatted ASCII Output with Argument List

```c
uint32_t lib$sys_faol(const struct dsc$descriptor_s *ctrstr,
                      uint16_t *outlen,
                      struct dsc$descriptor_s *outbuf,
                      const uint32_t *prmlst);
```

Like lib$sys_fao but takes an explicit argument array instead of varargs.

**Returns:** SS$_NORMAL, SS$_BUFFEROVF

**Status:** Fully implemented (delegates to sys$faol).

---

### Date/Time

#### lib$date_time -- Get Current Date/Time String

```c
uint32_t lib$date_time(struct dsc$descriptor_s *date_time_string);
```

Returns the current local time in VMS format: `"DD-MMM-YYYY HH:MM:SS.00"`.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented. Centiseconds are always `.00` (not tracked).

#### lib$day -- Get Day Number

```c
uint32_t lib$day(int32_t *days, const void *timadr, int32_t *day_time);
```

Returns the number of days since the VMS base date (November 17, 1858). If timadr is NULL, uses the current time.

| Parameter | Type | Description |
|-----------|------|-------------|
| days | `int32_t *` | Receives day count since base date |
| timadr | `const void *` | (Optional) VMS quadword time, or NULL for current |
| day_time | `int32_t *` | (Optional) Receives day-of-year (1-366) |

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$day_of_week -- Get Day of Week

```c
uint32_t lib$day_of_week(const void *timadr, int32_t *day_of_week);
```

Returns the day of the week: 1=Monday through 7=Sunday (VMS convention).

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$sub_times -- Subtract Quadword Times

```c
uint32_t lib$sub_times(const void *time1, const void *time2, void *result);
```

Computes time1 - time2 as quadword delta times.

**Returns:** SS$_NORMAL, LIB$_NEGTIM

**Status:** Declared in header. Not implemented in current RTL sources.

#### lib$add_times -- Add Quadword Times

```c
uint32_t lib$add_times(const void *time1, const void *time2, void *result);
```

**Status:** Declared in header. Not implemented in current RTL sources.

#### lib$mult_delta_time -- Multiply Delta Time by Scalar

```c
uint32_t lib$mult_delta_time(const int32_t *multiplier, void *timadr);
```

**Status:** Declared in header. Not implemented in current RTL sources.

#### lib$cvt_from_internal_time -- Convert from Internal Time

```c
uint32_t lib$cvt_from_internal_time(const uint32_t *operation,
                                     uint32_t *result, const void *time);
```

**Returns:** SS$_NORMAL (stub -- does no actual conversion)

**Status:** Stub. Always returns SS$_NORMAL without modifying result.

---

### Conversion

#### lib$cvt_dtb -- Decimal Text to Binary

```c
uint32_t lib$cvt_dtb(int32_t ndigits, const char *string, int32_t *value);
```

Parses `ndigits` decimal characters and converts to a 32-bit integer.

**Returns:** SS$_NORMAL, SS$_BADPARAM (on non-digit character)

**Status:** Fully implemented.

#### lib$cvt_htb -- Hexadecimal Text to Binary

```c
uint32_t lib$cvt_htb(int32_t ndigits, const char *string, int32_t *value);
```

Parses `ndigits` hex characters (case-insensitive) and converts to a 32-bit integer.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$cvt_otb -- Octal Text to Binary

```c
uint32_t lib$cvt_otb(int32_t ndigits, const char *string, int32_t *value);
```

Parses `ndigits` octal characters (0-7) and converts to a 32-bit integer.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

---

### Process and System Information

#### lib$getjpi -- Get Job/Process Information

```c
uint32_t lib$getjpi(const uint32_t *item_code, const uint32_t *pid,
                    const struct dsc$descriptor_s *prcnam,
                    void *resultant_value,
                    struct dsc$descriptor_s *resultant_string,
                    uint16_t *resultant_length);
```

Simplified wrapper around sys$getjpiw for single-item queries. Pass either resultant_value (numeric) or resultant_string (string), not both.

**Returns:** SS$_NORMAL, SS$_BADPARAM, SS$_NONEXPR

**Status:** Fully implemented (delegates to sys$getjpiw).

#### lib$getsyi -- Get System Information

```c
uint32_t lib$getsyi(const uint32_t *item_code, void *resultant_value,
                    struct dsc$descriptor_s *resultant_string,
                    uint16_t *resultant_length,
                    const uint32_t *cluster_id,
                    const struct dsc$descriptor_s *node_name);
```

Simplified wrapper around sys$getsyiw for single-item queries.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented (delegates to sys$getsyiw).

---

### Symbol and CLI

#### lib$get_symbol -- Get CLI Symbol Value

```c
uint32_t lib$get_symbol(const struct dsc$descriptor_s *symbol,
                        struct dsc$descriptor_s *value,
                        uint16_t *value_len, uint32_t *table_type);
```

Retrieves the value of a DCL symbol.

**Status:** Declared in header. Implementation is in the DCL layer.

#### lib$set_symbol -- Set CLI Symbol Value

```c
uint32_t lib$set_symbol(const struct dsc$descriptor_s *symbol,
                        const struct dsc$descriptor_s *value,
                        const uint32_t *table_type);
```

Sets a DCL symbol value. table_type: LIB$K_CLI_LOCAL_SYM (1) or LIB$K_CLI_GLOBAL_SYM (2).

**Status:** Declared in header. Implementation is in the DCL layer.

---

### Subprocess and File Operations

#### lib$spawn -- Spawn a Subprocess

```c
uint32_t lib$spawn(const struct dsc$descriptor_s *command_string,
                   const struct dsc$descriptor_s *input_file,
                   const struct dsc$descriptor_s *output_file,
                   const uint32_t *flags,
                   const struct dsc$descriptor_s *process_name,
                   uint32_t *process_id, uint32_t *completion_code,
                   const uint32_t *event_flag,
                   void *ast_routine, void *ast_argument,
                   const struct dsc$descriptor_s *prompt_string,
                   const struct dsc$descriptor_s *cli_name,
                   const struct dsc$descriptor_s *table_name);
```

Spawns a subprocess via fork/exec of `/bin/sh`. Supports command execution and I/O redirection. Waits for completion.

| Parameter | Type | Description |
|-----------|------|-------------|
| command_string | descriptor | Command to execute (NULL for interactive shell) |
| input_file | descriptor | (Optional) SYS$INPUT redirection |
| output_file | descriptor | (Optional) SYS$OUTPUT redirection |
| process_id | `uint32_t *` | (Optional) Receives child PID |
| completion_code | `uint32_t *` | (Optional) Receives exit status |

**Note:** flags, process_name, event_flag, ast_routine, ast_argument, prompt_string, cli_name, and table_name are accepted but ignored.

**Returns:** SS$_NORMAL, SS$_INSFMEM

**Status:** Partial. Core spawn/wait works. VMS-specific flags and async mode not implemented.

#### lib$find_file -- Find File by Wildcard

```c
uint32_t lib$find_file(const struct dsc$descriptor_s *filespec,
                       struct dsc$descriptor_s *resultspec,
                       uint32_t *context);
```

Iterative file search using glob(). On first call (`*context == 0`), performs glob expansion. On subsequent calls, returns the next match. Supports both CLASS_S and CLASS_D result descriptors.

| Parameter | Type | Description |
|-----------|------|-------------|
| filespec | descriptor | File specification with wildcards |
| resultspec | descriptor | Receives matched filename |
| context | `uint32_t *` | Must be 0 on first call |

**Returns:** RMS$_NORMAL (found), RMS$_NMF (no more files), SS$_BADPARAM, SS$_INSFMEM

**Status:** Fully implemented.

#### lib$find_file_end -- End File Search

```c
uint32_t lib$find_file_end(uint32_t *context);
```

Frees the glob result and resets context to 0. Must be called after lib$find_file is done.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$rename_file -- Rename a File

```c
uint32_t lib$rename_file(const struct dsc$descriptor_s *old_filespec,
                         const struct dsc$descriptor_s *new_filespec);
```

**Status:** Declared in header. Not implemented in current RTL sources.

#### lib$delete_file -- Delete a File

```c
uint32_t lib$delete_file(const struct dsc$descriptor_s *filespec);
```

**Status:** Declared in header. Not implemented in current RTL sources.

---

### Table-Driven Parser

#### lib$tparse -- Table-Driven Parser

```c
uint32_t lib$tparse(void *tparse_block, const void *state_table,
                    const void *key_table);
```

**Returns:** SS$_UNSUPPORTED

**Status:** Stub. Returns SS$_UNSUPPORTED without processing.

---

### Bit and Arithmetic Operations

#### lib$adawi -- Add Aligned Word Interlocked

```c
uint32_t lib$adawi(const int16_t *add, int16_t *sum, int16_t *sign);
```

Atomically adds `*add` to `*sum` using C11 stdatomic. Sets `*sign` to -1, 0, or 1 based on the result.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented with atomic operations.

#### lib$bbcci -- Branch on Bit Clear and Clear Interlocked

```c
uint32_t lib$bbcci(const int32_t *bit_position, void *base_address);
```

Atomically tests and clears a bit. Returns the old bit value (0 or 1), not a status code.

**Returns:** 0 or 1 (old bit value)

**Status:** Fully implemented with atomic operations.

#### lib$bbssi -- Branch on Bit Set and Set Interlocked

```c
uint32_t lib$bbssi(const int32_t *bit_position, void *base_address);
```

Atomically tests and sets a bit. Returns the old bit value (0 or 1), not a status code.

**Returns:** 0 or 1 (old bit value)

**Status:** Fully implemented with atomic operations.

#### lib$extv -- Extract Signed Bit Field

```c
int32_t lib$extv(const int32_t *pos, const uint8_t *size,
                 const void *base_address);
```

Extracts a bit field of `*size` bits starting at bit `*pos` from a byte array. The result is sign-extended to 32 bits. Fields may span byte boundaries. Max field size is 32 bits.

**Returns:** Sign-extended field value (not a status code)

**Status:** Fully implemented.

#### lib$extzv -- Extract Zero-Extended Bit Field

```c
uint32_t lib$extzv(const int32_t *pos, const uint8_t *size,
                   const void *base_address);
```

Like lib$extv but zero-extends instead of sign-extending.

**Returns:** Zero-extended field value (not a status code)

**Status:** Fully implemented.

#### lib$insv -- Insert Bit Field

```c
uint32_t lib$insv(const int32_t *source, const int32_t *pos,
                  const uint8_t *size, void *base_address);
```

Inserts the low `*size` bits of `*source` into the byte array at bit position `*pos`.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

---

### Event Flags

#### lib$get_ef -- Allocate Event Flag

```c
uint32_t lib$get_ef(uint32_t *efn);
```

Allocates the first available event flag from the range 24-63 (flags 0-23 are reserved for system use). Thread-safe via mutex.

**Returns:** SS$_NORMAL, SS$_BADPARAM, LIB$_INSEF

**Status:** Fully implemented.

#### lib$free_ef -- Release Event Flag

```c
uint32_t lib$free_ef(const uint32_t *efn);
```

Releases a previously allocated event flag.

**Returns:** SS$_NORMAL, SS$_BADPARAM, LIB$_EF_RESSYS, LIB$_EF_ALRFRE

**Status:** Fully implemented.

---

### String and Character Operations

#### lib$ichar -- Integer Value of First Character

```c
uint32_t lib$ichar(const struct dsc$descriptor_s *str);
```

Returns the ASCII value of the first character. Returns 0 for empty or invalid descriptors. This is a value return, not a status code.

**Status:** Fully implemented.

#### lib$index -- Find Substring Position

```c
uint32_t lib$index(const struct dsc$descriptor_s *str,
                   const struct dsc$descriptor_s *sub);
```

Returns the 1-based position of the first occurrence of `sub` in `str`, or 0 if not found. Empty substring matches at position 1.

**Status:** Fully implemented.

#### lib$len -- Length Excluding Trailing Spaces

```c
uint32_t lib$len(const struct dsc$descriptor_s *str);
```

Returns the string length not counting trailing spaces. This is a value return, not a status code.

**Status:** Fully implemented.

#### lib$locc -- Locate Character

```c
uint32_t lib$locc(const struct dsc$descriptor_s *char_to_find,
                  const struct dsc$descriptor_s *str);
```

Searches `str` for the first character of `char_to_find`. Returns 1-based position or 0 if not found.

**Status:** Fully implemented.

#### lib$matchc -- Match Characters (Substring Search)

```c
uint32_t lib$matchc(const struct dsc$descriptor_s *sub,
                    const struct dsc$descriptor_s *str);
```

Searches `str` for `sub`. Returns the 1-based position of the character AFTER the match end (position + length), or 0 if not found. This differs from lib$index which returns the match start.

**Status:** Fully implemented.

#### lib$movc3 -- Move Characters (3-Argument)

```c
uint32_t lib$movc3(const uint16_t *len, const void *src, void *dst);
```

Copies `*len` bytes from src to dst. Handles overlapping regions (uses memmove).

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$movc5 -- Move Characters with Fill (5-Argument)

```c
uint32_t lib$movc5(const uint16_t *src_len, const void *src,
                   const char *fill_char, const uint16_t *dst_len, void *dst);
```

Copies min(src_len, dst_len) bytes from src to dst. If dst is longer, remaining bytes are filled with `*fill_char`.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### lib$spanc -- Span Characters

```c
uint32_t lib$spanc(const struct dsc$descriptor_s *str,
                   const unsigned char *table,
                   const unsigned char *mask);
```

Scans `str` while `(table[char] & *mask) != 0`. Returns the 1-based position of the first non-matching character, or 0 if all matched.

**Status:** Fully implemented.

#### lib$lp_lines -- Lines Per Page

```c
uint32_t lib$lp_lines(void);
```

Returns the default lines per page for listing output. Always returns 66 (standard VMS default).

**Status:** Fully implemented (hardcoded to 66).

---

## STR$ -- String Manipulation Routines

All STR$ routines accept any descriptor class for input. Output descriptors must be CLASS_S (fixed-length, may truncate with space-padding) or CLASS_D (dynamic, resized automatically).

### String Copy

#### str$copy_dx -- Copy String by Descriptor

```c
uint32_t str$copy_dx(struct dsc$descriptor_s *dest,
                     const struct dsc$descriptor_s *src);
```

Copies src to dest. CLASS_D destinations are reallocated. CLASS_S destinations are truncated or space-padded.

**Returns:** SS$_NORMAL, STR$_TRU (truncated), SS$_BADPARAM

**Status:** Fully implemented.

#### str$copy_r -- Copy String by Reference

```c
uint32_t str$copy_r(struct dsc$descriptor_s *dest,
                    const uint16_t *srclen, const char *srcadr);
```

Copies a raw buffer (length + address) into a descriptor.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

### Concatenation

#### str$concat -- Concatenate Strings

```c
uint32_t str$concat(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src1,
                    const struct dsc$descriptor_s *src2, ...);
```

Concatenates src1 and src2 into dest. Supports both CLASS_S and CLASS_D destinations.

**Returns:** SS$_NORMAL, STR$_TRU (truncated), SS$_BADPARAM

**Status:** Fully implemented for two sources. Additional varargs sources beyond src2 are not consumed.

#### str$append -- Append to String

```c
uint32_t str$append(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src);
```

Appends src to dest. Requires CLASS_D destination.

**Returns:** SS$_NORMAL, SS$_BADPARAM, SS$_INSFMEM

**Status:** Fully implemented (CLASS_D only).

#### str$prefix -- Prepend to String

```c
uint32_t str$prefix(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src);
```

Prepends src to dest. Requires CLASS_D destination.

**Returns:** SS$_NORMAL, SS$_BADPARAM, SS$_INSFMEM

**Status:** Fully implemented (CLASS_D only).

### Comparison

#### str$compare -- Lexicographic Compare

```c
int32_t str$compare(const struct dsc$descriptor_s *str1,
                    const struct dsc$descriptor_s *str2);
```

Case-sensitive comparison. Shorter strings are less than longer ones when the common prefix matches.

**Returns:** -1, 0, or 1 (not a status code)

**Status:** Fully implemented.

#### str$compare_eql -- Equality Compare

```c
int32_t str$compare_eql(const struct dsc$descriptor_s *str1,
                         const struct dsc$descriptor_s *str2);
```

Tests exact equality: same length and same content. Does NOT pad shorter strings.

**Returns:** 0 if equal, non-zero if different (not a status code)

**Status:** Fully implemented.

#### str$case_blind_compare -- Case-Insensitive Compare

```c
int32_t str$case_blind_compare(const struct dsc$descriptor_s *str1,
                                const struct dsc$descriptor_s *str2);
```

**Returns:** -1, 0, or 1 (not a status code)

**Status:** Fully implemented.

#### str$compare_multi -- Multinational Compare

```c
int32_t str$compare_multi(const struct dsc$descriptor_s *str1,
                          const struct dsc$descriptor_s *str2,
                          const uint32_t *flags,
                          const struct dsc$descriptor_s *locale);
```

**Returns:** -1, 0, or 1 (not a status code)

**Status:** Stub. Falls back to str$compare; flags and locale are ignored.

### Extraction and Manipulation

#### str$left -- Extract Left Portion

```c
uint32_t str$left(struct dsc$descriptor_s *dest,
                  const struct dsc$descriptor_s *src,
                  const uint16_t *end_pos);
```

Copies characters 1 through `*end_pos` from src to dest.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$right -- Extract Right Portion

```c
uint32_t str$right(struct dsc$descriptor_s *dest,
                   const struct dsc$descriptor_s *src,
                   const uint16_t *start_pos);
```

Copies characters from `*start_pos` through end of src to dest. Position is 1-based.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$len_extr -- Extract by Position and Length

```c
uint32_t str$len_extr(struct dsc$descriptor_s *dest,
                      const struct dsc$descriptor_s *src,
                      const uint32_t *start, const uint32_t *len);
```

Extracts `*len` characters starting at 1-based position `*start`.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$element -- Extract Delimited Element

```c
uint32_t str$element(struct dsc$descriptor_s *dest,
                     const uint32_t *element,
                     const struct dsc$descriptor_s *delimiter,
                     const struct dsc$descriptor_s *src);
```

Extracts the `*element`-th (0-based) element from src, where elements are separated by the first character of the delimiter descriptor. If the element is not found, copies the delimiter to dest.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$replace -- Replace Portion of String

```c
uint32_t str$replace(struct dsc$descriptor_s *dest,
                     const struct dsc$descriptor_s *src,
                     const uint32_t *start_pos, const uint32_t *end_pos,
                     const struct dsc$descriptor_s *rep);
```

Replaces characters from `*start_pos` through `*end_pos` (1-based) in src with rep. Positions are clamped to valid range.

**Returns:** SS$_NORMAL, SS$_BADPARAM, SS$_INSFMEM

**Status:** Fully implemented for both CLASS_S and CLASS_D destinations.

### Search

#### str$find_first_substring -- Find First Substring

```c
uint32_t str$find_first_substring(const struct dsc$descriptor_s *src,
                                   uint32_t *index, uint32_t *sub_index,
                                   const struct dsc$descriptor_s *sub, ...);
```

Searches src for sub. Sets `*index` to the 1-based position and `*sub_index` to 1 if found. Both are 0 if not found.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Partially implemented. Only searches for the first substring argument; additional varargs substrings are not consumed.

#### str$position -- Find Substring Position

```c
uint32_t str$position(const struct dsc$descriptor_s *src,
                      const struct dsc$descriptor_s *sub,
                      const uint32_t *start);
```

Returns the 1-based position of sub in src starting from `*start` (1-based, default 1). Returns 0 if not found.

**Returns:** Position (not a status code)

**Status:** Fully implemented.

#### str$match_wild -- Wildcard Pattern Match

```c
uint32_t str$match_wild(const struct dsc$descriptor_s *candidate,
                        const struct dsc$descriptor_s *pattern);
```

Tests if candidate matches pattern. `*` matches any sequence, `%` matches any single character.

**Returns:** STR$_MATCH or STR$_NOMATCH

**Status:** Fully implemented.

### Modification

#### str$trim -- Remove Trailing Whitespace

```c
uint32_t str$trim(struct dsc$descriptor_s *dest,
                  const struct dsc$descriptor_s *src, uint16_t *result_len);
```

Copies src to dest with trailing spaces removed.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$upcase -- Convert to Uppercase

```c
uint32_t str$upcase(struct dsc$descriptor_s *dest,
                    const struct dsc$descriptor_s *src);
```

Copies src to dest, converting all characters to uppercase.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Fully implemented.

#### str$translate -- Translate Characters

```c
uint32_t str$translate(struct dsc$descriptor_s *dest,
                       const struct dsc$descriptor_s *src,
                       const struct dsc$descriptor_s *trans_table,
                       const struct dsc$descriptor_s *match_table);
```

**Returns:** SS$_UNSUPPORTED

**Status:** Stub. Not implemented.

### Dynamic String Management

#### str$free1_dx -- Free Dynamic String

```c
uint32_t str$free1_dx(struct dsc$descriptor_d *desc);
```

Releases storage for a CLASS_D descriptor and resets it to zero length.

**Returns:** SS$_NORMAL

**Status:** Fully implemented.

#### str$get1_dx -- Allocate Dynamic String

```c
uint32_t str$get1_dx(const uint16_t *length, struct dsc$descriptor_d *desc);
```

Allocates `*length` bytes for a CLASS_D descriptor.

**Returns:** SS$_NORMAL, SS$_BADPARAM, STR$_INSVIRMEM

**Status:** Fully implemented.

### Analysis

#### str$analyze_sdesc -- Analyze String Descriptor

```c
uint32_t str$analyze_sdesc(const struct dsc$descriptor_s *desc,
                           uint16_t *length, char **addr);
```

Extracts the length and data address from any descriptor class (CLASS_S, CLASS_D, CLASS_A supported).

**Returns:** SS$_NORMAL, SS$_BADPARAM, STR$_ILLSTRCLA

**Status:** Fully implemented for CLASS_S, CLASS_D, and CLASS_A.

---

## MTH$ -- Mathematics Routines

All MTH$ routines take arguments by reference (pointer to value) per VMS calling convention. Return values are returned by value. Double-precision routines use `double`, single-precision use `float`.

All routines delegate to the corresponding C math.h function. No VMS-specific error signaling (MTH$_INVARGMAT etc.) is implemented; C math library error handling applies.

### Trigonometric (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$sin` | `double mth$sin(const double *x)` | Sine (radians) |
| `mth$cos` | `double mth$cos(const double *x)` | Cosine (radians) |
| `mth$tan` | `double mth$tan(const double *x)` | Tangent (radians) |
| `mth$sincos` | `void mth$sincos(const double *x, double *sin_val, double *cos_val)` | Simultaneous sin and cos |

**Status:** Fully implemented.

### Trigonometric (Degrees)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$sind` | `double mth$sind(const double *x)` | Sine (degrees) |
| `mth$cosd` | `double mth$cosd(const double *x)` | Cosine (degrees) |
| `mth$tand` | `double mth$tand(const double *x)` | Tangent (degrees) |

**Status:** Fully implemented. Converts degrees to radians internally.

### Inverse Trigonometric (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$asin` | `double mth$asin(const double *x)` | Arc sine |
| `mth$acos` | `double mth$acos(const double *x)` | Arc cosine |
| `mth$atan` | `double mth$atan(const double *x)` | Arc tangent |
| `mth$atan2` | `double mth$atan2(const double *y, const double *x)` | Arc tangent of y/x |

**Status:** Fully implemented.

### Exponential and Logarithmic (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$exp` | `double mth$exp(const double *x)` | e^x |
| `mth$alog` | `double mth$alog(const double *x)` | Natural logarithm (ln) |
| `mth$alog10` | `double mth$alog10(const double *x)` | Common logarithm (log10) |
| `mth$alog2` | `double mth$alog2(const double *x)` | Base-2 logarithm |

**Status:** Fully implemented.

### Power and Root (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$sqrt` | `double mth$sqrt(const double *x)` | Square root |
| `mth$power` | `double mth$power(const double *base, const double *exp)` | base^exp |
| `mth$power_ji` | `int32_t mth$power_ji(const int32_t *base, const int32_t *exp)` | Integer base^exp |

**Status:** Fully implemented. mth$power_ji uses fast exponentiation by squaring.

### Hyperbolic (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$sinh` | `double mth$sinh(const double *x)` | Hyperbolic sine |
| `mth$cosh` | `double mth$cosh(const double *x)` | Hyperbolic cosine |
| `mth$tanh` | `double mth$tanh(const double *x)` | Hyperbolic tangent |

**Status:** Fully implemented.

### Miscellaneous (Double Precision)

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$abs` | `double mth$abs(const double *x)` | Absolute value |
| `mth$sign` | `double mth$sign(const double *x, const double *y)` | \|x\| with sign of y |
| `mth$nint` | `int32_t mth$nint(const double *x)` | Nearest integer |
| `mth$floor` | `double mth$floor(const double *x)` | Floor |
| `mth$ceil` | `double mth$ceil(const double *x)` | Ceiling |
| `mth$mod` | `double mth$mod(const double *x, const double *y)` | Modulus (fmod) |
| `mth$max` | `double mth$max(const double *x, const double *y)` | Maximum |
| `mth$min` | `double mth$min(const double *x, const double *y)` | Minimum |

**Status:** Fully implemented.

### Random Number Generation

#### mth$random -- Uniform Random Number

```c
float mth$random(uint32_t *seed);
```

Multiplicative congruential generator. Updates `*seed` in place. Returns a float in [0.0, 1.0).

**Status:** Fully implemented.

### Single-Precision Variants

| Routine | Signature | Description |
|---------|-----------|-------------|
| `mth$sinf` | `float mth$sinf(const float *x)` | Sine |
| `mth$cosf` | `float mth$cosf(const float *x)` | Cosine |
| `mth$tanf` | `float mth$tanf(const float *x)` | Tangent |
| `mth$asinf` | `float mth$asinf(const float *x)` | Arc sine |
| `mth$acosf` | `float mth$acosf(const float *x)` | Arc cosine |
| `mth$atanf` | `float mth$atanf(const float *x)` | Arc tangent |
| `mth$atan2f` | `float mth$atan2f(const float *y, const float *x)` | Arc tangent of y/x |
| `mth$expf` | `float mth$expf(const float *x)` | Exponential |
| `mth$alogf` | `float mth$alogf(const float *x)` | Natural logarithm |
| `mth$alog10f` | `float mth$alog10f(const float *x)` | Common logarithm |
| `mth$sqrtf` | `float mth$sqrtf(const float *x)` | Square root |
| `mth$absf` | `float mth$absf(const float *x)` | Absolute value |

**Status:** Fully implemented.

---

## OTS$ -- Object Time System Routines

OTS$ routines provide compiler-support functions for data type conversion, string operations, and arithmetic. They are typically called by compiler-generated code but are available for direct use.

### Text-to-Integer Conversion

These routines are declared in the header but not all have implementations in the current source.

| Routine | Header Signature | Status |
|---------|-----------------|--------|
| `ots$cvt_tu_l` | `uint32_t ots$cvt_tu_l(desc, dest, size, flags)` | Declared only |
| `ots$cvt_ti_l` | `uint32_t ots$cvt_ti_l(desc, dest, size, flags)` | Declared only |
| `ots$cvt_tl_l` | `uint32_t ots$cvt_tl_l(desc, dest, size, flags)` | Declared only |
| `ots$cvt_to_l` | `uint32_t ots$cvt_to_l(desc, dest, size, flags)` | Declared only |
| `ots$cvt_tz_l` | `uint32_t ots$cvt_tz_l(desc, dest, size, flags)` | Declared only |
| `ots$cvt_tb_l` | `uint32_t ots$cvt_tb_l(desc, dest, size, flags)` | Declared only |

**Note:** The implemented `ots$cvt_ti_l` in ots_routines.c has a different signature from the header declaration. The implementation takes `(desc, int32_t*, flags)` and converts decimal text to a signed longword via strtol.

### Text-to-Floating Conversion

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$cvt_t_f` | Text to F_floating (float) | Declared only |
| `ots$cvt_t_d` | Text to D_floating (double) | Declared only |
| `ots$cvt_t_g` | Text to G_floating (double) | Declared only |
| `ots$cvt_t_h` | Text to H_floating (long double) | Declared only |
| `ots$cvt_t_t` | Text to IEEE T_floating (double) | Declared only |
| `ots$cvt_t_s` | Text to IEEE S_floating (float) | Declared only |

### Integer-to-Text Conversion

#### ots$cvt_l_ti -- Integer to Signed Decimal Text

```c
uint32_t ots$cvt_l_ti(const int32_t *value, struct dsc$descriptor_s *dest,
                       const int32_t *min_digits, const int32_t *size,
                       const uint32_t *flags);
```

Formats `*value` as a right-justified decimal string in the descriptor, padding with leading spaces. min_digits, size, and flags parameters are accepted but not used.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Implemented. min_digits/size/flags ignored.

#### ots$cvt_l_tz -- Integer to Hexadecimal Text

```c
uint32_t ots$cvt_l_tz(const int32_t *value, struct dsc$descriptor_s *dest,
                       const int32_t *min_digits);
```

Formats `*value` as a right-justified uppercase hex string. min_digits is accepted but not used.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Implemented. min_digits ignored.

#### ots$cvt_l_to -- Integer to Octal Text

```c
uint32_t ots$cvt_l_to(const int32_t *value, struct dsc$descriptor_s *dest,
                       const int32_t *min_digits);
```

Formats `*value` as a right-justified octal string.

**Returns:** SS$_NORMAL, SS$_BADPARAM

**Status:** Implemented. min_digits ignored.

#### ots$cvt_l_tl, ots$cvt_l_tu, ots$cvt_l_tb

Declared in the header but not implemented in current sources.

### Floating-to-Text Conversion

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$cnvout_f` | F_floating to text | Declared only |
| `ots$cnvout_d` | D_floating to text | Declared only |
| `ots$cnvout_g` | G_floating to text | Declared only |
| `ots$cnvout_t` | IEEE T_floating to text | Declared only |

### String Copy

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$scopy_dxdx` | Copy descriptor to descriptor | Declared only |
| `ots$scopy_r_dx` | Copy buffer to descriptor | Declared only |

### Dynamic String Management

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$sget1_dd` | Allocate dynamic string | Declared only |
| `ots$sfree1_dd` | Free dynamic string | Declared only |
| `ots$sfreen_dd` | Free array of dynamic strings | Declared only |

### Memory Move

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$move3` | Copy memory (3-arg) | Declared only |
| `ots$move5` | Copy memory with fill (5-arg) | Declared only |

### Integer Power

| Routine | Description | Status |
|---------|-------------|--------|
| `ots$powjj` | Integer base^exponent | Declared only |

### Complex Arithmetic

The following are compiler-support routines for FORTRAN COMPLEX arithmetic. They are declared as extern symbols, not callable functions, and use architecture-specific register-return conventions.

| Symbol | Description | Status |
|--------|-------------|--------|
| `ots$divcg_r3` | Divide complex G_floating | Declared only |
| `ots$mulcg_r3` | Multiply complex G_floating | Declared only |
| `ots$powcgcg_r3` | Complex G ** complex G | Declared only |
| `ots$powcgj_r3` | Complex G ** integer | Declared only |
| `ots$divct_r3` | Divide complex T_floating | Declared only |
| `ots$mulct_r3` | Multiply complex T_floating | Declared only |
| `ots$powctct_r3` | Complex T ** complex T | Declared only |
| `ots$powctj_r3` | Complex T ** integer | Declared only |

---

## Condition Value Summary

### LIB$ Condition Values

| Constant | Value | Meaning |
|----------|-------|---------|
| LIB$_NORMAL | 0x00801001 | Normal completion |
| LIB$_STRTRU | 0x00801004 | String truncated (warning) |
| LIB$_INVARG | 0x00800014 | Invalid argument |
| LIB$_INSVIRMEM | 0x00800124 | Insufficient virtual memory |
| LIB$_INVSTRDES | 0x00800134 | Invalid string descriptor |
| LIB$_NEGTIM | 0x00800144 | Negative time |
| LIB$_NOTFOU | 0x00800154 | Not found |
| LIB$_FATERRLIB | 0x00800164 | Fatal error in library |
| LIB$_SYNTAXERR | 0x00800224 | Syntax error |
| LIB$_BADZONE | 0x008001E4 | Bad zone |
| LIB$_NOSUCHSYM | 0x008001C4 | No such symbol |

### STR$ Condition Values

| Constant | Value | Meaning |
|----------|-------|---------|
| STR$_NORMAL | 0x00801001 | Normal completion |
| STR$_TRU | 0x00801008 | String truncated (warning) |
| STR$_MATCH | 0x00801011 | String matched |
| STR$_NOMATCH | 0x00801018 | No match |
| STR$_NOELEM | 0x00801020 | No such element |
| STR$_ILLSTRCLA | 0x00801044 | Illegal string class |
| STR$_INSVIRMEM | 0x0080104C | Insufficient virtual memory |

### MTH$ Condition Values

| Constant | Value | Meaning |
|----------|-------|---------|
| MTH$_NORMAL | 0x00901001 | Normal completion |
| MTH$_FLOOVEMAT | 0x00901024 | Floating overflow |
| MTH$_INVARGMAT | 0x00901034 | Invalid argument |
| MTH$_SIGLOSMAT | 0x0090103C | Significance loss |
| MTH$_SQUROONEG | 0x0090104C | Square root of negative |
