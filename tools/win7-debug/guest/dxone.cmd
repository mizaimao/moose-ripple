@echo off
rem dxone: one Direct3D test of the Experience Index, alone, for 12 s; results in dxone.txt.
rem ARGS: 9 or 10, the test (Batch, Alpha, ALU, Tex, GeomV8, CBuffer; others take the Batch settings),
rem width and height. Run elevated.
cd /d C:\rb\pkg
set v=
if "%1"=="10" set v=-dx10
set extra=-totalobj 300 -batchcnt C(10) -objs C(26) -texpobj C(1)
if "%2"=="Alpha" set extra=-totalobj 300 -batchcnt C(75) -objs C(26) -texpobj C(1)
if "%2"=="GeomV8" set extra=-totalobj 75;100;120 -batchcnt C(25);C(100);C(150) -objs C(8);C(17);C(29) -noalpha -geomv8shader -texpobj C(0) -tierframes 60 -tiertime 1
if "%2"=="CBuffer" set extra=-totalobj 75 -batchcnt C(25) -objs C(8) -texpobj C(1) -cbuffershader -cbufa 2 -cbuff 5 -cbufp 6
if "%2"=="ALU" set extra=-totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -alushader -totaltex 10 -texpobj C(1)
if "%2"=="Tex" set extra=-totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -texshader -totaltex 10 -texpobj C(4)
winsat d3d %v% -aname %2 -time 12 -fbc 10 -disp off -animate 10 -width %3 -height %4 %extra% -rendertotex 6 -rtdelta 3 > dxone.txt 2>&1
call put.cmd dxone.txt
