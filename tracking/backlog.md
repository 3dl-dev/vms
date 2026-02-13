# OVMX Backlog

## Enhancement Ideas

- FUSE ODS-2 driver — BUILD_FUSE option exists in CMake, not yet implemented
- Buildroot integration — config and package stubs exist in `distro/buildroot/`
- VMS HELP content — populate help database for vms_help tool
- Rightslist integration — `rightslist.dat` exists but not fully wired into access checks
- DECnet-style logical name networking
- Cluster emulation concepts
- Job controller and process quota enforcement
- Multi-key indexed files and RMS journaling
- Batch/queue system for DCL

## Technical Debt

- `src/vmsdcl/dcl_builtin.c` is 1856 lines — candidate for splitting by command group
- Kernel module build is separate from CMake (standalone Makefiles)
- No CI/CD pipeline
- No static analysis or linting configured
- Test coverage is uneven — kernel modules well-tested, userspace less so
- No API documentation generation (doxygen or similar)

## Known Bugs

(none reported)

## Documentation Needs

- DCL command reference (all 24 built-in commands)
- System services API reference (sys$ functions)
- RTL API reference (lib$, str$, mth$, ots$ functions)
- Configuration file format docs (sysuaf.dat, ovmx.conf, sylogicals.conf)
- SYSUAF administration guide
