# 3DMark06's default run from start to score, for a like-for-like comparison of two display
# adapters: start it, press Run, mark each change of the full-screen size on the first serial
# port (with the guest's clock), and when the score window is up print what it and its Details
# window say. The whole run is capped; bench-go.exe, bench-observe.exe, bench-score.exe and
# bench-press.exe are next to this file.
param([int]$CapMinutes = 50)
$pkg = Split-Path -Parent $MyInvocation.MyCommand.Path
$gameDir = 'C:\Program Files (x86)\Futuremark\3DMark06'
function Mark($s) {
    $line = 'MARK ' + (Get-Date).ToString('HH:mm:ss') + ' ' + $s
    & cmd /d /c ("echo " + $line + " >COM1")
    Write-Output $line
}
if (!(Test-Path (Join-Path $gameDir '3DMark06.exe'))) { Mark 'not-installed'; exit 8 }
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = Join-Path $gameDir '3DMark06.exe'
$start.WorkingDirectory = $gameDir
$start.UseShellExecute = $false
$game = [Diagnostics.Process]::Start($start)
Mark ('started-' + $game.Id)
# With key.txt next to this file (a registration code, one line) the program is registered
# first, through its own dialogs, and started again; the code is never printed.
if (Test-Path "$pkg\key.txt") {
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 3
        $seen = @(& "$pkg\bench-go.exe")
        if ($seen -match "Please Register|3DMark06 - ") { break }
    }
    if ($seen -match "Please Register") {
        $reg = @(& "$pkg\bench-reg.exe" "$pkg\key.txt")
        $rc = $LASTEXITCODE
        Write-Output ($reg -join "`r`n")
        Mark ('register-' + $rc)
        Start-Sleep -Seconds 3
        try { $game.Kill() } catch {}
        Start-Sleep -Seconds 4
        $game = [Diagnostics.Process]::Start($start)
        Mark ('started-again-' + $game.Id)
    } else { Mark 'no-register-dialog' }
}
$clicked = $false
$answers = 0
for ($i = 0; $i -lt 200; $i++) {
    Start-Sleep -Seconds 3
    if ($game.HasExited) { Mark 'exited-before-run'; exit 2 }
    if ($i -lt 6) { continue }
    if ($answers -ge 3) { $ui = @(& "$pkg\bench-go.exe" run alt) } else { $ui = @(& "$pkg\bench-go.exe" run) }
    $rc = $LASTEXITCODE
    if ($rc -eq 3) { $answers++; Write-Output ($ui -join "`r`n"); Mark ('answered-' + $answers) }
    if ($rc -eq 0) { $clicked = $true; Write-Output ($ui -join "`r`n"); Mark 'run-clicked'; break }
}
if (!$clicked) { Mark 'run-unavailable'; Write-Output ($ui -join "`r`n"); try { $game.Kill() } catch {}; exit 3 }
$deadline = [DateTime]::UtcNow.AddMinutes($CapMinutes)
$last = ''
$seenFull = $false
while ([DateTime]::UtcNow -lt $deadline) {
    if ($game.HasExited) { Mark ('exited-' + $game.ExitCode); exit 4 }
    $windows = @(& "$pkg\bench-observe.exe")
    $size = 'none'
    $score = $false
    foreach ($line in $windows) {
        if ($line -match "window '3DMark06' .* at 0,0 ([0-9]+x[0-9]+)") { $size = $Matches[1] }
        if ($line -match "window '[^']*(Score|Result)[^']*'") { $score = $true }
    }
    if ($size -ne $last) { Mark ('fullscreen-' + $size); $last = $size; if ($size -ne 'none') { $seenFull = $true } }
    if ($windows -match 'error occurred|E_FAIL|benchmark aborted') { Mark 'benchmark-error'; Write-Output ($windows -join "`r`n"); try { $game.Kill() } catch {}; exit 6 }
    if ($score -and $seenFull) {
        Mark 'score-window'
        Start-Sleep -Seconds 3
        Write-Output '== score window'
        Write-Output ($windows -join "`r`n")
        Write-Output (@(& "$pkg\bench-score.exe") -join "`r`n")
        $press = @(& "$pkg\bench-press.exe" 'Details')
        Write-Output ($press -join "`r`n")
        Start-Sleep -Seconds 4
        Write-Output '== details'
        Write-Output (@(& "$pkg\bench-score.exe") -join "`r`n")
        Mark 'done'
        try { $game.Kill() } catch {}
        exit 0
    }
    Start-Sleep -Seconds 4
}
Mark 'timeout'
Write-Output ($windows -join "`r`n")
try { $game.Kill() } catch {}
exit 7
