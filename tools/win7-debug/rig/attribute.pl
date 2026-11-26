#!/usr/bin/perl
# attribute.pl SAMPLES MODS-PROCESS MODS-KERNEL [NAME=SYMBOLS ...]
# Where a guest's processors were, from ripstream.sh's lines ("RIP=.. CPL=.. CR3=.."): each
# sample is given to a kernel module (modlist.exe: "kernel BASE NAME") or, in user mode, to a
# module of the watched process (modlist.exe PID: "module BASE SIZE NAME"); for a module named
# with NAME=SYMBOLS (a file of "RVA-in-hex function" lines, sorted) also to a function.
# Shares are of all samples; with four processors 25% is one processor busy all the time.
# MIT licence (LICENSE at the repository root).
use strict; use warnings; no warnings "portable";
my ($samples, $pmods, $kmods, @syms) = @ARGV;
my (@k, @p, %sym);
open(my $f, "<", $kmods) or die "$kmods: $!";
while (<$f>) { push @k, [hex($1), $2] if /^kernel\s+(?:0x)?([0-9A-Fa-f]+)\s+(\S+)/; }
close $f;
@k = sort { $a->[0] <=> $b->[0] } @k;
open($f, "<", $pmods) or die "$pmods: $!";
while (<$f>) { push @p, [hex($1), hex($2), $3] if /^module\s+(?:0x)?([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s+(\S+)/; }
close $f;
for my $s (@syms) {
    my ($name, $file) = split /=/, $s, 2;
    open(my $g, "<", $file) or die "$file: $!";
    my @t;
    while (<$g>) { push @t, [hex($1), $2] if /^([0-9A-Fa-f]+)\s+(\S+)/; }
    close $g;
    $sym{lc $name} = [sort { $a->[0] <=> $b->[0] } @t];
}
sub below { my ($t, $v) = @_; my ($lo, $hi) = (0, $#$t); return undef if !@$t || $t->[0][0] > $v;
            while ($lo < $hi) { my $m = int(($lo + $hi + 1) / 2); if ($t->[$m][0] <= $v) { $lo = $m } else { $hi = $m - 1 } } $t->[$lo] }
my (%cr3, @rows, $n);
# The tokens come in the order instruction pointer, privilege level, page table base for each
# processor; a sample without a pointer was taken in 32-bit code by a sampler that only knew RIP.
open($f, "<", $samples) or die "$samples: $!";
my ($ip, $cpl, $nopointer);
while (<$f>) {
    for my $tok (split) {
        if ($tok =~ /^[RE]IP=([0-9a-f]+)/) { $ip = hex($1); }
        elsif ($tok =~ /^CPL=([0-3])/) { $cpl = $1; }
        elsif ($tok =~ /^CR3=([0-9a-f]+)/) {
            $n++;
            if (defined $ip) {
                push @rows, [$ip, $cpl // 0, $1];
                if (($cpl // 0) == 3) { for my $m (@p) { if ($ip >= $m->[0] && $ip < $m->[0] + $m->[1]) { $cr3{$1}++; last } } }
            } else { $nopointer++; }
            undef $ip; undef $cpl;
        }
    }
}
close $f;
die "no samples\n" unless $n;
my ($game) = sort { $cr3{$b} <=> $cr3{$a} } keys %cr3;
my (%mod, %fn, %other);
for my $r (@rows) {
    my ($rip, $cpl, $c) = @$r;
    if ($cpl != 3) {
        my $m = below(\@k, $rip);
        my $name = $m ? lc $m->[1] : "kernel ?";
        $name = "idle (hal, intelppm)" if $name =~ /^(hal\.dll|intelppm\.sys|amdppm\.sys)$/;
        $mod{$name}++;
        if ($m && $sym{lc $m->[1]}) { my $s = below($sym{lc $m->[1]}, $rip - $m->[0]); $fn{lc($m->[1]) . "!" . ($s ? $s->[1] : "?")}++; }
    } elsif (defined $game && $c eq $game) {
        my $hit;
        for my $m (@p) { if ($rip >= $m->[0] && $rip < $m->[0] + $m->[1]) { $hit = $m; last } }
        my $name = $hit ? lc $hit->[2] : "program, outside its listed modules";
        $mod{"program: $name"}++;
        if ($hit && $sym{lc $hit->[2]}) { my $s = below($sym{lc $hit->[2]}, $rip - $hit->[0]); $fn{lc($hit->[2]) . "!" . ($s ? $s->[1] : "?")}++; }
    } else {
        $mod{"other programs, user mode"}++;
    }
}
printf "%d processor samples; the program's page table base %s\n", $n, $game // "?";
printf "  %5.1f%%  32-bit user code, address not recorded\n", 100 * $nopointer / $n if $nopointer;
my $shown = 0;
for my $m (sort { $mod{$b} <=> $mod{$a} } keys %mod) { last if $shown++ >= 16; printf "  %5.1f%%  %s\n", 100 * $mod{$m} / $n, $m; }
if (%fn) { print "functions:\n"; $shown = 0;
    for my $m (sort { $fn{$b} <=> $fn{$a} } keys %fn) { last if $shown++ >= 25; printf "  %5.2f%%  %s\n", 100 * $fn{$m} / $n, $m; } }
