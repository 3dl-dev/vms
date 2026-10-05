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
# so the C RTL's own build -- musl and OVMX's runtime shareables, both built
# -D__OVMX_LIBC_BUILD -- still sees it and an alpha-dec-vms client does not.
# Kept regardless: the RTL's reserved __ names (errno and stdio macros expand to
# them), and the threads headers (pthread.h, sched.h, semaphore.h, threads.h --
# on OpenVMS those come from the separate threads RTL, not DECC$SHR; their
# surface is its own rung). A header whose functions are ALL outside the RTL,
# and which no other header includes (and is not an ISO C header), is refused
# whole (#error) for clients.
# Prints the counts; idempotent.
use strict;
use warnings;

my ($names, $inc) = @ARGV;
die "usage: $0 <names.txt> <include-dir>\n" unless defined $inc && -d $inc;
my %keep;
open(my $nf, '<', $names) or die "$names: $!\n";
while (<$nf>) { next if /^#/; chomp; $keep{$_} = 1 if length; }
close $nf;
my %skipfile = map { $_ => 1 } qw(pthread.h sched.h semaphore.h threads.h);
# ISO C standard headers are never refused whole (a C++ runtime includes them
# unconditionally); their unexported functions are still hidden.
my %isoc = map { $_ => 1 } qw(assert.h complex.h ctype.h errno.h fenv.h float.h
    inttypes.h iso646.h limits.h locale.h math.h setjmp.h signal.h stdalign.h
    stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h
    stdnoreturn.h string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h);
my %kw = map { $_ => 1 } qw(void int char short long unsigned signed float double
    const volatile struct union enum restrict inline static extern _Noreturn);
my $guard = '#if !defined(__VMS) || defined(__OVMX_LIBC_BUILD) /* vms-fe03: not in DECC$SHR */';
my ($hidden, $files, $refused) = (0, 0, 0);
my @all = (sort glob("$inc/*.h"), glob("$inc/*/*.h"));
# Headers another public header includes: never refused whole (sys/types.h pulls
# in sys/select.h, ...), only filtered.
my %included;
for my $f (@all) {
    open(my $fh, '<', $f) or die "$f: $!\n";
    while (<$fh>) { $included{$1} = 1 if /^\s*#\s*include\s*<([^>]+)>/; }
    close $fh;
}
for my $f (@all) {
    (my $base = $f) =~ s{.*/}{};
    next if $skipfile{$base} && $f eq "$inc/$base";
    open(my $fh, '<', $f) or die "$f: $!\n";
    my @in = <$fh>;
    close $fh;
    next if grep { index($_, 'vms-fe03') >= 0 } @in;    # already filtered
    my @out;
    my ($n, $kept) = (0, 0);
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
            $kept++;
        }
        push @out, $l;
    }
    next unless $n;
    # A header ALL of whose functions are outside the DEC C RTL (and that no
    # other header includes) is not part of it at all: an alpha-dec-vms client
    # that includes it gets an error, as it would on OpenVMS, so a configure
    # header check (HAVE_SYS_PRCTL_H ...) sees it absent rather than present
    # with nothing callable behind it.
    (my $rel = $f) =~ s{^\Q$inc\E/}{};
    if ($kept == 0 && !$included{$rel} && !$isoc{$rel}) {
        unshift @out, "#if defined(__VMS) && !defined(__OVMX_LIBC_BUILD) /* vms-fe03 */\n",
                      "#error \"<$rel> is not part of the DEC C RTL (no DECC\$SHR entry point)\"\n",
                      "#endif\n";
        $refused++;
    }
    open(my $oh, '>', $f) or die "$f: $!\n";
    print $oh @out;
    close $oh;
    $hidden += $n;
    $files++;
}
print "filter-headers: hid $hidden declaration(s) in $files header(s) from alpha-dec-vms clients; $refused header(s) refused whole\n";
