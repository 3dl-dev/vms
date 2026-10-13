# vms-db1: OVMX answers a booting VAX's kind-10 and kind-5 closes

The rule and predictions were registered before each fire; see `PREDICTION.md`.

Setup: pod `labd52c` (image `ovmx-vaxlab:7`, `ASYM_MSCP=1`). OVMXE founds first (EXPECTED_VOTES 1), then VAX1 and VAX2 (V7.3) boot 300 s later. OVMXE is booted through the gated `ovmx_fire.sh`.

## db1-r1 (integration `evac/int-db1` b43953904: kind 10 only)
- **Q1 held.** Each VAX sent OVMXE one kind-10 close. OVMXE pulled the 136-byte record, wrote it back, and answered 5 within 31 and 38 ms.
- Both VAXes then sent a kind-5 close, which this build withheld. Neither VAX reached `Username:`. 0 bugchecks.

## db1-r2 (integration 5ad8162a8: kind 5 and kind 10)
- **Q4, wire half: held.** OVMXE answered every kind-5 close (state 4, including the periodic ones every 300 s) and every kind-10 close (state 5). CN=3. 0 bugchecks.
- **Q4, Username half: failed.** Neither VAX reached `Username:` after 10 minutes.

Each VAX had also sent OVMXE one cat-0x0b op-0x01 with role 0x10 during startup. OVMXE treats that message as unroutable and never answers it. That is the next withheld answer, filed as rd vms-52d (p0).

Files: `db1-r1-*`, `db1-r2-*` (pcaps and consoles).
