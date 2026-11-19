@echo off
setlocal enabledelayedexpansion
rem msaa [SAMPLES [full]]: does multisampling smooth edges (tools/win7-hello/d3d9msaa.c)? Both process
rem sizes, with the driver log for what the host offers. Sends msaa.txt and umd-msaa.log.
cd /d C:\rb\pkg
call get.cmd d3d9msaa.exe d3d9msaa32.exe
copy /y d3d9msaa.exe C:\rb\d3d9msaa.exe >nul
copy /y d3d9msaa32.exe C:\rb\d3d9msaa32.exe >nul
del msaa.txt umd-msaa.log 2>nul
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-msaa.log
for %%p in (d3d9msaa.exe d3d9msaa32.exe) do (echo %%p %*>> msaa.txt & C:\rb\%%p %* >> msaa.txt 2>&1 & echo exit=!errorlevel!>> msaa.txt)
set RIPPLE_UMD_LOG=
call put.cmd msaa.txt umd-msaa.log
