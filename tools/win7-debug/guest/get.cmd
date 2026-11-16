@echo off
rem get FILE...: fetch files from the build host's package folder (RB_GET) into C:\rb\pkg
:next
if "%1"=="" goto :eof
bitsadmin /transfer g%1 /download /priority foreground %RB_GET%/%1 C:\rb\pkg\%1 | findstr /c:"Transfer complete" >nul || echo get %1 FAILED
shift
goto next
