@echo off
rem weirun TAG: Windows full assessment without the driver trace; sends wei-TAG-run.txt (what it printed)
rem and wei-TAG-details.txt (scores, limits and video metrics from the result file).
setlocal
cd /d C:\rb\pkg
set tag=%1
set RIPPLE_UMD_TRACE=
set RIPPLE_UMD_LOG=
call get.cmd weidetails.ps1
winsat formal -v > wei-%tag%-run.txt 2>&1
>>wei-%tag%-run.txt echo exit_code=%errorlevel%
call put.cmd wei-%tag%-run.txt
powershell -NoProfile -ExecutionPolicy Bypass -File weidetails.ps1 > wei-%tag%-details.txt 2>&1
call put.cmd wei-%tag%-details.txt
