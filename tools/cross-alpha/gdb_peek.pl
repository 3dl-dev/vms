#!/usr/bin/perl
# gdb_peek.pl - a minimal GDB remote-serial-protocol client for qemu-system-alpha's
# gdbstub (vms-5303e). Diagnostics only: stop the guest at one user-mode PC, print
# registers and a memory window each time it is hit, and let the guest run on.
#   gdb_peek.pl PORT PC HEXADDR_REG_NAME LEN OUTFILE
# e.g. gdb_peek.pl 1234 0x1bd234 r16 232 /work/peek.log  (dump LEN bytes at register's value)
use strict; use warnings; use IO::Socket::INET;
my ($port, $pc, $reg, $len, $out) = @ARGV;
$pc = hex($pc);
my %regno = map { ("r$_" => $_) } 0 .. 31;
open(my $log, '>', $out) or die; select((select($log), $| = 1)[0]);
my $s;
for (1 .. 600) {
    $s = IO::Socket::INET->new(PeerAddr => "127.0.0.1:$port", Proto => 'tcp') and last;
    select(undef, undef, undef, 0.5);
}
$s or do { print $log "gdb_peek: no gdbstub\n"; exit 1 };
sub csum { my $t = 0; $t += ord($_) for split //, shift; $t & 255 }
sub send_pkt { my $d = shift; print $s sprintf('$%s#%02x', $d, csum($d)); my $c; read($s, $c, 1); }
sub recv_pkt {
    my ($c, $d) = ('', '');
    do { read($s, $c, 1) or return undef } until $c eq '$';
    while (read($s, $c, 1) and $c ne '#') { $d .= $c }
    my $x; read($s, $x, 2); print $s '+';
    return $d;
}
sub le64 { my $h = shift; my $v = 0; for (my $i = 14; $i >= 0; $i -= 2) { $v = ($v << 8) | hex(substr($h, $i, 2)) } $v }
send_pkt('?'); recv_pkt();
send_pkt(sprintf('Z0,%x,4', $pc)); my $r = recv_pkt();
print $log "gdb_peek: breakpoint at ", sprintf('%#x', $pc), " -> $r\n";
my $n = 0;
my $first = 1;
while ($n < 400) {
    unless ($first) {
        # step off the breakpoint before continuing, as a debugger does
        send_pkt(sprintf('z0,%x,4', $pc)); recv_pkt();
        send_pkt('s'); recv_pkt();
        send_pkt(sprintf('Z0,%x,4', $pc)); recv_pkt();
    }
    $first = 0;
    send_pkt('c');
    my $stop = recv_pkt();
    last unless defined $stop;
    send_pkt('g'); my $g = recv_pkt();
    my @r = map { le64(substr($g, $_ * 16, 16)) } 0 .. 31;
    my $addr = $r[$regno{$reg}];
    $n++;
    printf $log "HIT %d stop=%s pc=%#x t0(r1)=%#x a0(r16)=%#x a1=%#x a2=%#x ra(r26)=%#x pv(r27)=%#x sp=%#x\n",
        $n, substr($stop, 0, 3), $pc, $r[1], $r[16], $r[17], $r[18], $r[26], $r[27], $r[30];
    if ($r[1] == 0 || $r[1] < 0x1000) {
        for my $w ([$addr - 64, $len + 128]) {
            send_pkt(sprintf('m%x,%x', $w->[0], $w->[1])); my $m = recv_pkt();
            print $log sprintf("MEM %#x:\n", $w->[0]);
            for (my $i = 0; $i < length($m); $i += 32) { print $log '  ', substr($m, $i, 32), "\n" }
        }
        last;
    }
}
send_pkt(sprintf('z0,%x,4', $pc)); recv_pkt();
send_pkt('D'); recv_pkt();
print $log "gdb_peek: done after $n hits\n";
