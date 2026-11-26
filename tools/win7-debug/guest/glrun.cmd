@echo off
rem The GL test at 64 and 32 bits, 300 frames each, results to the first serial port.
cd /d "%TEMP%"
echo GLRUN_BEGIN %1 >COM1
del gltest.txt 2>nul
"%~dp0gltest64.exe" 300
echo GLRUN 64-bit exit %errorlevel% >COM1
if exist gltest.txt type gltest.txt >COM1
del gltest.txt 2>nul
"%~dp0gltest32.exe" 300
echo GLRUN 32-bit exit %errorlevel% >COM1
if exist gltest.txt type gltest.txt >COM1
echo GLRUN_END %1 >COM1
