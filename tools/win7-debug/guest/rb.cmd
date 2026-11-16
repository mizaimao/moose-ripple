@echo off
rem rb NAME [ARGS]: fetch NAME.cmd from the build host and run it (docs/windows/test-vms.md,
rem "Scripts in the guest"). RB_GET is the URL of the build host's package folder.
bitsadmin /transfer rb%1 /download /priority foreground %RB_GET%/%1.cmd C:\rb\pkg\%1.cmd >nul
call C:\rb\pkg\%1.cmd %2 %3 %4 %5 %6 %7 %8 %9
