@echo off
rem For a pixel-test run (auto.cmd calls this when it is next to it): the Direct3D 9 pixel
rem tests of the CD's tests folder instead of the benchmark, then a shutdown. Test builds of
rem the user-mode driver next to this file (umd32-test.dll, umd64-test.dll) are swapped in
rem first; the 64-bit one is in use by the desktop, so the installed file is renamed.
if exist "%~dp0umd32-test.dll" copy /y "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul
if exist "%~dp0umd32-test.dll" fc /b "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul && echo UMD32_REPLACED >COM1
if exist "%~dp0umd64-test.dll" ren %windir%\System32\viogpu_d3d10.dll viogpu_d3d10.old
if exist "%~dp0umd64-test.dll" copy /y "%~dp0umd64-test.dll" %windir%\System32\viogpu_d3d10.dll >nul
if exist "%~dp0umd64-test.dll" fc /b "%~dp0umd64-test.dll" %windir%\System32\viogpu_d3d10.dll >nul && echo UMD64_REPLACED >COM1
rem the desktop first: a device made in the first seconds after the logon is refused
ping -n 41 127.0.0.1 >nul
call "%~dp0..\tests\vt.cmd"
shutdown /s /t 5
ping -n 90 127.0.0.1 >nul
