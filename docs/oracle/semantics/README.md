# Semantic oracle: OVMX services vs real OpenVMS (rd vms-8d1)

The constants gate (`tools/compat/check_oracle_constants.py`) proves OVMX's header
*values* match a real OpenVMS system. This directory does the same for service
*behaviour*: what `$TRNLNM`, `$SETEF`, `$GETJPI`, RMS and the RTLs actually do with a
given input, including the edge and error cases a manual reading gets wrong.

## How it works

1. **One spec per service family** — `tools/oracle/semantic/specs/<family>.py` is a
   table of calls (inputs, including edge and error cases) and of what to print
   after each.
2. **One spec, two programs** — `tools/oracle/semantic/spgen.py` generates
   * MACRO-32 for the real nodes (VAX V7.3 has MACRO and LINK but no C compiler;
     Alpha V8.4 compiles the same VAX MACRO source with its MACRO-32 compiler), and
   * C for OVMX, built with the OVMX toolchain (cc → LINK.EXE) against the shareable
     producer graph and mastered onto a test-only disk
     (`/boot/ovmx-distrib-semprobe.img`, `distro/Dockerfile.bootable`).

   Both print the same canonical transcript: one line per case,
   `<CASE-ID> st=<status> <field>=<value> ...`. The transcript printer never uses the
   service under test (MACRO formats with `$FAOL`, C with `printf`), constants come
   from the real system's STARLET dumps (`K()`), not from OVMX's headers, and values
   that legitimately differ between systems (PIDs, node names, a job table's address)
   are never printed raw.
3. **Goldens** — `<family>/alpha84.txt` and `<family>/vax73.txt` are what the probe
   printed on a real OpenVMS Alpha V8.4 and VAX V7.3 node. Each carries a provenance
   header and the sha256 of the spec it was captured from; editing a spec without
   re-capturing fails `tests/integration/test_semantic_oracle_static.sh`.
4. **The gate** — CI's persistent-boot job boots the probe disk on the real
   executive (`tests/qemu/test_semantic_oracle.sh`) and runs
   `tools/oracle/semantic/semantic_diff.py`. OVMX is compared with the **Alpha V8.4**
   golden (the newest real system, 64-bit like OVMX's x86_64 runtime; VAX is the
   fallback for a family with no Alpha golden). Cases where VAX and Alpha themselves
   differ are reported as ARCH-DIVERGENT.
5. **The ratchet** — `known-diff.txt` lists every case known to differ, each with the
   rd item that tracks the bug. An unlisted difference fails the gate; so does a
   listed case that now matches. The list only shrinks.

Every mismatch is a bug in OVMX (or, if the probe is wrong, in the probe — never fix
it by editing a golden by hand).

## Capturing a golden

Use your own disposable lab pod (never a shared oracle): clone the alphalab /
vaxlab pod spec under a new name in `ovmx-lab`, with memory request == limit and a
small CPU request. The stock alphalab console pump types one line per second; start
the pod with the pump at `-d 0.15`, e.g. container command
`sed 's#srmdrv.py -t 0 #srmdrv.py -d 0.15 -t 0 #' /usr/local/bin/entrypoint.sh > /tmp/ep.sh && exec bash /tmp/ep.sh`.

    tools/oracle/semantic/capture.py vax   <pod> <family> --login <password>
    tools/oracle/semantic/capture.py alpha <pod> <family> --login <password>

A freshly cloned Alpha golden disk asks SYSTEM for a new password at first login;
`--login` changes it to `semprobe2026` (VMS refuses the old one) and says so. Capture twice and diff to check a new
case is deterministic before committing.

Clean-room (AGENTS.md Rule 8): goldens are the observed output of real systems
running these probes. Nothing is disassembled and no VSI/HPE source is used.

## Routines OVMX does not have yet

A spec calls a routine the real system provides even when OVMX does not. List it in
`tools/oracle/semantic/ovmx_absent.txt`: the C probe then prints `st=ABSENT` for
those cases instead of failing to link, and the gate tracks them like any other
difference. Implementing the routine means deleting its line. The list is kept
outside the specs, so the real-VMS goldens stay valid.

## Observations that are not a probe

`capture.py --dcl <vax|alpha> <pod> <outfile> 'DCL' ...` runs DCL commands on the
node and keeps their output verbatim, with a provenance header. Use it for things
like an `ANALYZE/RMS_FILE/FDL` report or a `DUMP/RECORDS` of a system file. These
files are reference evidence, not gate input.

## Adding a family

Write `specs/<family>.py`, generate and assemble it on both nodes (`capture.py`),
add the family's cases to the OVMX build (automatic: `mk_semprobes.sh` builds every
spec), run the gate, and file each difference.
