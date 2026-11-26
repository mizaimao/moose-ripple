@echo off
rem relcheck LABEL: what display driver is installed and running, to the first serial port
rem (test mode, the service, the adapter, its version and INF, the installed files' hashes).
rem For the clean-install check of a release package (docs/windows/test-vms.md). Run elevated:
rem bcdedit needs it.
echo RELCHECK_BEGIN %1 >COM1
ver >COM1
bcdedit /enum {current} | findstr /i "testsigning" >COM1
sc query viogpu3d >COM1
wmic path Win32_VideoController get Name,DriverVersion,DriverDate,Status,InfFilename,CurrentHorizontalResolution,CurrentVerticalResolution /format:list >"%TEMP%\rel-video.txt" 2>&1
type "%TEMP%\rel-video.txt" >COM1
wmic path Win32_PnPSignedDriver where "DeviceClass='DISPLAY'" get DeviceName,DriverVersion,IsSigned,Signer,InfName /format:list >"%TEMP%\rel-signed.txt" 2>&1
type "%TEMP%\rel-signed.txt" >COM1
for %%F in (%windir%\system32\drivers\viogpu3d.sys %windir%\system32\viogpu_d3d10.dll %windir%\SysWOW64\viogpu_d3d10.dll) do (
  if exist %%F (certutil -hashfile %%F >COM1) else (echo MISSING %%F >COM1)
)
dir /b %windir%\inf\oem*.inf >COM1
reg query "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\abfc2519-3608-4c2a-94ea-171b0ed546ab" >COM1 2>&1
echo RELCHECK_END %1 >COM1
