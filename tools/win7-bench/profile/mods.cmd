@echo off
ping -n 150 127.0.0.1 >nul
"%~dp0modlist32.exe" 3DMark06.exe >R:\mods-3dmark.txt 2>&1
"%~dp0modlist64.exe" >R:\mods-kernel.txt 2>&1
echo MODS_WRITTEN >COM1
