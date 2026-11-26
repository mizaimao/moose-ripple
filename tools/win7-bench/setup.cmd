@echo off
rem Preparation of a disk for unattended benchmark runs, elevated, once:
rem - the second disk becomes R: (FAT32) when it has no volume yet: results and crash dumps go there
rem - a crash stops the guest (no automatic restart) and leaves a small dump in R:\Minidump;
rem   a program that crashes leaves a dump in R:\dumps and no dialog
rem - a task at logon runs auto.cmd from the CD's 3dm folder (which does nothing without go.txt)
echo SETUP_BEGIN >COM1
if not exist R:\ (
  (echo select disk 1& echo clean& echo create partition primary& echo format fs=fat32 quick label=RWDATA& echo assign letter=R) >"%TEMP%\rw-dp.txt"
  diskpart /s "%TEMP%\rw-dp.txt" >"%TEMP%\rw-dp-out.txt" 2>&1
  findstr /i "succe error fail" "%TEMP%\rw-dp-out.txt" >COM1
)
if exist R:\ (echo SETUP data disk R: is there >COM1) else (echo SETUP no data disk >COM1)
mkdir R:\Minidump 2>nul
mkdir R:\dumps 2>nul
wmic recoveros set AutoReboot=False >"%TEMP%\rw-wmic.txt" 2>&1
wmic recoveros set DebugInfoType=3 >>"%TEMP%\rw-wmic.txt" 2>&1
wmic recoveros set MiniDumpDirectory="R:\Minidump" >>"%TEMP%\rw-wmic.txt" 2>&1
wmic recoveros get AutoReboot,DebugInfoType,MiniDumpDirectory /format:list >>"%TEMP%\rw-wmic.txt" 2>&1
type "%TEMP%\rw-wmic.txt" >COM1
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting" /v DontShowUI /t REG_DWORD /d 1 /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpFolder /t REG_EXPAND_SZ /d R:\dumps /f >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 1 /f >nul
powercfg -change -monitor-timeout-ac 0
powercfg -change -standby-timeout-ac 0
schtasks /create /tn rw7auto /tr "cmd /c if exist D:\3dm\auto.cmd call D:\3dm\auto.cmd" /sc onlogon /rl highest /f >COM1 2>&1
schtasks /query /tn rw7auto /fo list | findstr /i "TaskName Status" >COM1
echo SETUP_END >COM1
