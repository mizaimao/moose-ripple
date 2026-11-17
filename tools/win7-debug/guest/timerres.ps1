# timerres.ps1 [SECONDS]: how much of the time Windows' timer runs fine (under 1 ms), sampled
# every 20 ms, and how often it changed. Run in the guest; prints one line.
param([int]$seconds = 6)
Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class RbTimer { [DllImport("ntdll.dll")] public static extern int NtQueryTimerResolution(out uint min, out uint max, out uint cur); }'
$min = 0; $max = 0; $cur = 0; $fine = 0; $n = 0; $changes = 0; $last = -1
$end = (Get-Date).AddSeconds($seconds)
while ((Get-Date) -lt $end) {
    [void][RbTimer]::NtQueryTimerResolution([ref]$min, [ref]$max, [ref]$cur)
    $f = [int]($cur -lt 10000)
    if ($last -ge 0 -and $f -ne $last) { $changes++ }
    $last = $f; $fine += $f; $n++
    [System.Threading.Thread]::Sleep(20)
}
"timer fine in {0} of {1} samples ({2}%), {3} changes, now {4} (100 ns units)" -f $fine, $n, [int](100 * $fine / $n), $changes, $cur
