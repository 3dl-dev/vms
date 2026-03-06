# DCL Command Qualifier Audit

Status of VMS qualifier implementation in OVMX DCL commands.

Legend: **Implemented** = functional | **Stub** = recognized but no-op | **Missing** = not yet recognized

## File Commands

### COPY
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /LOG | Implemented | Reports each file copied |
| /REPLACE | Stub | Recognized; overwrite is default behavior |
| /CONFIRM | Missing | |
| /ALLOCATION | Missing | |
| /CONCATENATE | Missing | |

### DELETE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /LOG | Implemented | Reports each file deleted |
| /CONFIRM | Implemented | Prompts before each delete |
| /SYMBOL | Implemented | Deletes a symbol instead of a file |
| /GLOBAL | Implemented | With /SYMBOL, deletes global symbol |
| /ENTRY | Implemented | Deletes a queue entry |
| /NOCONFIRM | Implemented | Suppresses /CONFIRM (parser handles negation) |
| /BEFORE | Missing | |
| /SINCE | Missing | |
| /CREATED | Missing | |
| /MODIFIED | Missing | |
| /EXPIRED | Missing | |

### RENAME
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /LOG | Implemented | Reports each file renamed |
| /CONFIRM | Missing | |

### CREATE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /DIRECTORY | Implemented | Creates a directory |
| /LOG | Missing | |
| /OWNER_UIC | Missing | |
| /PROTECTION | Missing | |

### APPEND
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /LOG | Missing | |
| /NEW_VERSION | Missing | |

### PURGE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /KEEP=n | Implemented | Number of versions to keep |
| /LOG | Implemented | Reports each file purged |
| /CONFIRM | Implemented | Prompts before purging each file |
| /BEFORE | Missing | |
| /SINCE | Missing | |

## Display Commands

### DIRECTORY
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /SIZE | Implemented | Show file sizes in blocks |
| /DATE | Implemented | Show file modification dates |
| /FULL | Implemented | Full listing (size + date + protection + owner) |
| /BRIEF | Implemented | Filename only |
| /COLUMNS=n | Implemented | Number of columns for default display |
| /OWNER | Implemented | Show file owner UIC |
| /TOTAL | Implemented | Show only totals, suppress file listing |
| /GRAND_TOTAL | Implemented | Show grand total across directories |
| /HEADING | Implemented | Show directory heading (default on) |
| /NOHEADING | Implemented | Suppress directory heading |
| /TRAILING | Implemented | Show trailing directory information |
| /PROTECTION | Missing | Show file protection separately from /FULL |
| /SINCE | Missing | |
| /BEFORE | Missing | |
| /CREATED | Missing | |
| /MODIFIED | Missing | |
| /ACL | Missing | |
| /VERSIONS | Missing | |

### TYPE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /PAGE | Implemented | Paginated output |
| /OUTPUT | Missing | |

### SEARCH
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /EXACT | Implemented | Case-sensitive search |
| /NUMBERS | Implemented | Show line numbers with matches |
| /STATISTICS | Implemented | Show match count summary |
| /HIGHLIGHT | Missing | |
| /WINDOW | Missing | |
| /OUTPUT | Missing | |
| /REMAINING | Missing | |
| /MATCH=AND | Missing | |

### DUMP
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /BLOCKS | Implemented | Display block count |
| /BYTE | Missing | |
| /WORD | Missing | |
| /LONGWORD | Missing | |

### DIFFERENCES
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic diff) | Implemented | Compares two files |
| /OUTPUT | Missing | |
| /PARALLEL | Missing | |
| /CHANGE_BAR | Missing | |

## Process & System Commands

### SHOW PROCESS
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /PRIVILEGES | Implemented | Show process privileges |
| /QUOTAS | Implemented | Show process quotas |
| /ALL | Implemented | Show all processes |
| /CONTINUOUS | Missing | |
| /SUBPROCESSES | Missing | |
| /ACCOUNTING | Missing | |

### SHOW SYSTEM
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Shows system processes |
| /FULL | Missing | |
| /NODE | Missing | |

### SHOW MEMORY
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Shows memory info |
| /FULL | Missing | |
| /POOL | Missing | |

### SHOW DEVICE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /FULL | Implemented | Detailed device info |
| /MOUNTED | Missing | |
| /FILES | Missing | |

### SHOW TERMINAL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Shows terminal settings |

## Symbol & Logical Commands

### DEFINE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /TABLE=name | Implemented | Specify logical name table |
| /SYSTEM | Implemented | System-wide logical |
| /USER_MODE | Implemented | User-mode logical |
| /EXECUTIVE_MODE | Missing | |
| /LOG | Missing | |

### DEASSIGN
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /TABLE=name | Implemented | Specify logical name table |
| /ALL | Implemented | Deassign all from table |
| /LOG | Missing | |

### SHOW LOGICAL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /TABLE=name | Implemented | Show from specific table |
| /FULL | Missing | |

### SHOW SYMBOL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /ALL | Implemented | Show all symbols |
| /GLOBAL | Implemented | Show global symbols |
| /LOCAL | Implemented | Show local symbols |

## SET Commands

### SET DEFAULT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Change default directory |

### SET PROMPT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Change DCL prompt |

### SET VERIFY
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Enable/disable command verification |

### SET TERMINAL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /WIDTH=n | Implemented | Set terminal width |
| /PAGE=n | Implemented | Set terminal page size |
| /INQUIRE | Implemented | Query terminal capabilities |

### SET PROTECTION
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /DEFAULT | Implemented | Set default protection |
| (file) | Implemented | Set protection on files |

### SET FILE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /PROTECTION | Implemented | Set file protection |
| /OWNER_UIC | Implemented | Set file owner |
| /VERSION_LIMIT | Implemented | Set version limit |
| /ENTER | Implemented | Create directory entry |
| /REMOVE | Implemented | Remove directory entry |

### SET PROCESS
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /NAME | Implemented | Set process name |
| /PRIORITY | Implemented | Set process priority |
| /PRIVILEGES | Implemented | Set process privileges |

## Job/Queue Commands

### SUBMIT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /QUEUE=name | Implemented | Specify queue |
| /AFTER=time | Implemented | Schedule for later |
| /PRIORITY=n | Implemented | Set priority |
| /LOG_FILE | Missing | |
| /NOTIFY | Missing | |

### PRINT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /QUEUE=name | Implemented | Specify queue |
| /COPIES=n | Implemented | Number of copies |
| /FORM=name | Implemented | Print form |
| /NOTIFY | Missing | |
| /DELETE | Missing | |

### SHOW QUEUE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /ALL | Implemented | Show all queues |
| /FULL | Implemented | Full queue info |
| /FORM | Implemented | Show queue forms |

### SET ENTRY
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /PRIORITY=n | Implemented | Change entry priority |
| /AFTER=time | Implemented | Reschedule entry |
| /RELEASE | Implemented | Release held entry |
| /HOLD | Implemented | Hold entry |

## I/O Commands

### OPEN
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /READ | Implemented | Open for reading |
| /WRITE | Implemented | Open for writing |
| /APPEND | Implemented | Open for appending |
| /SHARE | Missing | |
| /ERROR | Missing | |

### READ
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Read from channel |
| /END_OF_FILE | Missing | |
| /ERROR | Missing | |

### WRITE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Write to channel |
| /SYMBOL | Missing | |
| /ERROR | Missing | |

## Other Commands

### SORT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /KEY=(options) | Implemented | Sort key specification |
| /STABLE | Missing | |
| /DUPLICATES | Missing | |

### SPAWN
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /NOLOGICAL_NAMES | Implemented | Don't inherit logicals |
| /WAIT | Implemented | Wait for subprocess |
| /INPUT | Missing | |
| /OUTPUT | Missing | |
| /PROCESS | Missing | |

### PIPE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic piping) | Implemented | cmd1 | cmd2 |

### MOUNT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /SYSTEM | Implemented | System-wide mount |
| /CLUSTER | Missing | |
| /FOREIGN | Missing | |

### EDIT
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /EDT | Implemented | EDT line editor |
| /TPU | Missing | |

### HELP
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Display help topics |

### RECALL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /ALL | Implemented | Show all recalled commands |
| /ERASE | Implemented | Erase recall buffer |

### MAIL
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | Send/read mail |

### INQUIRE
| Qualifier | Status | Notes |
|-----------|--------|-------|
| /NOPUNCTUATION | Implemented | Suppress trailing colon |
| /GLOBAL | Implemented | Create global symbol |
| /LOCAL | Implemented | Create local symbol |

### MONITOR
| Qualifier | Status | Notes |
|-----------|--------|-------|
| (basic) | Implemented | System monitoring |

---
*Generated as part of vms-898.6 qualifier audit. Last updated: 2026-03-05.*
