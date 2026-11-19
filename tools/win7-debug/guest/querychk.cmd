@echo off
setlocal enabledelayedexpansion
rem querychk exact|visibility: the occlusion query tests, both process sizes, Direct3D 9 and Direct3D 11 at
rem feature level 9. exact: a host that counts samples; visibility: one that can only say nothing or something.
cd /d C:\rb\pkg
set mode=--expect-supported
if "%1"=="visibility" set mode=--expect-visibility
call get.cmd d3d9query.exe d3d9query32.exe d3d11query.exe d3d11query32.exe
for %%p in (d3d9query.exe d3d9query32.exe d3d11query.exe d3d11query32.exe) do copy /y %%p C:\rb\%%p >nul
del querychk.txt 2>nul
for %%p in (d3d9query.exe d3d9query32.exe) do (echo %%p %mode%>> querychk.txt & C:\rb\%%p %mode% >> querychk.txt 2>&1 & echo exit=!errorlevel!>> querychk.txt)
for %%p in (d3d11query.exe d3d11query32.exe) do (echo %%p --force-level9 %mode%>> querychk.txt & C:\rb\%%p --force-level9 %mode% >> querychk.txt 2>&1 & echo exit=!errorlevel!>> querychk.txt)
call put.cmd querychk.txt
