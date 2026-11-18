@echo off
setlocal
cd /d C:\rb\pkg
call get.cmd dwmtiming.exe
rem Run away from package DLLs, so Windows loads the installed display driver.
copy /y dwmtiming.exe C:\rb\dwmtiming.exe >nul
for %%m in (idle move repaint resize animate) do (
  C:\rb\dwmtiming.exe %%m 12 > utm-%%m.csv 2> utm-%%m.txt
  call put.cmd utm-%%m.csv utm-%%m.txt
)
call rb.cmd utmstate
endlocal
