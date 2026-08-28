# Joint-e2e proof: real alpha-dec-vms crt0 vs. the GENUINE alpha DECC$SHR (vms-864)

`build-joint-image.sh` proves a REAL alpha-dec-vms GCC-port crt0 links
**zero-deferred** against the **genuine** alpha DECC$SHR — musl-alpha's own
`decc$*` surface (whole-archived from libc.a/libgcc.a) plus the OVMX
bootstrap surface (`decc$main`, `decc$malloc`, `C$_EXIT1`) mk_decc_shr.sh's
ALPHA/EVAX branch now emits (vms-864).

## What it is

- `crt0.s` — a real alpha-dec-vms cc1 `-mpointer-size=64` compile of the GCC
  port's own `libgcc/config/vms/vms-ucrt0.c` (GPLv3, gcc-14.2.0). Captured as
  text because the toolchain image discards the GCC source tree after build
  (`tools/cross-alpha-vms/Dockerfile` — kept small on purpose); assembled
  fresh by the real cross `as` on every run, no binary object checked in.
- `joint_main.c` — trivial OVMX proof code:
  `printf("OVMX crt0 join: activated, argc=%d\n", argc); return 3;`
  compiled fresh by the real cross cc1 on every run.
- `build-joint-image.sh` — builds, in one containerized run: musl-alpha
  `libc.a` (-g0), `LINK.EXE`, `LIBOTS$SHR.EXE`, the genuine alpha DECC$SHR
  (`OVMX_DECC_ARCH=alpha` FORCED — never auto-detected; auto-detection
  misidentifying a plain-alpine musl as "generic" is bead vms-2a0, and this
  script exists specifically so the joint-e2e proof cannot silently fall into
  that mismatch again), then links crt0.obj + joint_main.obj against it.

## Before vms-864 (the bug this bead closes)

mk_decc_shr.sh's ALPHA branch built a real, isolated, zero-deferred DECC$SHR
(the `decc$*` surface musl-alpha itself defines) but `exit 0`'d before the
generic tail that compiles `decc$main` / `C$_EXIT1` (OVMX-authored code no C
library defines). Any real crt0 — which references both unconditionally —
failed:

```
%LINK-F-UNDEF, EVAX: undefined symbol 'C$_EXIT1' referenced by crt0.obj
```

(A prior, since-superseded run of this proof also built DECC$SHR from a
plain **host** libc.a — mk_decc_shr.sh's `OVMX_DECC_ARCH=auto` misdetecting
generic on a container without musl-alpha present — which happened to carry
`decc$main`/`C$_EXIT1` from the GENERIC tail and so "worked" while linking a
genuine EM_ALPHA crt0.obj against a **non-alpha** DECC$SHR: exactly the
vms-2a0 mismatch. This script forces the real path so that cannot recur.)

## After vms-864

```
%LINK-I-GVALFOLD, EVAX globalvalue 'C$_EXIT1' folded to absolute 0x35a009 (link-time constant, no import cell)
%LINK-I-IMPORT, EVAX cross-image import 'decc$malloc'  bound to --use producer DECC$SHR.EXE [sv#541]
%LINK-I-IMPORT, EVAX cross-image import 'decc$main'    bound to --use producer DECC$SHR.EXE [sv#540]
%LINK-I-IMPORT, EVAX cross-image import 'decc$tprintf' bound to --use producer DECC$SHR.EXE [sv#473]
%LINK-S-CREATED, joint_e2e.exe: EVAX/Alpha ET_DYN image, .vms$xfer count=1
```

Zero `%LINK-F-UNDEF`. `readelf -h joint_e2e.exe`: `Machine: Alpha`,
`Type: DYN`. `.vms$imp`: 3 imports (`decc$malloc`, `decc$main`,
`decc$tprintf`) — `decc$tprintf` and `decc$_malloc64` are musl-alpha's own
(the port compiler auto-decorates `printf`/`malloc` at codegen); `decc$main`
and the `<4 GB decc$malloc` are the OVMX bootstrap surface
(`src/vmslink/ovmx_decc_crtl.c`, cross-compiled by the alpha-dec-vms cc1 in
mk_decc_shr.sh's ALPHA branch). `C$_EXIT1` folds to a link-time absolute
(`0x35a009`, oracle-grounded on lab-Alpha) — never an activation import.

## The activation round-trip (conductor / Alpha path, unchanged from before)

1. IMGACT activates joint_e2e.exe; fills `.vms$imp` (`decc$main`,
   `decc$malloc`, `decc$tprintf`) against the loaded DECC$SHR; resolves
   `.vms$xfer` -> the crt0 `__main` PV.
2. Alpha 6-arg standard call to `__main` (R16..R21 =
   progxfer,cli_util,imghdr,image_file_desc,linkflag,cliflag; R25=AI;
   R27=PV->PDSC+8).
3. crt0 `__main` calls `decc$main` -> produces argc/argv/envp (non-CLI:
   argv[0]=image spec).
4. crt0 calls `main(argc,argv,envp)` -> prints via `decc$tprintf`, returns 3.
5. crt0 maps 3 via `C$_EXIT1` -> executive `$EXIT` -> DCL `$STATUS` readback.

## Reproduce (fully in-tree tooling, containerized)

```sh
IMG=ovmx-cross-alpha-vms tools/cross-alpha-vms/joint-e2e/build-joint-image.sh [OUTDIR]
```

Needs network access to fetch musl 1.2.5 the first time (`build-musl.sh`
honors a pre-placed `musl-1.2.5.tar.gz` in its `$WORK` dir for an offline
run — see `tools/cross-alpha-vms/musl-arch/build-musl.sh`). Everything else
(the alpha-dec-vms cross toolchain, `LINK.EXE`, `LIBOTS$SHR.EXE`) is built
from in-tree source inside the container.
