@echo off
rem Undo kvasoff.cmd: remove the two values (Windows' default, mitigations on), then restart.
cd /d C:\rb\pkg
(
  echo %DATE% %TIME%
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverride /f
  reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverrideMask /f
) > kvason.txt 2>&1
call put.cmd kvason.txt
shutdown /r /t 0
