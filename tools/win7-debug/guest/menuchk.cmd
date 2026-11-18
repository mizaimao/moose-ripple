@echo off
setlocal
cd /d C:\rb\pkg
call get.cmd shellmenu.exe
copy /y shellmenu.exe C:\rb\shellmenu.exe >nul
C:\rb\shellmenu.exe 5 > shellmenu.txt 2>&1
echo exit=%ERRORLEVEL% >> shellmenu.txt
call put.cmd shellmenu.txt
call rb.cmd utmstate
endlocal
