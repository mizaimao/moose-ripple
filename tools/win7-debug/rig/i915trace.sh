#!/bin/sh
# Run on the VM host, as root; tracing is switched off again at the end.
# MIT licence (LICENSE at the repository root).
# i915trace.sh SECONDS: i915 events during SECONDS, counted by event (and engine/process), then tracing off again
T=/sys/kernel/tracing
E="i915_request_add i915_request_retire i915_request_wait_begin i915_request_wait_end i915_gem_object_create i915_gem_object_destroy i915_gem_object_pwrite i915_gem_object_pread i915_vma_bind i915_gem_object_clflush i915_gem_evict"
echo 0 > $T/tracing_on; echo > $T/trace
for e in $E; do echo 1 > $T/events/i915/$e/enable; done
echo 1 > $T/tracing_on; sleep $1; echo 0 > $T/tracing_on
for e in $E; do echo 0 > $T/events/i915/$e/enable; done
cp $T/trace /tmp/rb-i915.txt; echo > $T/trace
echo "events in $1 s:"; grep -o 'i915_[a-z_]*:' /tmp/rb-i915.txt | sort | uniq -c | sort -rn
echo "requests added by engine/context:"; grep i915_request_add /tmp/rb-i915.txt | grep -o 'engine=[0-9:]*\|ctx=[0-9]*\|class=[0-9]* instance=[0-9]*' | sort | uniq -c | sort -rn | head -8
echo "waits by process:"; grep i915_request_wait_begin /tmp/rb-i915.txt | awk '{print $1}' | sort | uniq -c | sort -rn | head -5
