$ErrorActionPreference='Stop'
$f = Get-ChildItem "$env:windir\Performance\WinSAT\DataStore\*Formal*.xml" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
[xml]$doc = [System.IO.File]::ReadAllText($f.FullName)
$f.Name
$doc.WinSAT.WinSPR.OuterXml
$doc.WinSAT.Metrics.GraphicsMetrics.OuterXml
$doc.WinSAT.Metrics.VideoMetrics.OuterXml
$doc.WinSAT.SystemConfig.Graphics.OuterXml
$doc.WinSAT.SystemConfig.Processor.OuterXml
