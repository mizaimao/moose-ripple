@echo off
rem wei: Windows' full assessment (the Windows Experience Index) with the driver's log, and the
rem results sent to the build host: winsat-formal.txt, scores.txt, the driver's log, a crash dump if any.
cd /d C:\rb\pkg
call prep.cmd
del /q C:\rb\dumps\*.dmp 2>nul
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-formal.log
set RIPPLE_UMD_TRACE=1
del umd-formal.log winsat-formal.txt scores.txt crash.dmp 2>nul
winsat formal -v > winsat-formal.txt 2>&1
echo exit code %errorlevel% >> winsat-formal.txt
wmic path Win32_WinSAT get CPUScore,D3DScore,DiskScore,GraphicsScore,MemoryScore,WinSPRLevel,WinSATAssessmentState /format:list > scores.txt 2>&1
for %%f in (C:\rb\dumps\*.dmp) do copy /y %%f C:\rb\pkg\crash.dmp >nul
call put.cmd winsat-formal.txt scores.txt umd-formal.log crash.dmp
echo done
