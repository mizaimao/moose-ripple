# Windows 7 PowerShell. Run elevated from the account used for the test logon.
param([Parameter(Mandatory=$true)][ValidateSet('arm','stop')][string]$Mode)
$ErrorActionPreference = 'Stop'
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$watch = Join-Path $dir 'desktopactive.exe'
$etl = Join-Path $dir 'welcome.etl'
$name = 'RippleWelcomeTiming'
$guid = '{48179E06-C364-423A-B28C-BD4C47856FA6}'
$key = "HKLM:\SYSTEM\CurrentControlSet\Control\WMI\Autologger\$name"
$scheduler = New-Object -ComObject Schedule.Service
$scheduler.Connect()
$folder = $scheduler.GetFolder('\')
$task = @($folder.GetTasks(1) | Where-Object { $_.Name -eq $name })

if ($Mode -eq 'stop') {
    $settings = Get-ItemProperty -LiteralPath $key
    if ($settings.Guid -ne $guid -or $settings.FileName -ne $etl) { throw 'Unexpected AutoLogger settings' }
    if ($task.Count -and $task[0].Definition.Actions.Item(1).Path -ne $watch) { throw 'Unexpected task action' }
    Set-ItemProperty -LiteralPath $key -Name Start -Value 0
    logman stop $name -ets
    $code = $LASTEXITCODE
    if ($task.Count) { $folder.DeleteTask($name, 0) }
    Remove-Item -LiteralPath $key -Recurse
    if ($code) { throw "AutoLogger configuration removed. logman stop returned $code" }
    return
}

if (!(Test-Path -LiteralPath $watch -PathType Leaf)) { throw "Missing $watch" }
if ((Test-Path -LiteralPath $key) -or $task.Count -or (Test-Path -LiteralPath $etl)) {
    throw 'The session, task or output already exists'
}
$null = New-Item $key
$registered = $false
try {
    New-ItemProperty $key Guid -PropertyType String -Value $guid | Out-Null
    New-ItemProperty $key FileName -PropertyType String -Value $etl | Out-Null
    # Sequential ETL, maximum 128 MiB, 256 KiB buffers, performance-counter clock.
    $options = @{LogFileMode=1; MaximumFileSize=128; BufferSize=256; ClockType=1}
    foreach ($option in $options.Keys) {
        New-ItemProperty $key $option -PropertyType DWord -Value $options[$option] | Out-Null
    }
    $providers = @{
        '{6C38BBA1-1768-48C6-9C07-40DADBA4B38F}' = 0     # UI probe
        '{8C9DD1AD-E6E5-4B07-B455-684A9D879900}' = 1     # Dwm-Core
        '{98583AF0-FC93-4E71-96D5-9F8DA716C6B8}' = 1     # Dwm-Udwm
        '{92AE46D7-6D9C-4727-9ED5-E49AF9C24CBF}' = 1     # Dwm-Api
        '{65CD4C8A-0848-4583-92A0-31C0FBAF00C0}' = 65535 # Windows 7 graphics kernel
    }
    foreach ($id in $providers.Keys) {
        $provider = New-Item "$key\$id"
        New-ItemProperty $provider.PSPath Enabled -PropertyType DWord -Value 1 | Out-Null
        New-ItemProperty $provider.PSPath EnableLevel -PropertyType DWord -Value 5 | Out-Null
        New-ItemProperty $provider.PSPath MatchAnyKeyword -PropertyType QWord -Value ([long]$providers[$id]) | Out-Null
    }
    $sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $def = $scheduler.NewTask(0)
    $def.Principal.UserId = $sid
    $def.Principal.LogonType = 3 # interactive token
    $def.Principal.RunLevel = 0
    $def.Settings.ExecutionTimeLimit = 'PT1M'
    $def.Settings.DisallowStartIfOnBatteries = $false
    $trigger = $def.Triggers.Create(9) # logon
    $trigger.UserId = $sid
    $action = $def.Actions.Create(0)
    $action.Path = $watch
    $null = $folder.RegisterTaskDefinition($name, $def, 2, $sid, $null, 3, $null)
    $registered = $true
    New-ItemProperty $key Start -PropertyType DWord -Value 1 | Out-Null
} catch {
    if ($registered) { $folder.DeleteTask($name, 0) }
    Remove-Item -LiteralPath $key -Recurse
    throw
}
