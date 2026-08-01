# VMS Image Activation Design Specification

**Bead**: vms-913.1
**Status**: Authoritative reference for all vms-913.x implementation beads
**Date**: 2026-03-06

---

## 1. Image Activator Architecture

### Overview

IMGACT.EXE is a static musl binary that serves as the OVMX dynamic linker.
It replaces the Linux `ld.so` for all OVMX executables by registering itself
as the ELF interpreter via PT_INTERP. The design follows the same pattern
as musl libc's own `ld-musl-*.so.1`, which is simultaneously the C library
and the dynamic linker in a single binary.

```
 Kernel exec()
     |
     v
 Read ELF PT_INTERP: /vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE
     |
     v
 Kernel loads IMGACT.EXE (static, no interpreter of its own)
     |
     v
 IMGACT.EXE reads target executable ELF headers
     |
     v
 mmap LOAD segments for executable
     |
     v
 Resolve DT_NEEDED --> load shareable images
     |
     v
 Relocate, init TLS, call constructors
     |
     v
 Transfer control to executable entry point
```

### Design Rationale

On real OpenVMS, the image activator (`SYS$IMGACT`) is a privileged kernel
service that maps executables and shareable images into process space, resolves
inter-image references via the Global Symbol Table (GST), and enforces image
version checks (GSMATCH). OVMX cannot replicate this in-kernel on Linux, but
we can achieve equivalent semantics in userspace by controlling the ELF
dynamic linker.

Key properties:

| Property | OpenVMS | OVMX |
|----------|---------|------|
| Activation trigger | `SYS$IMGACT` kernel service | PT_INTERP ELF interpreter |
| Image format | VMS object format (.EXE, .OLB) | ELF shared objects / executables |
| Symbol resolution | Global Symbol Table + transfer vectors | PLT/GOT (standard ELF) |
| Version checking | GSMATCH in image header | Custom `.vms.ident` ELF section |
| Known images | INSTALL utility / KFE list | INSTALL utility / VMS$KNOWN_IMAGES.DAT |
| Search path | SYS$SHARE logical name | Known Image DB -> hardcoded path -> RPATH |

### Static Inclusion of musl libc

IMGACT.EXE is built by statically linking musl libc into the binary, then
**exporting all musl symbols** so that dynamically loaded images can resolve
libc symbols against IMGACT.EXE itself. This is identical to how
`ld-musl-aarch64.so.1` works: the interpreter IS libc.

This means:
- No separate `libc.so` shared library is needed on the system disk
- All OVMX shareable images link against libc symbols provided by IMGACT.EXE
- IMGACT.EXE's symbol table serves as the root of the global symbol namespace

### Scope

IMGACT.EXE is **QEMU-mode only**. Docker mode continues using glibc's
`ld-linux-*.so` unchanged. The Docker build produces standard dynamically-linked
ELF binaries for the Ubuntu runtime. The QEMU build produces musl-linked binaries
activated through IMGACT.EXE.

---

## 2. Activation Sequence

The activation sequence runs every time the kernel encounters an ELF binary
whose PT_INTERP points to IMGACT.EXE.

### Step-by-Step

```
Step 1: Kernel entry
    Kernel reads executable ELF, finds PT_INTERP = IMGACT.EXE
    Kernel loads IMGACT.EXE into process address space
    Kernel sets up auxiliary vector (auxv) with:
        AT_PHDR, AT_PHNUM, AT_PHENT  (executable program headers)
        AT_ENTRY                      (executable entry point)
        AT_BASE                       (IMGACT.EXE load address)
        AT_EXECFN                     (executable filename)

Step 2: IMGACT.EXE _start
    Read auxv to locate executable program headers
    Parse AT_PHDR to find executable LOAD, DYNAMIC, TLS segments

Step 3: Map executable
    For each PT_LOAD segment:
        mmap(addr, filesz, prot, MAP_FIXED|MAP_PRIVATE, fd, offset)
        Zero BSS region if memsz > filesz

Step 4: Parse dependencies
    Read PT_DYNAMIC segment
    Extract DT_NEEDED entries --> list of shareable image SONAMEs
    Build dependency graph (detect circular deps)

Step 5: Load shareable images
    For each DT_NEEDED (breadth-first, topological sort):
        Resolve SONAME via search path (see Section 4)
        Open ELF file, read headers
        Validate GSMATCH version (see Section 5)
        mmap LOAD segments
        Recurse: parse DT_NEEDED of this image

Step 6: Relocate
    Process relocations for each loaded image (leaf-first order):
        R_AARCH64_RELATIVE  (base + addend)
        R_AARCH64_GLOB_DAT  (symbol lookup, write address)
        R_AARCH64_JUMP_SLOT (symbol lookup, write address)
        R_AARCH64_TLSDESC   (TLS descriptor, see Section 10)
    x86_64 equivalents:
        R_X86_64_RELATIVE
        R_X86_64_GLOB_DAT
        R_X86_64_JUMP_SLOT
        R_X86_64_TLSDESC

Step 7: Initialize TLS
    Calculate total TLS size across all images
    Allocate thread control block (TCB) + TLS blocks
    Copy .tdata initialization images
    Zero .tbss regions
    Set TP register (TPIDR_EL0 on aarch64, FS segment on x86_64)

Step 8: Call constructors
    For each image in dependency order (leaves first):
        Call DT_INIT function (if present)
        Call each function in DT_INIT_ARRAY

Step 9: Transfer control
    Jump to AT_ENTRY (executable's _start / entry point)
```

### Sequence Diagram

```
 Process creation (fork+exec or direct exec)
 ============================================

 Kernel                  IMGACT.EXE              Executable
 ------                  ----------              ----------
   |                        |                        |
   |-- load IMGACT.EXE ---->|                        |
   |-- set auxv ----------->|                        |
   |-- jump to _start ----->|                        |
   |                        |                        |
   |                        |-- read auxv            |
   |                        |-- open executable      |
   |                        |-- mmap LOAD segs       |
   |                        |-- parse DT_NEEDED      |
   |                        |                        |
   |                        |   [for each dep]       |
   |                        |-- search path lookup   |
   |                        |-- GSMATCH check        |
   |                        |-- mmap dep LOAD segs   |
   |                        |-- recurse deps         |
   |                        |                        |
   |                        |-- relocate all images  |
   |                        |-- init TLS             |
   |                        |-- call constructors    |
   |                        |                        |
   |                        |-- jmp entry point ---->|
   |                        |                        |-- main()
```

---

## 3. Shareable Image Format

### ELF Layout

Shareable images are standard ELF shared objects built with `musl-gcc -shared`.
The compiler generates standard PLT/GOT entries for inter-image references.
We do NOT implement VMS-style symbol vectors or transfer vectors — that would
require compiler modifications, which is an explicit non-goal.

```
ELF Header
    e_type = ET_DYN (shared object)

Program Headers:
    PT_LOAD     .text (R-X)
    PT_LOAD     .data/.bss (RW-)
    PT_DYNAMIC  dynamic section
    PT_TLS      thread-local storage (if any)
    PT_NOTE     .vms.ident, .vms.imgid (custom)

Section Headers:
    .text           Code
    .rodata         Read-only data
    .data           Initialized data
    .bss            Uninitialized data
    .dynsym         Dynamic symbol table
    .dynstr         Dynamic string table
    .rela.dyn       RELATIVE + GLOB_DAT relocations
    .rela.plt       JUMP_SLOT relocations
    .got            Global Offset Table
    .got.plt        PLT GOT entries
    .vms.ident      VMS image identity (custom)
    .vms.imgid      VMS image identification (custom)
```

### SONAME Convention

Shareable images use VMS-style names as their ELF SONAME:

```
LIBVMS$SHR.EXE          (libvms)
LIBVMSPROCESS$SHR.EXE   (libvmsprocess)
LIBVMSFS$SHR.EXE        (libvmsfs)
LIBVMSLNM$SHR.EXE       (libvmslnm)
LIBRMS$SHR.EXE          (librms)
LIBVMSDCL$SHR.EXE       (libvmsdcl)
```

Set via linker flag: `-Wl,-soname,LIBVMS$SHR.EXE`

### Custom ELF Sections

#### .vms.ident — Image Identity

Contains GSMATCH version information used by the image activator for
compatibility checking.

```c
/*
 * VMS Image Identity — stored in .vms.ident ELF section.
 *
 * Matches OpenVMS image header EIHD$W_MAJORID / EIHD$W_MINORID
 * and EIHI$T_IMGNAM semantics.
 */

#define VMS_GSMATCH_ALWAYS  0   /* Any version accepted */
#define VMS_GSMATCH_EQUAL   1   /* Exact match required */
#define VMS_GSMATCH_LEQUAL  2   /* Actual >= linked (default) */

struct vms_ident {
    char     imgnam[32];        /* Image name, null-terminated */
    uint8_t  gsmatch_op;        /* GSMATCH operator */
    uint8_t  reserved;
    uint16_t major;             /* Major version number */
    uint16_t minor;             /* Minor version number */
    uint32_t link_time;         /* Link timestamp (Unix epoch) */
    uint32_t flags;             /* Reserved for future use */
};

/* Placed in .vms.ident section by linker script or __attribute__ */
#define VMS_IMAGE_IDENT(name, op, maj, min) \
    __attribute__((section(".vms.ident"), used)) \
    static const struct vms_ident _vms_ident = { \
        .imgnam = name, \
        .gsmatch_op = (op), \
        .major = (maj), \
        .minor = (min), \
        .link_time = __TIME_EPOCH__, \
    }
```

#### .vms.imgid — Image Identification

Optional metadata section for diagnostic purposes. Not used for activation
decisions.

```c
/*
 * VMS Image Identification — stored in .vms.imgid ELF section.
 *
 * Matches OpenVMS EIHI (Image Header Identification) record.
 */
struct vms_imgid {
    char     linker_id[32];     /* Linker identification */
    char     build_host[32];    /* Build host name */
    char     build_ident[16];   /* Build identification string */
};
```

### Executables

Executables are ELF ET_EXEC (or ET_DYN for PIE) with:

- `PT_INTERP` pointing to `/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE`
- `DT_NEEDED` entries for shareable image SONAMEs
- A `.vms.needed_versions` section recording the GSMATCH version of each
  shareable image at link time (for version checking at activation)

```c
/*
 * Recorded in .vms.needed_versions section of executables.
 * One entry per DT_NEEDED shareable image.
 */
struct vms_needed_version {
    char     soname[32];        /* SONAME of required shareable image */
    uint16_t major;             /* Major version at link time */
    uint16_t minor;             /* Minor version at link time */
};
```

---

## 4. Search Path Resolution

When IMGACT.EXE encounters a `DT_NEEDED` entry, it resolves the SONAME
to a file path using this priority order:

```
Priority 1: Known Image Database (VMS$KNOWN_IMAGES.DAT)
    |  O(1) hash lookup by SONAME
    |  If image is /OPEN, use cached fd
    |  See Section 6 for details
    |
Priority 2: Hardcoded SYS$SHARE fallback
    |  /vms/SYS0/SYSCOMMON/SYSLIB/<SONAME>
    |  Always available, even before VMSLNMD starts
    |  This is the chicken-and-egg breaker
    |
Priority 3: SYS$SHARE logical name (after VMSLNMD)
    |  Translate SYS$SHARE via vmslnm API
    |  Search result directory for SONAME
    |  Only available after logical name daemon is running
    |
Priority 4: ELF RPATH/RUNPATH
    |  From DT_RPATH or DT_RUNPATH in the requesting image
    |  Last resort, mainly for third-party software
    |
    v
Not found: %IMGACT-F-IMGNOTFND error
```

### Chicken-and-Egg: VMSLNMD Bootstrap

The logical name daemon (`VMSLNMD.EXE`) is itself a dynamically linked binary
that requires image activation. But IMGACT.EXE normally uses logical names
(SYS$SHARE) to find shareable images. This creates a circular dependency.

Resolution: IMGACT.EXE has a hardcoded fallback path
(`/vms/SYS0/SYSCOMMON/SYSLIB/`) that it uses when:

1. The Known Image Database is not yet loaded (first activations after boot)
2. A SONAME is not found in the database

This means the boot sequence works without any daemon:

```
STARTUP.EXE (static) -> starts VMSLNMD.EXE
    |
    IMGACT.EXE activates VMSLNMD.EXE
        Uses hardcoded /vms/SYS0/SYSCOMMON/SYSLIB/ path
    |
    VMSLNMD starts, defines SYS$SHARE logical
    |
    STARTUP.EXE runs SYSTARTUP_VMS.COM
        INSTALL ADD for shareable images
        Subsequent activations use Known Image DB + SYS$SHARE
```

### Hardcoded Path Constant

Defined in `ovmx_layout.h`:

```c
#define VMS_IMGACT_PATH      "SYS$SYSTEM:IMGACT.EXE"
#define VMS_KNOWN_IMAGES     "SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT"

/* Linux path used as hardcoded fallback before VMSLNMD starts */
#define IMGACT_FALLBACK_SYSLIB  "/vms/SYS0/SYSCOMMON/SYSLIB"
```

---

## 5. GSMATCH Version Checking

GSMATCH (Global Section Match) ensures that an executable is activated only
with compatible versions of its shareable images. This is a core VMS feature
that prevents silent ABI breakage.

### Version Data Flow

```
Link time:
    Linker reads .vms.ident from each shareable image
    Records {SONAME, major, minor} in executable's .vms.needed_versions

Activation time:
    IMGACT.EXE reads .vms.needed_versions from executable
    For each shareable image loaded:
        Read .vms.ident from the actual .EXE file on disk
        Compare linked version vs actual version per operator
```

### Operators

| Operator | Code | Rule | Use Case |
|----------|------|------|----------|
| ALWAYS | 0 | Any version accepted | Development, testing |
| EQUAL | 1 | actual.major == linked.major AND actual.minor == linked.minor | Locked deployments |
| LEQUAL | 2 | actual.major == linked.major AND actual.minor >= linked.minor | Default — allows bugfix updates |

**LEQUAL semantics**: The major version must match exactly (major version
bumps indicate breaking changes). The minor version of the installed image
must be greater than or equal to the minor version recorded at link time
(minor bumps add features but preserve ABI).

### Match Algorithm

```c
/*
 * Returns 1 if version match succeeds, 0 if GSMATCH failure.
 */
static int gsmatch_check(const struct vms_needed_version *needed,
                          const struct vms_ident *actual)
{
    switch (actual->gsmatch_op) {
    case VMS_GSMATCH_ALWAYS:
        return 1;

    case VMS_GSMATCH_EQUAL:
        return (actual->major == needed->major &&
                actual->minor == needed->minor);

    case VMS_GSMATCH_LEQUAL:
        return (actual->major == needed->major &&
                actual->minor >= needed->minor);

    default:
        return 0;  /* Unknown operator = fail safe */
    }
}
```

### Failure Behavior

On GSMATCH mismatch, IMGACT.EXE:

1. Writes diagnostic to stderr:
   ```
   %IMGACT-F-GSMATCH, image LIBVMS$SHR.EXE GSMATCH failure
   -IMGACT-I-IMGVER, image version is 2.3, requires 2.5 (LEQUAL)
   ```
2. Returns status `IMGACT$_GSMATCH` to the process
3. Does NOT activate the image — the process receives SIGABRT

---

## 6. INSTALL Utility and Known Image Database

### Purpose

The INSTALL utility manages a persistent database of "known images" —
shareable images that are pre-registered for fast lookup and optional
performance optimizations. On real VMS, known images can be installed
with attributes like /OPEN, /SHARED, /HEADER_RESIDENT that affect
how the image activator handles them.

### Known Image Database Format

The database file `VMS$KNOWN_IMAGES.DAT` is a binary file stored in
SYS$SYSTEM. It is rebuilt every boot by `SYSTARTUP_VMS.COM`.

```c
/*
 * Known Image Database — on-disk format.
 *
 * File: SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT
 * Rebuilt each boot by SYSTARTUP_VMS.COM via INSTALL ADD commands.
 */

#define KFE_MAX_IMAGES  256
#define KFE_MAGIC       0x4B464521  /* "KFE!" */

/* Flags for kfe_entry.flags */
#define KFE_F_OPEN            0x0001  /* File kept open */
#define KFE_F_SHARED          0x0002  /* MAP_SHARED for page sharing */
#define KFE_F_HEADER_RESIDENT 0x0004  /* ELF headers cached in memory */

struct kfe_header {
    uint32_t magic;             /* KFE_MAGIC */
    uint32_t version;           /* Database format version */
    uint32_t count;             /* Number of entries */
    uint32_t reserved;
};

struct kfe_entry {
    char     soname[64];        /* SONAME (hash key) */
    char     path[256];         /* Full Linux path to .EXE file */
    uint32_t flags;             /* KFE_F_* flags */
    uint16_t major;             /* Cached GSMATCH major version */
    uint16_t minor;             /* Cached GSMATCH minor version */
    uint8_t  gsmatch_op;        /* Cached GSMATCH operator */
    uint8_t  reserved[7];
};
```

### In-Memory Representation

IMGACT.EXE loads the database into a hash table on first activation (lazy
init). The hash table provides O(1) lookup by SONAME.

```c
/*
 * In-memory hash table for known image lookup.
 * Simple open-addressing with linear probing.
 * Table size: 512 slots (power of 2, load factor < 0.5 for 256 max images).
 */

#define KFE_HASH_SIZE  512

struct kfe_runtime {
    struct kfe_entry entries[KFE_HASH_SIZE];
    int              occupied[KFE_HASH_SIZE];  /* 0 = empty, 1 = occupied */
    int              count;
    int              loaded;                    /* 0 until first load */
};
```

### INSTALL Commands

```
$ INSTALL ADD SYS$SHARE:LIBVMS$SHR.EXE /OPEN /SHARED /HEADER_RESIDENT
$ INSTALL ADD SYS$SHARE:LIBRMS$SHR.EXE /OPEN /SHARED
$ INSTALL LIST
$ INSTALL REMOVE SYS$SHARE:LIBVMS$SHR.EXE
```

DCL built-in verb `INSTALL` dispatches to the INSTALL utility
(`SYS$SYSTEM:INSTALL.EXE`), which:

1. Reads the current `VMS$KNOWN_IMAGES.DAT`
2. Adds/removes/lists entries
3. Writes the updated database

### Attribute Semantics

| Attribute | OpenVMS Behavior | OVMX Implementation |
|-----------|-----------------|---------------------|
| /OPEN | File kept open, faster activation | IMGACT.EXE caches fd from `open()` |
| /SHARED | Global sections shared between processes | `mmap(MAP_SHARED)` — Linux kernel handles page sharing automatically |
| /HEADER_RESIDENT | Image header locked in memory | ELF headers + .vms.ident cached in kfe_runtime |
| /PRIVILEGED | Image runs with elevated privileges | Not implemented (future: Linux capabilities) |
| /PROTECTED | Image cannot be replaced | Not implemented (future: inotify watch) |

Note: `/SHARED` on Linux gets automatic physical page sharing through the
kernel's page cache when using `MAP_SHARED` on the same file. This gives us
the same memory savings as VMS global sections without explicit shared memory
management.

---

## 7. Boot Sequence with Image Activation

### Phase Diagram

```
Phase 1: Kernel + Initramfs
==========================
    Linux kernel boots (vmlinuz)
    Unpack initramfs
    /init (init-wrapper.sh):
        Mount: proc, sysfs, devtmpfs, devpts, tmpfs
        Load: vms.ko, vmsfs.ko
        Generate /etc/passwd, /etc/group
        exec STARTUP.EXE

Phase 2: Static Bootstrap (STARTUP.EXE)
=======================================
    STARTUP.EXE is PID 1 (ALWAYS static, never activated by IMGACT.EXE)
    - Mount system disk (/vms)
    - Detect first boot -> run installation (see Section 9)
    - Start VMSLNMD.EXE (first dynamic activation via IMGACT.EXE)
    - Load system logicals from SYLOGICALS.CONF
    - Execute SYSTARTUP_VMS.COM

Phase 3: System Startup (SYSTARTUP_VMS.COM)
==========================================
    $ INSTALL ADD SYS$SHARE:LIBVMS$SHR.EXE /OPEN /SHARED /HEADER_RESIDENT
    $ INSTALL ADD SYS$SHARE:LIBVMSPROCESS$SHR.EXE /OPEN /SHARED
    $ INSTALL ADD SYS$SHARE:LIBVMSFS$SHR.EXE /OPEN /SHARED
    $ INSTALL ADD SYS$SHARE:LIBVMSLNM$SHR.EXE /OPEN /SHARED
    $ INSTALL ADD SYS$SHARE:LIBRMS$SHR.EXE /OPEN /SHARED
    $ INSTALL ADD SYS$SHARE:LIBVMSDCL$SHR.EXE /OPEN /SHARED
    ...
    $ START/NETWORK       (future)
    $ START/QUEUE         (future)

Phase 4: Login
=============
    STARTUP.EXE spawns login process
    LOGINOUT.EXE activated by IMGACT.EXE (dynamic)
    DCL.EXE activated by IMGACT.EXE (dynamic)
    User session begins
```

### Static vs Dynamic Binaries

| Binary | Build Mode | Reason |
|--------|-----------|--------|
| STARTUP.EXE | Static (musl) | PID 1, must work before anything else |
| IMGACT.EXE | Static (musl, exports symbols) | ELF interpreter, cannot depend on itself |
| VMSLNMD.EXE | Dynamic | First dynamically activated binary |
| DCL.EXE | Dynamic | Standard shareable image consumer |
| LOGINOUT.EXE | Dynamic | Standard shareable image consumer |
| INSTALL.EXE | Dynamic | Standard shareable image consumer |
| All tools | Dynamic | Standard shareable image consumers |

---

## 8. Build System Changes

### CMake Option

A new top-level CMake option `OVMX_IMGACT` controls the build mode:

```cmake
option(OVMX_IMGACT "Build with IMGACT.EXE image activation (musl, QEMU only)" OFF)
```

When `OVMX_IMGACT=ON`:

1. **Compiler**: `musl-gcc` for all targets
2. **Default linking**: Shared (dynamic) — all libraries become `.so` files
3. **PT_INTERP**: All executables get `-Wl,--dynamic-linker=/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE`
4. **SONAME**: Each library gets `-Wl,-soname,<VMS_NAME>$SHR.EXE`
5. **VMS sections**: Each library gets `.vms.ident` via compiled-in struct
6. **Exceptions**: STARTUP.EXE and IMGACT.EXE always built static

When `OVMX_IMGACT=OFF` (default):

- Builds exactly as today — static musl for QEMU, glibc shared for Docker
- No IMGACT.EXE built
- No VMS metadata sections

### Library Build Targets

```cmake
# Example: libvms becomes LIBVMS$SHR.EXE
if(OVMX_IMGACT)
    add_library(vms SHARED ${VMS_SOURCES})
    set_target_properties(vms PROPERTIES
        OUTPUT_NAME "LIBVMS\$SHR"
        SUFFIX ".EXE"
        SOVERSION "${VMS_MAJOR}"
        VERSION "${VMS_MAJOR}.${VMS_MINOR}"
    )
    target_link_options(vms PRIVATE
        -Wl,-soname,LIBVMS\$SHR.EXE
    )
else()
    add_library(vms STATIC ${VMS_SOURCES})
endif()
```

### IMGACT.EXE Build

```cmake
# IMGACT.EXE: static musl binary that exports all symbols
add_executable(imgact ${IMGACT_SOURCES})
target_link_options(imgact PRIVATE
    -static
    -Wl,--export-dynamic          # Export all symbols to loaded images
    -Wl,-e,_imgact_start          # Custom entry point
)
set_target_properties(imgact PROPERTIES
    OUTPUT_NAME "IMGACT"
    SUFFIX ".EXE"
)
```

### __getauxval Symbol Fix

GCC's runtime support calls `__getauxval()` (with double underscore), but
musl only exports `getauxval()`. This causes undefined symbol errors in
dynamically linked binaries.

Fix: IMGACT.EXE provides a weak alias:

```c
#include <sys/auxv.h>

/*
 * GCC expects __getauxval (compiler-rt / libgcc internal).
 * musl only provides getauxval. Bridge the gap.
 */
unsigned long __getauxval(unsigned long type)
    __attribute__((weak, alias("getauxval")));
```

This is compiled into IMGACT.EXE and exported to all loaded images via
`--export-dynamic`.

### Executable Build

```cmake
# Example: DCL.EXE (dynamic, uses IMGACT.EXE)
if(OVMX_IMGACT)
    target_link_options(dcl PRIVATE
        -Wl,--dynamic-linker=/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE
    )
endif()
```

### Output Layout

With `OVMX_IMGACT=ON`, the build produces:

```
build/
  SYS$SYSTEM/
    IMGACT.EXE              (static, ELF interpreter)
    STARTUP.EXE             (static, PID 1)
    DCL.EXE                 (dynamic)
    LOGINOUT.EXE            (dynamic)
    VMSLNMD.EXE             (dynamic)
    INSTALL.EXE             (dynamic)
    ...
  SYS$SHARE/
    LIBVMS$SHR.EXE          (shared object)
    LIBVMSPROCESS$SHR.EXE   (shared object)
    LIBVMSFS$SHR.EXE        (shared object)
    LIBVMSLNM$SHR.EXE       (shared object)
    LIBRMS$SHR.EXE          (shared object)
    LIBVMSDCL$SHR.EXE       (shared object)
```

---

## 9. System Disk Installation

### First-Boot Detection

STARTUP.EXE detects a blank system disk by checking for the existence of
`/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE`. If absent, it enters installation mode.

### Fat Initramfs

The bootable image (`Dockerfile.bootable`) produces a "fat" initramfs that
contains ALL system binaries and shareable images. This is the installation
source.

```
initramfs-ovmx.cpio.gz
    /init                       (init-wrapper.sh)
    /sbin/init                  (STARTUP.EXE, static)
    /vms_install/               (installation payload)
        SYS$SYSTEM/
            IMGACT.EXE
            DCL.EXE
            LOGINOUT.EXE
            VMSLNMD.EXE
            INSTALL.EXE
            AUTHORIZE.EXE
            VMS$KNOWN_IMAGES.DAT  (empty template)
            SYSUAF.DAT
            RIGHTSLIST.DAT
            ...
        SYS$SHARE/
            LIBVMS$SHR.EXE
            LIBVMSPROCESS$SHR.EXE
            ...
        SYS$MANAGER/
            SYSTARTUP_VMS.COM
            SYLOGIN.COM
            SYLOGICALS.CONF
            ...
        SYS$HELP/
            HELPLIB.HLP
```

### Installation Sequence

```
STARTUP.EXE detects blank disk
    |
    v
Create VMS directory hierarchy:
    /vms/SYS0/SYSCOMMON/SYSEXE/      (SYS$SYSTEM)
    /vms/SYS0/SYSCOMMON/SYSLIB/      (SYS$SHARE / SYS$LIBRARY)
    /vms/SYS0/SYSCOMMON/SYSMGR/      (SYS$MANAGER)
    /vms/SYS0/SYSCOMMON/SYSHLP/      (SYS$HELP)
    /vms/USERS/                       (SYS$USERS)
    /vms/SYSTMP/                      (SYS$SCRATCH)
    |
    v
Copy files from /vms_install/ to /vms/:
    /vms_install/SYS$SYSTEM/* -> /vms/SYS0/SYSCOMMON/SYSEXE/
    /vms_install/SYS$SHARE/*  -> /vms/SYS0/SYSCOMMON/SYSLIB/
    /vms_install/SYS$MANAGER/* -> /vms/SYS0/SYSCOMMON/SYSMGR/
    /vms_install/SYS$HELP/*   -> /vms/SYS0/SYSCOMMON/SYSHLP/
    |
    v
Generate SYSTARTUP_VMS.COM (if not already present):
    INSTALL ADD for each shareable image
    Start VMSLNMD
    |
    v
Write installation marker:
    /vms/SYS0/SYSCOMMON/SYSEXE/VMS$INSTALL.DAT
    |
    v
Continue normal boot (or reboot for clean state)
```

### Slim Boot

After installation, subsequent boots use the system disk directly. The
initramfs only needs:

```
initramfs-slim.cpio.gz
    /init                       (init-wrapper.sh)
    /sbin/init                  (STARTUP.EXE, static)
    kernel modules: vms.ko, vmsfs.ko
```

STARTUP.EXE mounts the system disk, finds `DCL.EXE` present, skips
installation, and proceeds to normal boot. IMGACT.EXE is loaded from the
system disk by the kernel on the first `exec()` of a dynamic binary.

---

## 10. TLS Implementation Details

### Current TLS Usage

Two source files currently use `__thread` storage:

| File | Variable | Purpose |
|------|----------|---------|
| `src/libvmssys/vms_kif.c` | `static __thread int vms_dev_fd` | Per-thread kernel interface fd |
| `src/vmsprocess/vms_pcb.c` | `static __thread struct vms_pcb *current_pcb` | Per-thread process control block |

Note: The bead plan originally listed 4 files (`dcl_messages.c`,
`lib_signal.c`). Those files do not currently use `__thread`. If TLS usage
expands during implementation, the same mechanism handles it.

### ELF TLS Model

All OVMX code uses the **Global Dynamic** TLS model (the default for shared
libraries). This is the most general model and works correctly when the TLS
variable's defining library may be loaded at any time.

```
__thread int x;
// Compiler generates:
//   aarch64: adrp + add to get TLSDESC GOT entry, blr to call descriptor
//   x86_64:  lea + call to __tls_get_addr or TLSDESC
```

### IMGACT.EXE TLS Initialization

IMGACT.EXE must set up the TLS data structures before transferring control
to the executable. The process:

```
1. Scan all loaded images for PT_TLS program headers
   For each PT_TLS:
       Record: alignment, tdata offset, tdata size, tbss size, module ID

2. Calculate total TLS block size:
   total = sum(align_up(tdata_size + tbss_size, alignment)) for all images

3. Allocate TCB + TLS block:
   +------------------+
   | TLS block img N  |  <-- highest address
   | TLS block img ...|
   | TLS block img 1  |
   | TLS block img 0  |
   +------------------+
   | Thread pointer   |  <-- TP register points here (variant I)
   | (TCB)            |
   +------------------+

   aarch64 uses TLS variant I: TP points to TCB, TLS blocks at positive
   offsets from TP.

   x86_64 uses TLS variant II: TP points to TCB, TLS blocks at negative
   offsets from TP.

4. Initialize TLS data:
   For each image with PT_TLS:
       memcpy(tls_block, tdata_source, tdata_size)
       memset(tls_block + tdata_size, 0, tbss_size)

5. Set thread pointer:
   aarch64: msr tpidr_el0, <tp>  (via arch_prctl equivalent)
   x86_64:  arch_prctl(ARCH_SET_FS, <tp>)
```

### TLSDESC Relocations

TLSDESC (TLS Descriptor) is the modern, efficient TLS access method. Each
TLS variable access goes through a descriptor in the GOT:

```c
struct tlsdesc {
    ptrdiff_t (*resolver)(struct tlsdesc *);
    ptrdiff_t arg;  /* Module ID + offset, or direct offset */
};
```

IMGACT.EXE processes `R_AARCH64_TLSDESC` / `R_X86_64_TLSDESC` relocations by
filling in the descriptor with a resolver function and the appropriate
module/offset argument. For statically-known TLS (all images loaded at
startup), the resolver simply returns a pre-computed offset from TP.

### Thread Creation

When a new thread is created (via `pthread_create` in musl), musl's thread
creation code allocates a new TLS block and copies the initialization images.
Since musl is statically linked into IMGACT.EXE, this works automatically —
musl's internal TLS bookkeeping is maintained by IMGACT.EXE's own code.

---

## 11. Error Handling

### Error Code Structure

IMGACT error codes follow the VMS condition value format with facility
code for IMGACT (to be assigned, using 0x200 provisionally):

```c
/*
 * IMGACT facility condition codes.
 *
 * Facility: IMGACT (0x200)
 * Follows VMS STSDEF.H bit layout.
 */

#define IMGACT_FACILITY  0x200

/* Severity encoding */
#define _IMGACT_SEV_WARNING  0
#define _IMGACT_SEV_SUCCESS  1
#define _IMGACT_SEV_ERROR    2
#define _IMGACT_SEV_INFO     3
#define _IMGACT_SEV_FATAL    4

/* Macro to build condition value */
#define _IMGACT_COND(msg, sev) \
    (((IMGACT_FACILITY) << 16) | ((msg) << 3) | (sev))

/* Fatal errors — process cannot be activated */
#define IMGACT$_IMGNOTFND    _IMGACT_COND(1, 4)   /* Image file not found */
#define IMGACT$_IMGFMTERR    _IMGACT_COND(2, 4)   /* Image format error (bad ELF) */
#define IMGACT$_GSMATCH      _IMGACT_COND(3, 4)   /* GSMATCH version failure */
#define IMGACT$_UNDSYM       _IMGACT_COND(4, 4)   /* Undefined symbol */
#define IMGACT$_FILACCERR    _IMGACT_COND(5, 4)   /* File access error */
#define IMGACT$_MAPFAIL      _IMGACT_COND(6, 4)   /* mmap failure */
#define IMGACT$_CIRCDEP      _IMGACT_COND(7, 4)   /* Circular dependency */
#define IMGACT$_PRIV         _IMGACT_COND(8, 4)   /* Privilege violation */
#define IMGACT$_TLSERR       _IMGACT_COND(9, 4)   /* TLS initialization error */

/* Warnings — activation continues */
#define IMGACT$_NOWRTACC     _IMGACT_COND(20, 0)  /* No write access to image */
#define IMGACT$_NOIDENT      _IMGACT_COND(21, 0)  /* No .vms.ident section */

/* Informational */
#define IMGACT$_IMGVER       _IMGACT_COND(30, 3)  /* Image version info */
#define IMGACT$_KFELOAD      _IMGACT_COND(31, 3)  /* Known image DB loaded */
#define IMGACT$_FALLBACK     _IMGACT_COND(32, 3)  /* Using hardcoded fallback path */
```

### Error Messages

All error messages follow VMS message format:

```
%IMGACT-F-IMGNOTFND, image file not found
  -IMGACT-I-FILENAME, file: LIBXYZ$SHR.EXE

%IMGACT-F-IMGFMTERR, image format error
  -IMGACT-I-FILENAME, file: BROKEN.EXE
  -IMGACT-I-DETAIL, not a valid ELF file

%IMGACT-F-GSMATCH, image LIBVMS$SHR.EXE GSMATCH failure
  -IMGACT-I-IMGVER, image version is 1.3, requires 2.0 (LEQUAL)

%IMGACT-F-UNDSYM, undefined symbol in image DCL.EXE
  -IMGACT-I-SYMNAM, symbol: sys$qio

%IMGACT-F-FILACCERR, error accessing image file
  -IMGACT-I-FILENAME, file: /vms/SYS0/SYSCOMMON/SYSLIB/LIBVMS$SHR.EXE
  -IMGACT-I-ERRNO, permission denied

%IMGACT-F-MAPFAIL, failed to map image into memory
  -IMGACT-I-FILENAME, file: LIBVMS$SHR.EXE
  -IMGACT-I-DETAIL, mmap returned ENOMEM

%IMGACT-F-CIRCDEP, circular dependency detected
  -IMGACT-I-FILENAME, file: LIBA$SHR.EXE
  -IMGACT-I-DETAIL, LIBA$SHR.EXE -> LIBB$SHR.EXE -> LIBA$SHR.EXE

%IMGACT-F-PRIV, insufficient privilege to activate image
  -IMGACT-I-FILENAME, file: PRIVILEGED$SHR.EXE

%IMGACT-F-TLSERR, TLS initialization failed
  -IMGACT-I-DETAIL, unable to allocate TLS block (out of memory)

%IMGACT-W-NOIDENT, image has no .vms.ident section (GSMATCH skipped)
  -IMGACT-I-FILENAME, file: THIRDPARTY.SO

%IMGACT-I-FALLBACK, using hardcoded fallback path for LIBVMS$SHR.EXE
```

### Error Output

Errors are written to file descriptor 2 (stderr). In VMS terms, this
corresponds to SYS$ERROR. Fatal errors cause the process to receive SIGABRT
after the diagnostic message is written.

---

## 12. Non-Goals and Explicit Exclusions

### Symbol Vectors

OpenVMS shareable images use symbol vectors (transfer vectors) — a fixed table
of JMP instructions at known offsets that provides ABI stability across image
versions. Callers jump to a vector slot, and the vector jumps to the actual
function, allowing the function to move without changing the caller.

**Not implemented.** ELF PLT/GOT provides equivalent indirect-call semantics.
True symbol vectors would require a custom compiler backend or linker plugin to
generate vector tables and redirect all external calls through them. The
engineering cost is not justified when PLT/GOT achieves the same practical
result (stable ABI for inter-image calls).

### Docker Mode

Docker mode continues using glibc's standard dynamic linker (`ld-linux-*.so`).
IMGACT.EXE is not built, installed, or referenced in Docker builds. The Docker
build uses `gcc` (not `musl-gcc`) and produces standard Ubuntu-compatible
binaries.

Rationale: Docker mode targets development and CI convenience. VMS-authentic
image activation only matters for the bootable QEMU distribution.

### Replacing STARTUP.EXE

STARTUP.EXE remains a fully static binary. It is PID 1 and must function
before any dynamic linking infrastructure exists. It is the only executable
(besides IMGACT.EXE itself) that does not go through image activation.

### Runtime Image Loading

`dlopen()`/`dlsym()` style runtime image loading (equivalent to
`LIB$FIND_IMAGE_SYMBOL` on VMS) is deferred to a future phase. All image
dependencies must be declared at link time via `DT_NEEDED`.

### Privileged Images

VMS supports installed images with privileges (`INSTALL/PRIVILEGED`). This
requires kernel support (setuid-like semantics with fine-grained privilege
masks). Deferred to a future phase — would require `vms.ko` kernel module
extensions for privilege checking.

### Cluster-Wide Known Image Databases

VMS clusters share a common known image database. OVMX does not support
clustering. The known image database is local to each system.

---

## Appendix A: Reference to OpenVMS Documentation

| Topic | OpenVMS Reference |
|-------|------------------|
| Image activator | *OpenVMS Internals and Data Structures*, Chapter 26 |
| GSMATCH | *OpenVMS Linker Utility Manual*, Chapter 3 (Shareable Images) |
| INSTALL utility | *OpenVMS System Management Utilities Reference*, INSTALL |
| Image header | *OpenVMS Internals*, EIHD (Executive Image Header Descriptor) |
| Global sections | *OpenVMS Programming Concepts Manual*, Chapter 12 |
| Symbol vectors | *OpenVMS Linker Utility Manual*, Chapter 3.3 |
| Shareable images | *OpenVMS Programming Concepts Manual*, Chapter 11 |

## Appendix B: Implementation Bead Map

Each section of this spec maps to implementation beads:

| Section | Bead | Title |
|---------|------|-------|
| 1-2, 6-7, 10 | vms-913.2 | IMGACT.EXE core image activator (aarch64) |
| 8 | vms-913.3 | Build system: OVMX_IMGACT cmake mode |
| 5 | vms-913.4 | GSMATCH version checking |
| 6 | vms-913.5 | INSTALL utility + known image DB |
| 9 (fat) | vms-913.6 | Fat initramfs with dynamic binaries |
| 9 (install) | vms-913.7 | System disk installation |
| 9 (slim) | vms-913.10 | Slim boot from installed disk |
| 2, 6-7 (x86) | vms-913.11 | IMGACT.EXE x86_64 support |
