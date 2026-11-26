@echo off
rem Install test of a Windows 2000/XP package (docs/win2k/development.md, Release package).
rem Install the package, restart, then run this from a folder that holds the package's
rem files and build/win2k's test programs:   pkgtest.cmd [output]   (default: the console)
rem It ends with "PKGTEST END failed=N"; N counts installed files that differ from the
rem package and an OpenGL renderer other than virgl. The Direct3D lines are read by hand.
setlocal
cd /d "%~dp0"
set OUT=%1
if "%OUT%"=="" set OUT=con
set FAILED=0
>>%OUT% echo PKGTEST begin
ver >>%OUT%
for %%f in (rb2kdisp.dll rbntmode.dll ripplegl.dll) do call :same %%f "%SystemRoot%\system32\%%f"
call :same rb2kmp.sys "%SystemRoot%\system32\drivers\rb2kmp.sys"
rem OpenGL must draw on the host GPU. A ripplegl.dll without the Windows 2000/XP path in
rem its winsys falls back to Mesa's softpipe (release 0.4.0 did).
glpresent.exe window > "%TEMP%\rb-gl.txt" 2>&1
type "%TEMP%\rb-gl.txt" | find "renderer=" >>%OUT%
find "renderer=virgl" "%TEMP%\rb-gl.txt" >nul
if errorlevel 1 (
    set /a FAILED+=1
    >>%OUT% echo PKGTEST opengl FAIL
) else (
    >>%OUT% echo PKGTEST opengl PASS
)
call :async d9pixels
call :async d9pixels sw
call :async d9stencil
call :async d8pixels
d9full.exe >>%OUT% 2>&1
d3d7probe.exe hal > "%TEMP%\rb-c.txt" 2>&1
type "%TEMP%\rb-c.txt" | find "result=" >>%OUT%
tpixels.exe tnl > "%TEMP%\rb-t.txt" 2>&1
type "%TEMP%\rb-t.txt" | find "result=" >>%OUT%
>>%OUT% echo PKGTEST END failed=%FAILED%
exit /b %FAILED%

:same
fc /b %1 %2 >nul
if errorlevel 1 (
    set /a FAILED+=1
    >>%OUT% echo PKGTEST %1 differs from the installed copy
)
goto :eof

rem A Direct3D check in the background, read after 30 seconds: one that hangs does not
rem stop the test.
:async
>>%OUT% echo PKGTEST %1 %2 begin
start "" /b cmd /c "%1.exe %2 > %TEMP%\rb-%1.txt 2>&1"
ping -n 31 127.0.0.1 >nul
type %TEMP%\rb-%1.txt | find "PIX end" >>%OUT%
type %TEMP%\rb-%1.txt | find "D9ST end" >>%OUT%
type %TEMP%\rb-%1.txt | find "no Direct3D" >>%OUT%
type %TEMP%\rb-%1.txt | find "result=UNAVAILABLE" >>%OUT%
goto :eof
