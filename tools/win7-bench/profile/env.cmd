@echo off
rem For a profiling run (auto.cmd calls this when it is next to it): the user-mode driver's
rem timing report and command statistics into R:\umd.txt, and two and a half minutes into the
rem run the module lists that turn sampled instruction addresses into names. A file
rem umd32-test.dll next to this one replaces the installed 32-bit user-mode driver first.
if exist "%~dp0umd32-test.dll" copy /y "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul
if exist "%~dp0umd32-test.dll" fc /b "%~dp0umd32-test.dll" %windir%\SysWOW64\viogpu_d3d10.dll >nul && echo UMD32_REPLACED >COM1
set RIPPLE_UMD_LOG=R:\umd.txt
set RIPPLE_UMD_TIMES=1
set RIPPLE_CMD_STATS=1
start "" /b cmd /c "%~dp0mods.cmd"
