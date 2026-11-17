@echo off
rem prof: as dxone, with the main modules' load addresses written to umd-prof.log (for a sampling profile)
rem setlocal: the variables set here end with the script (a trace left on in the prompt slows
rem every later Direct3D 9 test there)
setlocal
cd /d C:\rb\pkg
del umd-prof.log 2>nul
set RIPPLE_UMD_LOG=C:\rb\pkg\umd-prof.log
call dxone.cmd %1 %2 %3 %4
call put.cmd umd-prof.log
