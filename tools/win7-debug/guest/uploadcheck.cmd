@echo off
setlocal
cd /d C:\rb\pkg
call get.cmd d3d9upload.exe
copy /y d3d9upload.exe C:\rb\d3d9upload.exe >nul
C:\rb\d3d9upload.exe > d3d9upload.txt 2>&1
echo exit=%ERRORLEVEL% >> d3d9upload.txt
call put.cmd d3d9upload.txt
endlocal
