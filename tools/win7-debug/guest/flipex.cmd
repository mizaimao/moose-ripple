@echo off
setlocal enabledelayedexpansion
rem flipex: the presentation and surface checks of d3d9flipex, both process sizes, flip and copy model.
rem Run from C:\rb: next to the driver package a 64-bit program would load the package copy of the driver.
cd /d C:\rb\pkg
call get.cmd d3d9flipex.exe d3d9flipex32.exe
copy /y d3d9flipex.exe C:\rb\d3d9flipex.exe >nul
copy /y d3d9flipex32.exe C:\rb\d3d9flipex32.exe >nul
del flipex.txt 2>nul
for %%m in (flip copy) do for %%p in (d3d9flipex.exe d3d9flipex32.exe) do (echo %%p %%m>> flipex.txt & C:\rb\%%p %%m >> flipex.txt 2>&1 & echo exit=!errorlevel!>> flipex.txt)
call put.cmd flipex.txt
