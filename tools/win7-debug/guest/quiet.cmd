@echo off
rem quiet [TAG]: wait until Windows own background work is done (no process but this one above 15 percent of
rem a processor in three checks in a row, at most 12 minutes), and send what was busy meanwhile.
cd /d C:\rb\pkg
call get.cmd quiet.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File quiet.ps1 > quiet-%1.txt 2>&1
call put.cmd quiet-%1.txt
