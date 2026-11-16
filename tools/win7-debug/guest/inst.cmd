@echo off
rem inst: fetch the user-mode driver and put it in place; the compositor, which has it loaded,
rem is stopped around the change. Run elevated.
cd /d C:\rb\pkg
call get.cmd viogpu_d3d10.dll
net stop uxsms >nul
ren C:\Windows\System32\viogpu_d3d10.dll viogpu_d3d10.%random%.old
copy /y viogpu_d3d10.dll C:\Windows\System32\ >nul
del C:\Windows\System32\viogpu_d3d10.*.old 2>nul
net start uxsms >nul
