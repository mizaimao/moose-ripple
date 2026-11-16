@echo off
rem kinst: replace the kernel driver in place and restart the display device
cd /d C:\rb\pkg
call get.cmd viogpu3d.sys
net stop uxsms >nul
ren C:\Windows\System32\drivers\viogpu3d.sys viogpu3d.%random%.old
copy /y viogpu3d.sys C:\Windows\System32\drivers\ >nul
devcon restart "PCI\VEN_1AF4&DEV_1050*" | findstr /c:"Restarted"
ping -n 5 127.0.0.1 >nul
net start uxsms >nul
