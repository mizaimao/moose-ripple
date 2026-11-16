#!/bin/sh
# Run on the VM host, as root. MIT licence (LICENSE at the repository root).
# threads.sh VMID SECONDS: CPU use of each thread of the VM's QEMU over SECONDS, busiest first
# (CPU_n/KVM: the guest's processors; kvm: QEMU's main thread, where virglrenderer draws)
p=$(cat /var/run/qemu-server/$1.pid); t=$(getconf CLK_TCK)
snap() { for d in /proc/$p/task/*; do echo "$(basename $d) $(cat $d/comm | tr ' ' _) $(awk '{print $14+$15}' $d/stat)"; done; }
snap > /tmp/rb-th-a; sleep $2; snap > /tmp/rb-th-b
join /tmp/rb-th-a /tmp/rb-th-b | awk -v t=$t -v s=$2 '{d=($5-$3)*100/t/s; if (d>=2) printf "%5.0f%%  %s\n", d, $2}' | sort -rn | head -12
rm -f /tmp/rb-th-a /tmp/rb-th-b
