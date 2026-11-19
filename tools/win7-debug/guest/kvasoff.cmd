@echo off
rem Test: switch Windows' Meltdown and Spectre v2 mitigations off in this guest (Microsoft's
rem documented switch, KB4072699), then restart. Undo: kvason.cmd.
cd /d C:\rb\pkg
(
  echo %DATE% %TIME%
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverride /t REG_DWORD /d 3 /f
  reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverrideMask /t REG_DWORD /d 3 /f
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverride
  reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management" /v FeatureSettingsOverrideMask
) > kvasoff.txt 2>&1
call put.cmd kvasoff.txt
shutdown /r /t 0
