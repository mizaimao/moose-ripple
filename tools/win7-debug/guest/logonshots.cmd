@echo off
rem logonshots arm: capture the screen at the next logon (a task at logon), then restart.
rem logonshots fetch: remove the task and send back the sheet and its times. Elevated.
cd /d C:\rb\pkg
if /i "%1"=="fetch" goto fetch
call get.cmd logonshots.exe
copy /y logonshots.exe C:\rb\logonshots.exe >nul
del "%TEMP%\logonshots.bmp" "%TEMP%\logonshots.txt" 2>nul
schtasks /create /tn RippleLogonShots /tr C:\rb\logonshots.exe /sc onlogon /it /f > logonshots-arm.txt 2>&1
schtasks /query /tn RippleLogonShots >> logonshots-arm.txt 2>&1
call put.cmd logonshots-arm.txt
shutdown /r /t 0
goto :eof
:fetch
schtasks /delete /tn RippleLogonShots /f > logonshots-fetch.txt 2>&1
copy /y "%TEMP%\logonshots.bmp" C:\rb\pkg\ >> logonshots-fetch.txt 2>&1
copy /y "%TEMP%\logonshots.txt" C:\rb\pkg\ >> logonshots-fetch.txt 2>&1
call put.cmd logonshots-fetch.txt logonshots.txt logonshots.bmp
