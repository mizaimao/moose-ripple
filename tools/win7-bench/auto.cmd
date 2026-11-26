@echo off
rem Run by the logon task that setup.cmd makes. With go.txt next to this file naming a run
rem (one word), it does that run once: the name is noted on the data disk R: first, so a
rem restart after a crash does not start the run again. Then 3DMark06's default run with a
rem time cap, the result to the first serial port and to R:, and a shutdown.
setlocal
if not exist "%~dp0go.txt" exit /b
set /p label=<"%~dp0go.txt"
if "%label%"=="" exit /b
if not exist R:\ (echo AUTO no data disk, nothing run >COM1 & exit /b)
if exist "R:\run-%label%.txt" (echo AUTO %label% was started before, not again >COM1 & exit /b)
echo started %date% %time% >"R:\run-%label%.txt"
echo AUTO_BEGIN %label% %date% %time% >COM1
ping -n 41 127.0.0.1 >nul
wmic path Win32_VideoController get Name,DriverVersion,Status,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate /format:list >"%TEMP%\auto-video.txt" 2>&1
type "%TEMP%\auto-video.txt" >COM1
type "%TEMP%\auto-video.txt" >>"R:\run-%label%.txt"
wmic cpu get Name,NumberOfCores /format:list >"%TEMP%\auto-cpu.txt" 2>&1
type "%TEMP%\auto-cpu.txt" >COM1
wmic computersystem get TotalPhysicalMemory /format:list >"%TEMP%\auto-mem.txt" 2>&1
type "%TEMP%\auto-mem.txt" >COM1
"%~dp0keep-awake.exe" >nul 2>&1
powercfg -change -monitor-timeout-ac 0
powercfg -change -standby-timeout-ac 0
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp03dmfull.ps1" >"R:\run-%label%-3dm.txt" 2>&1
echo AUTO launcher exit %errorlevel% >COM1
type "R:\run-%label%-3dm.txt" >COM1
echo AUTO_END %label% %date% %time% >COM1
shutdown /s /t 15
