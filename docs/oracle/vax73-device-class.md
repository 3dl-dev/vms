# Oracle grounding: OpenVMS device classes (DC$_*) — for vms-8f7b

**Item:** `vms-8f7b` (authenticity epic `vms-898`). **Grounded:** 02-SEP-2026.
**Method:** clean-room grounding from the *documented* `$DCDEF` interface (CLAUDE.md Rule 8 —
documented constants, never VSI source, never disassembly), corroborated across two
independent published sources. This is the same lineage vms-d0b used to ground the `$SSDEF`
condition values (documented macro + F$GETDVI/F$MESSAGE round-trip).

## The problem

`src/libvms/include/dcdef.h` is authentic for indices 0–2 (UNKNOWN=0, DISK=1, TAPE=2) but
**sequential-fake from index 3 onward** (SCOM=3, CARD=4, LP=5, TERM=6, MAILBOX=7, NET=8,
REALTIME=9, WORKSTATION=10, SCANNER=11, PRINTER=12). Real OpenVMS `$DCDEF` values are
non-sequential. The repo is internally self-consistent (the executive mirrors dcdef.h), so
`$GETDVI DVI$_DEVCLASS` returns a wrong-but-consistent value — self-consistent fabrication,
exactly what INV-6 forbids.

## Grounded authentic table (`$DCDEF`, SYS$LIBRARY:STARLET.MLB)

| DC$_ symbol   | value | note |
|---------------|-------|------|
| DC$_UNKNOWN (a.k.a. DC$_ANY) | 0 | correct today |
| DC$_DISK      | 1     | correct today |
| DC$_TAPE      | 2     | correct today |
| DC$_SCOM      | 32    | serial-comms / LAN class (Ethernet controllers report this) |
| DC$_CARD      | 65    | card reader |
| DC$_TERM      | 66    | terminal |
| DC$_LP        | 67    | line printer — **note: 67, NOT 64** (see drift note) |
| DC$_WORKSTATION | 70  | workstation |
| DC$_REALTIME  | 96    | real-time device |
| DC$_DECVOICE  | 97    | (no OVMX consumer) |
| DC$_AUDIO     | 98    | (no OVMX consumer) |
| DC$_VIDEO     | 99    | (no OVMX consumer) |
| DC$_BUS       | 128   | (no OVMX consumer) |
| DC$_MAILBOX   | 160   | mailbox |
| DC$_REMCSL_STORAGE | 170 | (no OVMX consumer) |
| DC$_MISC      | 200   | miscellaneous |

**Sources (two independent, agreeing):**
- VSI OpenVMS Wiki, "Device class" (retrieved from `$DCDEF` in `sys$library:starlet.mlb`,
  page last updated 2018-08-22): https://wiki.vmssoftware.com/Device_class
- OpenVMS I/O User's Reference Manual / DCL Dictionary mirrors (odl.sysworks.biz,
  www0.mi.infn.it) — corroborate CARD=65, TERM=66, LP=67, SCOM=32, WORKSTATION=70,
  REALTIME=96, MAILBOX=160, MISC=200.

### Multi-source drift caught (MEMORY: multi-source drift is real)
The rd note and the dispatching session both carried **DC$_LP=64**. Both documented `$DCDEF`
sources give **DC$_LP=67**; neither lists any class at 64. **67 is authoritative; 64 was
wrong** and is not adopted.

### Non-authentic symbols to REMOVE (not renumber)
`$DCDEF` has **no DC$_NET, DC$_SCANNER, or DC$_PRINTER**. In dcdef.h these are fabricated
symbols (NET=8, SCANNER=11, PRINTER=12) with **zero consumers anywhere in the tree** (they
appear only on their own `#define` lines). Presenting a non-VMS symbol as real is the same
fabrication class this item exists to remove, so they are deleted, not renumbered. LAN/Ethernet
devices are already (correctly) classed DC$_SCOM by the executive
(`src/kernel-core/vms_devtab.c:412-414`, citing the VSI I/O manual) — there is no DC$_NET in
real VMS. Printers are DC$_LP; there is no separate DC$_PRINTER/DC$_SCANNER.

### Not added (no-gold-plating)
DECVOICE/AUDIO/VIDEO/BUS/REMCSL_STORAGE/MISC are authentic but have no OVMX consumer; they are
recorded here for provenance but **not** added to dcdef.h. A gap in the enum is authentic; an
invented entry is not.

## Optional Rule-10 confirmation (not required; grounding is already two-source)
The measurable subset can be confirmed on the reference lab VAX (V7.3, `~/vax/cluster`) via
`F$GETDVI(dev,"DEVCLASS")`: OPA0: → 66 (TERM), a disk → 1 (DISK), MBA → 160 (MAILBOX), a LAN
device → 32 (SCOM). Deliberately NOT run here: the lab is hosting the live CN=3 cluster drive
(capacity-sensitive), and two independent documented `$DCDEF` sources already agree. Available
if belt-and-suspenders confirmation is wanted.
