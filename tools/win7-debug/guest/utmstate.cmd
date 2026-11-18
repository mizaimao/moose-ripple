@echo off
setlocal
cd /d C:\rb\pkg
(
  echo Windows 7 UTM state
  echo %DATE% %TIME%
  tasklist /fi "imagename eq dwm.exe"
  tasklist /fi "imagename eq explorer.exe"
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /f RIPPLE
  reg query "HKCU\Environment" /f RIPPLE
  reg query "HKCU\Control Panel\Desktop\WindowMetrics" /v MinAnimate
  reg query "HKCU\Software\Microsoft\Windows\DWM"
  certutil -hashfile C:\Windows\System32\viogpu_d3d10.dll SHA256
  certutil -hashfile C:\Windows\SysWOW64\viogpu_d3d10.dll SHA256
  certutil -hashfile C:\Windows\System32\drivers\viogpu3d.sys SHA256
  wevtutil qe Application /c:6 /rd:true /f:text /q:"*[System[Provider[@Name='Application Error'] and (EventID=1000)]]"
  dir /o:d C:\rb\dumps
) > utmstate.txt 2>&1
call put.cmd utmstate.txt
endlocal
