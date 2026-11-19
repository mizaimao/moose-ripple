@echo off
rem gammahold MS: half brightness for MS milliseconds (fadeprobe), result sent back.
cd /d C:\rb\pkg
call get.cmd fadeprobe.exe
copy /y fadeprobe.exe C:\rb\fadeprobe.exe >nul
C:\rb\fadeprobe.exe 500 %1 > gammahold.txt 2>&1
echo exit=%ERRORLEVEL% >> gammahold.txt
call put.cmd gammahold.txt
