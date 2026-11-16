@echo off
rem chk: after a fresh install, the 32-bit and 64-bit checks and what is on the disk
cd /d C:\rb\pkg
call get.cmd prep.cmd wei.cmd ddprobe32.exe d3d9test32.exe d3d9test.exe
call prep.cmd
del ddprobe.txt d3d9test.txt d3d9test32.txt dxdiag.txt progs.txt umd-chk.log 2>nul
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-chk.log
set RIPPLE_UMD_TRACE=1
start /wait ddprobe32.exe
start /wait d3d9test32.exe 2
ren d3d9test.txt d3d9test32.txt
start /wait d3d9test.exe 2
dir /b "C:\Program Files (x86)" > progs.txt
dir /b "C:\Program Files" >> progs.txt
dir /b C:\Windows\SysWOW64\viogpu_d3d10.dll C:\Windows\System32\viogpu_d3d10.dll >> progs.txt
start /wait dxdiag /t C:\rb\pkg\dxdiag.txt
:w
if not exist dxdiag.txt (ping -n 3 127.0.0.1 >nul & goto w)
ping -n 3 127.0.0.1 >nul
call put.cmd ddprobe.txt d3d9test32.txt d3d9test.txt dxdiag.txt progs.txt umd-chk.log
call wei.cmd
