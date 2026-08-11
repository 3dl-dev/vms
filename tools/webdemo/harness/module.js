// Capture-harness QEMU argv. MUST match the deployed demo's module.js machine
// shape (same -M/-m/-drive), or the snapshot won't resume in the shipped page.
// This harness boots the disk FRESH (no -loadvm) so capture.js can savevm the
// pre-booted 'ovmx' snapshot into the qcow2.
if (typeof Module === 'undefined') { var Module = {}; }
Module['arguments'] = [
  '-nographic',
  '-M', 'pc',
  '-m', '256M',
  '-accel', 'tcg,tb-size=500',
  '-L', '/pack-rom/',
  '-nic', 'none',
  '-kernel', '/pack-kernel/vmlinuz',
  '-initrd', '/pack-initramfs/initramfs-ovmx.cpio.gz',
  '-append', 'console=ttyS0 loglevel=3 quiet',
  '-drive', 'file=/pack-disk/sysdisk.qcow2,format=qcow2,if=virtio',
  '-no-reboot',
];
Module['locateFile'] = function (p) { return new URL(p, document.baseURI).href; };
Module['mainScriptUrlOrBlob'] = new URL('out.js', document.baseURI).href;
