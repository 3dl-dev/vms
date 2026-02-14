# OpenVMS C Source Code Corpus for OVMX Testing

Research findings on open-source C programs written for or ported to OpenVMS, suitable as compatibility test targets for OVMX.

**Research Date:** 2026-02-13

---

## Executive Summary

Found **100+ open-source C programs and libraries** with source code available for OpenVMS. Organized by difficulty/value for OVMX testing, from "easiest to compile" to "most valuable for validation."

**Best Starting Points:**
1. Eight-Cubed.com examples (200+ simple programs, each testing 1-2 VMS APIs)
2. MMK make utility (moderate complexity, BSD license, self-contained)
3. vmsbackup (reads VMS backup format, moderate complexity)
4. GNV utilities (bash, coreutils, grep, sed - extensive VMS API coverage)

---

## Tier 1: Simple Test Programs (Start Here)

### 1. Eight-Cubed.com VMS Example Programs
- **URL:** https://www.eight-cubed.com/examples.shtml
- **What:** 200+ small C programs demonstrating individual VMS system services and RTL routines
- **APIs Exercised:**
  - Time/Date: `sys$bintim`, `lib$add_times`, `lib$convert_date_string`, `sys$gettim`, `sys$numtim`
  - File Ops: `lib$delete_file`, `lib$rename_file`, `lib$find_file`, `sys$create`, `sys$mount`
  - Process: `sys$creprc`, `sys$delprc`, `sys$setpri`, `sys$getjpi`
  - Memory: `lib$create_vm_zone`, `lib$get_vm`, `sys$crmpsc`, `sys$expreg_64`
  - String: `lib$movc3`, `lib$movc5`, `lib$locc`, `lib$matchc`
  - Device/System: `lib$getdvi`, `lib$getsyi`, `sys$assign`, `sys$getdvi`
- **Complexity:** ~50-100 LOC per example, single-file programs
- **License:** Not specified (educational examples)
- **Dependencies:** None (pure VMS C)
- **Build:** `$ cc filename && link filename`
- **Value:** **HIGHEST** - Each program is a focused unit test for specific VMS APIs

### 2. HP OpenVMS Example Programs
- **URL:** https://h41379.www4.hpe.com/opensource/cdsa_source.html
- **What:** Official HP example code (SSL, CDSA, Kerberos)
- **Location:** `SYS$EXAMPLES:`, `DECW$EXAMPLES:`, `TCPIP$EXAMPLES:`, `SSL$EXAMPLES:`
- **APIs Exercised:** SSL/TLS, security APIs, TCP/IP stack
- **Complexity:** 100-500 LOC per example
- **License:** Source code "as-is" from HP (unsupported)
- **Dependencies:** SSL, TCP/IP libraries
- **Value:** **HIGH** - Official HP code demonstrates correct VMS API usage

---

## Tier 2: Moderate Complexity (Good Validation Targets)

### 3. MMK - MadGoat Make Utility
- **URL:** https://github.com/endlesssoftware/mmk
- **What:** VMS 'make' utility compatible with DEC MMS syntax
- **APIs Exercised:**
  - CLI$ command-line processing (`clidefs.h`)
  - DEC/CMS code management integration
  - File I/O and dependency tracking
- **Complexity:** 57 files, 79.3% C, 326 commits
- **License:** BSD-3-Clause
- **Dependencies:** None (self-contained)
- **Platforms:** VAX/VMS, OpenVMS VAX, OpenVMS AXP, OpenVMS IA64
- **Value:** **HIGH** - Real-world build tool with no external dependencies

### 4. vmsbackup - VMS Backup Save Set Reader
- **URL:** https://github.com/FreddieAkeroyd/vmsbackup
- **What:** Reads OpenVMS backup save sets on non-VMS machines
- **APIs Exercised:**
  - VMS backup format parsing
  - File attribute handling
  - VMS constants (`vmsconstants.h`)
  - Command Language Definition (`vmsbackup.cld`)
- **Complexity:** 17 files, 92.8% C
- **License:** Not specified in search results
- **Dependencies:** Minimal
- **Value:** **MEDIUM-HIGH** - Tests VMS file format compatibility

### 5. VMSBackup (Alternative Implementation)
- **URL:** https://github.com/TonyBUK/VMSBackup
- **What:** OpenVMS Save Set Reader with C and Python implementations
- **APIs Exercised:** VMS tape/backup format (.BCK files)
- **Complexity:** Moderate
- **License:** Check repository
- **Value:** **MEDIUM** - Another validation point for backup format

---

## Tier 3: Complex Utilities (High Value, More Work)

### 6. GNV - GNU Not VMS
- **URL:** https://sourceforge.net/projects/gnv/
- **What:** Framework for porting GNU utilities to OpenVMS
- **Components Available:**
  - **bash** - Unix shell
  - **coreutils** - ls, cp, mv, rm, cat, etc.
  - **gawk** - Text processing
  - **grep** - Pattern matching
  - **sed** - Stream editor
  - **make** - Build automation
  - **diffutils, bzip2, file, gzip, which**
- **APIs Exercised:** Comprehensive VMS RTL and system services
- **Complexity:** Large (full GNU utilities)
- **License:** GPL (varies by component)
- **Dependencies:** GNV base kit v3.0.1+
- **Source:** Mercurial repositories on SourceForge
- **Platforms:** I64, AXP architectures
- **Value:** **VERY HIGH** - Comprehensive API coverage, real-world usage patterns

### 7. WASD HTTP Server
- **URL:** https://wasd.vsm.com.au/
- **What:** High-performance web server written specifically for VMS
- **Version:** 12.4.0
- **APIs Exercised:**
  - VMS process management
  - Network I/O (TCP/IP)
  - File system operations
  - Security/authentication
  - AST-based async I/O
- **Complexity:** Large (30+ years of development)
- **License:** Apache License 2.0
- **Dependencies:** VMS C compiler, optional SSL
- **Source:** Available with all source code included
- **Download:** https://wasd.vsm.com.au/wasd/
- **Value:** **VERY HIGH** - Demonstrates advanced VMS features (ASTs, async I/O, multiprocessing)

### 8. OpenSSL for OpenVMS
- **URL:** https://github.com/openssl/openssl
- **VMS Notes:** https://github.com/openssl/openssl/blob/master/NOTES-VMS.md
- **What:** Official OpenSSL with VMS support
- **APIs Exercised:**
  - VMS file I/O
  - Random number generation
  - Process/thread management
  - DEC C specific features
- **Requirements:** ODS-5 disk, DEC C 7.1+
- **Complexity:** Very large (full SSL/TLS stack)
- **License:** Apache License 2.0
- **Dependencies:** Minimal on VMS
- **Value:** **HIGH** - Major open-source project, extensive VMS integration
- **Note:** VSI also maintains SSL3 based on OpenSSL 3.0.X

### 9. libcurl / cURL for OpenVMS
- **URL:** https://github.com/curl/curl
- **VMS-Ports:** https://sourceforge.net/p/vms-ports/wiki/HaxxCurl/
- **What:** HTTP/FTP/etc. client library and command-line tool
- **APIs Exercised:**
  - Network I/O
  - SSL/TLS integration
  - File I/O
  - VMS-specific socket handling
- **Complexity:** Large (full networking stack)
- **License:** MIT-style
- **Dependencies:** ZLIB, OpenSSL
- **Platforms:** I64, Alpha, x86-64
- **Source:** Full source in kits at https://sourceforge.net/projects/vms-ports/files/
- **Value:** **HIGH** - Real-world networking code

---

## Tier 4: Libraries (Good for RTL Testing)

### 10. plibsys - Portable System Library
- **URL:** https://github.com/saprykin/plibsys
- **What:** Cross-platform C system library
- **Features:**
  - Threads and synchronization primitives
  - Sockets (TCP, UDP, SCTP), IPv4/IPv6
  - IPC mechanisms
  - Hash functions (MD5, SHA-1, SHA-2, SHA-3, GOST)
  - Binary trees (RB, AVL)
- **APIs Exercised:**
  - POSIX threads on VMS
  - System V semaphores (limited - broken on VMS)
  - Shared memory
  - Atomic operations
- **Complexity:** Moderate to large
- **License:** MIT
- **Platforms:** OpenVMS Alpha and IA64 (DEC C)
- **Build Info:** See `platforms/vms-general` directory
- **Value:** **MEDIUM-HIGH** - Tests cross-platform abstractions over VMS APIs
- **Known Issues:** Named semaphores and shared memory broken on VMS 8.4+

### 11. PCSI Kits - John Francis Collection
- **URL:** http://www.vsm.com.au/ftp/jfp/kits/
- **What:** Pre-built PCSI packages with full source code
- **Packages Available:**
  - **Zlib** V1.2.3 - compression library
  - **LibBZ2** V1.0.4 - bzip2 compression
  - **LibJPEG** V6.2b - JPEG image codec
  - **LibPNG** V1.2.22 - PNG image handling
  - **FreeType** V2.1.4 - TrueType fonts
  - **LibImaging** V1.1.6 - multi-format image processing
  - **LibGD** V2.0.35 - dynamic image creation
  - **GDChart** V1.1.4 - chart/graph generation
  - **Libxml2** V2.6.29 - XML parser
  - **Libxslt/Libexslt** V1.1.12 - XSLT transformations
  - **OpenSSL** V0.9.7I - SSL/TLS
  - **MySQL** V4.1.14-log - database server
  - **Python** - programming language
  - **SWISH-E** V2.4.3 - web indexing
- **APIs Exercised:** File I/O, memory management, VMS RTL
- **Complexity:** Varies (small to very large)
- **License:** Varies by package
- **Value:** **MEDIUM-HIGH** - Broad coverage of standard libraries ported to VMS

---

## Tier 5: Freeware Collections (Archive Sources)

### 12. VMS Freeware CD Collection
- **URLs:**
  - https://wiki.vmssoftware.com/Freeware_CD
  - https://www.digiater.nl/openvms/freeware/
  - https://vmssoftware.com/community/freeware/
- **What:** Historic collection of VMS freeware (8 versions: FREEWARE10-80)
- **Contents (v7.0 example - 100+ packages):**
  - **Languages:** Perl, Python, Ruby
  - **Editors:** Emacs, Jed, Vile, NEdit
  - **Build Tools:** MMK
  - **Compression:** GNU-zip, Info-zip, Unrar, Unzip
  - **Graphics:** ImageMagick, GD, Freetype, Libpng, Libtiff
  - **Web:** Samba, Lynx, WASD, OSCommerce
  - **Development:** DCL utilities, Find, Flist
- **APIs Exercised:** Comprehensive (everything)
- **License:** Mostly GNU GPL
- **Access:** Digiater provides menu-based browsing of binaries and source
- **Value:** **MEDIUM** - Historical reference, many packages superseded

### 13. vms-ports SourceForge Project
- **URL:** https://sourceforge.net/projects/vms-ports/
- **Wiki:** https://sourceforge.net/p/vms-ports/wiki/Home/
- **What:** Centralized repository for OpenVMS open-source ports and freeware
- **Purpose:** Preserve existing code and assist new ports
- **Available Projects (Mercurial repos):**
  - **ncurses** - terminal handling
  - **s3270** - terminal emulation
  - **OSU** - networking utilities
  - **dmpipe** - data management pipes
  - **comm_rtl** - community runtime library
  - **cpython** - Python for VMS
  - **hg** - Mercurial version control
  - **libffi** - foreign function interface
  - **SQLite** - database
- **APIs Exercised:** Varies by project
- **License:** Varies (open source)
- **Value:** **MEDIUM-HIGH** - Active preservation effort, modern ports

### 14. OpenVMS Hobbyist Archive (archive.org)
- **URL:** https://archive.org/details/compaq-hp-openvms-vax-7.2-hobbyist-edition-version-2.0
- **What:** Historical OpenVMS hobbyist distributions
- **Contents:** OS distribution, layered products, some freeware
- **Value:** **LOW-MEDIUM** - Historical reference, limited source availability

---

## Tier 6: GitHub OpenVMS Projects

### 15. GitHub OpenVMS Topic
- **URL:** https://github.com/topics/openvms
- **What:** Collection of 30+ OpenVMS-related projects
- **Notable Repositories:**
  - **plibsys** (753 stars) - covered above
  - **netelf** (284 stars) - run executables from memory over network
  - **vmsbackup** (15 stars) - covered above
  - **cmatrix** (9 stars) - Matrix terminal effect with VT320 support
  - **PC-DCL** (8 stars) - DCL emulator for PC
  - **fun-with-c** (5 stars) - C routines for systems work
  - **OpenVMS-IRC** (4 stars) - IRC client
  - **vms-laxdriver** (4 stars) - load average driver for 64-bit VMS
  - **gnv-bash** (4 stars) - GNU bash port
  - **VAXMODEM** (3 stars) - XMODEM/YMODEM file transfer
  - **GNU ports by Jake Hamby** - coreutils, diffutils, sed, make
- **Value:** **MEDIUM** - Varying quality, some abandoned, some active

---

## Tier 7: Commercial/Limited Source

### 16. Process Software Products
- **URL:** https://www.process.com/resources/openvms/
- **Products:** MultiNet, TCPware, PMDF, PreciseMail
- **Availability:** Free for hobbyist use (binaries)
- **Source:** NOT publicly available
- **Value:** **LOW** - No source access

### 17. VSI Official Products
- **URL:** https://vmssoftware.com/
- **Products:** OpenVMS OS, layered products, Git for VMS
- **Source:** Limited/proprietary (OS itself not open source)
- **Open Source Tools:** VSI ports Git, Python, and other tools
- **Value:** **LOW** - Mostly proprietary, some open-source wrappers

---

## APIs NOT Covered Well in Corpus

After reviewing all sources, these VMS API areas have limited open-source coverage:

1. **DECnet** - No modern open-source DECnet implementations found
2. **Cluster Services** - Limited cluster-aware applications
3. **DECthreads** - Documentation available but **source NOT publicly available**
4. **RMS Advanced Features** - Most code uses basic sequential/indexed files
5. **System Generation (SYSGEN)** - No open-source equivalents
6. **Volume Shadowing** - Proprietary feature
7. **Rights Database (ACLs)** - Limited examples in corpus
8. **Mailbox QIOs** - Some usage but not extensively tested
9. **Asynchronous System Traps (ASTs)** - WASD uses them, but few other examples

---

## Recommended Testing Strategy

### Phase 1: Smoke Tests (Week 1)
1. **Eight-Cubed examples** - Pick 20-30 covering core APIs you've implemented
2. Compile and run each against OVMX
3. Expected: ~50% success rate initially

### Phase 2: Real-World Utilities (Weeks 2-4)
1. **MMK** - Build system is critical infrastructure
2. **vmsbackup** - Tests file format compatibility
3. **Simple GNV tools** - grep, sed (smaller than bash/coreutils)

### Phase 3: Complex Applications (Months 2-3)
1. **WASD** - Full server application (stretch goal)
2. **curl** - Networking stack validation
3. **OpenSSL** - Crypto and advanced I/O

### Phase 4: Library Ecosystem (Month 4+)
1. **PCSI kits** - zlib, libpng, libxml2
2. **GNV full suite** - bash, coreutils, all utilities

---

## Prioritized Compilation Targets

Ranked by "easiest to hardest" with value weighting:

| Rank | Program | Difficulty | Value | Reason |
|------|---------|------------|-------|--------|
| 1 | Eight-Cubed examples | Very Easy | Very High | Focused API tests, no dependencies |
| 2 | HP Examples (SSL, etc.) | Easy | High | Official examples, well-structured |
| 3 | vmsbackup | Easy-Medium | Medium-High | Small codebase, clear VMS formats |
| 4 | MMK | Medium | High | Self-contained, real-world tool |
| 5 | Simple PCSI libs (zlib) | Medium | Medium | Standard library, limited VMS APIs |
| 6 | plibsys | Medium | Medium-High | Cross-platform tests, documented VMS quirks |
| 7 | GNV grep/sed | Medium-Hard | Very High | Real GNU tools, extensive API usage |
| 8 | libcurl | Hard | High | Networking validation, SSL integration |
| 9 | GNV bash | Hard | Very High | Full shell, process control, comprehensive |
| 10 | OpenSSL | Hard | High | Crypto validation, advanced I/O |
| 11 | WASD | Very Hard | Very High | ASTs, async I/O, multiprocess architecture |
| 12 | MySQL | Very Hard | Medium | Database, complex dependencies |

---

## Download Instructions

### Immediate Actions:
```bash
# Clone key repositories
git clone https://github.com/endlesssoftware/mmk.git
git clone https://github.com/FreddieAkeroyd/vmsbackup.git
git clone https://github.com/saprykin/plibsys.git
git clone https://github.com/openssl/openssl.git
git clone https://github.com/curl/curl.git

# Download Eight-Cubed examples (manual - scrape site or contact author)
# Site: https://www.eight-cubed.com/examples.shtml

# Download WASD
wget https://wasd.vsm.com.au/wasd/ -r -np -nH --cut-dirs=1

# Access GNV via SourceForge
# Main: https://sourceforge.net/projects/gnv/files/
# Use Mercurial to clone specific utilities

# Access PCSI kits
wget -r -np -nH --cut-dirs=2 http://www.vsm.com.au/ftp/jfp/kits/

# Browse Freeware CD v7.0
wget -r -np -nH --cut-dirs=3 https://www.digiater.nl/openvms/freeware/v70/
```

---

## License Summary

| License | Projects |
|---------|----------|
| BSD-3-Clause | MMK |
| Apache 2.0 | WASD, OpenSSL |
| MIT | plibsys, curl |
| GPL | GNV utilities, most Freeware CD |
| Educational/Unspecified | Eight-Cubed examples, HP examples |
| Mixed | PCSI kits (varies by package) |

---

## Next Steps for OVMX Project

1. **Create test corpus directory** - `/home/baron/projects/vms/test-corpus/`
2. **Download Tier 1-2 sources** - Eight-Cubed, MMK, vmsbackup, HP examples
3. **Create compatibility tracking** - Bead for each major program
4. **Build first test** - Start with simplest Eight-Cubed example
5. **Document API gaps** - Track which VMS APIs are exercised vs. implemented
6. **Iterate** - Fix bugs, add missing APIs, re-test

---

## Sources

- [VSI Freeware CD Wiki](https://wiki.vmssoftware.com/Freeware_CD)
- [VSI Freeware Download](https://vmssoftware.com/community/freeware/)
- [Digiater OpenVMS Freeware](https://www.digiater.nl/openvms/freeware/)
- [WASD HTTP Server](https://wasd.vsm.com.au/)
- [GNV GNU Not VMS](https://sourceforge.net/projects/gnv/)
- [GNV Wiki](https://sourceforge.net/p/gnv/wiki/Home/)
- [VMS-Ports SourceForge](https://sourceforge.net/projects/vms-ports/)
- [VMS-Ports Wiki](https://sourceforge.net/p/vms-ports/wiki/Home/)
- [GitHub OpenVMS Topic](https://github.com/topics/openvms)
- [MMK GitHub](https://github.com/endlesssoftware/mmk)
- [vmsbackup GitHub](https://github.com/FreddieAkeroyd/vmsbackup)
- [plibsys GitHub](https://github.com/saprykin/plibsys)
- [plibsys OpenVMS Wiki](https://github.com/saprykin/plibsys/wiki/OpenVMS)
- [OpenSSL VMS Notes](https://github.com/openssl/openssl/blob/master/NOTES-VMS.md)
- [cURL for OpenVMS](https://vmssoftware.com/docs/curl-release-notes.pdf)
- [VMS-Ports cURL Wiki](https://sourceforge.net/p/vms-ports/wiki/HaxxCurl/)
- [HP OpenVMS Open Source](https://h41379.www4.hpe.com/opensource/opensource.html)
- [Eight-Cubed C Examples](https://www.eight-cubed.com/examples.shtml)
- [John Francis PCSI Kits](http://www.vsm.com.au/ftp/jfp/kits/)
- [VSI Open Source Software Wiki](https://wiki.vmssoftware.com/Open_Source_Software_for_OpenVMS)
- [Process Software Resources](https://www.process.com/resources/openvms/)
- [OpenVMS Archive.org](https://archive.org/details/compaq-hp-openvms-vax-7.2-hobbyist-edition-version-2.0)
