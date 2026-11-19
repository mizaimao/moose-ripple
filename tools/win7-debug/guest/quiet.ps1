$start = Get-Date
$deadline = $start.AddMinutes(12)
$calm = 0
while ((Get-Date) -lt $deadline -and $calm -lt 3) {
    $busy = @(Get-WmiObject Win32_PerfFormattedData_PerfProc_Process |
        Where-Object { $_.Name -notmatch "^(_Total|Idle|powershell|WmiPrvSE)" -and $_.PercentProcessorTime -ge 15 })
    if ($busy.Count) {
        $calm = 0
        "{0,4:N0} s busy: {1}" -f ((Get-Date) - $start).TotalSeconds, (($busy | ForEach-Object { $_.Name + " " + $_.PercentProcessorTime }) -join ", ")
    } else {
        $calm++
    }
    Start-Sleep -Seconds 4
}
"quiet after {0:N0} s" -f ((Get-Date) - $start).TotalSeconds
