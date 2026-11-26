@echo off
rem 3DMark06's default run from start to score without PowerShell (Vista has none unless it
rem was added): the same steps and marks as 3dmfull.ps1, with the helper programs next to this
rem file. Marks go to the first serial port and to standard output.   3dmfull.cmd [CAP_MINUTES]
setlocal enabledelayedexpansion
set "pkg=%~dp0"
set "game=C:\Program Files (x86)\Futuremark\3DMark06"
set "cap=%1"
if "%cap%"=="" set cap=50
if not exist "%game%\3DMark06.exe" (call :mark not-installed & exit /b 8)
call :launch started
if exist "%pkg%key.txt" call :register
set answers=0
set clicked=0
for /l %%i in (1,1,200) do if !clicked!==0 (
    ping -n 4 127.0.0.1 >nul
    if %%i GTR 6 (
        "%pkg%bench-go.exe" run >"%TEMP%\bg.txt"
        set rc=!errorlevel!
        if !rc!==1 (call :mark exited-before-run & exit /b 2)
        if !rc!==3 (set /a answers+=1 & call :mark answered-!answers!)
        if !rc!==0 (set clicked=1 & call :mark run-clicked)
    )
)
if !clicked!==0 (call :mark run-unavailable & type "%TEMP%\bg.txt" & taskkill /im 3DMark06.exe /f >nul 2>&1 & exit /b 3)
set "last="
set seen=0
set /a rounds=cap*12
for /l %%i in (1,1,!rounds!) do (
    "%pkg%bench-observe.exe" >"%TEMP%\bo.txt"
    if errorlevel 1 (call :mark exited & exit /b 4)
    set size=none
    for /f "tokens=*" %%L in ('findstr /r /c:"window '3DMark06' .* at 0,0 [0-9]*x[0-9]*" "%TEMP%\bo.txt"') do for %%T in (%%L) do set size=%%T
    if not "!size!"=="!last!" (
        call :mark fullscreen-!size!
        set "last=!size!"
        if not "!size!"=="none" set seen=1
    )
    findstr /c:"error occurred" /c:"E_FAIL" /c:"benchmark aborted" "%TEMP%\bo.txt" >nul
    if not errorlevel 1 (call :mark benchmark-error & type "%TEMP%\bo.txt" & taskkill /im 3DMark06.exe /f >nul 2>&1 & exit /b 6)
    if !seen!==1 (
        findstr /r /c:"window '[^']*Score" /c:"window '[^']*Result" "%TEMP%\bo.txt" >nul
        if not errorlevel 1 goto score
    )
    ping -n 5 127.0.0.1 >nul
)
call :mark timeout
type "%TEMP%\bo.txt"
taskkill /im 3DMark06.exe /f >nul 2>&1
exit /b 7

:score
call :mark score-window
ping -n 4 127.0.0.1 >nul
echo == score window
type "%TEMP%\bo.txt"
"%pkg%bench-score.exe"
"%pkg%bench-press.exe" Details
ping -n 5 127.0.0.1 >nul
echo == details
"%pkg%bench-score.exe"
call :mark done
taskkill /im 3DMark06.exe /f >nul 2>&1
exit /b 0

:launch
pushd "%game%"
start "" 3DMark06.exe
popd
call :mark %1
exit /b 0

:register
rem With key.txt next to this file (a registration code, one line) the program is registered
rem first, through its own dialogs, and started again; the code is never printed.
set found=0
for /l %%i in (1,1,20) do if !found!==0 (
    ping -n 4 127.0.0.1 >nul
    "%pkg%bench-go.exe" >"%TEMP%\bg.txt"
    findstr /c:"Please Register" "%TEMP%\bg.txt" >nul && set found=1
    if !found!==0 findstr /c:"window '3DMark06 - " "%TEMP%\bg.txt" >nul && set found=2
)
if not !found!==1 (call :mark no-register-dialog & exit /b 0)
"%pkg%bench-reg.exe" "%pkg%key.txt"
call :mark register-!errorlevel!
ping -n 4 127.0.0.1 >nul
taskkill /im 3DMark06.exe /f >nul 2>&1
ping -n 5 127.0.0.1 >nul
call :launch started-again
exit /b 0

:mark
echo MARK !time:~0,8! %1 >COM1
echo MARK !time:~0,8! %1
exit /b 0
