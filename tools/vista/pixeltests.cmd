@echo off
rem The pixel tests of tools/vista at 32 and 64 bits (the six programs next to this script),
rem results to the first serial port: a "VT RUN" and a "VT EXIT code" line for each, then its
rem output; VT_END at the end. The clip-plane and video-processor tests pass on Vista only.
setlocal
set "t=%~dp0"
cd /d "%TEMP%" || goto failed
echo VT_BEGIN >COM1
call :arch i686
call :arch x86_64
echo VT_END >COM1
exit /b 0
:arch
call :run positiont-%1.exe
call :run positiont-%1.exe --fetch4
call :run positiont-%1.exe --points
call :run positiont-%1.exe --clip-planes
call :run video-processor-%1.exe
call :run fullscreen-present-%1.exe
exit /b 0
:run
echo VT RUN %* >COM1
"%t%%1" %2 >vt-out.txt 2>&1
echo VT EXIT %errorlevel% >COM1
type vt-out.txt >COM1
exit /b 0
:failed
echo VT_FAILED >COM1
exit /b 1
