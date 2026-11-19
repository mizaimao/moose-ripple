@echo off
setlocal enabledelayedexpansion
rem vidplay [FILE]: does a video show a picture? Both playback pipelines, both process sizes; with the
rem Direct3D 9 trace at level 3 for the 32-bit DirectShow run. Sends vidplay.txt and umd-vidplay.log.
cd /d C:\rb\pkg
set clip=%~1
if "%clip%"=="" set clip=C:\Users\Public\Videos\Sample Videos\Wildlife.wmv
call get.cmd vidplay.exe vidplay32.exe
copy /y vidplay.exe C:\rb\vidplay.exe >nul
copy /y vidplay32.exe C:\rb\vidplay32.exe >nul
del vidplay.txt umd-vidplay.log 2>nul
for %%m in (ds mf) do for %%p in (vidplay.exe vidplay32.exe) do (echo %%p %%m>> vidplay.txt & C:\rb\%%p %%m "%clip%" 4 >> vidplay.txt 2>&1 & echo exit=!errorlevel!>> vidplay.txt)
call put.cmd vidplay.txt
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-vidplay.log
set RIPPLE_UMD_TRACE=3
C:\rb\vidplay32.exe ds "%clip%" 3 >nul 2>&1
set RIPPLE_UMD_TRACE=
set RIPPLE_UMD_LOG=
call put.cmd umd-vidplay.log
