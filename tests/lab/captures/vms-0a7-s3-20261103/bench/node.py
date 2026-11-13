#!/usr/bin/env python3
"""Tiny pexpect driver for booting OVMX x86 nodes under local KVM on a shared
qemu mcast socket L2 segment (a stand-in for the demo's in-page hub). Copied from the vms-05d bench; REL defaults to the vms-3106 bench release dir."""
import pexpect, sys, time, os

REL = os.environ.get('REL', '/tmp/vms3106-lab/rel')

class Node:
    def __init__(self, name, disk, mac, log, mcast='230.0.0.1:12345', kernel=None, initrd=None, mem='512M', pcap=None):
        kernel = kernel or f'{REL}/vmlinuz'
        initrd = initrd or f'{REL}/initramfs-ovmx-slim.cpio.gz'
        cmd = ['qemu-system-x86_64', '-enable-kvm', '-kernel', kernel, '-initrd', initrd,
               '-nographic', '-append', 'console=ttyS0 loglevel=3 quiet',
               '-m', mem, '-smp', '1', '-nodefaults', '-serial', 'stdio',
               '-netdev', f'socket,id=n0,mcast={mcast}',
               '-device', f'virtio-net-pci,netdev=n0,mac={mac},romfile=',
               *(['-object', f'filter-dump,id=fd0,netdev=n0,file={pcap}'] if pcap else []),
               '-drive', f'file={disk},format=raw,if=virtio,cache=writethrough', '-no-reboot']
        self.name = name
        self.logf = open(log, 'ab')
        self.p = pexpect.spawn(cmd[0], cmd[1:], timeout=60); self.p.logfile_read = self.logf

    def send(self, s):
        self.p.send(s + '\r')

    def expect(self, pat, t=60):
        return self.p.expect(pat, timeout=t)

    def boot_login(self, user='SYSTEM', pw=os.environ.get('OVMX_PW'), t=400):
        end = time.time() + t
        while time.time() < end:
            try:
                self.p.expect('Username:', timeout=5)
                break
            except pexpect.TIMEOUT:
                self.p.send('\r')
        else:
            raise RuntimeError(f'{self.name}: no Username:')
        time.sleep(0.5)
        self.send(user)
        self.p.expect('Password:', timeout=60)
        time.sleep(0.3)
        self.send(pw)
        self.p.expect(r'\$ ', timeout=120)

    def cmd(self, c, t=60, until=r'\$ '):
        self.send(c)
        self.p.expect(until, timeout=t)
        return self.p.before.decode('latin1', 'replace')

    def close(self):
        try:
            self.p.terminate(force=True)
        except Exception:
            pass
