@echo off
rem After a profiling run: the installed 32-bit user-mode driver itself, for its symbols.
copy /y %windir%\SysWOW64\viogpu_d3d10.dll R:\umd32.dll >nul
copy /y %windir%\System32\viogpu_d3d10.dll R:\umd64.dll >nul
echo POST_DONE >COM1
