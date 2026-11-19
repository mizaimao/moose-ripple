@echo off
rem hdclips TAG: the video playback tests of Windows own assessment, each alone, as the full assessment runs them.
rem Sends hdclips-TAG.txt (what each printed) and hdclips-TAG-N.xml (its result file).
setlocal
cd /d C:\rb\pkg
set tag=%1
set RIPPLE_UMD_TRACE=
set RIPPLE_UMD_LOG=
del hdclips-%tag%.txt 2>nul
echo === 1: -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_MPEG2_HD_15mbps.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_MPEG2_HD_15mbps.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps -v -xml hdclips-%tag%-1.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
echo === 2: -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_VC1_15mbps.wmv -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_VC1_15mbps.wmv -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog -v -xml hdclips-%tag%-2.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
echo === 3: -input C:\Windows\Performance\WinSAT\Clip_480i_5sec_6mbps_new.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_480i_5sec_6mbps_new.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps -v -xml hdclips-%tag%-3.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
echo === 4: -input C:\Windows\Performance\WinSAT\Clip_480p_5sec_6mbps_new.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_480p_5sec_6mbps_new.mpg -dshow -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog -v -xml hdclips-%tag%-4.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
echo === 5: -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_10mbps_h264.mp4 -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_1080_5sec_10mbps_h264.mp4 -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 135 -expfrmmax 150 -overrideframetype prog -v -xml hdclips-%tag%-5.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
echo === 6: -input C:\Windows\Performance\WinSAT\Clip_480_5sec_6mbps_h264.mp4 -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps>> hdclips-%tag%.txt
winsat mediaex -input C:\Windows\Performance\WinSAT\Clip_480_5sec_6mbps_h264.mp4 -video offscreen -audio mute -width 100%%m -height 100%%m -constrain -savereg -autoprofname -expfrmmin 270 -expfrmmax 300 -overrideframetype int2fps -v -xml hdclips-%tag%-6.xml >> hdclips-%tag%.txt 2>&1
>>hdclips-%tag%.txt echo exit=%errorlevel%
call put.cmd hdclips-%tag%.txt hdclips-%tag%-1.xml hdclips-%tag%-2.xml hdclips-%tag%-3.xml hdclips-%tag%-4.xml hdclips-%tag%-5.xml hdclips-%tag%-6.xml
