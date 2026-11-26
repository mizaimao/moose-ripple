#!/usr/bin/perl
# placeips.pl IPS MAP1 SYMBOL1 RUNTIME1 [MAP2 SYMBOL2 RUNTIME2 ...]
# Counts sampled guest addresses by function of each driver, using its link map and the run-time
# address of one known function; addresses outside the drivers are counted by 64 KB region.
# The addresses come from the host: perf record -e cycles:G -p <qemu> -o g.data, then
# perf script -i g.data -F ip --guestkallsyms=<empty file> --guestmodules=<empty file>.
# Maps: build.py with RIPPLE_LINK_MAP=1. Run-time addresses: the D7CODE lines of d7draw, e.g.
#   placeips.pl ips rb2kdisp.dll.map _rb_d7_drawstats@12 <display> rb2kmp.sys.map _rb_gpu_frame_reserve@16 <miniport>
use strict; use warnings;
my $ipfile = shift @ARGV;
my @mods;
while (@ARGV >= 3) {
    my ($map, $known, $runtime) = splice(@ARGV, 0, 3);
    my (@syms, $knownva, $name);
    ($name = $map) =~ s{.*/}{}; $name =~ s/\.map$//;
    open my $m, '<', $map or die "$map: $!";
    while (<$m>) {
        next unless /^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{8,16})\s/i;
        my ($sym, $va) = ($1, hex($2));
        push @syms, [$va, $sym];
        $knownva = $va if $sym eq $known;
    }
    die "$known not in $map" unless defined $knownva;
    @syms = sort { $a->[0] <=> $b->[0] } @syms;
    my $delta = hex($runtime) - $knownva;
    push @mods, { name => $name, syms => \@syms, delta => $delta, lo => $syms[0][0] + $delta,
                  hi => $syms[-1][0] + $delta + 0x10000 };
}
my (%count, $total);
open my $f, '<', $ipfile or die;
while (my $ip = <$f>) {
    chomp $ip; next unless $ip =~ /^[0-9a-f]+$/i; next if length($ip) > 8;   # host addresses
    my $a = hex($ip); ++$total;
    my $placed;
    for my $mod (@mods) {
        next if $a < $mod->{lo} || $a >= $mod->{hi};
        my $va = $a - $mod->{delta}; my $best = '?';
        for my $s (@{$mod->{syms}}) { last if $s->[0] > $va; $best = $s->[1]; }
        $count{"$mod->{name} $best"}++; $placed = 1; last;
    }
    $count{sprintf("elsewhere %08x", $a & 0xffff0000)}++ unless $placed;
}
printf "%d samples\n", $total;
for my $k (sort { $count{$b} <=> $count{$a} } keys %count) {
    printf "%6.2f%%  %s\n", 100 * $count{$k} / $total, $k;
    last if $k eq (sort { $count{$b} <=> $count{$a} } keys %count)[29];
}
