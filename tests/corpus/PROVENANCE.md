# Corpus Provenance

This file documents the origin of every source in the OVMX test corpus.

Corpus acquired: 2026-02-19

---

## tier1-examples/ — Eight-Cubed VMS C Examples

**Source:** https://www.eight-cubed.com/examples.shtml
**Author:** James F. Duff
**Download date:** 2026-02-19
**Method:** HTTP fetch via `fetch_examples.py` — each file fetched from
`https://www.eight-cubed.com/examples/framework.php?file=<FILENAME>.c`,
HTML entities decoded and `<pre>` block extracted to produce raw C source.
**Files downloaded:** 229 / 229 (100%)
**License:** Eight-Cubed custom — see LICENSE-AUDIT.md for analysis

Individual example URLs follow the pattern:
```
https://www.eight-cubed.com/examples/framework.php?file=<FILENAME>.c
```

All 229 files carry the header:
```c
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */
```

### File inventory (229 files)

```
ots_cnvout.c             lib_addx.c               lib_analyze_sdesc.c
ots_cvt_t_x.c            lib_adawi.c              lib_analyze_sdesc_64.c
ots_cvt_x_xx.c           lib_ast_in_prog.c        lib_asn_wth_mbx.c
ots_cvt_xx_x.c           lib_attach.c             lib_bbcci.c
ots_divc.c               lib_bbssi.c              lib_callg.c
ots_movx.c               lib_char.c               lib_convert_date_string.c
ots_mulc.c               lib_crc.c                lib_create_dir.c
ots_powc.c               lib_ctrl.c               lib_currency.c
ots_powcj.c              lib_cvt_dtb.c            lib_cvt_dx_dx.c
ots_powjj.c              lib_cvt_htb.c            lib_cvt_otb.c
ots_scopy_dxdx.c         lib_cvt_vectim.c         lib_cvtf_from_internal_time.c
ots_scopy_r_dx.c         lib_cvtf_to_internal_time.c  lib_date_time.c
sys_acm.c                lib_day.c                lib_day_of_week.c
sys_adjwsl.c             lib_delete_file.c        lib_delete_logical.c
sys_align_faults.c       lib_delete_symbol.c      lib_digit_sep.c
sys_alloc.c              lib_do_command.c         lib_ediv.c
sys_ascutc.c             lib_emodg.c              lib_emul.c
sys_asctim.c             lib_establish.c          lib_extv.c
sys_asctoid.c            lib_extzv.c              lib_ffc.c
sys_assign.c             lib_ffs.c                lib_fid_to_name.c
sys_audit_event.c        lib_file_scan.c          lib_find_file.c
sys_avoid_preempt.c      lib_find_image_symbol.c  lib_format_date_time.c
sys_bio.c                lib_free_ef.c            lib_freen_dd.c
sys_brkthruw.c           lib_get_command.c        lib_get_date_format.c
sys_cancel.c             lib_get_ef.c             lib_get_foreign.c
sys_capabilities.c       lib_get_input.c          lib_get_logical.c
sys_cef.c                lib_get_symbol.c         lib_get_users_language.c
sys_check_access.c       lib_get_vm_page.c        lib_getdvi.c
sys_check_fen.c          lib_getjpi.c             lib_getqui.c
sys_check_privilege.c    lib_getsyi.c             lib_host.c
sys_chkpro.c             lib_ichar.c              lib_index.c
sys_clrast.c             lib_insv.c               lib_invo_ctx.c
sys_cmexec.c             lib_len.c                lib_lock_image.c
sys_cmkrnl.c             lib_locc.c               lib_lookup_key.c
sys_cluevt.c             lib_lp_lines.c           lib_matchc.c
sys_cpu_transition.c     lib_movc3.c              lib_movc5.c
sys_create_gpfn.c        lib_movtc.c              lib_mult_delta_time.c
sys_create_uid.c         lib_pause.c              lib_polyg.c
sys_crembx.c             lib_put_common.c         lib_put_output.c
sys_crempsc.c            lib_que.c                lib_radix_point.c
sys_creprc.c             lib_rename_file.c        lib_run_program.c
sys_cretva_64.c          lib_scanc.c              lib_scopy_dxdx.c
sys_cvt_filename.c       lib_scopy_r_dx.c         lib_set_logical.c
sys_dclast.c             lib_set_symbol.c         lib_sig_to.c
sys_dclcmh.c             lib_signal.c             lib_skpc.c
sys_dclexh.c             lib_spanc.c              lib_spawn.c
sys_delprc.c             lib_stop.c               lib_subx.c
sys_delmbx.c             lib_sys_asctim.c         lib_sys_fao.c
sys_device_path.c        lib_sys_faol.c           lib_sys_getmsg.c
sys_device_scan.c        lib_timer.c              lib_tparse.c
sys_enqw.c               lib_tra_asc_ebc.c        lib_tree.c
sys_erapat.c             lib_vm.c                 lib_wait.c
sys_exit.c               sys_bintim.c             sys_fastio.c
sys_fao.c                sys_filescan.c           sys_find_held.c
sys_format_audit.c       sys_forcex.c             sys_get_arith.c
sys_get_entropy.c        sys_get_region_info.c    sys_getrmi.c
sys_getsyi.c             sys_gettim_prec.c        sys_getdvi.c
sys_getdviw.c            sys_getenv.c             sys_getuai.c
sys_getjpi.c             sys_getjpiw.c            sys_getutc.c
sys_glx_lock.c           sys_gs64.c               sys_hash_pwd.c
sys_icc.c                sys_ident.c              sys_ieee.c
sys_init_vol.c           sys_io_fastpath.c        sys_lckpag.c
sys_lkwset.c             sys_lnm.c                sys_numtim.c
sys_perm_align_fault.c   sys_persona.c            sys_power.c
sys_process_affinity.c   sys_process_scan.c       sys_proxy.c
sys_purge_ws.c           sys_purgws.c             sys_queue.c
sys_resched.c            sys_rpcc_64.c            sys_rms_seq.c
sys_schdwk.c             sys_set_implicit_affinity.c  sys_set_process_properties.c
sys_set_security.c       sys_setast.c             sys_setdfprot.c
sys_setimr.c             sys_setpra.c             sys_setpri.c
sys_setprn.c             sys_setprv.c             sys_setswm.c
sys_setshlv.c            sys_setrwm.c             sys_show_intr.c
sys_sigprc.c             sys_snderr.c             sys_sndopr.c
sys_subsystem.c          sys_suspend.c            sys_sys_event.c
sys_timcon.c             sys_trans.c              sys_trnlnm.c
sys_unwind.c
```

---

## tier3-mmk/ — MMK (MadGoat Make)

**Source:** https://github.com/endlesssoftware/mmk
**Clone command:** `git clone --depth 1 https://github.com/endlesssoftware/mmk.git`
**Cloned commit:** 28f8efb (Merge pull request #105 from craigberry/master)
**Download date:** 2026-02-19
**Authors:** Matthew Madison; Endless Software Solutions
**License:** BSD 3-Clause — see `tier3-mmk/license.txt`
**C source files:** 17
**Total files:** ~57
**Description:** MadGoat Make (MMK) — VMS-native make utility. Demonstrates
VMS process control, RMS file I/O, DCL symbol manipulation, and MMS
descriptor file parsing. Real-world build tool used on OpenVMS systems.

---

## tier3-netlib/ — NETLIB

**Source:** https://github.com/endlesssoftware/netlib
**Clone command:** `git clone --depth 1 https://github.com/endlesssoftware/netlib.git`
**Cloned commit:** 2ea2568 (Added PDF documentation)
**Download date:** 2026-02-19
**Authors:** Matthew Madison / MadGoat Software (1993–2004)
**License:** Custom / "Other" (GitHub classification) — copyright notice in
source headers reads "ALL RIGHTS RESERVED". No explicit open-source license
file found. See LICENSE-AUDIT.md.
**C source files:** 10
**Total files:** ~36
**Description:** NETLIB — VMS QIO-based async TCP/IP networking library.
Demonstrates QIO with AST callbacks, socket I/O, DNS resolution. Key VMS
async networking patterns.

---

## tier4-mx/ — MX Email Server

**Source:** https://github.com/endlesssoftware/mx
**Clone command:** `git clone --depth 1 https://github.com/endlesssoftware/mx.git`
**Cloned commit:** 28e2721
**Download date:** 2026-02-19
**Authors:** Matthew Madison / Endless Software Solutions (copyright 2008+)
**License:** BSD 3-Clause (per source file headers in BLISS modules)
**C source files:** 5 (primarily BLISS .b32 — 114 files)
**Total files:** 307
**Description:** MX — Full VMS email server (SMTP, local delivery, routing).
20–50k LOC. Primarily written in BLISS-32 with some C. Demonstrates
complex real-world VMS system interactions.

---

## Corpus acquisition round 2 (`vms-e86`, 2026-08-04)

The four directories below were added under `vms-e86` — a breadth pass that
also produced `tests/corpus/inventory.json` (machine-readable, 55 records
covering source AND binary leads) and `tests/corpus/INVENTORY.md` (human
summary, top targets, flagged list). See those files for the full survey;
this section only documents what actually landed in git.

## tier6-laxdriver/ — vms-laxdriver

**Source:** https://github.com/jhamby/vms-laxdriver
**Clone command:** `git clone --depth 1 https://github.com/jhamby/vms-laxdriver.git`
**Cloned commit:** b6e15c2d9b7b98953814e022c9e66dd3e0949cf0 (2022-06-17)
**Download date:** 2026-08-04
**Author:** Jake Hamby
**License:** MIT — see `tier6-laxdriver/LICENSE`
**Description:** LAX0: — Load Average eXtended driver for 64-bit OpenVMS.
A real VMS device driver written in C (fixed-point load-average arithmetic),
plus a test harness (`test-lax-driver.c`) that opens and reads the device.
The only corpus item exercising VMS driver-level APIs rather than
application-level RTL/system-service calls.

## tier6-ipc-benchmark/ — vms-ipc_benchmark

**Source:** https://github.com/jhamby/vms-ipc_benchmark
**Clone command:** `git clone --depth 1 https://github.com/jhamby/vms-ipc_benchmark.git`
**Cloned commit:** 1a1175981bfbe6dc5f4f3d9c1caab18d008e9a21 (2023-10-07)
**Download date:** 2026-08-04
**Author:** Jake Hamby
**License:** MIT — see `tier6-ipc-benchmark/LICENSE`
**Description:** IPC benchmark suite ported to OpenVMS — one small C program
per mechanism: pipes, FIFOs, UNIX-domain sockets, TCP, UDP, POSIX message
queues, shared memory, socketpair. Broad IPC/mailbox/shared-memory API
coverage in a small, clearly-licensed package.

## tier6-memtester/ — vms-memtester

**Source:** https://github.com/jhamby/vms-memtester
**Clone command:** `git clone --depth 1 https://github.com/jhamby/vms-memtester.git`
**Cloned commit:** 6f95a02610918e590741c7bbac5cb69ae90c4126 (2022-05-12)
**Download date:** 2026-08-04
**Author:** Jake Hamby
**License:** GPL-2.0 — see `tier6-memtester/COPYING`
**Description:** Port of the classic `memtester` memory-diagnostic tool to
64-bit OpenVMS (Alpha, IA64, x86). Exercises memory allocation/locking and
signal handling.

## tier6-cmatrix/ — cmatrix

**Source:** https://github.com/jhamby/cmatrix
**Clone command:** `git clone --depth 1 https://github.com/jhamby/cmatrix.git`
**Cloned commit:** 67d93eade64c62580c348619e9d1e9e096294e69 (2021-01-28)
**Download date:** 2026-08-04
**Author:** Jake Hamby (port); original cmatrix by Chris Allegretta
**License:** GPL-3.0 — see `tier6-cmatrix/COPYING`
**Description:** OpenVMS port of the "Matrix" terminal-rain screensaver,
with VT320 font handling. Low VMS-API value, cheap acquisition, useful as a
terminal/VT-control smoke test.
