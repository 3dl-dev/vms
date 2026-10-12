# vms-4822 / vms-239: OVMX mounts a disk a real VAX serves and reads a file over MSCP

Pod `mscp-239` (image `ovmx-vaxlab:7`, `ASYM_MSCP=1`). VAX1 and VAX2 (OpenVMS VAX
V7.3) form the cluster. VAX1 privately owns an RA81 on rq2 (`$2$DUA2:`, label
`DATA2`), which holds `LIST.TXT` (17 blocks, written on VAX1, not mounted there).
OVMXE was booted with artifacts built from commit
41d4c1fae591a80f0539a797259c417d5f62b5fb. The boot went through
`tests/lab/tools/ovmx_fire.sh`, which runs the frame-conformance gate first
(`mscp_cmd`, `scs_conreq_mscp`).

On OVMXE (run r20, 2026-10-11 09:31 UTC):

    $ MOUNT VAX1$DUA16386: DATA2
    %MOUNT-I-MOUNTED, DATA2 mounted on _VAX1$DUA16386:
    $ TYPE VAX1$DUA16386:[000000]LIST.TXT
    ... (the whole directory listing VAX1 wrote) ...
    Total of 362 files.

`mount-type-over-mscp.pcap` captures every 0x6007 frame on the bridge. It
contains 1 ONLINE and 32 READ commands from OVMXE, and 32 opcode-0x15 READ END
frames from VAX1, each with its block trailer, all delivered. The VAX consoles
show 0 bugchecks (`vax1-console-tail.log`, `vax2-console-tail.log`).

## The two halts that came before, and their cause

- r17 (`r17-vax2-bugcheck-console.log`): VAX2 halted with `VAXPORT, Fatal error
  detected by VAX port driver` on OVMXE's first READ.
- r19 (`r19-vax1-bugcheck-console.log`): VAX1 halted the same way. Buffer names
  were already in the wire's form for this run.

The cause of both: the READ buffer descriptor's last longword carried OVMX's own
Con.ID. In 71/71 real READ/WRITE commands it is the server's Con.ID, the
destination of the command frame. The fix is in rd vms-239, and the
frame-conformance gate now pins it.
