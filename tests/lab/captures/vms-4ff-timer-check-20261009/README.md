# Node A's "IO-APIC + timer doesn't work!" panic, reproduced and A/B'd (rd vms-4ff, 2026-10-09)

The browser cluster demo's Node A (OVMX/x86_64 under qemu-wasm in a Web Worker) was observed on the
live page panicking in early boot with

```
..MP-BIOS bug: 8254 timer not connected to IO-APIC
Kernel panic - not syncing: IO-APIC + timer doesn't work!
  setup_IO_APIC+0x836/0x850 / apic_intr_mode_init / x86_late_time_init / start_kernel
```

(`tests/lab/captures/vms-deploy-v074-20260930/graded-with-the-broken-instrument/pass2-04-b-first-kernel-panic/OVMXA.console.log`).
V0.7-5 shipped `no_timer_check` on the demo's qemu command line on the MECHANISM, because the
condition could not be reproduced on demand at the time. This capture reproduces it, names the
mechanism exactly, and A/Bs the flag at the same starvation level.

## Method — no browser, no container, host QEMU only

The booted bytes are the ones the public page serves at `V0.7-8` (openvmx-site `9fba5d8`):

| artifact | sha256 (20) |
|---|---|
| `boot/vmlinuz` (OVMX 6.12.103-ovmx) | `262fac45df1ea4dc2518` |
| `demo/cluster/nodeA/initramfs-ovmx-nodeA.cpio.gz` | `ce94b1c2ee25cbce7e62` |
| `demo/cluster/nodeA/sysdisk-nodeA.qcow2` (gunzipped) | `2e5cbd07ed0128897acf` |

Host `qemu-system-x86_64` 8.2.2, the demo worker's own arguments
(`-M pc -m 256M -accel tcg,tb-size=500`, virtio-net + virtio-blk, `console=ttyS0 loglevel=3`), with
the Worker's scheduling condition modelled two ways:

* **vCPU slowdown** (`rig/slowboot.sh`): qemu pinned to ONE host core, sharing it with N
  equal-priority spinners, so the guest runs at roughly 1/(N+1) of host TCG speed — the
  three-heavy-Workers-in-one-tab condition. "~6x" below means 5 spinners, "~12x" means 11.
* **whole-VM stall** (`rig/stall-exp.sh`): SIGSTOP/SIGCONT, modelling a descheduled Worker.

`rig/stamp.py` prefixes host wall-clock seconds; the bracketed `[    0.012000]` numbers inside a line
are the guest's own printk timestamps.

## Result

| run | condition | outcome | log |
|---|---|---|---|
| A | unstarved, no flag | check passes; IRQ0 stays on IO-APIC pin 2; boots | `A-unstarved-flagless.log` |
| B | ~6x slowdown, no flag | `..MP-BIOS bug: 8254 timer not connected to IO-APIC`, IO-APIC pin **torn down**, IRQ0 silently falls back to **Virtual Wire**; boots | `B-6x-flagless-route-swap.log` |
| C | ~8x slowdown, no flag | same route swap; boots | `C-8x-flagless-route-swap.log` |
| D | ~12x slowdown, no flag | all four routes "failed" → **panic**, guest t=0.012 s, never boots | `D-12x-flagless-PANIC.log` |
| E | ~12x slowdown, `no_timer_check` | boots: mount 69 s, `%STDRV-I-STARTUP` 108 s | `E-12x-no_timer_check-boots.log` |
| F | ~12x, `no_timer_check`, `-smp 2` | boots (the shipped launcher's CPU count) | `F-12x-no_timer_check-smp2-boots.log` |
| G | 90 s SIGSTOP/CONT (50 ms on / 3000 ms off) applied AFTER `%OVMX-I-MOUNTED` | boot RESUMES and reaches `%STDRV-I-STARTUP` + the startup dialog | `G-poststall-90s-sigstop-resumes.log` |

## The mechanism, from the kernel's own source (v6.12.103)

`arch/x86/kernel/apic/io_apic.c`:

* `check_timer()` verifies the legacy timer IRQ with `timer_irq_works()`, trying four routes in turn
  (IO-APIC pin1, the 8259A, Virtual Wire, ExtINT) and `panic("IO-APIC + timer doesn't work!")` if all
  four fail.
* `timer_irq_works()` spins in `delay_with_tsc()` — `while ((rdtsc() - start) < 40000000000ULL/HZ ...)` —
  and then requires `time_after(jiffies, t1 + 4)`.

With `CONFIG_HZ_1000` that window is 40,000,000 TSC cycles. **Under TCG the guest TSC advances at host
wall-clock rate** (measured "Detected 2208.093 MHz processor" — the host's rate, calibrated against
HPET), so the window is ~18 ms of WALL time, while the vCPU executes orders of magnitude slower than
real silicon. The test therefore demands that a software-emulated CPU take and service **5 IRQ0 ticks
inside 18 ms of wall clock**. A slowed or descheduled vCPU cannot, and the kernel concludes its own
(fully emulated, perfectly good) timer is broken. Every run in this capture — including the unstarved
one — also shows the same family of mis-measurement in the TSC calibration itself:
`tsc: Fast TSC calibration failed` / `tsc: Unable to calibrate against PIT` → `using HPET reference
calibration`. Nothing is wrong with the timer; the measurement's window is wall-clock-bounded and the
guest's execution is not.

## Why `no_timer_check` is the correct configuration and cannot be dropped

* It is **upstream Linux's own policy for virtual machines**: `arch/x86/kernel/kvm.c`
  (`paravirt_ops_setup`) and `arch/x86/kernel/cpu/vmware.c` both set `no_timer_check = 1`
  unconditionally for guests they identify, because the check is only meaningful on physical
  hardware. A QEMU **TCG** guest is a virtual machine Linux cannot identify (no KVM/VMware CPUID
  signature), so it must be told on the command line.
* Its ONLY consumer in the whole kernel is `timer_irq_works()` (declared at `io_apic.c:1466`, read at
  `io_apic.c:1523`). It skips the MEASUREMENT and nothing else — and the route then kept is the pin1
  IO-APIC route, i.e. **the same configuration a correct measurement keeps** (run A). So the flag
  cannot be the cause of a later stall, and dropping it costs determinism: runs B and C show one
  unchanged image producing a load-dependent interrupt topology.
* Run D vs run E is the drop test: same image, same arguments, same starvation — panic without,
  boots with.

## What this capture does NOT prove

* It does not explain a Node A that **stalls after `%OVMX-I-MOUNTED`** (the failure mode reported
  for two V0.7-7 live runs). Run G starved exactly that phase to a 1.6 % duty cycle for 90 s and the
  boot resumed; the phase between `%OVMX-I-MOUNTED` and `%OVMX-I-SCSNODE` is `stage_boot_images()`
  (`src/ovmx_init/ovmx_init.c` Step 2a — ACP staging of the first-hop images off the ODS-2 volume),
  and it took 2.8 s unstarved, 39 s at ~12x, and survived the stall. No wedge was reproduced here.
* It is host QEMU/TCG, not qemu-wasm. The mechanism is accelerator-independent (a wall-clock-bounded
  measurement of an emulated tick rate), but the exact slowdown factor at which the browser crosses
  the threshold is not measured by this capture.
