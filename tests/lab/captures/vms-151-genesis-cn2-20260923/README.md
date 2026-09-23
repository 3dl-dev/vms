# CN=2 from cold between two OVMX/x86 nodes — symmetric genesis (rd vms-151)

**What this is.** Two fresh OVMX/x86 executives, booted together on one LAN with
no cluster in existence and no real VAX to coordinate: they elect a founder,
form generation 1, admit each other, and BOTH say so on OPA0:. Every prior
"CN=2 achieved" capture in this directory is OVMX *joining a club a real VAX
already founded*. This is the first capture of OVMX founding one.

**Rig.** Host QEMU/KVM, two x86_64 guests on one `-netdev socket,mcast` LAN --
the same images, kernel and boot cmdline the in-browser demo uses
(`console=ttyS0 loglevel=3 quiet`, `-M pc -m 256M`, virtio-net), run natively so
a 10-minute browser run is a 40-second one. Node A = SCSNODE OVMXA, SCSSYSTEMID
1987; node D = OVMXD, 1990; both cluster group 257, VOTES=1, EXPECTED_VOTES=1,
VAXCLUSTER=2.

**Executive under test.** `work/vms-151-genesis` + the console-policy fix: the
branch's vms.ko swapped into the proven demo initramfs with
`tests/lab/tools/cpio_replace_member.py`, and PID 1 (STARTUP.EXE) rebuilt with
it, so a diff of these images against the demo's is exactly the executive and
the boot orchestrator.

## What the consoles show (the bar for this item)

Node A, the founder -- it wins the election on the lower SCSSYSTEMID, and every
line is the executive's own:

    %CNXMAN, this node has quorum by its own votes: forming an OpenVMS Cluster
    %CNXMAN, this node is a member of the cluster
    %CNXMAN, completed VAXcluster state transition
    %CNXMAN, this node is now a VAXcluster member

Node D, the loser -- it stands down, says so ONCE, and is admitted on the
ordinary op-0x02 path the moment A is a member:

    %CNXMAN, another system is also waiting to form an OpenVMS Cluster and takes precedence; this node will join it
    ...
    %CNXMAN, the cluster assigned this node a cluster system id
    %CNXMAN, this node is a member of the cluster
    %CNXMAN, VAXcluster state transition in progress
    %CNXMAN, this node is now a VAXcluster member

And SHOW CLUSTER on each node, read from the real guest console after login:

    View of Cluster from system ID 1987 node: OVMXA
    | OVMXA  | 00010001 | VMX V0.7        | MEMBER           |
    | 1990   | 00010002 |                 | MEMBER           |

    View of Cluster from system ID 1990 node: OVMXD
    | OVMXD  | 00010002 | VMX V0.7        | MEMBER           |
    | 1987   | 00010001 |                 | MEMBER           |

CSIDs 0x00010001 / 0x00010002 are generation 1, CSV slots 1 and 2 -- the
round-robin slot a coordinator hands out (p. 7-25), not a function of the
SCSSYSTEMID.

The in-browser run of the same images reads back the same two tables from the
same two guest consoles (`browser-stage1-result.json`, `A_cn2`/`D_cn2` true).

## Why the earlier runs of this item looked like a hang

They were not hangs. `ovmx_boot_mute_kernel_console()` lowers the Linux console
sink to a level that dropped every line the executive writes with
`exec_console_printf()`, so the cluster formed in silence: SHOW CLUSTER already
said MEMBER while the console had said nothing since login. The pair of levels
now lives in `src/kernel/ovmx_console_policy.h` with the invariant asserted at
compile time. Nothing about the formation itself changed to produce this
capture -- only whether the operator could hear it.

## Reproducing it

Per node (A shown; D differs only in MAC, initramfs and its own qcow2), with the
demo's `vmlinuz` and a config-injected initramfs + system disk:

    qemu-system-x86_64 -enable-kvm -M pc -m 256M -smp 1 -nographic \
      -kernel vmlinuz -initrd initramfs-nodeA.cpio.gz \
      -append "console=ttyS0 loglevel=3 quiet" \
      -drive file=sysdisk-nodeA.qcow2,format=qcow2,if=virtio \
      -netdev socket,id=vmnic,mcast=230.0.0.7:14507 \
      -device virtio-net-pci,netdev=vmnic,mac=52:54:00:00:00:0A \
      -no-reboot -serial <your console> -display none

`-netdev socket,mcast=` is the whole LAN: both guests join the same host
multicast group, so the LAVC multicast HELLOs reach each other exactly as they
do through the browser demo's in-page L2 hub -- with the same guest images, at
KVM speed instead of TCG's. The in-browser run of the same images is the
parent item's own harness (rd vms-735); this rig is the fast loop under it.

## Files

- `nodeA-OVMXA.console.log` -- node A's whole console, boot to SHOW CLUSTER.
- `nodeD-OVMXD.console.log` -- node D's, same run.
- `browser-stage1-result.json` -- the IN-BROWSER run of the SAME images on the
  k3s pod (two `node.html` iframes on one in-page L2 hub, qemu-wasm/TCG): both
  consoles' tails and the harness's own verdict,
  `{"pass":true,"elapsed_s":176,"A_pass":true,"D_pass":true,"A_cn2":true,"D_cn2":true}`
  -- `*_pass` is the membership announcement on that node's own console, `*_cn2`
  is that node's own SHOW CLUSTER naming two members.
- `browser-run.log` -- that run's transcript: `CN2_STAGE1 PASS` (both consoles
  name each other) and `CN2_STAGE1 READBACK` (both tables name two members).

**In-browser reliability, honestly.** Three earlier runs of these images on the
same pod went: both-MEMBER pass; formed-then-lost (node A's VC dropped at
t=252 s, `%CNXMAN, quorum lost, blocking activity`, and startup then blocked on
the quorum hang -- the executive doing the right thing on a real connectivity
loss); and founder-only (node D's admission stalled with `%CNXMAN, the peer has
spent every receive buffer this node extended`). The host-QEMU/KVM rig with the
identical images formed CN=2 every time. That reads as qemu-wasm/TCG throughput
starvation rather than an executive defect, but it has NOT been measured to the
point where that can be asserted -- it is rd vms-d25, under the browser-demo
item, not a claim made here.
