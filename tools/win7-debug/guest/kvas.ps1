$sig = @"
using System;
using System.Runtime.InteropServices;
public static class Nt {
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int cls, ref uint info, int len, out int ret);
}
"@
Add-Type -TypeDefinition $sig
foreach ($c in 196, 201) {
    $v = [uint32]0
    $r = 0
    $st = [Nt]::NtQuerySystemInformation($c, [ref]$v, 4, [ref]$r)
    "class {0}: status 0x{1:x8} value 0x{2:x8} len {3}" -f $c, $st, $v, $r
}
