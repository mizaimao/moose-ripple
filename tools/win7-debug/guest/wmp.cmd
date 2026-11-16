@echo off
rem wmp: play the sample video for about 8 s with the Direct3D 9 trace at level 3, then send the trace
cd /d C:\rb\pkg
del umd-wmp.log 2>nul
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-wmp.log
set RIPPLE_UMD_TRACE=3
start "" "C:\Program Files\Windows Media Player\wmplayer.exe" "C:\Users\Public\Videos\Sample Videos\Wildlife.wmv"
ping -n 9 127.0.0.1 >nul
taskkill /im wmplayer.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
call put.cmd umd-wmp.log
