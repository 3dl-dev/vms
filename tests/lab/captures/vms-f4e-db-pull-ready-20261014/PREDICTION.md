vms-f4e repro, pre-registered 2026-10-14:
Topology c-after-a-up: OVMXA (lab 1021, group 1) boots first and waits; VAX1 (golden, root SYS0) is held at sim> until OVMXA is waiting, then continues, forms alone, and admits OVMXA during its own early startup.
R1: VAX1 bugchecks INVEXCEPTN within ~1 s of "completing VAXcluster state transition" (adding OVMXA).
R2: the capture shows which OVMX->VAX1 frame(s) precede the crash; the last frame before VAX1's departure is the candidate trigger.
Control: DC1/DC1b/e09b (OVMX joins AFTER VAX1 reached Username:) did not crash.
Amendment (run 1 formed the wrong way, OVMXA founded on seeing VAX1's vote): run 2 boots OVMXA with EXPECTED_VOTES 5 so only the VAX can form.

## Run 2 result (pod labf4e2, lab/int-94c 7946dcbfd ~ V0.7-9 code)
R1 held: VAX1 and OVMXA formed together (class FORM). 0.85 s after the last barrier step,
OVMXA sent VAX1 0b/05; VAX1 took a fatal INVEXCEPTN at once. The register dump matches the
demo gate's (R0=8, R1=04080000, R7=34, PSL=04080009, current process NULL).
R2: the last OVMX->VAX1 CM frame before VAX1's departure was the 0b/05. VAX1's PARAMS to OVMXA
had said body[82] = 0x2a. Files: r2-cm.pcap (cat 0x01/0x0b), r2-*-console.log.

## Fix proof, registered before it runs (work/vms-f4e-lnm-ready)
Same topology. P1: OVMXA sends no 0b/05 while VAX1's PARAMS said 0x2a and VAX1 has sent it
no 0b/01. P2: if VAX1 sends OVMXA a 0b/01, OVMXA answers it and only then pulls. P3: 0
bugchecks; VAX1 offers Username:.

## Fix proof result (pod labf4e4, work/vms-f4e-lnm-ready 256155428, artifacts run 38491844882)
Same topology: VAX1 and OVMXA formed together; VAX1 had said body[82] = 0x2a.
- P1 held: no 0b/05 for 63.7 s, while VAX1 was starting up.
- P2 held: VAX1 sent OVMXA its 0b/01 at 63.70 s, OVMXA answered 8b/01, and pulled 0.79 s later
  (0b/05 -> 8b/05, 0b/06 -> 8b/04). This is the real pattern of lnm1, where a requester pulled
  1.08 s after the member's own 0b/01.
- P3 held: 0 bugchecks; VAX1 offers Username:.
(Run labf4e3 was abandoned: the ^E halt landed inside VMB's disk load and the simulator never resumed.)
Files: r4-cm.pcap, r4-*-console.log.
