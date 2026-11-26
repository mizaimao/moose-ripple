#!/bin/sh
# Run on the VM host, as root; tracing is switched off again at the end and the trace buffer
# gets its size back. MIT licence (LICENSE at the repository root).
# maintrace.sh TID SECONDS [LABEL]: what one thread does over SECONDS, from the kernel's own
# trace events for that thread only: its share of the time on a processor (scheduler
# switches), and its system calls by kind: how many, how long in all, and for ioctl the
# request codes. For QEMU's main thread (the process id of a QEMU started by refvm.sh), where
# virglrenderer draws: is it computing, or asking the kernel for something, and what.
set -eu
T=/sys/kernel/tracing
tid=$1; secs=$2; label=${3:-}
out=$(mktemp /tmp/maintrace.XXXXXX)
old=$(cat $T/buffer_size_kb | sed 's/ .*//')
E="raw_syscalls/sys_enter raw_syscalls/sys_exit sched/sched_switch"
echo 0 >$T/tracing_on; echo >$T/trace
echo 16384 >$T/buffer_size_kb
echo "$tid" >$T/set_event_pid
for e in $E; do echo 1 >$T/events/$e/enable; done
echo 1 >$T/tracing_on; sleep "$secs"; echo 0 >$T/tracing_on
for e in $E; do echo 0 >$T/events/$e/enable; done
echo >$T/set_event_pid
cat $T/trace >"$out"; echo >$T/trace
echo "$old" >$T/buffer_size_kb
perl -e '
my ($tid, $secs, $label, $file) = @ARGV;
my %name = (0,"read",1,"write",3,"close",7,"poll",16,"ioctl",19,"readv",20,"writev",23,"select",
            39,"getpid",46,"sendmsg",47,"recvmsg",202,"futex",228,"clock_gettime",232,"epoll_wait",
            257,"openat",271,"ppoll",281,"epoll_pwait",284,"eventfd",290,"eventfd2",17,"pread64",18,"pwrite64");
my (%n, %t, %start, %ioctl, %ioctl_t, $req, $on, $t_on, $first, $last, $switches, $lost);
open(my $f, "<", $file) or die;
while (<$f>) {
    $lost++ if /LOST|overrun/ && /^#|CPU:/;
    next unless /\s(\d+\.\d+): (\w+): (.*)/;
    my ($ts, $ev, $rest) = ($1, $2, $3);
    $first //= $ts; $last = $ts;
    if ($ev eq "sched_switch") {
        if ($rest =~ /next_pid=$tid\b/) { $t_on = $ts; }
        if ($rest =~ /prev_pid=$tid\b/) { $on += $ts - ($t_on // $first); undef $t_on; $switches++; }
    } elsif ($ev eq "sys_enter" && $rest =~ /^NR (\d+) \(([0-9a-f]+), ([0-9a-f]+)/) {
        $start{$1} = $ts; if ($1 == 16) { $ioctl{$3}++; $req = $3; }
    } elsif ($ev eq "sys_exit" && $rest =~ /^NR (\d+)/) {
        if (defined $start{$1}) {
            $n{$1}++; $t{$1} += $ts - $start{$1};
            $ioctl_t{$req} += $ts - $start{$1} if $1 == 16 && defined $req;
            delete $start{$1};
        }
    }
}
$on += $last - $t_on if defined $t_on;
my $span = ($last // 0) - ($first // 0);
$span = $secs if $span <= 0;
my $sys = 0; $sys += $_ for values %t;
printf "%s: %.1f s traced, on a processor %.0f%% of the time (%d times switched out), in system calls %.0f%%\n",
       $label, $span, 100 * ($on // 0) / $span, $switches // 0, 100 * $sys / $span;
for my $k (sort { $t{$b} <=> $t{$a} } keys %t) {
    printf "  %-14s %7d calls (%6.0f a second), %7.1f ms in all, %6.1f us each\n", $name{$k} // "nr $k",
           $n{$k}, $n{$k} / $span, 1000 * $t{$k}, 1e6 * $t{$k} / $n{$k};
}
my @c = sort { $ioctl_t{$b} <=> $ioctl_t{$a} } keys %ioctl;
printf "  ioctl requests by time: %s\n", join(", ", map { sprintf("0x%s x%d %.0f ms", $_, $ioctl{$_}, 1000 * ($ioctl_t{$_} // 0)) } @c[0 .. ($#c < 7 ? $#c : 7)]) if @c;
print "  (trace events were lost: the numbers are low)\n" if $lost;
' "$tid" "$secs" "$label" "$out"
rm -f "$out"
