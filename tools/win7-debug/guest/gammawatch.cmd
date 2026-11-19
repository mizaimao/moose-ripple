@echo off
rem gammawatch arm: watch the display's gamma ramp through the next logon (a task at logon),
rem then restart. gammawatch fetch: remove the task and send the record back. Elevated.
cd /d C:\rb\pkg
if /i "%1"=="fetch" goto fetch
call get.cmd gammawatch.exe
copy /y gammawatch.exe C:\rb\gammawatch.exe >nul
del "%TEMP%\gammawatch.txt" 2>nul
schtasks /create /tn RippleGammaWatch /tr C:\rb\gammawatch.exe /sc onlogon /it /f > gammawatch-arm.txt 2>&1
call put.cmd gammawatch-arm.txt
shutdown /r /t 0
goto :eof
:fetch
schtasks /delete /tn RippleGammaWatch /f > gammawatch-fetch.txt 2>&1
copy /y "%TEMP%\gammawatch.txt" C:\rb\pkg\ >> gammawatch-fetch.txt 2>&1
call put.cmd gammawatch-fetch.txt gammawatch.txt
