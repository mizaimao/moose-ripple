@echo off
setlocal
cd /d C:\rb\pkg
call get.cmd d3d9upload32.exe
copy /y d3d9upload32.exe C:\rb\d3d9upload32.exe >nul
C:\rb\d3d9upload32.exe > d3d9upload32.txt 2>&1
echo exit=%ERRORLEVEL% >> d3d9upload32.txt
call put.cmd d3d9upload32.txt
endlocal
