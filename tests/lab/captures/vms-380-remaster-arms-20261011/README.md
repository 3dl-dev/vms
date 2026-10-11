# vms-380: controlled lock-tree remaster arms on three real VAXes

Pod `rm380` (image `ovmx-vaxlab:7`): VAX1 (SYS0), VAX2 (SYS1), VAX3 (SYS2) on
one shared golden system disk, no OVMX node. Lock holders are `RMHOLD.MAR`
(this directory), built on VAX1 into `SYS$COMMON:[SYSMGR]`: argument
`<root mode><sub mode><n subs><W|-|A><name>` -- `W` writes a known value block
(`RMLVB-0123456789`), `A` drives 300 rounds of NL<->sub-mode conversions.
Ground truth (every lock id, mode, master copy) was read with SDA on each node.

- RA1/RA1b: VAX1 masters RMT1, VAX2/VAX3 hold it, VAX1 SHUTDOWN REMOVE_NODE.
  RMT1 is NOT remastered: shutdown moves only file-system trees
  (CACHE$/F11B$/SYS$_$/VCC$/MSCP$, as in dlmlab E1/E2/rm-A); RMT1 is rebuilt by
  failover (op 08) after VAX1 leaves.
- RA2 (`ra2-rmt2-remaster.pcap`, `RA2-truth.txt`): VAX2 lock activity on RMT2;
  no move while VAX1 holds its own locks; after VAX1 stops its holder the tree
  moves VAX1 -> VAX2 at 07:04:14. VAX1>VAX2 03/00 0e 0f 10 13 (each 82/15);
  VAX1>VAX3 0f/11 (msgtype 0x5b); the HOLDER VAX3 sends one 0x12 per own lock
  (root then subs) to VAX2, each answered 82/01 with a NEW-master lock id;
  VAX3>VAX2 0x13; VAX2 sends 0x14 to VAX3 and VAX1. Fields are named in rd
  vms-380 against this SDA truth.
- RA3 (`RA3-truth.txt`): the master HOLDING its own locks does not give the tree
  up to an equal-LOCKDIRWT node's activity within 30 s.
