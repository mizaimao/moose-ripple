# A screenshot from inside the guest, after a delay, sent to a receiver on the build host
# (an HTTP server that stores the body of a PUT under the name in the URL).
#   powershell -ExecutionPolicy Bypass -File shot.ps1 URL NAME [SECONDS]
# For a guest whose picture the host cannot see: a graphics card passed through, where QEMU has
# no display of its own to capture (docs/windows/test-vms.md, "Reference pictures"). With the
# delay, several can be started hidden at once and the scenes set up from outside meanwhile:
#   start /min powershell -WindowStyle Hidden -ExecutionPolicy Bypass -File shot.ps1 URL flip3d 54
param([string]$url, [string]$name = "shot", [int]$delay = 0)
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Start-Sleep -Seconds $delay
$s = [Windows.Forms.Screen]::PrimaryScreen.Bounds
$b = New-Object Drawing.Bitmap $s.Width,$s.Height
$g = [Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen(0,0,0,0,$b.Size)
$f = "$env:TEMP\$name.png"
$b.Save($f)
(New-Object Net.WebClient).UploadData("$url/$name.png","PUT",[IO.File]::ReadAllBytes($f)) | Out-Null
