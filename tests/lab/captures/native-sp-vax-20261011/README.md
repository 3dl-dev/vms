# The semantic-oracle probes LINKed on real OpenVMS VAX V7.3 (2026-10-11)

Evidence for rd vms-b869 (the VAX rung of vms-3b3f): the probe images OVMX/VAX
runs unchanged in its $STATUS gate (tests/native-images/vax/sp/). Captured on
a disposable lab pod (`nexe-vax`, node `VAX1`, OpenVMS VAX V7.3 under SIMH,
lab image :6) with the tracked tools only:

- `tools/oracle/semantic/capture.py vax nexe-vax <family>` typed
  `spgen.py --mar` of each spec into the node, which assembled it with its own
  MACRO, LINKed and RUN it. Every transcript was identical to the committed
  golden `docs/oracle/semantics/<family>/vax73.txt` (ef fao tt io lnm info lock rtl).
- `capture.py --dcl ... 'DUMP SP_<F>.EXE'` -> `SP_<F>.DMP.txt`; the `.EXE` was
  rebuilt byte for byte by `tests/lab/tools/undump_vms.py` (the static semantic
  test re-checks it against this DUMP).
- `capture.py --dcl ... 'LINK/NOEXECUTABLE/MAP=SP_<F>.MAP/FULL SP_<F>'` ->
  `SP_<F>.MAP.txt`: each image calls its system services at their P1 vector
  addresses and its LIB$ and STR$ routines through LIBRTL; every one is in NATIVEACT's
  tables at the address the map gives.

Observed output of the real system only (clean-room Rule 8).
