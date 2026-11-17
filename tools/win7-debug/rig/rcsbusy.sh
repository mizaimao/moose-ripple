#!/bin/sh
# Run on the VM host, as root. MIT licence (LICENSE at the repository root).
# rcsbusy.sh SECONDS: share of time the Intel GPU's render engine was executing requests over
# SECONDS, from the i915 driver's own counter (the PMU event i915/rcs0-busy/), read with the
# perf_event_open system call from perl, so that neither perf nor intel_gpu_top is needed.
# gpubusy.sh says how long the GPU was awake; this says how long it was working.
set -eu
exec perl -e '
my $secs = shift // 5;
sub slurp { open(my $f, "<", $_[0]) or die "$_[0]: $!"; my $v = <$f>; chomp $v; $v }
my $dev = "/sys/bus/event_source/devices/i915";
my $type = slurp("$dev/type");
my ($config) = slurp("$dev/events/rcs0-busy") =~ /config=(\S+)/;
my $cpu = (split /[,-]/, slurp("$dev/cpumask"))[0];
my $attr = pack("L L Q", $type, 64, hex($config)) . ("\0" x 48);
my $fd = syscall(298, $attr, -1, $cpu + 0, -1, 0);   # numbers, or perl passes a pointer
die "perf_event_open: $!" if $fd < 0;
open(my $ev, "<&=", $fd) or die;
sub count { my $b; seek($ev, 0, 0); sysread($ev, $b, 8) == 8 or die "read: $!"; unpack("Q", $b) }
my $a = count(); select(undef, undef, undef, $secs); my $b = count();
printf "render engine busy %.0f%% of %g s\n", ($b - $a) / ($secs * 1e9) * 100, $secs;
' "${1:-5}"
