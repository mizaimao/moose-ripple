@echo off
rem dx: the Direct3D tests of the Experience Index, one by one (not the compositor test, which runs only
rem inside the full assessment); results in dx.txt
cd /d C:\rb\pkg
del dx.txt 2>nul
winsat d3d -aname Batch -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 300 -batchcnt C(10) -objs C(26) -rendertotex 6 -rtdelta 3 -texpobj C(1) >> dx.txt 2>&1
winsat d3d -aname Alpha -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 300 -batchcnt C(75) -objs C(26) -rendertotex 6 -rtdelta 3 -texpobj C(1) >> dx.txt 2>&1
winsat d3d -aname Tex -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -texshader -totaltex 10 -texpobj C(4) -rendertotex 6 -rtdelta 3 >> dx.txt 2>&1
winsat d3d -aname ALU -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -alushader -totaltex 10 -texpobj C(1) -rendertotex 6 -rtdelta 3 >> dx.txt 2>&1
winsat d3d -dx10  -aname Batch -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 300 -batchcnt C(10) -objs C(26) -rendertotex 6 -rtdelta 3 -texpobj C(1) >> dx.txt 2>&1
winsat d3d -dx10  -aname Alpha -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 300 -batchcnt C(75) -objs C(26) -rendertotex 6 -rtdelta 3 -texpobj C(1) >> dx.txt 2>&1
winsat d3d -dx10  -aname Tex -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -texshader -totaltex 10 -texpobj C(4) -rendertotex 6 -rtdelta 3 >> dx.txt 2>&1
winsat d3d -dx10  -aname ALU -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 500 -batchcnt C(125) -objs C(20) -noalpha -alushader -totaltex 10 -texpobj C(1) -rendertotex 6 -rtdelta 3 >> dx.txt 2>&1
winsat d3d -dx10  -aname GeomF4 -time 7 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 150;200;241 -batchcnt C(50);C(200);C(300) -objs C(12);C(26);C(45) -noalpha -geomf4shader -texpobj C(0) -rendertotex 6 -rtdelta 3 -tierframes 60 -tiertime 1 >> dx.txt 2>&1
winsat d3d -dx10  -aname GeomV8 -time 7 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 75;100;120 -batchcnt C(25);C(100);C(150) -objs C(8);C(17);C(29) -noalpha -geomv8shader -texpobj C(0) -rendertotex 6 -rtdelta 3 -tierframes 60 -tiertime 1 >> dx.txt 2>&1
winsat d3d -dx10  -aname CBuffer -time 5 -fbc 10 -disp off -animate 10 -width 1280 -height 1024 -totalobj 75 -batchcnt C(25) -objs C(8) -rendertotex 6 -rtdelta 3 -texpobj C(1) -cbuffershader -cbufa 2 -cbuff 5 -cbufp 6 >> dx.txt 2>&1
call put.cmd dx.txt
