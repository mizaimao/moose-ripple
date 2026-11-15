@echo off
rem put FILE...: send files of C:\rb\pkg to the receiver on the build host (RB_PUT)
:next
if "%1"=="" goto :eof
if exist C:\rb\pkg\%1 powershell -command "(New-Object Net.WebClient).UploadData('%RB_PUT%/%1','PUT',[IO.File]::ReadAllBytes('C:\rb\pkg\%1')) | Out-Null"
shift
goto next
