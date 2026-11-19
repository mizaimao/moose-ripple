@echo off
rem Is Windows' kernel page-table isolation (KVA shadow) active in this guest?
cd /d C:\rb\pkg
call get.cmd kvas.ps1
(
  echo %DATE% %TIME%
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\rb\pkg\kvas.ps1
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverride
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverrideMask
  wmic cpu get Name,Manufacturer,NumberOfLogicalProcessors /value
  wmic os get Version,BuildNumber,CSDVersion /value
  wmic datafile where "name='C:\\Windows\\System32\\ntoskrnl.exe'" get Version /value
) > kvas.txt 2>&1
call put.cmd kvas.txt
