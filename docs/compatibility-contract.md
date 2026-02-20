# OVMX Source Compatibility Contract

**Version**: 1.0
**Date**: 2026-02-19
**Bead**: vms-801.1

This document defines what "source compatible with OpenVMS" means concretely for the OVMX project. It is the acceptance criteria for all downstream compatibility milestones and the authoritative reference for what OVMX does and does not implement.

---

## 1. Scope

### Target

**Source compatibility with OpenVMS Alpha C programs** compiled with DEC C or VSI C. A program is source-compatible with OVMX when it can be recompiled on Linux against OVMX headers and produce correct behavior without modifying its source code.

The reference platform is **OpenVMS Alpha V8.4** (the last DEC/VSI supported version with broad third-party software compatibility).

### What Is In Scope

- SYS$ system service entry points that are meaningful on a single-node userspace system
- VMS descriptor types and macros (`descrip.h`, `$DESCRIPTOR`, `CLASS_S`, `CLASS_D`, `CLASS_A`)
- VMS status codes and the status value structure (`ssdef.h`, `stsdef.h`, `rmsdef.h`)
- Run-time library routines: `lib$`, `str$`, `mth$`, `ots$`
- Record Management Services: FAB, RAB, NAM, XAB control blocks and sys$ RMS entry points
- Logical name services (process and system tables)
- Event flags (local cluster, flags 0-63)
- AST delivery model (via pthreads signal simulation)
- Process management: create, delete, suspend, resume, hibernate, wake
- Lock manager: enqueue/dequeue for single-node use
- Condition handling facility (CHF): signal arrays, mechanism arrays, handler chain
- I/O channel model: assign, deassign, QIO/QIOW for terminal and file devices
- DCL shell: interactive and script execution
- Privilege definitions (`prvdef.h`) as constants (actual enforcement is advisory)

### What Is Permanently Out of Scope

See Section 7 for the complete list. Summary: binary compatibility, VAX-specific APIs, Itanium-specific APIs, cluster services, DECwindows/Motif, MACRO-32 assembly calling conventions, OpenVMS POSIX subsystem, ACMS transaction processing, DECnet networking APIs, and kernel-mode privileged services.

---

## 2. Compatibility Tiers

### Tier 1 — Must Work

A program using only Tier 1 APIs must compile, link, and run correctly. Tier 1 is the pass/fail line for the project.

| Subsystem | Coverage |
|-----------|----------|
| Descriptors (`descrip.h`) | All CLASS_S, CLASS_D, CLASS_A structs; `$DESCRIPTOR`, `$DESCRIPTOR_D`, `DSC$INIT_S`, `DSC$INIT_D` macros; helper functions |
| Status codes (`ssdef.h`, `stsdef.h`) | Full status bit layout; all defined SS$_ codes; `$VMS_STATUS_SUCCESS`, `$VMS_STATUS_SEVERITY`, `STS$MATCH` macros |
| RMS status codes (`rmsdef.h`) | All defined RMS$_ codes; `RMS$SUCCESS`, `RMS$FAILURE` macros |
| Time services | `sys$gettim`, `sys$numtim`, `sys$bintim`, `sys$asctim`, `sys$setimr`, `sys$cantim` |
| Logical names | `sys$crelnm`, `sys$dellnm`, `sys$trnlnm`; LNM$_ item codes; predefined table names |
| RMS sequential I/O | `sys$open`, `sys$close`, `sys$create`, `sys$erase`, `sys$connect`, `sys$disconnect`, `sys$get`, `sys$put`, `sys$rewind`; FAB, RAB, NAM, XABDAT, XABPRO |
| Terminal I/O (lib$) | `lib$put_output`, `lib$get_input` |
| Memory management (lib$) | `lib$get_vm`, `lib$free_vm`, `lib$get_vm_page`, `lib$free_vm_page`, `lib$create_vm_zone`, `lib$delete_vm_zone` |
| Condition handling | `lib$signal`, `lib$stop`, `lib$sig_to_ret`, `lib$establish`, `lib$revert`; CHF signal and mechanism arrays |
| String routines (str$) | `str$copy_dx`, `str$copy_r`, `str$concat`, `str$append`, `str$prefix`, `str$compare`, `str$compare_eql`, `str$case_blind_compare`, `str$left`, `str$right`, `str$len_extr`, `str$element`, `str$trim`, `str$upcase`, `str$translate`, `str$replace`, `str$free1_dx`, `str$get1_dx`, `str$analyze_sdesc`, `str$position`, `str$match_wild` |
| Math routines (mth$) | All declared double and float precision routines (full implementations wrapping libm) |
| Integer conversion (ots$) | `ots$cvt_l_ti`, `ots$cvt_ti_l`, `ots$cvt_l_tz`, `ots$cvt_l_to` |
| Process info | `sys$getjpi`, `sys$getjpiw`, `lib$getjpi`; all JPI$_ item codes |
| System info | `sys$getsyi`, `sys$getsyiw`, `lib$getsyi`; all SYI$_ item codes |
| Process management | `sys$exit`, `sys$hiber`, `sys$wake`, `sys$suspend`, `sys$resume`, `sys$creprc`, `sys$delprc`, `sys$forcex`, `sys$setpri` |
| FAO formatting | `sys$fao`, `sys$faol`, `lib$sys_fao`, `lib$sys_faol`; all standard FAO directives |
| Message services | `sys$getmsg`, `sys$putmsg`; MSG$M_ flag bits |
| DCL shell | All Tier 1 DCL commands and flow control (see Section 6) |

### Tier 2 — Should Work

Programs using these APIs should work in the common cases. Edge cases and rarely-used options may not be implemented.

| Subsystem | Coverage Target |
|-----------|----------------|
| RMS indexed files (ISAM) | `sys$find`, `sys$update`, `sys$delete`; XABKEY with primary key; sequential access to indexed files |
| RMS wildcard file operations | `sys$parse`, `sys$search`; `lib$find_file`, `lib$find_file_end` |
| Event flags (local) | `sys$setef`, `sys$clref`, `sys$waitfr`, `sys$wflor`, `sys$wfland`, `sys$synch`, `sys$readef` |
| Common event flag clusters | `sys$ascefc`, `sys$dacefc`, `sys$dlcefc` (flags 64-127) |
| ASTs | `sys$dclast`, `sys$setast`; timer ASTs via `sys$setimr` |
| Lock manager | `sys$enq`, `sys$enqw`, `sys$deq`; all lock modes (NL through EX) |
| Exit handlers | `sys$dclexh` |
| CLI symbol access | `lib$get_symbol`, `lib$set_symbol`; LIB$K_CLI_LOCAL_SYM, LIB$K_CLI_GLOBAL_SYM |
| File operations (lib$) | `lib$find_file`, `lib$find_file_end`, `lib$rename_file`, `lib$delete_file` |
| Subprocess creation | `lib$spawn`; CLI$M_NOWAIT and other spawn flags |
| Date/time (lib$) | `lib$day`, `lib$day_of_week`, `lib$date_time`, `lib$sub_times`, `lib$add_times`, `lib$mult_delta_time`, `lib$cvt_from_internal_time` |
| Number conversion (lib$) | `lib$cvt_dtb`, `lib$cvt_htb`, `lib$cvt_otb` |
| String search | `str$find_first_substring`, `str$compare_multi` |
| Memory sections | `sys$crmpsc`, `sys$cretva`, `sys$deltva`, `sys$expreg` |
| I/O channels | `sys$assign`, `sys$dassgn`, `sys$qio`, `sys$qiow`, `sys$cancel`; IO$_ codes for terminal read/write |
| Mailboxes | `sys$crembx`, `sys$delmbx`; basic interprocess communication |

### Tier 3 — Stretch Goals

These require significant additional work and are tracked as separate backlog beads.

| Subsystem | Notes |
|-----------|-------|
| QIO networking (UCX/TCPIP) | Socket-like QIO operations; maps to POSIX sockets |
| lib$tparse | Table-driven parser; currently stubbed as SS$_UNSUPPORTED |
| RMS indexed: multi-key | Multiple alternate keys in ISAM files |
| RMS indexed: journaling | Journaled indexed file updates |
| RMS relative files | FAB$C_REL organization |
| CLI$ callback interface | Full `cli$get_value`, `cli$present`, `cli$get_cmd` |
| DECC$ compatibility symbols | `decc$feature_get_name`, `decc$feature_set_value`, etc. |
| OTS$ additional routines | Float/double conversions, string-to-float |
| MAIL$ services | Electronic mail API |
| Advanced XAB types | XAB$C_FHC (file header characteristics), XAB$C_ALL (allocation) |

---

## 3. API Surface Inventory

### 3.1 SYS$ System Services

All declared in `src/libvms/include/starlet.h`.

| Function | Group | Status | Notes |
|----------|-------|--------|-------|
| `sys$assign` | I/O Channel | implemented | Maps to Linux open-style device model |
| `sys$dassgn` | I/O Channel | implemented | Closes channel |
| `sys$crembx` | Mailbox | implemented | Uses named FIFO or pipe |
| `sys$delmbx` | Mailbox | implemented | |
| `sys$qio` | QIO | implemented | Terminal and file; async via thread |
| `sys$qiow` | QIO | implemented | Synchronous wrapper |
| `sys$setef` | Event Flag | implemented | |
| `sys$clref` | Event Flag | implemented | |
| `sys$waitfr` | Event Flag | implemented | |
| `sys$wflor` | Event Flag | implemented | |
| `sys$wfland` | Event Flag | implemented | |
| `sys$synch` | Event Flag | implemented | |
| `sys$readef` | Event Flag | implemented | |
| `sys$ascefc` | Event Flag | implemented | Common event flag clusters (EF 64-127) |
| `sys$dacefc` | Event Flag | implemented | |
| `sys$dlcefc` | Event Flag | implemented | |
| `sys$gettim` | Time | implemented | Uses `clock_gettime(CLOCK_REALTIME)` |
| `sys$numtim` | Time | implemented | |
| `sys$bintim` | Time | implemented | |
| `sys$asctim` | Time | implemented | |
| `sys$setimr` | Time | implemented | Via `timer_create`/POSIX timers |
| `sys$cantim` | Time | implemented | |
| `sys$crelnm` | Logical Name | implemented | Process and system tables |
| `sys$dellnm` | Logical Name | implemented | |
| `sys$trnlnm` | Logical Name | implemented | Iterative translation (up to LNM$C_MAXDEPTH=10) |
| `sys$creprc` | Process | implemented | Maps to `fork`+`execve` |
| `sys$delprc` | Process | implemented | Maps to `kill(SIGTERM)` |
| `sys$hiber` | Process | implemented | `pause()` until `sys$wake` |
| `sys$wake` | Process | implemented | Sends signal to hibernating process |
| `sys$exit` | Process | implemented | Calls `exit()` |
| `sys$getjpi` | Process Info | implemented | Async form; most JPI$_ item codes |
| `sys$getjpiw` | Process Info | implemented | Synchronous form |
| `sys$getsyi` | System Info | implemented | Most SYI$_ item codes |
| `sys$getsyiw` | System Info | implemented | Synchronous form |
| `sys$cretva` | Memory | implemented | Maps to `mmap` |
| `sys$deltva` | Memory | implemented | Maps to `munmap` |
| `sys$expreg` | Memory | implemented | Extends data segment |
| `sys$crmpsc` | Memory | implemented | File-backed sections via `mmap` |
| `sys$dclast` | AST | implemented | Queued via thread signal mechanism |
| `sys$setast` | AST | implemented | Enable/disable AST delivery |
| `sys$dclexh` | AST/Exit | implemented | Registered via `atexit` chain |
| `sys$enq` | Lock | implemented | Single-node distributed lock manager |
| `sys$enqw` | Lock | implemented | Synchronous form |
| `sys$deq` | Lock | implemented | |
| `sys$setprv` | Security | implemented | Advisory; privileges tracked in PCB |
| `sys$chkpro` | Security | stubbed | Returns SS$_NORMAL (no real enforcement) |
| `sys$open` | RMS File | implemented | |
| `sys$close` | RMS File | implemented | |
| `sys$create` | RMS File | implemented | |
| `sys$erase` | RMS File | implemented | |
| `sys$parse` | RMS File | implemented | Parses VMS filespec syntax |
| `sys$search` | RMS File | implemented | Wildcard file search |
| `sys$display` | RMS File | implemented | Populates FAB fields from open file |
| `sys$connect` | RMS Record | implemented | |
| `sys$disconnect` | RMS Record | implemented | |
| `sys$get` | RMS Record | implemented | Sequential and keyed access |
| `sys$put` | RMS Record | implemented | |
| `sys$update` | RMS Record | implemented | In-place record update |
| `sys$delete` | RMS Record | implemented | Delete current record |
| `sys$find` | RMS Record | implemented | Position without reading |
| `sys$rewind` | RMS Record | implemented | |
| `sys$fao` | FAO | implemented | All standard FAO directives |
| `sys$faol` | FAO | implemented | List-form variant |
| `sys$getmsg` | Message | implemented | Built-in message table |
| `sys$putmsg` | Message | implemented | MSG$M_ flag bits honored |
| `sys$forcex` | Process | implemented | Sends signal to target |
| `sys$suspend` | Process | implemented | `kill(SIGSTOP)` |
| `sys$resume` | Process | implemented | `kill(SIGCONT)` |
| `sys$setpri` | Process | implemented | Maps to `nice()`/`setpriority()` |
| `sys$cancel` | I/O | implemented | Cancels pending QIO on channel |

### 3.2 LIB$ Run-Time Library

All declared in `src/libvms/include/lib$routines.h`.

| Function | Status | Notes |
|----------|--------|-------|
| `lib$get_vm` | implemented | Zone-based allocator with lookaside lists |
| `lib$free_vm` | implemented | |
| `lib$get_vm_page` | implemented | VMS page = 512 bytes |
| `lib$free_vm_page` | implemented | |
| `lib$create_vm_zone` | implemented | Up to 64 zones |
| `lib$delete_vm_zone` | implemented | Bulk-free via munmap |
| `lib$put_output` | implemented | Writes to stdout with newline |
| `lib$get_input` | implemented | Reads from stdin with optional prompt |
| `lib$put_common` | implemented | Process common area via shared memory |
| `lib$get_common` | implemented | |
| `lib$signal` | implemented | Full CHF handler chain traversal |
| `lib$stop` | implemented | Signal + force exit |
| `lib$sig_to_ret` | implemented | Converts signal to return status |
| `lib$establish` | implemented | Thread-local handler stack |
| `lib$revert` | implemented | Pop handler from stack |
| `lib$sys_fao` | implemented | Calls sys$fao |
| `lib$sys_faol` | implemented | List-form variant |
| `lib$day` | implemented | Days since VMS base date (Nov 17, 1858) |
| `lib$day_of_week` | implemented | 1=Monday through 7=Sunday |
| `lib$date_time` | implemented | "DD-MMM-YYYY HH:MM:SS.CC" format |
| `lib$sub_times` | implemented | |
| `lib$add_times` | implemented | |
| `lib$mult_delta_time` | implemented | |
| `lib$cvt_from_internal_time` | implemented | |
| `lib$cvt_dtb` | implemented | Decimal text to binary |
| `lib$cvt_htb` | implemented | Hex text to binary |
| `lib$cvt_otb` | implemented | Octal text to binary |
| `lib$getjpi` | implemented | Single-item wrapper for sys$getjpiw |
| `lib$getsyi` | implemented | Single-item wrapper for sys$getsyiw |
| `lib$get_symbol` | implemented | Read DCL symbol from local or global table |
| `lib$set_symbol` | implemented | Write DCL symbol |
| `lib$spawn` | implemented | Fork subprocess via shell; CLI$M_NOWAIT supported |
| `lib$find_file` | implemented | Wildcard file search using sys$search |
| `lib$find_file_end` | implemented | Release wildcard context |
| `lib$rename_file` | implemented | Maps to rename(2) |
| `lib$delete_file` | implemented | Maps to unlink(2) |
| `lib$tparse` | **stubbed** | Returns SS$_UNSUPPORTED; Tier 3 |

### 3.3 STR$ String Routines

All declared in `src/libvms/include/str$routines.h`.

| Function | Status | Notes |
|----------|--------|-------|
| `str$copy_dx` | implemented | CLASS_S and CLASS_D targets |
| `str$copy_r` | implemented | Copy from length+address pair |
| `str$concat` | implemented | Variadic; NULL-terminated arg list |
| `str$append` | implemented | Append to CLASS_D |
| `str$prefix` | implemented | Prepend to CLASS_D |
| `str$compare` | implemented | Lexicographic with space-pad |
| `str$compare_eql` | implemented | Exact equality (no padding) |
| `str$case_blind_compare` | implemented | Case-insensitive |
| `str$compare_multi` | implemented | Locale flags ignored; behaves as case_blind |
| `str$left` | implemented | Characters 1..end_pos |
| `str$right` | implemented | Characters start_pos..end |
| `str$len_extr` | implemented | Substring by position and length |
| `str$element` | implemented | Delimited element extraction (0-based) |
| `str$find_first_substring` | implemented | Leftmost match of any substring |
| `str$position` | implemented | First occurrence of substring |
| `str$match_wild` | implemented | `*` and `%` wildcards |
| `str$trim` | implemented | Remove trailing spaces and tabs |
| `str$upcase` | implemented | ASCII uppercase |
| `str$translate` | implemented | 256-byte translation table |
| `str$replace` | implemented | Replace portion by position range |
| `str$free1_dx` | implemented | Free CLASS_D storage |
| `str$get1_dx` | implemented | Allocate CLASS_D of specified length |
| `str$analyze_sdesc` | implemented | Extract length+address from any class |

### 3.4 MTH$ Mathematics Routines

All declared in `src/libvms/include/mth$routines.h`. All routines are **implemented** by wrapping the host libm. Arguments are passed by reference (pointer to value) per the VMS calling standard.

| Function | Precision | Notes |
|----------|-----------|-------|
| `mth$sin` | double | |
| `mth$cos` | double | |
| `mth$tan` | double | |
| `mth$sincos` | double | Simultaneous sin and cos |
| `mth$asin` | double | |
| `mth$acos` | double | |
| `mth$atan` | double | |
| `mth$atan2` | double | |
| `mth$exp` | double | |
| `mth$alog` | double | Natural log (Fortran naming) |
| `mth$alog10` | double | Base-10 log |
| `mth$alog2` | double | Base-2 log |
| `mth$sqrt` | double | |
| `mth$power` | double | `base^exp` |
| `mth$power_ji` | int32_t | Integer exponentiation |
| `mth$sinh` | double | |
| `mth$cosh` | double | |
| `mth$tanh` | double | |
| `mth$sinf` | float | Single-precision |
| `mth$cosf` | float | |
| `mth$tanf` | float | |
| `mth$asinf` | float | |
| `mth$acosf` | float | |
| `mth$atanf` | float | |
| `mth$atan2f` | float | |
| `mth$expf` | float | |
| `mth$alogf` | float | |
| `mth$alog10f` | float | |
| `mth$sqrtf` | float | |
| `mth$abs` | double | |
| `mth$absf` | float | |
| `mth$sign` | double | Transfer of sign |
| `mth$nint` | int32_t | Nearest integer |
| `mth$floor` | double | |
| `mth$ceil` | double | |
| `mth$mod` | double | |
| `mth$max` | double | |
| `mth$min` | double | |
| `mth$random` | float | Multiplicative congruential |
| `mth$sind` | double | Sine of degrees |
| `mth$cosd` | double | Cosine of degrees |
| `mth$tand` | double | Tangent of degrees |

**Floating-point note**: VMS Alpha uses IEEE S-floating (single) and T-floating (double), the same formats as x86_64. No floating-point conversion is needed. VAX F/D/G/H floating formats are not supported (see Section 7).

### 3.5 OTS$ Object Time System Routines

Implemented in `src/libvms/rtl/ots_routines.c`.

| Function | Status | Description |
|----------|--------|-------------|
| `ots$cvt_l_ti` | implemented | Longword integer to decimal text descriptor |
| `ots$cvt_ti_l` | implemented | Decimal text descriptor to longword integer |
| `ots$cvt_l_tz` | implemented | Longword to hex text |
| `ots$cvt_l_to` | implemented | Longword to octal text |

OTS$ float/string and FORTRAN-specific routines are not currently declared or implemented.

### 3.6 RMS Control Block Entry Points

Declared in `src/vmsrms/include/rms/rms.h` and also in `starlet.h`.

| Function | Status | Notes |
|----------|--------|-------|
| `sys$open` | implemented | |
| `sys$close` | implemented | |
| `sys$create` | implemented | |
| `sys$erase` | implemented | |
| `sys$display` | implemented | |
| `sys$connect` | implemented | |
| `sys$disconnect` | implemented | |
| `sys$get` | implemented | Sequential, keyed, and RFA access modes |
| `sys$put` | implemented | |
| `sys$update` | implemented | |
| `sys$delete` | implemented | |
| `sys$find` | implemented | |
| `sys$parse` | implemented | |
| `sys$search` | implemented | |
| `sys$rewind` | implemented | |
| `sys$flush` | implemented | |

### 3.7 DCL Shell Commands

DCL is implemented in `src/vmsdcl/`. The following commands are registered in the built-in verb table.

**Flow control** (handled in `dcl_exec.c`, not in the verb table):

| Statement | Status | Notes |
|-----------|--------|-------|
| `IF ... THEN` | implemented | Single-line and block IF |
| `ELSE` | implemented | |
| `ENDIF` | implemented | |
| `GOTO label` | implemented | Within script only |
| `GOSUB label` | implemented | Subroutine call within script |
| `RETURN` | implemented | |
| `ON condition THEN GOTO/CONTINUE` | implemented | Error handling |
| `CALL` | implemented | Call named subroutine |

**Built-in commands**:

| Command | Status | Notes |
|---------|--------|-------|
| `CLOSE` | implemented | Close file channel |
| `COPY` | implemented | Copy files |
| `CREATE` | implemented | Create empty file |
| `DEASSIGN` | implemented | Remove logical name |
| `DEFINE` | implemented | Create logical name |
| `DELETE` | implemented | Delete files |
| `DIRECTORY` | implemented | List directory (VMS format) |
| `EXIT` | implemented | Exit script or session |
| `HELP` | implemented | Built-in help system |
| `INQUIRE` | implemented | Read input to symbol |
| `LOGOUT` | implemented | End interactive session |
| `OPEN` | implemented | Open file for I/O |
| `PIPE` | implemented | Shell pipeline pass-through |
| `PURGE` | implemented | Delete old file versions |
| `READ` | implemented | Read record from file |
| `RENAME` | implemented | Rename file |
| `RUN` | implemented | Execute program image |
| `SEARCH` | implemented | Search file for string |
| `SET DEFAULT` | implemented | Change default directory |
| `SET PROMPT` | implemented | Change prompt string |
| `SET VERIFY` | implemented | Enable/disable command echo |
| `SET TERMINAL` | implemented | Terminal characteristics |
| `SET PROTECTION` | implemented | Set file protection mask |
| `SET PASSWORD` | implemented | Change user password |
| `SHOW DEFAULT` | implemented | Display current directory |
| `SHOW LOGICAL` | implemented | Display logical name translations |
| `SHOW PROCESS` | implemented | Display process information |
| `SHOW PROTECTION` | implemented | Display protection settings |
| `SHOW SYMBOL` | implemented | Display symbol value |
| `SHOW SYSTEM` | implemented | Display system information |
| `SHOW TIME` | implemented | Display current date/time |
| `SHOW USERS` | implemented | Display logged-in users |
| `SHOW VERIFY` | implemented | Show verify state |
| `SPAWN` | implemented | Create subprocess |
| `STOP` | implemented | Stop process |
| `TYPE` | implemented | Display file contents |
| `WRITE` | implemented | Write record to file |

**Lexical functions** (implemented in `dcl_lexical.c`): F$CONTEXT, F$CSID, F$CVSI, F$CVTIME, F$CVUI, F$DEVICE, F$DIRECTORY, F$ENVIRONMENT, F$EXTRACT, F$FAO, F$GETDVI, F$GETJPI, F$GETQUI, F$GETSYI, F$INTEGER, F$LENGTH, F$LOCATE, F$MESSAGE, F$MODE, F$PARSE, F$PID, F$PRIVILEGE, F$PROCESS, F$SEARCH, F$STRING, F$TIME, F$TRNLNM, F$TYPE, F$UNIQUE, F$USER, F$VERIFY.

---

## 4. Header Inventory

All public headers are under `src/libvms/include/` and `src/vmsrms/include/`.

| Header | Location | Defines | Layout Verified |
|--------|----------|---------|-----------------|
| `starlet.h` | `src/libvms/include/` | Master include; all sys$ prototypes | N/A (aggregator) |
| `descrip.h` | `src/libvms/include/` | `dsc$descriptor_s`, `dsc$descriptor_d`, `dsc$descriptor_a`, `dsc$descriptor`; all DSC$K_ and DSC$M_ constants; `$DESCRIPTOR` macros | **Verified** — field layout matches OpenVMS Alpha |
| `ssdef.h` | `src/libvms/include/` | 75 SS$_ condition codes; status testing macros | **Verified** — numeric values match VMS message file |
| `stsdef.h` | `src/libvms/include/` | Status value bit layout; STS$K_/STS$M_/STS$V_ constants; `$VMS_STATUS_*` macros; `STS$MATCH`, `STS$VALUE` | **Verified** — bit positions match Alpha ABI |
| `iodef.h` | `src/libvms/include/` | IO$_ function codes; IO$M_ modifier bits; `_iosb`, `_tt_iosb`, `_dk_iosb` structs | **Verified** — codes match VMS I/O User Reference |
| `lnmdef.h` | `src/libvms/include/` | LNM$_ item codes; LNM$M_ attribute bits; LNM$C_ limits; PSL$C_ access modes; `item_list_3`, `lnm_item_list` structs; predefined table name strings | **Verified** |
| `rmsdef.h` | `src/libvms/include/` | 60+ RMS$_ condition codes; RMS$SUCCESS/FAILURE macros | **Verified** — numeric values match VMS RMS Reference |
| `prcdef.h` | `src/libvms/include/` | PRC$M_/PRC$V_ process flags; PRC$K_ state and priority constants; JPI$_ item codes (51 codes); SYI$_ item codes (15 codes); `_uic` struct | **Verified** |
| `prvdef.h` | `src/libvms/include/` | 39 PRV$M_ privilege mask bits; PRV$V_ bit positions | **Verified** — bit positions match OpenVMS Alpha |
| `chfdef.h` | `src/libvms/include/` | `chf$signal_array`, `chf$mech_array`, `chf$handler_block` structs; CHF$L_ offsets; STS$K_ severity codes; `chf$handler_t` typedef | Simplified — mechanism array fields are subset of Alpha |
| `msgdef.h` | `src/libvms/include/` | MSG$M_ component flags; MSG$K_ severity chars; `msg$vector` struct; MSG$_FAC_ facility numbers | **Verified** |
| `libclidef.h` | `src/libvms/include/` | CLI$K_ callback request types; CLI$_ status returns; CLI$M_ spawn flags; LIB$K_CLI_ symbol table types | **Verified** |
| `lib$routines.h` | `src/libvms/include/` | All lib$ prototypes; LIB$_ condition codes; LIB$K_ constants | N/A (prototypes only) |
| `str$routines.h` | `src/libvms/include/` | All str$ prototypes; STR$_ condition codes | N/A (prototypes only) |
| `mth$routines.h` | `src/libvms/include/` | All mth$ prototypes; MTH$_ condition codes | N/A (prototypes only) |
| `jpidef.h` | `src/libvms/include/` | JPI$_ item codes (separate header, also in prcdef.h) | **Verified** |
| `syidef.h` | `src/libvms/include/` | SYI$_ item codes (separate header, also in prcdef.h) | **Verified** |
| `rms/fab.h` | `src/vmsrms/include/` | `struct FAB`; FAB$C_/FAB$M_ constants; `cc$rms_fab` init macro | **Partially verified** — VMS-compatible fields verified; OVMX extension fields appended |
| `rms/rab.h` | `src/vmsrms/include/` | `struct RAB`, `RFA` typedef; RAB$C_/RAB$M_ constants; `cc$rms_rab` init macro | **Partially verified** — VMS-compatible fields verified; OVMX extension fields appended |
| `rms/nam.h` | `src/vmsrms/include/` | `struct NAM`; NAM$C_ limits; NAM$M_ flag bits; `cc$rms_nam` init macro | **Verified** |
| `rms/xab.h` | `src/vmsrms/include/` | `struct XABKEY`, `struct XABDAT`, `struct XABPRO`; XAB$C_/XAB$M_ constants; init macros | **Verified** |
| `rms/rms.h` | `src/vmsrms/include/` | Master RMS include; all sys$ RMS prototypes | N/A (aggregator) |

---

## 5. Status Code Coverage

### SS$_ System Status Codes

OVMX defines **75 SS$_ codes** in `ssdef.h`. OpenVMS Alpha defines approximately 1,200 SS$_ codes in its system message file.

The 75 codes defined cover:

- All codes returned by OVMX-implemented system services
- All codes required by the `$VMS_STATUS_*` macro contract
- The most frequently tested codes in typical VMS C programs

**Gap analysis**: The codes absent from OVMX's `ssdef.h` are codes for hardware faults, device driver errors, cluster operations, security auditing, and other services that are permanently out of scope. Code that tests for these absent codes will see them as undefined symbols at compile time, which is the correct and detectable failure mode.

The status bit layout (facility, severity, message number) in `stsdef.h` is complete and matches the Alpha ABI. Programs that construct or decompose status codes programmatically using the STS$_ macros will work correctly regardless of which specific codes are defined.

### RMS$_ Status Codes

OVMX defines **50+ RMS$_ codes** in `rmsdef.h`. This covers:

- All success codes returned by OVMX RMS operations
- All error codes for the operations OVMX implements
- The complete set of codes expected by programs doing RMS error handling

### LIB$_, STR$_, MTH$_ Condition Codes

All facility-specific condition codes are defined alongside their routine declarations in the respective headers:

- `lib$routines.h`: 19 LIB$_ codes
- `str$routines.h`: 12 STR$_ codes
- `mth$routines.h`: 9 MTH$_ codes

---

## 6. Struct Layout Contract

The following structures **must match OpenVMS Alpha layout** exactly for source-compatible programs that use the VMS calling standard or that initialize these structs with the provided macros. Any divergence is a compatibility bug.

### dsc$descriptor_s (CLASS_S)

```c
struct dsc$descriptor_s {
    uint16_t  dsc$w_length;    /* offset 0, size 2 */
    uint8_t   dsc$b_dtype;     /* offset 2, size 1 */
    uint8_t   dsc$b_class;     /* offset 3, size 1 */
    char     *dsc$a_pointer;   /* offset 4 (or 8 on 64-bit), size 4 (or 8) */
};
```

**OVMX status**: Matches Alpha layout on 64-bit Linux. The pointer field is 8 bytes on both Alpha 64-bit and x86_64. Programs that treat the descriptor as a 12-byte struct on 32-bit VAX and cast it will not work, but that is a VAX-specific program (out of scope).

### dsc$descriptor_d (CLASS_D)

Identical field layout to CLASS_S. The distinction is in the `dsc$b_class` field value. Programs that access `dsc$a_pointer` directly (for their own reallocation logic) will work correctly.

### item_list_3

```c
struct item_list_3 {
    uint16_t  buflen;      /* offset 0 */
    uint16_t  item_code;   /* offset 2 */
    void     *bufaddr;     /* offset 4 (or 8) */
    uint16_t *retlen;      /* offset 8 (or 16) */
};
```

**OVMX status**: Layout matches Alpha. The terminator convention (zero longword) is implemented: `{ 0, 0, NULL, NULL }`.

**Known divergence from real VMS**: On OpenVMS Alpha, the item_list_3 `bufaddr` and `retlen` fields are 32-bit pointers even in 64-bit mode (the structure packs as 12 bytes using "short pointers"). OVMX uses native 64-bit pointers, making the struct 24 bytes on 64-bit Linux. Programs that explicitly sizeof() the struct or index item list arrays by byte offset will break. Programs that declare item lists statically and pass pointers to them will work.

### struct FAB

**OVMX status**: VMS-compatible fields occupy the same relative positions as on OpenVMS Alpha. Three internal-use fields have been appended after the last VMS-defined field:

```c
/* Internal state - not part of VMS FAB */
int      _linux_fd;
char     _resolved_path[1024];
void    *_rms_state;
```

Programs that `sizeof(FAB)` and use the result for allocation or block-copying will get a larger size than on VMS. Programs that access named fields (`fab$b_org`, `fab$l_fop`, etc.) will work correctly. The `fab$b_bln` field is set to `sizeof(struct FAB)` by `cc$rms_fab` to reflect the actual block size — this diverges from VMS where `fab$b_bln` is the standard FAB size. RMS entry points that check `fab$b_bid` will work; those that check `fab$b_bln` may reject the FAB.

### struct RAB

Same situation as FAB. Internal fields appended after VMS-defined fields:

```c
off_t    _current_offset;
void    *_rms_stream;
int      _eof;
off_t    _last_rec_offset;
uint16_t _last_rec_size;
```

### struct NAM

**OVMX status**: Fully compatible. One internal field (`nam$$l_context`) is appended. The double-dollar prefix (`nam$$`) follows the VMS convention for internal-use fields, which are present in real VMS NAM blocks.

### XABKEY, XABDAT, XABPRO

**OVMX status**: Fully compatible with OpenVMS Alpha field layout. All VMS-defined fields are present with correct types and byte offsets.

---

## 7. What Will Never Be Supported

The following are permanently excluded from OVMX's compatibility scope. Programs that require these cannot be ported without substantial modification.

### Hardware and Architecture

- **VAX floating-point formats**: F/D/G/H floating point. OVMX uses IEEE floating point (same as OpenVMS Alpha and Itanium). VMS programs that specify `/FLOAT=D_FLOAT` or use `DSC$K_DTYPE_D`, `DSC$K_DTYPE_G`, `DSC$K_DTYPE_H` data types for descriptor-based numeric operations will not produce correct results.
- **VAX-specific calling conventions**: Register-based calling standard used by MACRO-32 and old VAX C. Not applicable on x86_64.
- **VAX-only system services**: Any SS$_ services that only existed on VAX (pre-Alpha).
- **MACRO-32 assembly**: The MACRO assembler and MACRO-32 object files.

### System Architecture

- **Binary compatibility**: ELF or COFF/Alpha object files and shared images cannot run on Linux without recompilation.
- **Privileged kernel-mode services**: SYS$CMKRNL, SYS$CMEXEC, and any service that actually transitions processor access mode. On OVMX, access mode constants (PSL$C_KERNEL, PSL$C_USER) are defined but have no enforcement effect.
- **Physical memory management**: PFN-based services (SYS$LKWSET, SYS$ULKPAG, SYS$SETSWM, etc.). Linux virtual memory management is not compatible.
- **VAX memory layout**: P0/P1 regions map to Linux `mmap` regions; code that relies on specific virtual address ranges will break.

### Clustering

- **OpenVMS Cluster services**: SYS$CRMPSC with global sections, SYS$LCKPAG cluster locks, CSID-based SYS$GETSYI queries for remote nodes.
- **Distributed Lock Manager (cluster-wide)**: sys$enq with `ENQ$M_NODLCKBLK` or cross-node locks.
- **Galaxy services**: SYS$GALAXY_*, hardware partitions.
- **Clusterwide logical names**: LNM$M_CLUSTERWIDE flag silently degrades to system-wide.

### Subsystems

- **DECwindows/Motif**: XUI, DECwindows transport, Motif widget toolkit. Any program using `#include <X11/...>` or `#include <Xm/...>` cannot be ported as-is.
- **MAIL$**: The VMS MAIL facility API. Programs that call MAIL$SEND, MAIL$MESSAGE_*, etc.
- **ACMS**: Transaction processing monitor services.
- **MDMS/HSM**: Media and Device Management Services, hierarchical storage management.
- **CDA**: Compound Document Architecture document conversion services.
- **RDB/OpenVMS**: Relational database services (SQL access through RDB C API).
- **AXP SDA/debugger**: System Dump Analyzer, TDC (Terminal Device Controller) APIs.

### Networking

- **DECnet**: SYS$QIO with IO$_ACCESS to "_NLA0:", "_NET:", network object services. Programs that open DECnet connections via logical unit numbers or task-to-task communication.
- **UCX/TCPIP Services (QIO interface)**: Low-level socket operations through SYS$QIO with TCPIP channels. The high-level C socket library (`#include <socket.h>` on VMS) may work through POSIX, but QIO-based TCP/IP does not apply.
- **LAT**: Local Area Transport.

### CLI and Command Definitions

- **CDU**: Command Definition Utility-compiled command tables for use with CLI$ services. The OVMX DCL parser does not consume CDU-format command definition files.
- **FDL**: File Definition Language for controlled file creation via `sys$create` with XAB chains defining RMS structure. Partial support only.
- **DCL commands not listed in Section 3.7**: ACCOUNTING, ANALYZE, ATTACH, BACKUP, CONVERT, DIFFERENCES, DISMOUNT, DUMP, INITIALIZE, INSTALL, LOGI, MACRO, MOUNT, PATCH, PRINT, SUBMIT, WAIT.

### Other

- **SYS$CHKPRO with full UIC/ACL semantics**: Returns SS$_NORMAL unconditionally. No real protection enforcement.
- **Rights database**: SYS$FIND_HELD, SYS$FIND_HOLDER, identifier-based access control.
- **Audit server**: SYS$AUDIT_EVENT.
- **Shadowing**: Volume shadowing (DSSA) services.
- **POSIX/UNIXRTL**: The DECC$ POSIX extensions (`decc$feature_*`, `/UNIX_STYLE_UID`, etc.) are not emulated. OVMX programs run natively on Linux POSIX.

---

## 8. Validation Milestones

These milestones define progressive proof that OVMX achieves meaningful source compatibility. They originate from bead vms-801 and are the acceptance criteria for the compatibility work stream.

### Milestone 1 — Eight-Cubed Examples Pass at 80%+

**Definition**: The OpenVMS Programming Examples collection ("Eight-Cubed," approximately 512 example programs from DEC/VSI documentation and the OpenVMS Programmer's Handbook) can be recompiled and executed on OVMX with 80% or more producing correct output.

**What this validates**:
- Descriptor usage (`$DESCRIPTOR`, CLASS_S, CLASS_D)
- Basic sys$ service calls (time, logical names, process info)
- LIB$ I/O (`lib$put_output`, `lib$get_input`)
- STR$ string operations
- Simple RMS sequential I/O
- FAO formatting
- Condition handling (lib$signal, lib$establish)

**Blocking gaps** (must be resolved before this milestone can be claimed): lib$tparse (approximately 5% of examples use it), full FAO directive set, and RMS variable-length record format.

### Milestone 2 — MMK Compiles and Runs

**Definition**: MMK (Make-like utility for VMS, written in C for OpenVMS) can be recompiled from source on OVMX and successfully build a non-trivial project.

**What this validates** (beyond Milestone 1):
- Complex RMS file operations (parsing .MMS makefiles, wildcard expansion)
- CLI$ parsing integration (`lib$get_symbol`, `lib$set_symbol`)
- Process creation via `lib$spawn` with synchronization
- Logical name table manipulation at build time
- Symbol table operations
- lib$tparse (MMK's dependency parser uses it heavily)

**Why MMK**: MMK is real-world open-source VMS software with well-understood behavior. Success means OVMX's environment is sufficient for non-trivial system utility programs.

### Milestone 3 — NETLIB Compiles

**Definition**: NETLIB (the VMS portable networking library) can be recompiled from source on OVMX without modification of its VMS-specific code.

**What this validates** (beyond Milestones 1 and 2):
- Advanced RMS record-level operations (indexed file access for configuration databases)
- `sys$assign`/`sys$qio`/`sys$qiow` channel I/O model
- Complex condition handling (NETLIB has elaborate error handling chains)
- Logical name resolution for device and service lookup
- The full breadth of the LIB$ and STR$ RTL

**Note**: NETLIB milestone validates the compile step. Whether NETLIB's network operations function depends on the QIO networking implementation (Tier 3), which is not required for Milestone 3.

---

## Appendix A: Facility Numbers

| Facility | Number | Header |
|----------|--------|--------|
| SYSTEM | 0 | `ssdef.h` |
| RMS | 1 | `rmsdef.h` |
| CLI | 3 | `libclidef.h` |
| LIB$ | 21 (0x15) | `lib$routines.h` |
| STR$ | 36 (0x24) | `str$routines.h` |
| MTH$ | 22 (0x16) | `mth$routines.h` |

Note: `stsdef.h` lists MTH facility as 22 and STR facility as 36; `msgdef.h` lists them differently (MSG$_FAC_MTH=9, MSG$_FAC_STR=8). The values in the individual facility headers (`lib$routines.h`, `str$routines.h`, `mth$routines.h`) are the authoritative definitions for status codes produced by those routines.

## Appendix B: OVMX-Internal Extensions

The following identifiers are defined by OVMX and do not exist on OpenVMS. Programs that inadvertently use them would not compile on real VMS:

- `vms_init_descriptor()` — inline helper in `descrip.h`
- `vms_desc_to_cstr()` — inline helper in `descrip.h`
- `vms_cstr_to_desc()` — inline helper in `descrip.h`
- `vms_desc_alloc()` — inline helper in `descrip.h`
- `vms_desc_free()` — inline helper in `descrip.h`
- `dsc$init()` — inline helper in `descrip.h`
- `dsc$strncpy()` — inline helper in `descrip.h`
- `DSC$INIT_S()`, `DSC$INIT_D()` — runtime init macros in `descrip.h`
- `ITEM_LIST_END`, `LNM$_ITEM_LIST_END` — convenience macros in `lnmdef.h`
- FAB/RAB internal fields prefixed with `_linux_` and `_rms_`

These helpers are provided for convenience when writing new OVMX programs. They should not appear in code that is intended to be portable back to real VMS.
