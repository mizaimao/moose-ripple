@echo off
rem fadeprobe: time the fading desktop switch at two fade lengths and send the result back.
cd /d C:\rb\pkg
call get.cmd fadeprobe.exe
copy /y fadeprobe.exe C:\rb\fadeprobe.exe >nul
C:\rb\fadeprobe.exe 1000 > fadeprobe.txt 2>&1
echo exit=%ERRORLEVEL% >> fadeprobe.txt
ping -n 4 127.0.0.1 >nul
C:\rb\fadeprobe.exe 3000 >> fadeprobe.txt 2>&1
echo exit=%ERRORLEVEL% >> fadeprobe.txt
call put.cmd fadeprobe.txt
