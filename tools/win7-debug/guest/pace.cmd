@echo off
rem pace TAG [ROUNDS] [cpu]: what paces the compositor while a window is dragged and resized. Records the
rem compositor (DWM-Core) and the probe marks around the standard motion workload; with cpu, a sampled
rem processor profile too. Sends TAG-samples.txt and TAG-trace.xml (and TAG-cpu.xml).
setlocal
cd /d C:\rb\pkg
set tag=%1
set rounds=%2
if "%rounds%"=="" set rounds=3
call get.cmd dragtest.exe
>paceprov.txt echo {8c9dd1ad-e6e5-4b07-b455-684a9d879900} 0xffffffffffffffff 5
>>paceprov.txt echo {6c38bba1-1768-48c6-9c07-40dadba4b38f} 0xffffffffffffffff 5
del pace.etl pacecpu.etl %tag%-trace.xml %tag%-cpu.xml %tag%-status.txt 2>nul
if "%3"=="cpu" logman start "NT Kernel Logger" -ets -o pacecpu.etl -ow -bs 128 -nb 20 120 -p "{9e814aad-3204-11d2-9a82-006008a86939}" 0x01000007 >> %tag%-status.txt 2>&1
logman start RipplePace -ets -o pace.etl -ow -bs 512 -nb 32 256 -pf paceprov.txt >> %tag%-status.txt 2>&1
dragtest.exe %rounds% motion > %tag%-samples.txt 2>&1
>>%tag%-status.txt echo probe=%errorlevel%
logman stop RipplePace -ets >> %tag%-status.txt 2>&1
if "%3"=="cpu" logman stop "NT Kernel Logger" -ets >> %tag%-status.txt 2>&1
call put.cmd %tag%-status.txt %tag%-samples.txt
tracerpt pace.etl -o %tag%-trace.xml -of XML -y >> %tag%-status.txt 2>&1
call put.cmd %tag%-trace.xml
if "%3"=="cpu" tracerpt pacecpu.etl -o %tag%-cpu.xml -of XML -y >> %tag%-status.txt 2>&1
if "%3"=="cpu" call put.cmd %tag%-cpu.xml
call put.cmd %tag%-status.txt
