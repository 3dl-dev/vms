# VMS System Services API Reference

## Overview

The OVMX system services layer implements the VMS `sys$` API on top of Linux
primitives. Programs include `<starlet.h>` to access all system service
prototypes, just as on real OpenVMS. Status codes are defined in `<ssdef.h>`
and follow the VMS convention: odd values indicate success, even values
indicate failure.

**VMS status code convention:**

```c
uint32_t status = sys$some_service(...);
if (status & 1) {
    /* success */
} else {
    /* failure - check status against SS$_ constants */
}
```

All system services return `uint32_t` status codes. The status structure is:

| Bits | Field | Description |
|------|-------|-------------|
| 0-2 | Severity | 0=warning, 1=success, 2=error, 3=info, 4=severe |
| 3-15 | Message number | Within the facility |
| 16-27 | Facility number | 0=SYSTEM (SS$_) |
| 28 | Customer bit | Customer-defined messages |

Use the `$VMS_STATUS_SUCCESS(code)` macro to test success without knowing
the specific code.

## Common Types

### Descriptors (`<descrip.h>`)

VMS strings are passed as descriptors, not null-terminated C strings. The most
common type is the fixed-length (CLASS_S) descriptor:

```c
struct dsc$descriptor_s {
    uint16_t  dsc$w_length;    /* length in bytes (no null terminator) */
    uint8_t   dsc$b_dtype;     /* data type (DSC$K_DTYPE_T for text) */
    uint8_t   dsc$b_class;     /* class (DSC$K_CLASS_S for fixed) */
    char     *dsc$a_pointer;   /* pointer to string data */
};
```

The `$DESCRIPTOR` macro initializes a static descriptor from a string literal:

```c
$DESCRIPTOR(devnam, "SYS$OUTPUT:");
sys$assign(&devnam, &chan, 0, NULL);
```

The `vms_cstr_to_desc()` helper initializes a descriptor from a C string at
runtime.

### Item Lists (`<lnmdef.h>`)

Several services accept item lists — arrays of `struct item_list_3` terminated
by a zero longword:

```c
struct item_list_3 {
    uint16_t  buflen;      /* buffer length */
    uint16_t  item_code;   /* item code constant */
    void     *bufaddr;     /* buffer address */
    uint16_t *retlen;      /* receives actual length (may be NULL) */
};
```

Item lists are terminated by an entry with both `buflen` and `item_code` equal
to zero. The `ITEM_LIST_END` macro produces the terminator.

### I/O Status Block (`<iodef.h>`)

QIO and other async services return completion status in an IOSB:

```c
struct _iosb {
    uint16_t  iosb$w_status;      /* completion status */
    uint16_t  iosb$w_bcnt;        /* bytes transferred */
    uint32_t  iosb$l_dev_depend;  /* device-dependent info */
};
```

Always check `iosb$w_status` after a QIO completes, not just the return
value of `sys$qio` itself.

---

## Services by Category

### I/O Channel Services
- [sys$assign](#sysassign) — Assign I/O channel to a device or file
- [sys$dassgn](#sysdassgn) — Deassign I/O channel
- [sys$qio](#sysqio) — Queue I/O request (asynchronous)
- [sys$qiow](#sysqiow) — Queue I/O request and wait
- [sys$cancel](#syscancel) — Cancel pending I/O on a channel

### Mailbox Services
- [sys$crembx](#syscrembx) — Create mailbox and assign channel
- [sys$delmbx](#sysdelmbx) — Delete mailbox

### Event Flag Services
- [sys$setef](#syssetef) — Set event flag
- [sys$clref](#sysclref) — Clear event flag
- [sys$waitfr](#syswaitfr) — Wait for single event flag
- [sys$wflor](#syswflor) — Wait for any flag in a set (OR)
- [sys$wfland](#syswfland) — Wait for all flags in a set (AND)
- [sys$synch](#syssynch) — Synchronize with async service completion
- [sys$readef](#sysreadef) — Read event flag cluster state
- [sys$ascefc](#sysascefc) — Associate common event flag cluster (stub)
- [sys$dacefc](#sysdacefc) — Disassociate from common event flag cluster (stub)
- [sys$dlcefc](#sysdlcefc) — Delete common event flag cluster (stub)

### Time Services
- [sys$gettim](#sysgettim) — Get current system time
- [sys$numtim](#sysnumtim) — Convert binary time to numeric components
- [sys$asctim](#sysasctim) — Convert binary time to ASCII string
- [sys$bintim](#sysbintim) — Convert ASCII time string to binary time
- [sys$setimr](#syssetimr) — Set timer
- [sys$cantim](#syscantim) — Cancel timer

### Logical Name Services
- [sys$crelnm](#syscrelnm) — Create logical name
- [sys$dellnm](#sysdellnm) — Delete logical name
- [sys$trnlnm](#systrnlnm) — Translate logical name

### Process Management Services
- [sys$creprc](#syscreprc) — Create subprocess
- [sys$delprc](#sysdelprc) — Delete (terminate) process
- [sys$exit](#sysexit) — Exit image
- [sys$hiber](#syshiber) — Hibernate (suspend) current process
- [sys$wake](#syswake) — Wake a hibernating process
- [sys$suspend](#syssuspend) — Suspend a process
- [sys$resume](#sysresume) — Resume a suspended process
- [sys$forcex](#sysforcex) — Force image exit on a process
- [sys$setpri](#syssetpri) — Set process priority
- [sys$dclexh](#sysdclexh) — Declare exit handler

### AST (Asynchronous System Trap) Services
- [sys$dclast](#sysdclast) — Declare AST
- [sys$setast](#syssetast) — Enable or disable AST delivery

### Process and System Information Services
- [sys$getjpi](#sysgetjpi) — Get job/process information
- [sys$getjpiw](#sysgetjpiw) — Get job/process information (wait)
- [sys$getsyi](#sysgetsyi) — Get system information
- [sys$getsyiw](#sysgetsyiw) — Get system information (wait)

### Memory Management Services
- [sys$expreg](#sysexpreg) — Expand program region
- [sys$cretva](#syscretva) — Create virtual address space
- [sys$deltva](#sysdeltva) — Delete virtual address space
- [sys$crmpsc](#syscrmpsc) — Create and map section

### Lock Manager Services
- [sys$enq](#sysenq) — Enqueue lock request (asynchronous)
- [sys$enqw](#sysenqw) — Enqueue lock request and wait
- [sys$deq](#sysdeq) — Dequeue (release) lock

### Security Services
- [sys$setprv](#syssetprv) — Set or clear process privileges
- [sys$chkpro](#syschkpro) — Check object protection

### Message Services
- [sys$getmsg](#sysgetmsg) — Get message text for a condition value
- [sys$putmsg](#sysputmsg) — Output formatted message(s)

### Formatted ASCII Output Services
- [sys$fao](#sysfao) — Formatted ASCII output (varargs)
- [sys$faol](#sysfaol) — Formatted ASCII output with argument list

### RMS System Service Interface

The following RMS services are declared in `<starlet.h>` and implemented in
`src/vmsrms/`. They take FAB (File Access Block) or RAB (Record Access Block)
pointers. See the RMS documentation for details.

- `sys$open` — Open existing file
- `sys$close` — Close file
- `sys$create` — Create new file
- `sys$erase` — Erase (delete) file
- `sys$parse` — Parse file specification
- `sys$search` — Search for file (wildcard)
- `sys$display` — Display file attributes
- `sys$connect` — Connect record stream
- `sys$disconnect` — Disconnect record stream
- `sys$get` — Get (read) record
- `sys$put` — Put (write) record
- `sys$update` — Update record in place
- `sys$delete` — Delete current record
- `sys$find` — Find record (position without reading)
- `sys$rewind` — Rewind record stream

---

## Detailed Reference

---

### sys$assign

**Assign I/O channel to a device or file**

```c
uint32_t sys$assign(
    const struct dsc$descriptor_s *devnam,
    uint16_t                      *chan,
    uint32_t                       acmode,
    const struct dsc$descriptor_s *mbxnam
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `devnam` | `const struct dsc$descriptor_s *` | Yes | Device or file name descriptor |
| `chan` | `uint16_t *` | Yes | Receives the assigned channel number |
| `acmode` | `uint32_t` | No | Access mode (0=kernel, 3=user); currently ignored |
| `mbxnam` | `const struct dsc$descriptor_s *` | No | Associated mailbox name; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Channel assigned successfully |
| `SS$_BADPARAM` | NULL descriptor or channel pointer |
| `SS$_IVDEVNAM` | Empty device name or MBA: device (use sys$crembx instead) |
| `SS$_EXQUOTA` | No free channel slots in the PCB channel table |
| `SS$_NOSUCHDEV` | Could not open the device or file |

**Description:**

Assigns a VMS-style channel number to a device or file. VMS programs use channel
numbers rather than file descriptors for all I/O. The channel table is stored in
the Per-Process Control Block (PCB).

OVMX resolves VMS device names to Linux equivalents:

| VMS Device Name | Linux Equivalent |
|-----------------|------------------|
| `TT:` or `TT0:` | `/dev/tty` |
| `SYS$INPUT:` | stdin (fd 0) |
| `SYS$OUTPUT:` | stdout (fd 1) |
| `SYS$ERROR:` | stderr (fd 2) |
| `NLA0:` | `/dev/null` |
| `SYS$DISK:` | Current default directory |

If the name does not match a known device, OVMX attempts to open it as a
plain file path.

**Example:**

```c
#include <starlet.h>

uint16_t chan;
$DESCRIPTOR(devnam, "SYS$OUTPUT:");

uint32_t status = sys$assign(&devnam, &chan, 0, NULL);
if (!(status & 1)) {
    /* handle error */
}

/* use chan with sys$qio / sys$qiow */

sys$dassgn(chan);
```

**VMS Compatibility:** Device name resolution covers the most common VMS
devices. Physical disk devices (DKA0:, etc.), tape devices, and cluster-wide
devices are not implemented. The `acmode` and `mbxnam` parameters are accepted
but ignored.

---

### sys$dassgn

**Deassign I/O channel**

```c
uint32_t sys$dassgn(uint16_t chan);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `chan` | `uint16_t` | Yes | Channel number to deassign |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Channel deassigned |
| `SS$_IVCHAN` | Invalid or unassigned channel number |

**Description:**

Closes the underlying Linux file descriptor and releases the channel table
entry. For mailbox channels (created by `sys$crembx`), also closes the peer
end of the socket pair.

**VMS Compatibility:** On real VMS, deassigning a channel with a reference
count greater than one does not close the device. OVMX closes the fd
unconditionally when the slot is freed.

---

### sys$qio

**Queue I/O request (asynchronous)**

```c
uint32_t sys$qio(
    uint32_t  efn,
    uint16_t  chan,
    uint32_t  func,
    void     *iosb,
    void    (*astadr)(uint32_t),
    uint32_t  astprm,
    void     *p1,
    uint32_t  p2,
    uint32_t  p3,
    uint32_t  p4,
    uint32_t  p5,
    uint32_t  p6
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | No | Event flag to set on completion (0 = none) |
| `chan` | `uint16_t` | Yes | I/O channel number |
| `func` | `uint32_t` | Yes | I/O function code (`IO$_` value with optional `IO$M_` modifiers) |
| `iosb` | `void *` | No | I/O Status Block; receives completion status and byte count |
| `astadr` | `void (*)(uint32_t)` | No | AST completion routine (called when I/O completes) |
| `astprm` | `uint32_t` | No | Parameter passed to `astadr` |
| `p1` | `void *` | Func-dependent | For read/write: pointer to buffer |
| `p2` | `uint32_t` | Func-dependent | For read/write: buffer length in bytes |
| `p3` | `uint32_t` | Func-dependent | For block I/O: byte offset (0 = use file position) |
| `p4`-`p6` | `uint32_t` | No | Reserved; pass 0 |

**Supported `func` codes:**

| Code | Value | Description |
|------|-------|-------------|
| `IO$_NOP` | 0 | No operation |
| `IO$_WRITEVBLK` | 48 | Write virtual block |
| `IO$_READVBLK` | 49 | Read virtual block |
| `IO$_READLBLK` | 50 | Read logical block |
| `IO$_WRITELBLK` | 51 | Write logical block |
| `IO$_READPBLK` | 52 | Read physical block |
| `IO$_WRITEPBLK` | 53 | Write physical block |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | I/O queued successfully |
| `SS$_IVCHAN` | Invalid channel number |
| `SS$_BADPARAM` | NULL buffer pointer |
| `SS$_ILLIOFUNC` | Unsupported function code |

**Description:**

Submits an I/O request using Linux `io_uring` for true asynchronous I/O.
Returns immediately; the IOSB is filled, the event flag set, and the AST
called when the I/O completes. Falls back to synchronous `read()`/`write()`
if `io_uring` is not available on the system.

**VMS Compatibility:** Only block read/write operations and NOP are
implemented. Terminal I/O functions (`IO$_READPROMPT`, etc.), network I/O,
and tape operations are not supported and return `SS$_ILLIOFUNC`.
Parameters `p4`-`p6` are ignored.

---

### sys$qiow

**Queue I/O request and wait for completion**

```c
uint32_t sys$qiow(
    uint32_t  efn,
    uint16_t  chan,
    uint32_t  func,
    void     *iosb,
    void    (*astadr)(uint32_t),
    uint32_t  astprm,
    void     *p1,
    uint32_t  p2,
    uint32_t  p3,
    uint32_t  p4,
    uint32_t  p5,
    uint32_t  p6
);
```

Same parameters as `sys$qio`. Blocks until the I/O operation completes before
returning. The IOSB status reflects the result of the completed I/O.

**Return Values:** Same as `sys$qio`, plus:

| Status | Meaning |
|--------|---------|
| `SS$_ENDOFFILE` | Read reached end of file |
| `SS$_ABORT` | I/O error (underlying read/write failed) |

**Example:**

```c
uint16_t chan;
struct _iosb iosb;
char buf[512];
$DESCRIPTOR(devnam, "SYS$INPUT:");

sys$assign(&devnam, &chan, 0, NULL);
uint32_t status = sys$qiow(0, chan, IO$_READVBLK, &iosb, NULL, 0,
                            buf, sizeof(buf), 0, 0, 0, 0);
if (status & 1) {
    /* iosb.iosb$w_bcnt bytes read into buf */
}
sys$dassgn(chan);
```

---

### sys$cancel

**Cancel pending I/O on a channel**

```c
uint32_t sys$cancel(uint16_t chan);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `chan` | `uint16_t` | Yes | Channel number |

**Return Values:** Always returns `SS$_NORMAL`.

**Description:** Stub — returns `SS$_NORMAL` without canceling anything.
The current OVMX I/O model completes all operations synchronously before
returning, so there are no pending operations to cancel.

**VMS Compatibility:** Not functionally compatible. Asynchronous I/O
cancellation is deferred to a future implementation.

---

### sys$crembx

**Create mailbox and assign channel**

```c
uint32_t sys$crembx(
    int                            prmflg,
    uint16_t                      *chan,
    uint32_t                       maxmsg,
    uint32_t                       bufquo,
    uint32_t                       promsk,
    uint32_t                       acmode,
    const struct dsc$descriptor_s *lognam
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `prmflg` | `int` | No | Permanent flag (1=permanent, 0=temporary); currently ignored |
| `chan` | `uint16_t *` | Yes | Receives the assigned channel number |
| `maxmsg` | `uint32_t` | No | Maximum message size in bytes; currently ignored (default 256) |
| `bufquo` | `uint32_t` | No | Buffer quota in bytes; currently ignored (default 1024) |
| `promsk` | `uint32_t` | No | Protection mask; currently ignored |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |
| `lognam` | `const struct dsc$descriptor_s *` | No | Logical name to assign to this mailbox |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Mailbox created |
| `SS$_BADPARAM` | NULL channel pointer |
| `SS$_EXQUOTA` | No free channel slots |
| `SS$_SSFAIL` | `socketpair()` failed |

**Description:**

Creates a message-oriented IPC channel implemented as a Unix `SOCK_DGRAM`
socket pair. One end is assigned to the returned channel number; the other end
(the peer) is stored internally for use by readers. The mailbox is given a
device name of the form `MBA<n>:`.

If `lognam` is provided, a logical name pointing to the mailbox device name
is created in `LNM$PROCESS_TABLE`.

**Example:**

```c
uint16_t mbx_chan;
$DESCRIPTOR(mbx_name, "MY_MAILBOX");

uint32_t status = sys$crembx(0, &mbx_chan, 256, 1024, 0, 0, &mbx_name);
/* mbx_name now translates to "MBA1:" (or similar) in the process table */
```

**VMS Compatibility:** Permanent mailboxes and cross-process mailbox sharing
are not implemented. The `prmflg`, `maxmsg`, `bufquo`, `promsk`, and `acmode`
parameters are accepted but ignored.

---

### sys$delmbx

**Delete mailbox**

```c
uint32_t sys$delmbx(uint16_t chan);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `chan` | `uint16_t` | Yes | Channel number of the mailbox to delete |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Mailbox deleted |
| `SS$_IVCHAN` | Invalid channel or not a mailbox channel |

**Description:**

Closes both ends of the socket pair and releases the channel table entry.
On real VMS, `sys$delmbx` marks the mailbox for deletion when all channels
are deassigned; in OVMX, deletion is immediate.

**VMS Compatibility:** Deferred deletion (mark for deletion) is not
implemented.

---

### sys$setef

**Set event flag**

```c
uint32_t sys$setef(uint32_t efn);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Event flag number (0-127) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_WASCLR` (= `SS$_NORMAL` = 1) | Flag was clear; now set |
| `SS$_WASSET` (= 9) | Flag was already set |
| `SS$_ILLEFC` | Flag number out of range (>= 128) |

**Description:**

Sets the specified event flag bit in the PCB's event flag cluster array.
Any threads blocked on this flag via `sys$waitfr`, `sys$wflor`, or
`sys$wfland` are awakened.

VMS event flags are organized as four clusters of 32 flags each:
- Cluster 0: flags 0-31 (local)
- Cluster 1: flags 32-63 (local)
- Cluster 2: flags 64-95 (common — stub in OVMX)
- Cluster 3: flags 96-127 (common — stub in OVMX)

---

### sys$clref

**Clear event flag**

```c
uint32_t sys$clref(uint32_t efn);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Event flag number (0-127) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_WASSET` | Flag was set; now cleared |
| `SS$_WASCLR` | Flag was already clear |
| `SS$_ILLEFC` | Flag number out of range |

**Description:** Clears the specified event flag bit.

---

### sys$waitfr

**Wait for single event flag**

```c
uint32_t sys$waitfr(uint32_t efn);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Event flag number (0-127) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Flag is set |
| `SS$_ILLEFC` | Flag number out of range |

**Description:**

If the flag is already set, returns immediately. Otherwise blocks using a
pthread condition variable until another thread or AST sets the flag.

**Example:**

```c
/* Queue async I/O then wait */
sys$qio(3, chan, IO$_READVBLK, &iosb, NULL, 0, buf, 512, 0, 0, 0, 0);
sys$waitfr(3);
/* iosb is now valid */
```

---

### sys$wflor

**Wait for any flag in a set (logical OR)**

```c
uint32_t sys$wflor(uint32_t efn, uint32_t mask);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Any flag number within the target cluster (determines cluster) |
| `mask` | `uint32_t` | Yes | Bitmask of flags within the cluster to wait on |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | At least one flag in the mask is set |
| `SS$_ILLEFC` | Flag number out of range |

**Description:**

Blocks until at least one of the flags indicated by `mask` within the cluster
selected by `efn / 32` becomes set.

---

### sys$wfland

**Wait for all flags in a set (logical AND)**

```c
uint32_t sys$wfland(uint32_t efn, uint32_t mask);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Any flag number within the target cluster |
| `mask` | `uint32_t` | Yes | Bitmask of flags; all must be set |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | All masked flags are simultaneously set |
| `SS$_ILLEFC` | Flag number out of range |

**Description:** Blocks until all flags indicated by `mask` within the
cluster are set simultaneously.

---

### sys$synch

**Synchronize with async service completion**

```c
uint32_t sys$synch(uint32_t efn, void *iosb);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Event flag to wait on |
| `iosb` | `void *` | No | I/O Status Block (NULL = wait for flag only) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| (IOSB status) | The value of `iosb$w_status` if `iosb` is non-NULL |
| `SS$_NORMAL` | Flag set and `iosb` is NULL |
| (error from sys$waitfr) | If the wait itself fails |

**Description:**

Implements the standard VMS pattern for waiting on an async service:

```c
status = sys$qio(efn, chan, func, &iosb, NULL, 0, buf, len, 0, 0, 0, 0);
if (status & 1) {
    status = sys$synch(efn, &iosb);
}
/* now check status and iosb.iosb$w_status */
```

---

### sys$readef

**Read event flag cluster state**

```c
uint32_t sys$readef(uint32_t efn, uint32_t *state);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | Yes | Event flag number |
| `state` | `uint32_t *` | No | Receives the full 32-bit cluster state |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_WASSET` | The specified flag `efn` is currently set |
| `SS$_WASCLR` | The specified flag `efn` is currently clear |
| `SS$_ILLEFC` | Flag number out of range |

**Description:** Non-blocking read of the current flag state. The full
32-bit cluster bitmask is written to `*state` if non-NULL.

---

### sys$ascefc

**Associate common event flag cluster (stub)**

```c
uint32_t sys$ascefc(
    uint32_t                       efn,
    const struct dsc$descriptor_s *name,
    uint32_t                       prot,
    uint32_t                       perm
);
```

**Return Values:** Always returns `SS$_NORMAL`.

**VMS Compatibility:** Stub implementation. Common event flag cluster
sharing between processes (flags 64-127) is not implemented. All parameters
are ignored.

---

### sys$dacefc

**Disassociate from common event flag cluster (stub)**

```c
uint32_t sys$dacefc(uint32_t efn);
```

**Return Values:** Always returns `SS$_NORMAL`.

**VMS Compatibility:** Stub. See `sys$ascefc`.

---

### sys$dlcefc

**Delete common event flag cluster (stub)**

```c
uint32_t sys$dlcefc(const struct dsc$descriptor_s *name);
```

**Return Values:** Always returns `SS$_NORMAL`.

**VMS Compatibility:** Stub. See `sys$ascefc`.

---

### sys$gettim

**Get current system time**

```c
uint32_t sys$gettim(uint64_t *timadr);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `timadr` | `uint64_t *` | Yes | Receives the 64-bit VMS time value |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL pointer |

**Description:**

Returns the current time as a 64-bit VMS time value: the number of
100-nanosecond intervals since November 17, 1858 00:00:00.00 (the
Smithsonian Modified Julian Date base epoch). Uses
`clock_gettime(CLOCK_REALTIME)` for nanosecond precision.

**Example:**

```c
uint64_t now;
sys$gettim(&now);
```

**VMS Compatibility:** The time base and units match real VMS exactly.

---

### sys$numtim

**Convert binary time to numeric components**

```c
uint32_t sys$numtim(uint16_t timbuf[7], const uint64_t *timadr);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `timbuf` | `uint16_t[7]` | Yes | Output array: year, month, day, hour, minute, second, hundredths |
| `timadr` | `const uint64_t *` | No | VMS time to convert (NULL = current time) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL `timbuf` pointer |

**Description:**

Decomposes a VMS binary time into its calendar components. Output buffer
layout:

| Index | Field | Range |
|-------|-------|-------|
| 0 | Year | e.g., 2026 |
| 1 | Month | 1-12 |
| 2 | Day | 1-31 |
| 3 | Hour | 0-23 |
| 4 | Minute | 0-59 |
| 5 | Second | 0-59 |
| 6 | Hundredths | 0-99 |

---

### sys$asctim

**Convert binary time to ASCII string**

```c
uint32_t sys$asctim(
    uint16_t               *timlen,
    struct dsc$descriptor_s *timbuf,
    const uint64_t         *timadr,
    uint32_t                cvtflg
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `timlen` | `uint16_t *` | No | Receives the output string length |
| `timbuf` | `struct dsc$descriptor_s *` | Yes | Output buffer descriptor |
| `timadr` | `const uint64_t *` | No | VMS time to convert (NULL = current time) |
| `cvtflg` | `uint32_t` | No | Conversion flags; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL or invalid output descriptor |

**Description:**

Converts a VMS binary time to the standard VMS ASCII date/time format:
`DD-MMM-YYYY HH:MM:SS.CC` (23 characters). Example: `19-FEB-2026 14:30:00.00`.

**VMS Compatibility:** The `cvtflg` parameter (which controls date-only vs.
full format) is accepted but ignored; the full format is always produced.

---

### sys$bintim

**Convert ASCII time string to binary time**

```c
uint32_t sys$bintim(
    const struct dsc$descriptor_s *timbuf,
    uint64_t                      *timadr
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `timbuf` | `const struct dsc$descriptor_s *` | Yes | Input time string descriptor |
| `timadr` | `uint64_t *` | Yes | Receives the 64-bit VMS binary time |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL pointer or parse failure |

**Description:**

Parses the VMS date/time format `DD-MMM-YYYY HH:MM:SS.CC` and converts it
to a VMS 64-bit binary time. The time component is optional; date-only strings
are accepted.

**Example:**

```c
uint64_t target;
$DESCRIPTOR(timestr, "25-DEC-2026 00:00:00.00");
sys$bintim(&timestr, &target);
```

---

### sys$setimr

**Set timer request**

```c
uint32_t sys$setimr(
    uint32_t        efn,
    const uint64_t *daytim,
    void          (*astadr)(uint32_t),
    uint32_t        reqidt,
    uint32_t        flags
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | No | Event flag to set on expiration (0 = none) |
| `daytim` | `const uint64_t *` | Yes | Expiration time; negative = delta (relative), positive = absolute VMS time |
| `astadr` | `void (*)(uint32_t)` | No | AST routine to call on expiration |
| `reqidt` | `uint32_t` | No | Request ID for later cancellation via `sys$cantim` |
| `flags` | `uint32_t` | No | Reserved; pass 0 |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Timer set |
| `SS$_BADPARAM` | NULL `daytim` or invalid time value |
| `SS$_EXQUOTA` | Timer table full (max 32 concurrent timers) |
| `SS$_INSFMEM` | Could not create POSIX timer |

**Description:**

Schedules a one-shot timer using a POSIX `timer_create` / `timer_settime`.
On expiration, the event flag (if non-zero) is set and the AST routine (if
non-NULL) is called.

VMS delta times are represented as negative 64-bit values:
- Negative `daytim`: relative time (delta) from now
- Positive `daytim`: absolute VMS time

**Example:**

```c
/* Wait 1 second: -10,000,000 ticks (10M * 100ns = 1s) */
uint64_t delta = (uint64_t)(-10000000LL);
sys$setimr(5, &delta, NULL, 1, 0);
sys$waitfr(5);  /* blocks for 1 second */
```

**VMS Compatibility:** Repeating timers are not supported (VMS also does not
repeat automatically, but the mechanism is cleaner). The maximum of 32
concurrent timers is an OVMX limitation not present in VMS.

---

### sys$cantim

**Cancel timer request**

```c
uint32_t sys$cantim(uint32_t reqidt, uint32_t acmode);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `reqidt` | `uint32_t` | Yes | Request ID to cancel (0 = cancel all) |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |

**Return Values:** Always returns `SS$_NORMAL`.

**Description:** Cancels the POSIX timer(s) matching `reqidt`. If `reqidt`
is 0, all active timers are cancelled.

---

### sys$crelnm

**Create logical name**

```c
uint32_t sys$crelnm(
    const uint32_t                *attr,
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t                 *acmode,
    const struct item_list_3      *itmlst
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `attr` | `const uint32_t *` | No | Logical name attributes (`LNM$M_` bits); NULL = no attributes |
| `tabnam` | `const struct dsc$descriptor_s *` | Yes | Table name (e.g., `LNM$_PROCESS_TABLE`) |
| `lognam` | `const struct dsc$descriptor_s *` | Yes | Logical name to create |
| `acmode` | `const uint8_t *` | No | Access mode; currently ignored |
| `itmlst` | `const struct item_list_3 *` | Yes | Item list with `LNM$_STRING` entry for equivalence value |

**Relevant item codes:**

| Code | Meaning |
|------|---------|
| `LNM$_STRING` (2) | The equivalence string |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Logical name created |
| `SS$_SUPERSEDE` | An existing name was replaced |
| `SS$_BADPARAM` | NULL required parameter |
| `SS$_IVLOGNAM` | Empty or too-long logical name |
| `SS$_EXQUOTA` | Logical name table full (max 1024 entries) |

**Description:**

Creates or replaces a logical name in the specified table. Logical name
lookups are case-insensitive (names are stored uppercased). The equivalence
string is extracted from the item list entry with code `LNM$_STRING`.

**Standard table names:**

| Constant | String | Scope |
|----------|--------|-------|
| `LNM$_PROCESS_TABLE` | `"LNM$PROCESS_TABLE"` | Process-private |
| `LNM$_JOB_TABLE` | `"LNM$JOB"` | Job (process group) |
| `LNM$_GROUP_TABLE` | `"LNM$GROUP"` | Group |
| `LNM$_SYSTEM_TABLE` | `"LNM$SYSTEM_TABLE"` | System-wide |

**Example:**

```c
$DESCRIPTOR(tabnam, LNM$_PROCESS_TABLE);
$DESCRIPTOR(lognam, "MY_DIR");
char equiv[] = "/home/user/project";

struct item_list_3 itmlst[2] = {
    { sizeof(equiv) - 1, LNM$_STRING, equiv, NULL },
    ITEM_LIST_END
};

sys$crelnm(NULL, &tabnam, &lognam, NULL, itmlst);
```

**VMS Compatibility:** Multi-valued logical names (multiple `LNM$_STRING`
entries per name), concealed devices, and cluster-wide logical names are
not implemented. Only the first `LNM$_STRING` item is used.

---

### sys$dellnm

**Delete logical name**

```c
uint32_t sys$dellnm(
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t                 *acmode
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `tabnam` | `const struct dsc$descriptor_s *` | Yes | Table name |
| `lognam` | `const struct dsc$descriptor_s *` | Yes | Logical name to delete |
| `acmode` | `const uint8_t *` | No | Access mode; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Deleted successfully |
| `SS$_BADPARAM` | NULL required parameter |
| `SS$_NOLOGNAM` | Name not found in the specified table |

---

### sys$trnlnm

**Translate logical name**

```c
uint32_t sys$trnlnm(
    const uint32_t                *attr,
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t                 *acmode,
    const struct item_list_3      *itmlst
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `attr` | `const uint32_t *` | No | Attribute match criteria; currently ignored |
| `tabnam` | `const struct dsc$descriptor_s *` | No | Table to search (NULL = search standard order) |
| `lognam` | `const struct dsc$descriptor_s *` | Yes | Logical name to translate |
| `acmode` | `const uint8_t *` | No | Access mode; currently ignored |
| `itmlst` | `const struct item_list_3 *` | Yes | Item list receiving translation results |

**Supported item codes in `itmlst`:**

| Code | Value | Description |
|------|-------|-------------|
| `LNM$_STRING` | 2 | Equivalence string |
| `LNM$_LENGTH` | 3 | Length of equivalence string (uint32_t) |
| `LNM$_ATTRIBUTES` | 5 | Logical name attributes (uint32_t) |
| `LNM$_MAX_INDEX` | 7 | Maximum translation index (always 0) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Name found and translated |
| `SS$_BADPARAM` | NULL logical name |
| `SS$_NOLOGNAM` | Name not found |

**Description:**

Translates a logical name and returns results via the item list. When `tabnam`
is NULL, searches tables in the standard VMS order:
`LNM$PROCESS_TABLE` → `LNM$JOB` → `LNM$GROUP` → `LNM$SYSTEM_TABLE`.

**Example:**

```c
char equiv[256];
uint16_t retlen;
$DESCRIPTOR(tabnam, LNM$_PROCESS_TABLE);
$DESCRIPTOR(lognam, "MY_DIR");

struct item_list_3 itmlst[2] = {
    { sizeof(equiv), LNM$_STRING, equiv, &retlen },
    ITEM_LIST_END
};

uint32_t status = sys$trnlnm(NULL, &tabnam, &lognam, NULL, itmlst);
if (status & 1) {
    equiv[retlen] = '\0';  /* equiv is now a C string */
}
```

---

### sys$creprc

**Create subprocess**

```c
uint32_t sys$creprc(
    uint32_t                      *pidadr,
    const struct dsc$descriptor_s *image,
    const struct dsc$descriptor_s *input,
    const struct dsc$descriptor_s *output,
    const struct dsc$descriptor_s *error,
    const void                    *prvadr,
    const void                    *quota,
    const struct dsc$descriptor_s *prcnam,
    uint32_t                       baspri,
    uint32_t                       uic,
    uint32_t                       mbxunt,
    uint32_t                       stsflg
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `uint32_t *` | No | Receives new process PID |
| `image` | `const struct dsc$descriptor_s *` | Yes | Path to executable image |
| `input` | `const struct dsc$descriptor_s *` | No | SYS$INPUT equivalent (file path) |
| `output` | `const struct dsc$descriptor_s *` | No | SYS$OUTPUT equivalent (file path) |
| `error` | `const struct dsc$descriptor_s *` | No | SYS$ERROR equivalent (file path) |
| `prvadr` | `const void *` | No | Privilege mask (uint64_t*); NULL = inherit parent |
| `quota` | `const void *` | No | Quota list; currently ignored |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name (max 15 chars) |
| `baspri` | `uint32_t` | No | Base priority (0-31); currently ignored |
| `uic` | `uint32_t` | No | UIC for new process (0 = inherit parent) |
| `mbxunt` | `uint32_t` | No | Termination mailbox unit; currently ignored |
| `stsflg` | `uint32_t` | No | Status flags (`PRC$M_` values); currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Process created |
| `SS$_BADPARAM` | NULL image descriptor |
| `SS$_INSFMEM` | `fork()` failed |

**Description:**

Creates a subprocess using `fork()` + `execl()`. The child inherits VMS
context (privileges, UIC, username, default directory) from the parent PCB
when parameters are zero/NULL. I/O redirection to files is set up from the
`input`, `output`, and `error` descriptors.

**VMS Compatibility:** VMS images are EXE files located via `sys$system:`
logical names; OVMX uses plain file paths. Process quotas, base priority
mapping, and the termination mailbox are not implemented. I/O redirection
only supports file paths, not device names or channel numbers.

---

### sys$delprc

**Delete (terminate) process**

```c
uint32_t sys$delprc(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `const uint32_t *` | No | Process ID to terminate; NULL = current process |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Signal sent |
| `SS$_NONEXPR` | Process does not exist |

**Description:** Sends `SIGTERM` to the target process. If `pidadr` is NULL
and `prcnam` is NULL, terminates the calling process.

**VMS Compatibility:** On VMS, `sys$delprc` on the current process cleanly
exits the image and runs exit handlers. In OVMX, use `sys$exit` for clean
self-termination.

---

### sys$exit

**Exit image**

```c
uint32_t sys$exit(uint32_t code);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `code` | `uint32_t` | Yes | VMS exit status code |

**Return Values:** Does not return.

**Description:**

Runs exit handlers declared via `sys$dclexh` in LIFO order, then calls
`_exit()`. VMS success (odd `code`) maps to Linux exit code 0; VMS failure
(even `code`) maps to Linux exit code 1.

---

### sys$hiber

**Hibernate current process**

```c
uint32_t sys$hiber(void);
```

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Returned after being awakened |

**Description:**

Suspends the current process until awakened by `sys$wake` or a signal.
Implemented using `pause()`, which blocks until any signal is delivered.

**VMS Compatibility:** On VMS, hibernation is a clean wait state with no
signal interaction. The `pause()`-based implementation means that any signal
(not just `SIGCONT`) will awaken the process.

---

### sys$wake

**Wake a hibernating process**

```c
uint32_t sys$wake(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `const uint32_t *` | No | PID to wake; NULL = current process |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Signal sent |
| `SS$_NONEXPR` | Process does not exist |

**Description:** Sends `SIGCONT` to the target process, waking it from
`sys$hiber` or `sys$suspend`.

---

### sys$suspend

**Suspend a process**

```c
uint32_t sys$suspend(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `const uint32_t *` | No | PID to suspend; NULL = current process |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Signal sent |
| `SS$_NONEXPR` | Process does not exist |

**Description:** Sends `SIGSTOP` to the target process. Resume with
`sys$resume`.

---

### sys$resume

**Resume a suspended process**

```c
uint32_t sys$resume(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam
);
```

**Parameters:** Same as `sys$suspend`.

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Signal sent |
| `SS$_NONEXPR` | Process does not exist |

**Description:** Sends `SIGCONT` to resume a process suspended by
`sys$suspend` or `sys$hiber`.

---

### sys$forcex

**Force image exit on a process**

```c
uint32_t sys$forcex(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam,
    uint32_t                       code
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `const uint32_t *` | No | Target PID; NULL = current process |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |
| `code` | `uint32_t` | Yes | Exit status code to force |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_NONEXPR` | Process does not exist |

**Description:**

If `pidadr` and `prcnam` are both NULL, calls `sys$exit(code)` on the current
process. Otherwise sends `SIGUSR1` to the target process. Note that the `code`
is only used when forcing exit on the calling process.

**VMS Compatibility:** On real VMS, `sys$forcex` sends the exit status to the
target's image exit handler. OVMX uses `SIGUSR1` which does not carry the
exit code.

---

### sys$setpri

**Set process priority**

```c
uint32_t sys$setpri(
    const uint32_t                *pidadr,
    const struct dsc$descriptor_s *prcnam,
    uint32_t                       pri,
    uint32_t                      *prvpri
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pidadr` | `const uint32_t *` | No | Target PID; currently ignored (always operates on current process) |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |
| `pri` | `uint32_t` | Yes | New priority (0=lowest, 31=highest) |
| `prvpri` | `uint32_t *` | No | Receives previous priority |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Priority set |
| `SS$_NOPRIV` | `setpriority()` failed (likely insufficient privilege) |

**Description:**

Maps VMS priority (0-31) to Linux nice values (-20 to 19):
VMS 31 (highest) → nice -20; VMS 0 (lowest) → nice 19.
Only affects the calling process regardless of `pidadr`.

**VMS Compatibility:** Cannot set priority on other processes. Priority
increases (below current nice value) typically require root on Linux.

---

### sys$dclexh

**Declare exit handler**

```c
uint32_t sys$dclexh(void *desblk);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `desblk` | `void *` | Yes | Exit handler control block |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Exit handler registered |
| `SS$_BADPARAM` | NULL pointer |
| `SS$_EXQUOTA` | Handler table full |

**Description:**

Registers an exit handler to be called by `sys$exit` before the process
terminates. Handlers are called in LIFO (last-in, first-out) order. The
`desblk` pointer is stored and passed back to the handler.

VMS exit handler control block layout (conventional):

| Offset | Field | Description |
|--------|-------|-------------|
| 0 | Forward link | Managed by system |
| 4/8 | Handler address | Routine to call |
| 8/16 | Argument count | Number of additional args |
| 12/24 | Status address | Address of exit status longword |

**VMS Compatibility:** The full VMS exit handler calling convention with
argument lists is not enforced; handlers receive only the exit status code
address as documented.

---

### sys$dclast

**Declare AST (Asynchronous System Trap)**

```c
uint32_t sys$dclast(
    void    (*astadr)(uint32_t),
    uint32_t  astprm,
    uint32_t  acmode
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `astadr` | `void (*)(uint32_t)` | Yes | AST routine to call |
| `astprm` | `uint32_t` | Yes | Parameter passed to the AST routine |
| `acmode` | `uint32_t` | Yes | Access mode for AST queue (0=kernel, 3=user) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | AST queued |
| `SS$_BADPARAM` | NULL routine address |
| `SS$_INSFMEM` | Could not allocate queue entry |
| `SS$_EXASTLM` | AST queue limit reached |

**Description:**

Queues an AST in the PCB's per-mode AST queue. If AST delivery is enabled
for the specified access mode, the process is signaled via `SIGUSR1` to
deliver the AST. ASTs are delivered in FIFO order within each access mode.

ASTs are more structured than Unix signals: each carries its own function
pointer and parameter and can be individually queued without loss.

**VMS Compatibility:** AST delivery uses `SIGUSR1` as the delivery
mechanism, which may interact with other uses of `SIGUSR1`. Common event
flag cluster ASTs are not implemented.

---

### sys$setast

**Enable or disable AST delivery**

```c
uint32_t sys$setast(uint32_t enable);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `enable` | `uint32_t` | Yes | 1 to enable AST delivery, 0 to disable |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_WASSET` | AST delivery was previously enabled |
| `SS$_WASCLR` | AST delivery was previously disabled |

**Description:**

Enables or disables AST delivery for the current access mode. Disabling
ASTs prevents their delivery but does not discard them; queued ASTs are
delivered when ASTs are re-enabled. When enabling, any pending queued ASTs
are delivered immediately.

---

### sys$getjpi

**Get job/process information**

```c
uint32_t sys$getjpi(
    uint32_t                   efn,
    const uint32_t            *pidadr,
    const struct dsc$descriptor_s *prcnam,
    const struct item_list_3  *itmlst,
    void                      *iosb,
    void                     (*astadr)(uint32_t),
    uint32_t                   astprm
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | No | Event flag; currently ignored |
| `pidadr` | `const uint32_t *` | No | Target PID; NULL = current process |
| `prcnam` | `const struct dsc$descriptor_s *` | No | Process name; currently ignored |
| `itmlst` | `const struct item_list_3 *` | Yes | Item list for results |
| `iosb` | `void *` | No | I/O status block; currently ignored |
| `astadr` | `void (*)(uint32_t)` | No | AST completion routine; currently ignored |
| `astprm` | `uint32_t` | No | AST parameter; currently ignored |

**Implemented item codes:**

| Code | Value | Type | Description |
|------|-------|------|-------------|
| `JPI$_PID` | 0x0101 | `uint32_t` | Process ID |
| `JPI$_PRCNAM` | 0x0100 | string | Process name |
| `JPI$_USERNAME` | 0x0105 | string | Username |
| `JPI$_UIC` | 0x0104 | `uint32_t` | User Identification Code `[group,member]` |
| `JPI$_CPUTIM` | 0x010D | `uint32_t` | CPU time in 10ms units |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL item list |

**Description:**

Returns process information via an item list. Only the five item codes listed
above are implemented; unrecognized item codes are silently skipped. The
current implementation always completes synchronously.

**VMS Compatibility:** Most `JPI$_` item codes defined in `<prcdef.h>` are
not implemented and will be silently ignored. The `efn`, `iosb`, `astadr`,
and `astprm` parameters (async completion) are not used.

---

### sys$getjpiw

**Get job/process information (wait for completion)**

```c
uint32_t sys$getjpiw(
    uint32_t                   efn,
    const uint32_t            *pidadr,
    const struct dsc$descriptor_s *prcnam,
    const struct item_list_3  *itmlst,
    void                      *iosb,
    void                     (*astadr)(uint32_t),
    uint32_t                   astprm
);
```

Same parameters and return values as `sys$getjpi`. In OVMX, this is an alias
for `sys$getjpi` since the implementation is already synchronous.

---

### sys$getsyi

**Get system information**

```c
uint32_t sys$getsyi(
    uint32_t                   efn,
    const uint32_t            *csidadr,
    const struct dsc$descriptor_s *nodename,
    const struct item_list_3  *itmlst,
    void                      *iosb,
    void                     (*astadr)(uint32_t),
    uint32_t                   astprm
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | No | Event flag; currently ignored |
| `csidadr` | `const uint32_t *` | No | Cluster system ID; currently ignored |
| `nodename` | `const struct dsc$descriptor_s *` | No | Cluster node name; currently ignored |
| `itmlst` | `const struct item_list_3 *` | Yes | Item list for results |
| `iosb` | `void *` | No | I/O status block; currently ignored |
| `astadr` | `void (*)(uint32_t)` | No | AST completion routine; currently ignored |
| `astprm` | `uint32_t` | No | AST parameter; currently ignored |

**Implemented item codes:**

| Code | Value | Type | Description |
|------|-------|------|-------------|
| `SYI$_NODENAME` | 0x0200 | string | System hostname |
| `SYI$_VERSION` | 0x0202 | string | OVMX version string (`"V0.1"`) |
| `SYI$_HW_NAME` | 0x0204 | string | Hardware description (`"OVMX Virtual System"`) |
| `SYI$_AVAILCPU_CNT` | 0x0205 | `uint32_t` | Number of online CPUs |
| `SYI$_ACTIVECPU_CNT` | 0x0206 | `uint32_t` | Number of online CPUs (same value) |
| `SYI$_MEMSIZE` | 0x0207 | `uint32_t` | Physical memory in VMS 512-byte pages |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL item list |

**VMS Compatibility:** Cluster-related information is not applicable.
Most `SYI$_` item codes are not implemented and are silently skipped.

---

### sys$getsyiw

**Get system information (wait for completion)**

Same as `sys$getsyi`. OVMX alias — always synchronous.

---

### sys$expreg

**Expand program region**

```c
uint32_t sys$expreg(
    uint32_t  pagcnt,
    void     *retadr,
    uint32_t  acmode,
    uint32_t  region
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `pagcnt` | `uint32_t` | Yes | Number of VMS 512-byte pages to allocate |
| `retadr` | `void *` | Yes | Two-pointer array receiving `[start, end]` of allocated range |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |
| `region` | `uint32_t` | No | 0=P0 region, 1=P1 region; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Memory allocated |
| `SS$_BADPARAM` | NULL `retadr` or zero `pagcnt` |
| `SS$_INSFMEM` | `mmap()` failed |

**Description:**

Allocates `pagcnt` VMS pages (512 bytes each) of anonymous private memory
using `mmap(MAP_PRIVATE | MAP_ANONYMOUS)`. The actual addresses allocated are
returned in `retadr[0]` (first byte) and `retadr[1]` (last byte).

**VMS Compatibility:** VMS P0/P1 region distinction and program region
expansion semantics are not implemented. The memory is anonymous — on VMS,
`sys$expreg` expands the program region at the current end.

---

### sys$cretva

**Create virtual address space**

```c
uint32_t sys$cretva(
    const void *inadr,
    void       *retadr,
    uint32_t    acmode
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `inadr` | `const void *` | Yes | Requested address range: two-pointer array `[start, end]` |
| `retadr` | `void *` | No | Actual address range allocated: `[start, end]` |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Memory mapped |
| `SS$_BADPARAM` | NULL `inadr` or inverted range |
| `SS$_INSFMEM` | `mmap()` failed |

**Description:**

Attempts to map memory at the requested address range using
`MAP_FIXED_NOREPLACE`. If the exact address is unavailable, falls back to
any available address. The actual allocated range is written to `retadr`.

---

### sys$deltva

**Delete virtual address space**

```c
uint32_t sys$deltva(
    const void *inadr,
    void       *retadr,
    uint32_t    acmode
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `inadr` | `const void *` | Yes | Address range to unmap: `[start, end]` |
| `retadr` | `void *` | No | Receives the unmapped range |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Memory unmapped |
| `SS$_BADPARAM` | NULL `inadr` or inverted range |

**Description:** Unmaps the specified address range using `munmap()`.

---

### sys$crmpsc

**Create and map section**

```c
uint32_t sys$crmpsc(
    const void                    *inadr,
    void                          *retadr,
    uint32_t                       acmode,
    uint32_t                       flags,
    const struct dsc$descriptor_s *gsdnam,
    uint64_t                      *ident,
    uint32_t                       relpag,
    uint16_t                       chan,
    uint32_t                       pagcnt,
    uint32_t                       vbn,
    uint32_t                       prot,
    uint32_t                       pfc
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `inadr` | `const void *` | No | Requested address range; currently ignored |
| `retadr` | `void *` | No | Actual mapped address range: `[start, end]` |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |
| `flags` | `uint32_t` | No | Section flags; currently ignored |
| `gsdnam` | `const struct dsc$descriptor_s *` | No | Global section name; currently ignored |
| `ident` | `uint64_t *` | No | Section version ID; currently ignored |
| `relpag` | `uint32_t` | No | Relative page offset; currently ignored |
| `chan` | `uint16_t` | No | I/O channel for file-backed section (0 = anonymous) |
| `pagcnt` | `uint32_t` | Yes | Number of VMS 512-byte pages to map |
| `vbn` | `uint32_t` | No | Virtual block number (1-based) for file offset |
| `prot` | `uint32_t` | No | Protection mask; currently ignored |
| `pfc` | `uint32_t` | No | Page fault cluster size; currently ignored |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Section mapped |
| `SS$_BADPARAM` | Zero `pagcnt` |
| `SS$_IVCHAN` | Non-zero `chan` not valid |
| `SS$_INSFMEM` | `mmap()` failed |

**Description:**

Maps `pagcnt` VMS pages using `mmap()`. When `chan` is 0, creates an
anonymous shared section. When `chan` is non-zero, maps the file associated
with that channel, using `vbn` as the starting block (1-based, 512 bytes
per block). Named global sections (`gsdnam`) are not implemented.

**VMS Compatibility:** Global section naming, copy-on-reference sections,
and most flags are not implemented.

---

### sys$enq

**Enqueue lock request (asynchronous)**

```c
uint32_t sys$enq(
    uint32_t                       efn,
    uint32_t                       lkmode,
    void                          *lksb,
    uint32_t                       flags,
    const struct dsc$descriptor_s *resnam,
    uint32_t                       parid,
    void                         (*astadr)(uint32_t),
    uint32_t                       astprm,
    void                         (*blkastadr)(uint32_t),
    uint32_t                       acmode,
    uint32_t                       rsdm_id
);
```

**Lock modes:**

| Constant | Value | Description |
|----------|-------|-------------|
| `LCK$K_NLMODE` | 0 | Null — no access |
| `LCK$K_CRMODE` | 1 | Concurrent Read |
| `LCK$K_CWMODE` | 2 | Concurrent Write |
| `LCK$K_PRMODE` | 3 | Protected Read |
| `LCK$K_PWMODE` | 4 | Protected Write |
| `LCK$K_EXMODE` | 5 | Exclusive |

**Lock flags (`flags`, `LCK$M_`):** real OpenVMS `$LCKDEF` bit values. The
kernel lock manager honors `VALBLK`, `CONVERT`, `NOQUEUE`, and `SYSTEM`; the
remaining flags are accepted but not yet acted on.

| Constant | Value | Honored | Description |
|----------|-------|---------|-------------|
| `LCK$M_VALBLK` | `0x0001` | yes | Read/write the 16-byte lock value block |
| `LCK$M_CONVERT` | `0x0002` | yes | Convert the existing lock in `lksb$l_lkid` to `lkmode` |
| `LCK$M_NOQUEUE` | `0x0004` | yes | Fail with `SS$_NOTQUEUED` if not immediately grantable |
| `LCK$M_SYNCSTS` | `0x0008` | no | Synchronous-completion hint |
| `LCK$M_SYSTEM` | `0x0010` | yes | System-wide resource namespace |

> **Compat note:** these are the authentic `$LCKDEF` values, which differ
> from the kernel lock manager's internal bitmask; `sys_lock.c` translates
> at the `/dev/vms` boundary so kernel numbering never appears in the public
> contract. Values are a single-lineage community reproduction (FreeVMS
> `lckdef.h`) — VSI/HPE publish the names but no numeric values. See the
> provenance comment in `starlet.h`.

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `efn` | `uint32_t` | No | Event flag to set on completion |
| `lkmode` | `uint32_t` | Yes | Lock mode (`LCK$K_` value) |
| `lksb` | `void *` | Yes | Lock Status Block — receives lock ID and status |
| `flags` | `uint32_t` | No | Lock flags (`LCK$M_` bits; see table above) |
| `resnam` | `const struct dsc$descriptor_s *` | Yes | Resource name descriptor |
| `parid` | `uint32_t` | No | Parent lock ID; currently ignored |
| `astadr` | `void (*)(uint32_t)` | No | Completion AST; delivered on a later grant of a queued async `sys$enq` request (via `VMS_IOCTL_DELIVERAST`). Not used for `sys$enqw`, which blocks in-kernel instead |
| `astprm` | `uint32_t` | No | AST parameter |
| `blkastadr` | `void (*)(uint32_t)` | No | Blocking AST routine; delivered to holders of an incompatible granted lock when a new request blocks on their resource |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |
| `rsdm_id` | `uint32_t` | No | Resource domain ID; currently ignored |

**Lock Status Block layout:**

```c
struct lksb {
    uint16_t lksb$w_status;       /* completion status */
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;         /* lock ID (for sys$deq) */
    char     lksb$b_valblk[16];   /* lock value block */
};
```

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Lock granted (or queued, for async `sys$enq`) |
| `SS$_BADPARAM` | NULL `lksb` |
| `SS$_NOTQUEUED` | `LCK$M_NOQUEUE` set and the mode was not immediately grantable |
| `SS$_DEADLOCK` | Deadlock detected by the kernel lock manager |
| `SS$_NOSUCHDEV` | `/dev/vms` unavailable (e.g. Docker mode — locking is kernel-only) |

Status codes are translated from the kernel lock manager's internal values
to the public `$SSDEF` codes at the library boundary (`kstat_to_ss` in
`sys_lock.c`).

**Description:**

`sys$enq`/`sys$enqw` route through the **kernel lock manager** (`vms.ko`, via
`/dev/vms`) — the single authoritative lock manager — using its 6-mode
compatibility matrix, value blocks, blocking ASTs, deadlock detection, and
lock conversion. There is no userspace `flock` path; locking requires the
kernel module and is therefore a QEMU-mode capability (in Docker mode, with
no `/dev/vms`, these services return `SS$_NOSUCHDEV`).

`sys$enqw` waits until the requested mode is granted; `sys$enq` returns
immediately with the granted-or-queued status.

`sys$enqw`'s wait is implemented entirely in-kernel: it sets `LCK_M_SYNC` on
the request and blocks on `wait_event_interruptible_timeout` until the lock
manager grants the request or declares a deadlock, instead of the caller
busy-polling `$GETLKI` from userspace. If the request is still queued when
the wait times out (500ms), the lock manager re-runs deadlock detection
against the resource's current wait-for graph before re-arming the wait —
so a deadlock that forms *after* the request was queued is detected, not
just deadlocks visible at enqueue time.

For an async `sys$enq` that is queued (not immediately grantable) with an
`astadr` supplied, the kernel lock manager queues a completion AST to the
process's user-mode AST queue when the lock is later granted; userspace
retrieves it via `VMS_IOCTL_DELIVERAST`. Completion ASTs are not queued for
a synchronous (`LCK_M_SYNC`) waiter — that caller is blocked in-kernel and
is woken directly on grant instead.

**VMS Compatibility:** Value blocks, lock conversion (`LCK$M_CONVERT`),
blocking ASTs, completion ASTs on delayed grant, deadlock detection
(including deadlocks that form after a request is queued), and the full
mode-compatibility matrix are supported through the kernel lock manager.

---

### sys$enqw

**Enqueue lock request and wait (synchronous)**

Same parameters and behavior as `sys$enq`. In OVMX, both functions are
synchronous.

---

### sys$deq

**Dequeue (release) lock**

```c
uint32_t sys$deq(
    uint32_t  lkid,
    void     *valblk,
    uint32_t  acmode,
    uint32_t  flags
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `lkid` | `uint32_t` | Yes | Lock ID from `lksb$l_lkid` |
| `valblk` | `void *` | No | 16-byte value block to store in lock (copied to LKSB) |
| `acmode` | `uint32_t` | No | Access mode; currently ignored |
| `flags` | `uint32_t` | No | Dequeue flags (translated to the kernel bitmask; only `VALBLK` is currently acted on) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Lock released |
| `SS$_IVLOCKID` | Lock ID not found / invalid |
| `SS$_NOSUCHDEV` | `/dev/vms` unavailable (kernel-only, as for `sys$enq`) |

**Description:**

Releases the lock identified by `lkid` through the kernel lock manager. If
`valblk` is supplied (with `LCK$M_VALBLK`), its 16 bytes update the resource's
value block. Note: real OpenVMS `$DEQ` has its own flag namespace
(`LCK$M_DEQALL`/`CANCEL`/`INVVALBLK`) distinct from the `$ENQ` flags; only the
value-block behavior is currently honored.

---

### sys$setprv

**Set or clear process privileges**

```c
uint32_t sys$setprv(
    uint32_t         enbflg,
    const uint64_t  *prvadr,
    uint32_t         prmflg,
    uint64_t        *prvprv
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `enbflg` | `uint32_t` | Yes | 1 = enable privileges, 0 = disable |
| `prvadr` | `const uint64_t *` | Yes | 64-bit privilege mask indicating which privileges to change |
| `prmflg` | `uint32_t` | Yes | 1 = permanent change, 0 = current session only |
| `prvprv` | `uint64_t *` | No | Receives previous privilege mask |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Privileges set |
| `SS$_BADPARAM` | Could not get PCB |

**Description:**

Modifies the process privilege mask stored in the PCB. Privileges are a
64-bit mask; the `PRV$M_` bit definitions are in `<prvdef.h>`. When `prmflg`
is 1, the permanent privilege mask is also updated.

**VMS Compatibility:** Privilege enforcement is limited — most system services
in OVMX do not check privileges before proceeding. Privilege masking does not
interact with Linux capability checks.

---

### sys$chkpro

**Check object protection**

```c
uint32_t sys$chkpro(void *objpro);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `objpro` | `void *` | Yes | Protection check block |

**Protection check block layout:**

```c
struct {
    uint32_t owner_uic;    /* UIC of the object owner */
    uint16_t protection;   /* SOGW 16-bit protection mask */
    uint16_t access_type;  /* requested access (PROT$M_READ etc.) */
};
```

**Protection mask bit layout (SOGW, deny semantics):**

| Bits | Category | Access bits (RWED) |
|------|----------|-------------------|
| 15-12 | System | Read/Write/Execute/Delete |
| 11-8 | Owner | Read/Write/Execute/Delete |
| 7-4 | Group | Read/Write/Execute/Delete |
| 3-0 | World | Read/Write/Execute/Delete |

A SET bit means access is DENIED (opposite of Unix permission bits).

**Access type flags:**

| Flag | Value | Meaning |
|------|-------|---------|
| `PROT$M_READ` | 0x08 | Read access |
| `PROT$M_WRITE` | 0x04 | Write access |
| `PROT$M_EXECUTE` | 0x02 | Execute access |
| `PROT$M_DELETE` | 0x01 | Delete access |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Access is granted |
| `SS$_NOPRIV` | Access is denied |
| `SS$_BADPARAM` | NULL `objpro` pointer |

**Description:**

Determines the caller's UIC category (system/owner/group/world) relative to
the object owner's UIC, then checks whether the corresponding protection
bits deny the requested access. UID 0 (root) is treated as system category.

UIC format: upper 16 bits = group, lower 16 bits = member. Mapped from Linux
`getgid()` and `getuid()`.

---

### sys$getmsg

**Get message text for a condition value**

```c
uint32_t sys$getmsg(
    uint32_t                 msgid,
    uint16_t                *msglen,
    struct dsc$descriptor_s *bufadr,
    uint32_t                 flags,
    uint32_t                *outadr
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `msgid` | `uint32_t` | Yes | Condition value (VMS status code) |
| `msglen` | `uint16_t *` | No | Receives the output string length |
| `bufadr` | `struct dsc$descriptor_s *` | Yes | Output buffer descriptor |
| `flags` | `uint32_t` | Yes | Component flags (`MSG$M_` bits); 0 = all components |
| `outadr` | `uint32_t *` | No | Result vector; currently ignored |

**Message component flags:**

| Flag | Value | Component |
|------|-------|-----------|
| `MSG$M_TEXT` | 0x01 | Message text |
| `MSG$M_IDENT` | 0x02 | Message identifier |
| `MSG$M_SEVERITY` | 0x04 | Severity prefix |
| `MSG$M_FACILITY` | 0x08 | Facility name prefix |
| `MSG$M_ALL` | 0x0F | All components |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL or invalid output descriptor |
| `SS$_BUFFEROVF` | Output truncated (buffer too small) |

**Description:**

Formats a VMS condition value into human-readable text. With `MSG$M_ALL`
(flags=0), produces: `%FACILITY-S-IDENT, message text`

For example, `SS$_NORMAL` (code 1) produces:
`%SYSTEM-S-NORMAL, Normal successful completion`

**VMS Compatibility:** The message database is limited to the `SS$_` codes
defined in `<ssdef.h>`. Unknown codes produce a generic formatted output.

---

### sys$putmsg

**Output formatted message(s)**

```c
uint32_t sys$putmsg(
    const uint32_t                        *msgvec,
    uint32_t (*actrtn)(struct dsc$descriptor_s *, uint32_t),
    const struct dsc$descriptor_s         *facnam,
    uint32_t                               actprm
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `msgvec` | `const uint32_t *` | Yes | Message vector |
| `actrtn` | function pointer | No | Action routine called for each line; non-zero return suppresses output |
| `facnam` | `const struct dsc$descriptor_s *` | No | Facility name override; currently ignored |
| `actprm` | `uint32_t` | No | Parameter passed to `actrtn` |

**Message vector layout:**

| Index | Content |
|-------|---------|
| `msgvec[0]` | Argument count (number of longwords following) |
| `msgvec[1]` | Primary message code (condition value) |
| `msgvec[2]` | FAO argument count for primary message |
| `msgvec[3+]` | FAO arguments |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL `msgvec` or zero count |

**Description:**

Formats the primary message code from the message vector and writes it to
`stderr`. If `actrtn` is provided, calls it with the formatted line and
`actprm`; if `actrtn` returns non-zero, the output is suppressed. FAO
substitution into the message text is not currently implemented.

**VMS Compatibility:** Multi-message vectors (secondary messages), FAO
argument substitution into message text, and facility name override are
not implemented.

---

### sys$fao

**Formatted ASCII output (varargs)**

```c
uint32_t sys$fao(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t                      *outlen,
    struct dsc$descriptor_s       *outbuf,
    ...
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ctrstr` | `const struct dsc$descriptor_s *` | Yes | FAO control string descriptor |
| `outlen` | `uint16_t *` | No | Receives actual output length |
| `outbuf` | `struct dsc$descriptor_s *` | Yes | Output buffer descriptor |
| `...` | varargs | Directive-dependent | FAO directive arguments (up to 64) |

**Return Values:**

| Status | Meaning |
|--------|---------|
| `SS$_NORMAL` | Success |
| `SS$_BADPARAM` | NULL or invalid descriptors |
| `SS$_BUFFEROVF` | Output truncated (static buffer too small) |
| `SS$_INSFMEM` | Dynamic descriptor reallocation failed |

**Description:**

Processes the FAO control string and writes formatted output to `outbuf`.
The control string uses `!` as the directive prefix. Dynamic descriptors
(`DSC$K_CLASS_D`) are reallocated to fit the output; static descriptors
are truncated with `SS$_BUFFEROVF`.

**Implemented FAO directives:**

| Directive | Description |
|-----------|-------------|
| `!AS` | Insert string from descriptor argument |
| `!AD` | Insert counted string (length, pointer arguments) |
| `!SL` | Signed longword (32-bit) decimal |
| `!UL` | Unsigned longword decimal |
| `!SW` | Signed word (16-bit) decimal |
| `!UW` | Unsigned word decimal |
| `!SB` | Signed byte (8-bit) decimal |
| `!UB` | Unsigned byte decimal |
| `!XL` | Longword hexadecimal |
| `!XW` | Word hexadecimal |
| `!XB` | Byte hexadecimal |
| `!OL` | Longword octal |
| `!OW` | Word octal |
| `!OB` | Byte octal |
| `!ZL` | Zero-padded longword (8 digits) |
| `!ZW` | Zero-padded word (4 digits) |
| `!ZB` | Zero-padded byte (3 digits) |
| `!/` | Newline |
| `!_` | Horizontal tab |
| `!!` | Literal `!` |
| `!n*c` | Repeat character `c` n times |
| `!%S` | Conditional plural `s` (based on last numeric arg) |

**Example:**

```c
char outbuf[128];
struct dsc$descriptor_s ctrl_dsc, out_dsc;
vms_cstr_to_desc(&ctrl_dsc, "Process !UL: !AS");
out_dsc.dsc$w_length = sizeof(outbuf);
out_dsc.dsc$b_dtype = DSC$K_DTYPE_T;
out_dsc.dsc$b_class = DSC$K_CLASS_S;
out_dsc.dsc$a_pointer = outbuf;

$DESCRIPTOR(name, "MYPROC");
uint16_t outlen;
sys$fao(&ctrl_dsc, &outlen, &out_dsc, (uint64_t)1234, &name);
/* outbuf: "Process 1234: MYPROC" */
```

**VMS Compatibility:** The `sys$fao` varargs implementation extracts
up to 64 arguments. Callers with more arguments should use `sys$faol`.
Unrecognized directives are silently skipped rather than causing an error.

---

### sys$faol

**Formatted ASCII output with argument list**

```c
uint32_t sys$faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t                      *outlen,
    struct dsc$descriptor_s       *outbuf,
    const uint64_t                *prmlst
);
```

**Parameters:**

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `ctrstr` | `const struct dsc$descriptor_s *` | Yes | FAO control string descriptor |
| `outlen` | `uint16_t *` | No | Receives actual output length |
| `outbuf` | `struct dsc$descriptor_s *` | Yes | Output buffer descriptor |
| `prmlst` | `const uint64_t *` | Yes | Array of FAO arguments; consumed in directive order |

**Return Values:** Same as `sys$fao`.

**Description:**

Like `sys$fao`, but arguments are passed as an array (`prmlst`) rather than
varargs. This is the preferred form when the number of arguments is large or
known at compile time. `sys$fao` is implemented by forwarding to `sys$faol`.

**VMS Compatibility:** On VMS, `sys$faol` is the canonical form and `sys$fao`
is a convenience wrapper. OVMX matches this design.

---

## Status Code Quick Reference

Common status codes returned by system services (from `<ssdef.h>`):

| Constant | Value | Meaning |
|----------|-------|---------|
| `SS$_NORMAL` | 1 | Normal successful completion |
| `SS$_WASCLR` | 1 | Previous state was clear |
| `SS$_WASSET` | 9 | Previous state was set |
| `SS$_ACCVIO` | 12 | Access violation |
| `SS$_BADPARAM` | 20 | Bad parameter value |
| `SS$_EXQUOTA` | 28 | Exceeded quota |
| `SS$_NOPRIV` | 36 | No privilege for operation |
| `SS$_INSFMEM` | 292 | Insufficient dynamic memory |
| `SS$_IVTIME` | 388 | Invalid time value |
| `SS$_NOLOGNAM` | 444 | No logical name match |
| `SS$_IVCHAN` | 602 | Invalid channel |
| `SS$_IVDEVNAM` | 608 | Invalid device name |
| `SS$_ILLIOFUNC` | 580 | Illegal I/O function |
| `SS$_ENDOFFILE` | 2160 | End of file |
| `SS$_NOSUCHDEV` | 2680 | No such device |
| `SS$_NONEXPR` | 2540 | Nonexistent process |
| `SS$_SUPERSEDE` | 844 | Logical name superseded |
| `SS$_BUFFEROVF` | 4 | Buffer overflow (warning) |
| `SS$_DEADLOCK` | 708 | Deadlock detected |
| `SS$_EXENQLM` | 2748 | Exceeded enqueue limit |
| `SS$_EXASTLM` | 2756 | Exceeded AST limit |
| `SS$_SSFAIL` | 636 | System service failure |
| `SS$_UNSUPPORTED` | 2296 | Unsupported operation |
| `SS$_MSGNOTFND` | 2308 | Message not found |
| `SS$_ILLEFC` | 2260 | Illegal event flag cluster number |

Use `$VMS_STATUS_SUCCESS(code)` to test for success; use
`$VMS_STATUS_SEVERITY(code)` to extract severity. Both macros are defined
in `<ssdef.h>`.

---

## Implementation Notes

### Thread Safety

All system services that access shared state use pthread mutexes. The PCB
channel table, event flag clusters, AST queues, logical name table, and lock
manager table are all protected by their respective locks. Services are safe
to call from multiple threads simultaneously.

### PCB (Per-Process Control Block)

Most system services retrieve or update per-process state through the
`vms_pcb_get()` function, which returns the current thread's process control
block. The PCB stores:

- Channel table (I/O channel assignments)
- Event flag clusters (128 flags in 4 clusters)
- AST queues (one per access mode)
- Process identity (PID, UIC, username, process name)
- Privilege masks (current and permanent)
- Exit handler list
- io_uring state (for QIO)
- Default directory

### io_uring and Async I/O

`sys$qio` attempts to use Linux `io_uring` for truly asynchronous I/O. If
`io_uring` initialization fails (older kernel, container restrictions), both
`sys$qio` and `sys$qiow` fall back to synchronous `read()`/`write()` calls
that behave identically to `sys$qiow`.

### VMS Time Epoch

VMS times are 100-nanosecond intervals since November 17, 1858. The offset
from the Unix epoch (January 1, 1970) is:

```
0x007C95674BEB4000 ticks  (= 3,506,716,800 seconds)
```

This constant is defined as `VMS_EPOCH_OFFSET` in `sys_time.c`.

### Descriptor Helper Functions

`<descrip.h>` provides several helper functions for descriptor management:

| Function | Description |
|----------|-------------|
| `vms_init_descriptor(desc, str, len)` | Initialize static descriptor |
| `vms_cstr_to_desc(desc, cstr)` | Initialize from C string |
| `vms_desc_to_cstr(desc, buf, bufsz)` | Convert to null-terminated C string |
| `vms_desc_alloc(desc, length)` | Allocate dynamic descriptor |
| `vms_desc_free(desc)` | Free dynamic descriptor storage |
| `dsc$init(str)` | Return initialized descriptor by value |

The `$DESCRIPTOR(name, string)` macro is the standard way to declare
compile-time static descriptors.
