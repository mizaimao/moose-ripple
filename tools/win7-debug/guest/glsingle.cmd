@echo off
rem The single-buffered GL test at 64 and 32 bits, results to the first serial port.
cd /d "%TEMP%"
echo GLSINGLE_BEGIN %1 >COM1
del glsingle.txt 2>nul
"%~dp0glsingle64.exe"
echo GLSINGLE 64-bit exit %errorlevel% >COM1
if exist glsingle.txt type glsingle.txt >COM1
del glsingle.txt 2>nul
"%~dp0glsingle32.exe"
echo GLSINGLE 32-bit exit %errorlevel% >COM1
if exist glsingle.txt type glsingle.txt >COM1
echo GLSINGLE_END %1 >COM1
