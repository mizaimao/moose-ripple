@echo off
rem For a timed run with test builds of the user-mode driver and its timing report (auto.cmd
rem calls this when it is next to it): umd32-test.dll and umd64-test.dll next to this file
rem are swapped in before the benchmark starts. The 64-bit file is in use by the desktop, so
rem the installed one is renamed (a name left by an earlier swap is deleted first).
del /f /q %windir%\System32\viogpu_d3d10.old 2>nul
if exist "%~dp0umd32-test.dll" copy /y "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul
if exist "%~dp0umd32-test.dll" fc /b "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul && echo UMD32_REPLACED >COM1
if exist "%~dp0umd64-test.dll" ren %windir%\System32\viogpu_d3d10.dll viogpu_d3d10.old
if exist "%~dp0umd64-test.dll" copy /y "%~dp0umd64-test.dll" %windir%\System32\viogpu_d3d10.dll >nul
if exist "%~dp0umd64-test.dll" fc /b "%~dp0umd64-test.dll" %windir%\System32\viogpu_d3d10.dll >nul && echo UMD64_REPLACED >COM1
set RIPPLE_UMD_LOG=R:\umd-%label%.txt
set RIPPLE_UMD_TIMES=1
