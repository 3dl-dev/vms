# GCC / binutils alpha-dec-vms patch audit (vms-839)

`tools/cross-alpha-vms/patches/` carries 13 patches that `build-toolchain.sh`
applies to the pinned upstream sources (GCC 14.2.0, binutils 2.43; a patch is
routed to binutils when its diff touches `bfd/`, `gas/`, ...). The self-host
rule is that OVMX builds the real GCC sources unchanged. Each patch therefore
falls into one of two classes:

- **(a) OVMX accommodation.** The patch exists because an OVMX component (the
  assembler or linker we drive, LINK.EXE, IMGACT, DECC$SHR, the headers)
  behaves differently from OpenVMS Alpha V8.4. Fix OVMX, delete the patch,
  and show the build that used the patch still passes.
- **(b) Defect in the GCC/binutils alpha-vms port.** Real OpenVMS hits it
  too. Keep the patch and record the evidence.

Method: each patch was read against the unpatched source in the vendored
tarballs (`tools/cross-alpha-vms/gcc-14.2.0.tar.xz`, binutils 2.43). The
question for each patch was whether the failure needs any OVMX component. A
defect that shows up while the Linux-hosted cross `cc1`/`gas`/`nm` is still
compiling, or that is a C/C++ language error in GCC's own VMS-host files,
does not depend on OVMX. One claim depended on runtime behaviour and was
checked against a real VMS system; the 0013 row says which. Line numbers
refer to the unpatched upstream files.

## Result

All 13 patches are class (b). None of them makes up for an OVMX divergence,
so none was deleted.

| Patch | What it changes | Class | Evidence |
|---|---|---|---|
| `0001-vms-f97-alpha-en-label-decorated-name.patch` | `alpha_start_function` derives the `..en` entry label from the alias-resolved name (`decc$strlen..en`), the same name `.ent`/`.pdesc` use | (b) | `gcc/config/alpha/alpha.cc:8011,8063` builds the label from the raw `fnname`, but `assemble_name` resolves `IDENTIFIER_TRANSPARENT_ALIAS` (`gcc/config/vms/vms.cc`) when it writes `.pdesc`. As a result `.pdesc` names a label that is never defined, and GAS rejects the object on the Linux build host before any OVMX component runs. On real VMS the same happens to any GCC-compiled definition of a C RTL name. Exercised by the musl `libc.a` build (`build-musl.sh`, ci-alpha). |
| `0002-vms-f97-vmsdbgout-en-decorated-name.patch` | The VMS DST routine-begin record uses the alias-resolved `..en` name | (b) | Same defect at a second site: `gcc/vmsdbgout.cc:657` does `concat (rtnname, "..en")` on the raw name. GCC emits this DST record for every function, so GAS rejects the object on the cross host. Exercised by the musl build. |
| `0003-vms-a0c-weakalias-en.patch` | Defines `ASM_OUTPUT_DEF_FROM_DECLS` for VMS so that a function alias also aliases its `..en` entry | (b) | Every VMS call site refers to `<callee>..en` through the linkage pair (`alpha_write_one_linkage`). The VMS port never defines `ASM_OUTPUT_DEF_FROM_DECLS` (`gcc/varasm.cc:6333` falls back to `ASM_OUTPUT_DEF`), so for `__attribute__((alias))` the label `alias..en` is never defined and GAS fails. The ELF Alpha port already solves the same problem for `..ng` (`gcc/config/alpha/elf.h:87`). The failure is in the cross `gas`, with no OVMX involvement. |
| `0004-vms-52c1-evax-weak-func-alias.patch` (binutils) | The vms-alpha back end emits defined WEAK symbols into the EGSD, and `obj-evax` derives an alias's entry symbol from `<name>..en` | (b) | `bfd/vms-alpha.c` `_bfd_vms_write_egsd` (≈3680) drops any defined symbol that lacks `BSF_GLOBAL`, and `gas` never sets that flag on a `.weak` symbol. The weak-flag code a few lines further down can therefore never run. `gas/config/obj-evax.c` `evax_frob_symbol` (≈108) walks an equate's value expression after it has been resolved to `O_constant`, so even a STRONG alias fails to assemble ("no entry symbol for global function"). Both are host-independent binutils defects. |
| `0005-vms-7b96-defer-dst-at-scan.patch` (binutils) | Skips the EDBG/ETBT (DST) replay during the object-scan pass, as is already done for ETIR | (b) | In `bfd/vms-alpha.c` (≈2723) the scan replays the DST into a `$DST$` section of size 0. `image_write` rejects the store, `alpha_vms_add_lw_reloc` is a stub that returns false, and there is no link `info`. The result is that GNU `nm`/`objdump` on Linux cannot read any `-gvms` object. This is a binutils read-side defect and does not depend on OVMX. |
| `0006-vms-fd1-alpha-vms-pt-null-hfp-elimination.patch` | `alpha_vms_can_eliminate` keeps the hard-frame-pointer elimination available for PT_NULL procedures | (b) | `gcc/config/alpha/alpha.cc:7407` allows only `to == STACK_POINTER_REGNUM`. Reload can later turn the procedure into PT_STACK and withdraw the SP elimination, which leaves no elimination for the soft frame pointer, and `cc1` emits `stq ..,0($63)`, which GAS rejects. This is a cross-host compiler defect. Test: `tools/cross-alpha-vms/test/run_pt_null_elim.sh` (ci-alpha). |
| `0007-vms-1045-p32-eh-udata8.patch` | 32-bit-pointer code encodes its EH addresses as `DW_EH_PE_udata8` | (b) | libgcc is always built with 64-bit pointers on VMS (`libgcc/config/alpha/t-vms:11` `HOST_LIBGCC2_CFLAGS=-mpointer-size=64`). The unwinder therefore reads `absptr` as `sizeof (void *)` = 8 (`libgcc/unwind-pe.h:79-80`), while 32-bit code writes 4-byte values. The two sides of GCC disagree with each other. A real VMS host running GCC's own libgcc has the same mismatch. Test: `run_p32_eh_encoding.sh` (ci-alpha). |
| `0008-vms-fd1-xm-vms-delete-if-ordinary-error.patch` | `xm-vms.h` `DELETE_IF_ORDINARY` reports through `error ("%s: %m")` | (b) | Bit-rot: `gcc/config/vms/xm-vms.h:50` calls `perror_with_name`, which GCC 14 no longer defines. `gcc/gcc.cc:2696-2703` has its own default that uses `error ("%s: %m")`. A VMS-hosted build of the driver fails on any VMS. Test: `run_xm_vms_delete.sh` (ci-alpha). |
| `0009-vms-fd1-vms-ld-ar-declarations.patch` | `vms-ld.c` and `vms-ar.c` declare `SYS$TRNLNM`, `SYS$CRELNM`, `decc$to_vms` and `mkstemps` | (b) | Neither file includes `<starlet.h>`, `<unixlib.h>` or `config.h` (`gcc/config/vms/vms-ld.c:28-35`, `vms-ar.c:21-29`), but they call these functions (`vms-ld.c:168,434,470`; `vms-ar.c:76,308`). `include/libiberty.h` does not declare `mkstemps`. GCC 14 rejects implicit function declarations, so the build fails whatever headers the host has. Test: `run_vms_ld_ar_decls.sh` (ci-alpha). |
| `0010-vms-e839-alpha-vms-frame-locals-overlap.patch` | The VMS frame size and arg-pointer offset round the register-save area before adding the locals | (b) | `gcc/config/alpha/alpha.cc:7465` puts the locals at `ALPHA_ROUND (sa_size + pv_save_size)`. The frame size (`:7338`) and the arg pointer (`:7468`) round the unrounded sum instead. When the save area is 8 mod 16, the last local quadword lies above the frame, inside the caller-owned argument area of the Alpha stack-frame layout (OpenVMS Calling Standard, Alpha stack procedure frame). The bug is visible in `cc1` assembly alone. Test: `run_vms_frame_layout.sh` (ci-alpha). |
| `0011-vms-fd1-gcov-tool-nftw.patch` | `configure` checks for `nftw`, and `gcov-tool.cc` uses it only when the check succeeds | (b) | `gcc/gcov-tool.cc:86-87` calls `nftw` whenever `<ftw.h>` exists. The DEC C RTL provides `ftw` but not `nftw`; GCC's own map of the DECC$SHR entry points lists `ftw` (`gcc/config/vms/vms-crtlmap.map:363`) and has no `nftw`. OVMX's header set copies that name set (`tools/cross-alpha-vms/musl-arch/decc-crtl-names.txt`), so OVMX matches VMS here. Test: `run_gcov_tool_nftw.sh` (ci-alpha). |
| `0012-vms-fd1-vmsdbgout-vms-host-cxx.patch` | The VMS-host `vms_file_stats_name` compiles as C++ (typed descriptor initializers, `const char *` file spec) | (b) | `gcc/vmsdbgout.cc:1716,1725-1731` initializes `__fibdef_ptr32` (GCC's own `struct dsc$descriptor_fib`, `:1638-1642`) and `dsc$a_pointer` from `(void *)`, and `:1677` takes `char *` but is given a `const char *`. C++ forbids both conversions, and GCC has been compiled as C++ since 4.8. Test: `run_vmsdbgout_vms_host_cxx.sh` (cxx gate in `tools/cross-alpha/run-module-gp-activation-alpha.sh`). |
| `0013-vms-fd1-inline-variable-weak-without-comdat.patch` | An initialized C++17 inline variable is made WEAK on a target that has weak symbols but no COMDAT | (b) | VMS sets `SUPPORTS_ONE_ONLY` = 0 (`gcc/defaults.h:300-305`; the port does not define `MAKE_DECL_ONE_ONLY`). `gcc/cp/decl.cc:8925` runs `maybe_commonize_var` again only when `!flag_weak`, so the variable is emitted as a strong `.globl` in every translation unit, while a template static member is emitted `.weak`. Observed on real VMS (lab `vaxlab-4` VAX1, 2026-11-02): linking two objects that both define `DUP839::` gives `%LINK-W-MULDEF` and `$STATUS = %X10648268`, a warning. GCC's own `vms-ld.c:936` treats any status without the success bit (`(status & 1) != 1`) as a failed link. Linking cc1 therefore fails on real VMS too. Test: `run_inline_variable_weak.sh` (cxx gate). |

## Class (b) list

0001, 0002, 0003, 0004, 0005, 0006, 0007, 0008, 0009, 0010, 0011, 0012, 0013.
0004 and 0005 apply to binutils 2.43, not to GCC.

Two decisions belong to Baron: whether class (b) patches are acceptable in the
R7 self-host proof, and whether to report these defects upstream to GCC or
binutils. This audit does not post anything outside the project.

## Side finding: OVMX LINK.EXE MULDEF severity

The lab check for 0013 showed that real VMS reports a strong-versus-strong
multiple definition as `%LINK-W-MULDEF`: a warning, after which the link still
writes the image. OVMX `src/vmslink/link.c` reports `%LINK-F-MULDEF` and does
not write an image. This does not affect 0013, because GCC's `vms-ld` fails the
link on a warning just as it does on a fatal error. It is still a
compatibility difference and is tracked as vms-bff.

## Keeping this table current

`tools/cross-alpha-vms/test/check_patch_audit.sh` fails when a file in
`patches/` has no row here, or when a row names a patch that no longer exists.
It runs in CI / Core Gates on every PR.
