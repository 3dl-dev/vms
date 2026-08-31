# OVMX DCL Command Reference

This document covers all built-in DCL commands implemented in `src/vmsdcl/dcl_builtin.c`. Commands are listed alphabetically. All commands support minimum-uniqueness abbreviation (the minimum abbreviation length is shown in parentheses).

Commands are registered in the `builtin_verbs[]` table. VMS file specifications, logical names, and wildcards (`*`, `%`) are supported throughout.

---

## Command Summary

| Command | Min Abbrev | Description |
|---------|-----------|-------------|
| ACCOUNTING | ACCO | Display login accounting information |
| ANALYZE | ANAL | Analyze system components |
| APPEND | APP | Append source file to destination |
| ASSIGN | ASS | Assign a logical name |
| ATTACH | ATT | Transfer terminal control to another process |
| BACKUP | BAC | Create, restore, or list a saveset file |
| CLOSE | CL | Close a file channel |
| CONTINUE | CONT | Resume an interrupted image |
| CONVERT | CONV | Convert file format |
| COPY | COP | Copy a file |
| CREATE | CRE | Create a new file or directory |
| DEASSIGN | DEAS | Remove a logical name |
| DEFINE | DEFI | Create a logical name definition |
| DELETE | DEL | Delete a file or symbol |
| DIFFERENCES | DIFF | Compare two files |
| DIRECTORY | DIR | List files in a directory |
| DISMOUNT | DISM | Dismount a volume |
| DUMP | DU | Display file contents in hex/ASCII |
| EDIT | ED | Invoke the text editor |
| EXIT | EX | Terminate command procedure or session |
| HELP | HE | Display help information |
| INQUIRE | INQ | Read input and assign to symbol |
| INSTALL | INST | Manage known images |
| LIBRARY | LIB | Manage text/help/object libraries |
| LINK | LIN | Link object modules |
| LOGOUT | LO | Terminate interactive session |
| MAIL | MA | Send/receive electronic mail |
| MONITOR | MON | Display system activity statistics |
| MOUNT | MOU | Mount a volume on a device |
| OPEN | OP | Open a file for I/O |
| PHONE | PHO | Interactive conversation utility |
| PIPE | PIP | Execute a DCL pipeline |
| PRINT | PRI | Queue a file for printing |
| PRODUCT | PROD | Software product management |
| PURGE | PUR | Delete old file versions |
| READ | RE | Read a record from a file |
| RECALL | REC | Show or re-execute command history |
| RENAME | REN | Rename a file |
| REPLY | REP | Send operator reply |
| REQUEST | REQ | Send operator request |
| RUN | RU | Execute a program image |
| SEARCH | SEA | Search a file for text |
| SET | SE | Set system/process/file characteristics |
| SHOW | SH | Display system/process/file information |
| SORT | SO | Sort records in a file |
| SPAWN | SP | Create a subprocess |
| STOP | ST | Stop the current process |
| SUBMIT | SUB | Submit to a batch queue |
| SYSGEN | SYSG | System parameter utility |
| SYSMAN | SYSM | System management utility |
| TCPIP | TCP | TCP/IP network management |
| TYPE | TY | Display file contents |
| WAIT | WA | Wait for a time interval |
| WRITE | WR | Write a record to a file |

---

## File Operations

### APPEND

Append the contents of a source file to a destination file.

```
APPEND source-file destination-file
```

**Parameters:**
- source-file -- File to read from
- destination-file -- File to append to (created if it does not exist)

**VMS Compatibility:** Fully compatible. No qualifier support.

### COPY

Copy a file to a new location.

```
COPY source-file destination-file
```

**Parameters:**
- source-file -- File to copy
- destination-file -- Target file or directory

**Qualifiers:**
- `/LOG` -- Report each file copied

If the destination is a directory, the source filename is appended automatically.

**VMS Compatibility:** Fully compatible for single-file copy. Wildcard copy not implemented.

### CREATE

Create a new file or directory.

```
CREATE filespec
CREATE /DIRECTORY directory-spec
```

**Parameters:**
- filespec -- Name of file to create

**Qualifiers:**
- `/DIRECTORY` -- Create a directory instead of a file

Without `/DIRECTORY`, creates an empty file. In interactive mode, reads lines from SYS$INPUT until EOF (Ctrl-Z). Validates filenames against ODS-2 naming rules.

**VMS Compatibility:** Fully compatible for basic file/directory creation.

### DELETE

Delete a file, symbol, or queue entry.

```
DELETE filespec [,filespec...]
DELETE /SYMBOL symbol-name
DELETE /ENTRY=n
```

**Parameters:**
- filespec -- File(s) to delete (wildcards supported)

**Qualifiers:**
- `/SYMBOL` -- Delete a DCL symbol instead of a file
- `/GLOBAL` -- (With /SYMBOL) Delete from global symbol table
- `/ENTRY=n` -- Delete a queue entry
- `/CONFIRM` -- Prompt before each deletion
- `/LOG` -- Report each file deleted

Supports wildcard file deletion. With `/SYMBOL`, removes a symbol from the local or global table.

**VMS Compatibility:** Fully compatible for basic operations. Version-specific deletion supported via VMS file versioning.

### DIFFERENCES

Compare two files and display differences.

```
DIFFERENCES file1 file2
```

**Parameters:**
- file1 -- First file to compare
- file2 -- Second file to compare

Performs a line-by-line comparison with VMS-style output format.

**VMS Compatibility:** Partial. Basic line diff only; no /OUTPUT, /CHANGE_BAR, or /SLP qualifiers.

### DIRECTORY

List files in VMS format.

```
DIRECTORY [filespec]
```

**Parameters:**
- filespec -- (Optional) Directory or file pattern; defaults to current directory

**Qualifiers:**
- `/SIZE` -- Show file sizes
- `/DATE` -- Show file dates
- `/FULL` -- Show full details (size, date, owner, protection)
- `/BRIEF` -- Show filenames only (no heading)
- `/OWNER` -- Show file owner
- `/TOTAL` -- Show only totals
- `/GRAND_TOTAL` -- Show grand total across directories
- `/NOHEADING` -- Suppress directory heading
- `/PROTECTION` -- Show file protection
- `/COLUMNS=n` -- Number of columns for display
- `/VERSIONS=n` -- Limit version display

Output is formatted in VMS style with ODS-2 filenames, VMS-format dates, and block-count sizes.

**VMS Compatibility:** Fully compatible for common usage patterns.

### DUMP

Display file contents in hexadecimal and ASCII.

```
DUMP filespec
```

**Parameters:**
- filespec -- File to dump

**Qualifiers:**
- `/BLOCKS` -- Dump by blocks
- `/RECORDS` -- Dump by records

Displays file contents in a hex + ASCII format similar to VMS DUMP output.

**VMS Compatibility:** Partial. Basic hex dump; no /HEADER, /BYTE, /WORD, /LONGWORD qualifiers.

### PURGE

Delete all but the highest N versions of files.

```
PURGE [filespec]
```

**Parameters:**
- filespec -- (Optional) File pattern; defaults to `*.*` in the current directory

**Qualifiers:**
- `/KEEP=n` -- Number of versions to keep (default: 1)
- `/LOG` -- Report each file deleted

Uses the VMS file versioning system (vmsfs_purge_versions).

**VMS Compatibility:** Fully compatible.

### RENAME

Change the name and/or location of a file.

```
RENAME old-filespec new-filespec
```

**Parameters:**
- old-filespec -- Current file specification
- new-filespec -- New file specification

**VMS Compatibility:** Fully compatible for basic rename. No wildcard rename.

### SEARCH

Search a file for a text string.

```
SEARCH filespec search-string
```

**Parameters:**
- filespec -- File to search
- search-string -- Text to find

**Qualifiers:**
- `/EXACT` -- Case-sensitive search (default is case-insensitive)
- `/NUMBERS` -- Show line numbers
- `/STATISTICS` -- Show match count

Output format matches VMS SEARCH with file header banner.

**VMS Compatibility:** Fully compatible for single-file search. No /WINDOW or /FORMAT qualifiers.

### SORT

Sort records in a file.

```
SORT input-file output-file
```

**Parameters:**
- input-file -- File to sort
- output-file -- Sorted output file

**Qualifiers:**
- `/KEY=(POSITION:n,SIZE:n)` -- Sort key specification

**VMS Compatibility:** Partial. Basic sort by lines; key specification parsing is limited.

### TYPE

Display the contents of a file.

```
TYPE filespec
```

**Parameters:**
- filespec -- File to display

**Qualifiers:**
- `/PAGE` -- Pause output every 24 lines

**VMS Compatibility:** Fully compatible.

---

## Logical Name Operations

### ASSIGN

Assign an equivalence name to a logical name.

```
ASSIGN equivalence-name logical-name
```

**Parameters:**
- equivalence-name -- The value the logical name translates to
- logical-name -- The logical name to create (automatically uppercased)

**Qualifiers:**
- `/PROCESS` -- (Default) Process-level assignment

**Note:** ASSIGN uses the traditional VMS argument order (equivalence first, logical name second). All table qualifiers map to the global symbol table in the current implementation.

**VMS Compatibility:** Partial. Single-table implementation; /JOB, /GROUP, /SYSTEM qualifiers accepted but not distinguished.

### DEASSIGN

Remove a logical name.

```
DEASSIGN logical-name
```

**Parameters:**
- logical-name -- Logical name to remove

**Qualifiers:**
- `/PROCESS` -- (Default) Remove from process table
- `/JOB` -- Remove from job table
- `/GROUP` -- Remove from group table
- `/SYSTEM` -- Remove from system table
- `/ALL` -- Remove from all tables

Uses the logical name manager (LNM) when available; falls back to global symbol deletion.

**VMS Compatibility:** Fully compatible when LNM manager is running.

### DEFINE

Create a logical name definition.

```
DEFINE logical-name equivalence-string
```

**Parameters:**
- logical-name -- Logical name to create (automatically uppercased)
- equivalence-string -- The translation value

**Qualifiers:**
- `/PROCESS` -- (Default) Process table
- `/JOB` -- Job table
- `/GROUP` -- Group table
- `/SYSTEM` -- System table

**Note:** DEFINE uses the reverse argument order from ASSIGN (logical name first). Uses the LNM manager for proper logical name tables; falls back to global symbols.

**VMS Compatibility:** Fully compatible when LNM manager is running.

---

## I/O Operations

### OPEN

Open a file for reading or writing.

```
OPEN channel-name filespec
```

**Parameters:**
- channel-name -- Logical name for the file channel (max 16 channels)
- filespec -- File to open

**Qualifiers:**
- `/READ` -- (Default) Open for reading
- `/WRITE` -- Open for writing (creates/truncates)
- `/APPEND` -- Open for appending

**VMS Compatibility:** Fully compatible for basic I/O.

### CLOSE

Close a file channel.

```
CLOSE channel-name
```

**Parameters:**
- channel-name -- Channel name from a previous OPEN command

**VMS Compatibility:** Fully compatible.

### READ

Read a record from a file into a symbol.

```
READ channel-name symbol-name
```

**Parameters:**
- channel-name -- Channel name from a previous OPEN command
- symbol-name -- Symbol to receive the line

**Qualifiers:**
- `/END_OF_FILE=label` -- Label to branch to on EOF

Sets `$STATUS` and `$SEVERITY` after each read.

**VMS Compatibility:** Fully compatible.

### WRITE

Write a record to a file.

```
WRITE channel-name expression
```

**Parameters:**
- channel-name -- Channel name from a previous OPEN command
- expression -- String to write (symbol substitution is performed)

**VMS Compatibility:** Fully compatible.

---

## SHOW Subcommands

```
SHOW keyword [parameters]
```

### SHOW TIME

Display the current date and time in VMS format.

```
$ SHOW TIME
  5-MAR-2026 14:30:22.00
```

### SHOW DEFAULT

Display the current default directory.

```
$ SHOW DEFAULT
  DKA0:[SYS0.SYSCOMMON.SYSEXE]
```

### SHOW LOGICAL

Display logical name definitions.

```
SHOW LOGICAL [name]
```

Without a name, shows all logical names. With a name, shows the translation for that specific logical.

**Qualifiers:**
- `/FULL` -- Show detailed information including table and access mode
- `/SYSTEM` -- Show system table only
- `/PROCESS` -- Show process table only
- `/GROUP` -- Show group table only
- `/JOB` -- Show job table only
- `/TABLE=name` -- Show specific table

### SHOW SYSTEM

Display information about all processes on the system.

Shows PID, process name, state, priority, CPU time, and page count for each process.

### SHOW PROCESS

Display information about the current process.

Shows process name, PID, UIC, priority, default directory, and image.

**Qualifiers:**
- `/PRIVILEGES` -- Show process privileges
- `/QUOTAS` -- Show process quotas
- `/ALL` -- Show all process information

### SHOW USERS

Display currently logged-in users.

Shows username, terminal, PID, and login time in VMS format.

### SHOW SYMBOL

Display the value of a DCL symbol.

```
SHOW SYMBOL [symbol-name]
SHOW SYMBOL /ALL
```

**Qualifiers:**
- `/ALL` -- Show all defined symbols
- `/GLOBAL` -- Show global symbols

### SHOW VERIFY

Display the current SET VERIFY state.

### SHOW PROTECTION

Display the current default file protection.

### SHOW DEVICE

Display information about system devices.

```
SHOW DEVICE [device-name]
```

Shows device name, status, mount status, and capacity for disk devices.

**Qualifiers:**
- `/FULL` -- Show full device details
- `/MOUNTED` -- Show only mounted devices

### SHOW MEMORY

Display system memory information.

Shows total, used, and free physical memory in VMS page format (512-byte pages).

### SHOW STATUS

Display current process status.

Shows $STATUS, $SEVERITY, PID, image name, CPU time, and page faults.

### SHOW TERMINAL

Display terminal characteristics.

Shows device type, width, page size, and terminal attributes.

### SHOW TRANSLATION

Display the translation of a logical name (iterative).

### SHOW LICENSE

Display software license information.

### SHOW CLUSTER

Display cluster information (returns "not a member of a cluster").

### SHOW NETWORK

Display network information.

### SHOW ERROR

Display device error counts.

### SHOW WORKING_SET

Display process working set information.

### SHOW ACCOUNTING

Display accounting status.

### SHOW AUDIT

Display security audit status.

### SHOW QUOTA

Display disk quota information.

### SHOW ROOT

Display the VMS root directory mapping.

### SHOW QUEUE

Display batch and print queue status.

```
SHOW QUEUE [queue-name]
```

**Qualifiers:**
- `/FULL` -- Show detailed queue information
- `/ALL_JOBS` -- Show all jobs in queue

### SHOW ENTRY

Display queue entry status.

```
SHOW ENTRY [entry-number]
```

### SHOW INTRUSION

Display intrusion detection database entries.

---

## SET Subcommands

```
SET keyword [parameters]
```

### SET DEFAULT

Change the current default directory.

```
SET DEFAULT directory-spec
```

Supports VMS directory specs (`[.SUBDIR]`, `[-]`, `DKA0:[DIR]`) and relative navigation.

**VMS Compatibility:** Fully compatible.

### SET PROMPT

Change the DCL prompt string.

```
SET PROMPT="new-prompt"
```

### SET VERIFY / SET NOVERIFY

Enable or disable command procedure verification (echo).

```
SET VERIFY
SET NOVERIFY
```

### SET TERMINAL

Set terminal characteristics.

```
SET TERMINAL [device-name]
```

**Qualifiers:**
- `/WIDTH=n` -- Set terminal width
- `/PAGE=n` -- Set page size
- `/DEVICE_TYPE=type` -- Set terminal type
- `/INQUIRE` -- Query terminal for capabilities
- `/ECHO` / `/NOECHO` -- Enable/disable character echo
- `/WRAP` / `/NOWRAP` -- Enable/disable line wrapping
- `/INSERT` / `/OVERSTRIKE` -- Set editing mode

### SET PROTECTION

Set default file protection.

```
SET PROTECTION=(S:RWED,O:RWED,G:RE,W:E) [/DEFAULT]
```

**Qualifiers:**
- `/DEFAULT` -- Set the default protection for new files

### SET PASSWORD

Change the user password.

```
SET PASSWORD
```

Prompts for old password, new password, and verification.

### SET MESSAGE

Control message display format.

```
SET MESSAGE [/FACILITY] [/SEVERITY] [/IDENTIFICATION] [/TEXT]
SET MESSAGE /NOFACILITY /NOSEVERITY
```

### SET CONTROL

Enable or disable Ctrl-Y and Ctrl-T handling.

```
SET CONTROL=Y
SET NOCONTROL=Y
SET CONTROL=T
```

### SET PROCESS

Set process characteristics.

```
SET PROCESS
```

**Qualifiers:**
- `/NAME=name` -- Set process name
- `/PRIORITY=n` -- Set process priority
- `/PRIVILEGES=(priv,...)` -- Modify process privileges

### SET FILE

Set file attributes.

```
SET FILE filespec
```

**Qualifiers:**
- `/PROTECTION=(prot)` -- Set file protection
- `/OWNER=[uic]` -- Set file owner
- `/VERSION_LIMIT=n` -- Set maximum file version count

### SET UIC

Set the process UIC (User Identification Code).

```
SET UIC [group,member]
```

### SET WORKING_SET

Set process working set parameters.

```
SET WORKING_SET
```

**Qualifiers:**
- `/EXTENT=n` -- Set working set extent
- `/QUOTA=n` -- Set working set quota

### SET TIME

Set the system time (requires OPER or SETPRV privilege).

```
SET TIME=dd-mmm-yyyy:hh:mm:ss
```

### SET HOST

Connect to a remote system (stub).

### SET AUDIT

Configure security auditing (stub).

### SET ACCOUNTING

Enable or disable system accounting. The enabled state is a real, persisted,
system-wide flag that gates the accounting record path (not a per-process
value). The ACCOUNTING.DAT record journal itself is not yet implemented.

### SET VOLUME

Set volume characteristics. Dispatches a real SET VOLUME sub-handler with its
own keyword validation, short of VMS's full option set.

### SET ON / SET NOON

Enable or disable error handling in command procedures.

```
SET ON       ! Re-enable error handler
SET NOON     ! Suppress error handler at current level
```

### SET ENTRY

Modify a queue entry.

```
SET ENTRY entry-number
```

**Qualifiers:**
- `/AFTER=time` -- Delay execution
- `/PRIORITY=n` -- Set priority
- `/JOB_NAME=name` -- Set job name

### SET QUEUE

Modify queue characteristics.

```
SET QUEUE queue-name
```

**Qualifiers:**
- `/START` -- Start the queue
- `/STOP` -- Stop the queue
- `/BASE_PRIORITY=n` -- Set base priority

---

## Process Control

### ATTACH

Transfer terminal control to another process.

```
ATTACH process-name
```

**VMS Compatibility:** Partial. Maps to an interactive selection of running processes.

### CONTINUE

Resume execution of an interrupted image (stopped by Ctrl-Y).

```
CONTINUE
```

Sends SIGCONT to the stopped child process.

**VMS Compatibility:** Fully compatible.

### EXIT

Terminate a command procedure or interactive session.

```
EXIT [status-code]
```

If a status code is provided, sets `$STATUS`.

**VMS Compatibility:** Fully compatible.

### LOGOUT

Terminate an interactive session.

```
LOGOUT
```

Displays session summary including CPU time and elapsed time.

**VMS Compatibility:** Fully compatible.

### RUN

Execute a program image.

```
RUN image-name
```

**Parameters:**
- image-name -- Path to executable (`.exe` extension tried automatically)

Forks a child process and waits for completion. Supports Ctrl-Y interruption (child is stopped, resumable with CONTINUE).

**SYS$INPUT from the procedure (vms-1a9).** When RUN is invoked from a command
procedure, the image's SYS$INPUT is the procedure's following data lines -- the
lines that do NOT begin with `$`, up to the next `$`-command or end-of-file
(VSI OpenVMS User's Manual, "Data Lines in a Command Procedure"). This is the
mechanism DEC install/config procedures use to drive a utility's REPL
(AUTHORIZE, SYSGEN) non-interactively.

**Apostrophe substitution on those data lines (vms-963).** DCL performs forced
apostrophe (`'symbol'`) substitution on the SYS$INPUT data lines exactly as on
command lines (VSI OpenVMS User's Manual / DCL Concepts, "Symbol
Substitution") -- the same `dcl_sym_substitute()` used for command lines,
applied in `dcl_sysinput_setup()`. So a run-time value can be passed into a
utility's input:

```
$ RUN SYS$SYSTEM:AUTHORIZE
MODIFY SYSTEM/PASSWORD='OVMX_PW1'
EXIT
```

Here `'OVMX_PW1'` interpolates the value of the DCL symbol `OVMX_PW1` before
AUTHORIZE reads the line. A data line with no apostrophe/ampersand marker is
passed byte-for-byte; a lone apostrophe not introducing a symbol is kept
(VMS literal-apostrophe behaviour). Covered by
`tests/dcl/test_run_sysinput_apostrophe.sh` and
`tests/dcl/test_run_sysinput_procedure.sh`.

**VMS Compatibility:** Fully compatible for basic image execution.

### SPAWN

Create a DCL subprocess.

```
SPAWN [command]
```

**Qualifiers:**
- `/NOWAIT` -- Run subprocess in background
- `/OUTPUT=file` -- Redirect output to file
- `/NOLOGIN` -- Skip login initialization

Without a command, spawns an interactive DCL subprocess. With a command, executes it and returns.

**VMS Compatibility:** Fully compatible.

### STOP

Stop the current process or a named process.

```
STOP
```

**VMS Compatibility:** Fully compatible.

### WAIT

Wait for a specified time interval.

```
WAIT delta-time
```

**Parameters:**
- delta-time -- Time in `HH:MM:SS.cc` format (e.g., `00:00:30`, `::05`)

**VMS Compatibility:** Fully compatible.

---

## Symbol Operations

### INQUIRE

Read input from SYS$INPUT and assign to a symbol.

```
INQUIRE symbol-name [prompt-string]
```

**Parameters:**
- symbol-name -- Symbol to receive input
- prompt-string -- (Optional) Prompt to display

Displays the prompt (with `: ` appended), reads a line, and assigns it to the symbol.

**VMS Compatibility:** Fully compatible.

---

## Pipeline Operations

### PIPE

Execute a DCL pipeline.

```
PIPE command1 | command2 | command3
```

Connects commands with Unix-style pipes. Each command runs in a child process with stdout connected to the next command's stdin.

**VMS Compatibility:** Compatible with VMS 7.x+ PIPE command.

---

## Queue Operations

### SUBMIT

Submit a command procedure to a batch queue.

```
SUBMIT filespec
```

**Qualifiers:**
- `/QUEUE=name` -- Target queue (default: SYS$BATCH)
- `/AFTER=time` -- Delay execution
- `/LOG_FILE=file` -- Specify log file
- `/PRIORITY=n` -- Job priority
- `/NOTIFY` -- Notify on completion
- `/JOB_NAME=name` -- Job name

**VMS Compatibility:** Fully compatible.

### PRINT

Queue a file for printing.

```
PRINT filespec
```

**Qualifiers:**
- `/QUEUE=name` -- Target queue (default: SYS$PRINT)
- `/COPIES=n` -- Number of copies
- `/NOTIFY` -- Notify on completion
- `/JOB_NAME=name` -- Job name
- `/AFTER=time` -- Delay execution

**VMS Compatibility:** Fully compatible.

---

## Network Operations

### TCPIP

TCP/IP Services network management.

```
TCPIP subcommand [parameters]
```

**Subcommands:**

**TCPIP SHOW INTERFACE** -- Display network interfaces (IP address, netmask, flags)

**TCPIP SHOW ROUTE** -- Display routing table

**TCPIP SHOW HOST** -- Display host name resolution

**TCPIP SHOW VERSION** -- Display TCP/IP stack version

**TCPIP SET HOST** -- Add host table entry

**TCPIP SET NAME_SERVICE** -- Configure DNS resolver

**TCPIP SET INTERFACE** -- Configure network interface

**TCPIP SET ROUTE** -- Add or remove routes

**VMS Compatibility:** Partial. Implements the most common TCPIP management subcommands.

---

## System Utilities

### ACCOUNTING

Display login accounting information for the current user.

```
ACCOUNTING
```

Shows username, login time, CPU time, and other session statistics.

### ANALYZE

Analyze system components.

```
ANALYZE subcommand
```

Supports `/RMS_FILE` and `/OBJECT` qualifiers for file analysis.

**VMS Compatibility:** Partial. Basic analysis only.

### BACKUP

Create, restore, or list a saveset file.

```
BACKUP source destination
```

Implementation is in `dcl_backup.c`.

**VMS Compatibility:** Partial.

### CONVERT

Convert file format.

```
CONVERT input-file output-file
```

Copies file content (basic format conversion).

**VMS Compatibility:** Stub. Performs simple file copy.

### EDIT

Invoke the text editor.

```
EDIT filespec
```

Launches `vi` or `$EDITOR` on the specified file.

**VMS Compatibility:** Partial. Uses the host system editor instead of EDT/TPU.

### HELP

Display help information about DCL commands.

```
HELP [topic]
```

Without a topic, lists all available commands with descriptions. With a topic, displays help for that specific command.

**VMS Compatibility:** Fully compatible for command-level help.

### INSTALL

Manage known images.

```
INSTALL subcommand image-name
```

Supports `ADD`, `REMOVE`, `LIST`, `REPLACE` subcommands with `/OPEN`, `/SHARED`, `/HEADER_RESIDENT`, `/PRIVILEGED`, `/PROTECTED` qualifiers.

**VMS Compatibility:** Partial. Tracks known images but does not implement VMS-style image activation.

### LIBRARY

Manage text, help, and object libraries.

```
LIBRARY library-name
```

Implementation is in `dcl_library.c`.

**VMS Compatibility:** Partial.

### LINK

Link object modules into an executable image.

```
LINK filespec
```

**Qualifiers:**
- `/EXECUTABLE=name` -- Output executable name
- `/MAP` -- Generate link map
- `/DEBUG` -- Include debug information
- `/SHAREABLE` -- Create shared image

Invokes the host system linker (cc/gcc) with appropriate flags.

**VMS Compatibility:** Partial. Uses host system toolchain.

### MAIL

Send and receive electronic mail messages.

```
MAIL [recipient]
```

**VMS Compatibility:** Implemented. Native MAIL utility (`tools/vms_mail.c`).

### MONITOR

Display real-time system activity statistics.

```
MONITOR class
```

**VMS Compatibility:** Implemented. Native MONITOR utility (`tools/vms_monitor.c`).

### MOUNT

Mount a volume on a device.

```
MOUNT device: [volume-label] [logical-name]
```

**Qualifiers:**
- `/SYSTEM` -- System-wide mount
- `/FOREIGN` -- Mount without file structure validation

Maps to Linux mount/bind operations.

**VMS Compatibility:** Partial.

### DISMOUNT

Dismount a volume from a device.

```
DISMOUNT device:
```

**VMS Compatibility:** Partial.

### PHONE

Interactive conversation utility.

```
PHONE [user]
```

**VMS Compatibility:** Stub.

### PRODUCT

Software product management.

```
PRODUCT subcommand
```

**VMS Compatibility:** Implemented. PRODUCT/PCSI kit reader; INSTALL, SHOW
PRODUCT, and SHOW HISTORY are real, with rooted-layout install proven
end-to-end in CI.

### RECALL

Show or re-execute commands from command history.

```
RECALL [/ALL]
RECALL command-number
```

**Qualifiers:**
- `/ALL` -- Show entire command history

Displays previous commands and allows re-execution by number.

**VMS Compatibility:** Fully compatible.

### REPLY

Send an operator reply or enable/disable operator terminal.

```
REPLY "message"
REPLY /ENABLE=class
REPLY /DISABLE=class
```

**Qualifiers:**
- `/ENABLE=class` -- Enable operator terminal for a class
- `/DISABLE=class` -- Disable operator terminal for a class

**VMS Compatibility:** Partial. Message delivery is simulated.

### REQUEST

Send a request message to the operator.

```
REQUEST "message"
```

**Qualifiers:**
- `/TO=class` -- Target operator class

**VMS Compatibility:** Partial.

### SYSGEN

Invoke SYSGEN system parameter utility.

```
SYSGEN
```

**VMS Compatibility:** Implemented (thin). SET/SHOW round-trips a real,
file-backed parameter and enforces its range; ~6 of ~600 VMS tunables are
present. AUTOGEN is not implemented.

### SYSMAN

Invoke SYSMAN system management utility.

```
SYSMAN [subcommand]
```

**VMS Compatibility:** Partial.

---

## VMS Compatibility Notes

**Fully compatible commands** -- These behave identically to their VMS counterparts for standard usage: APPEND, CLOSE, CONTINUE, COPY, CREATE, DEFINE, DEASSIGN, DELETE, DIRECTORY, EXIT, HELP, INQUIRE, LOGOUT, OPEN, PIPE, PRINT, PURGE, READ, RECALL, RUN, SEARCH, SET DEFAULT, SET VERIFY, SHOW DEFAULT, SHOW LOGICAL, SHOW TIME, SPAWN, SUBMIT, TYPE, WAIT, WRITE.

**Partially compatible commands** -- These work but lack some VMS-specific features: ANALYZE, ASSIGN (single table), ATTACH, BACKUP, CONVERT, DIFFERENCES, DUMP, EDIT, INSTALL, LIBRARY, LINK, MAIL, MONITOR, MOUNT/DISMOUNT, PRODUCT, REPLY/REQUEST, SORT, SYSGEN, SYSMAN, SET (various subcommands), SHOW (various subcommands), TCPIP.

**Stub commands** -- These are recognized but provide minimal or no functionality: PHONE.

### Abbreviation Rules

All commands support minimum-uniqueness abbreviation. The minimum number of characters required is defined per command (shown in the summary table). For example, `DIR` matches DIRECTORY, `SH` matches SHOW, and `DEL` matches DELETE.

### File Specifications

OVMX accepts VMS-style file specifications throughout:
- `DKA0:[DIR.SUBDIR]FILE.EXT;version`
- `[.SUBDIR]FILE.EXT`
- `[-]` (parent directory)
- Wildcards: `*` (any sequence), `%` (single character)

File specifications are translated to Linux paths at the point of use via `dcl_resolve_path()`.
