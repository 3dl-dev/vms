#!/usr/bin/perl
# filter-headers.pl -- the OVMX C RTL headers declare, for alpha-dec-vms
# clients, only the functions DECC$SHR exports (vms-fe03).
#
#   perl filter-headers.pl <decc-crtl-names.txt> <musl include dir>
#
# A single-line function prototype in a public header whose name the port's
# CRTL name map does not cover (so a client call would bind a bare name that
# DECC$SHR does not export) is wrapped in
#   #if !defined(__VMS) || defined(__OVMX_LIBC_BUILD)
# so the C RTL's own build still sees it and an alpha-dec-vms client does not.
# Kept regardless: the RTL's reserved __ names (errno and stdio macros expand to
# them), and the threads headers (pthread.h, sched.h, semaphore.h, threads.h --
# on OpenVMS those come from the separate threads RTL, not DECC$SHR; their
# surface is its own rung). Prints the hidden count; idempotent.
use strict;
use warnings;

my ($names, $inc) = @ARGV;
die "usage: $0 <names.txt> <include-dir>\n" unless defined $inc && -d $inc;
my %keep;
open(my $nf, '<', $names) or die "$names: $!\n";
while (<$nf>) { next if /^#/; chomp; $keep{$_} = 1 if length; }
close $nf;
my %skipfile = map { $_ => 1 } qw(pthread.h sched.h semaphore.h threads.h);
my %kw = map { $_ => 1 } qw(void int char short long unsigned signed float double
    const volatile struct union enum restrict inline static extern _Noreturn);
my $guard = '#if !defined(__VMS) || defined(__OVMX_LIBC_BUILD) /* vms-fe03: not in DECC$SHR */';
my ($hidden, $files) = (0, 0);
for my $f (sort glob("$inc/*.h"), glob("$inc/*/*.h")) {
    (my $base = $f) =~ s{.*/}{};
    next if $skipfile{$base} && $f eq "$inc/$base";
    open(my $fh, '<', $f) or die "$f: $!\n";
    my @in = <$fh>;
    close $fh;
    next if grep { index($_, 'vms-fe03') >= 0 } @in;    # already filtered
    my @out;
    my $n = 0;
    for my $l (@in) {
        if ($l =~ /^[A-Za-z_]/ && $l !~ /^(typedef|struct|union|enum|static|extern\s+"C"|#)/
            && $l =~ /\)\s*;\s*$/ && $l !~ /\(\s*\*/
            && $l =~ /^[^(]*?\b([A-Za-z_]\w*)\s*\(/) {
            my $name = $1;
            if (!$kw{$name} && $name !~ /^_/ && !$keep{$name}) {
                push @out, "$guard\n", $l, "#endif\n";
                $n++;
                next;
            }
        }
        push @out, $l;
    }
    next unless $n;
    open(my $oh, '>', $f) or die "$f: $!\n";
    print $oh @out;
    close $oh;
    $hidden += $n;
    $files++;
}
print "filter-headers: hid $hidden declaration(s) in $files header(s) from alpha-dec-vms clients\n";
