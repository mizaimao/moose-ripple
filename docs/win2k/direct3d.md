# Direct3D 8 and 9 on Windows XP and 2000

The application-local WineD3D runtime translates a 32-bit game's Direct3D
calls into OpenGL. Windows loads Ripple's OpenGL driver, which sends the
drawing commands to the host through virgl. Each game selects the runtime
from its own executable directory.

## Run a game

1. Install the Ripple display driver with `ripplegl.dll` and restart Windows.
2. For a Direct3D 8 game, copy `d3d8.dll`, `wined3d.dll` and `libwine.dll`
   from the build's `d3d8` folder beside the game's executable. For a
   Direct3D 9 game, use the three DLLs from `d3d9` instead. Choose the
   executable that draws the game if its launcher is in another folder.
   Use a separate game copy if that directory already contains any of the
   three names. Keep these files out of the Windows system directories.
3. Start the game normally. To return to its original renderer, remove
   the three DLLs you added to that game copy.

The build uses Wine 1.7.52 and the project's x86 OpenGL driver. Other
applications continue to use their own choice of Direct3D runtime.
Games that import D3DX or other DirectX helper libraries also need the
dependencies supplied by their normal installer.
[Upstream Windows port](https://fdossena.com/?p=wined3d/index.frag)

System-wide Direct3D enumeration and games that create Direct3D 7 or earlier
through DirectDraw still use the installed Windows driver. Morrowind and
Max Payne 2 await individual game tests. [Direct3D investigation](https://github.com/mizaimao/moose-ripple/issues/106)

## Build the loose files

Pass the upstream **source** archive to the normal driver build. On a
64-bit Linux host, Wine's native build tools need a C compiler, Make,
Bison and Flex. The Windows DLLs use the project's MSVCRT llvm-mingw
toolchain. Set `LLVM_MINGW` to its directory if it is outside `.tools`.
The build verifies the archive before compiling and writes `d3d8` and
`d3d9` under the driver output. Once the source is present it runs
offline. [Wine 1.7.52 source archive](https://dl.winehq.org/wine/source/1.7/wine-1.7.52.tar.bz2)

```sh
WINED3D_SOURCE=/path/to/wine-1.7.52.tar.bz2 sh win2k/build.sh
```

The expected SHA-256 is
`0773b32a0c358323db4c383ceb1e9e28d5d4ed4ea37570af2bcb41fecf0d554b`.
Each runtime folder includes Wine's licence in `COPYING.LIB`.
[Wine source at 1.7.52](https://github.com/wine-mirror/wine/tree/wine-1.7.52)
· [Windows build project](https://github.com/adolfintel/wined3d4win)

On Ripple's virgl renderer, WineD3D uses separate linear and sRGB textures.
With the shared texture, a clear requested as RGB (5, 9, 19) returned
(38, 53, 77) even with conversion disabled. A Direct3D clear now follows
the application's sRGB write setting, including when the app changes it
between draws. [Direct3D 9 gamma behaviour](https://learn.microsoft.com/en-us/windows/win32/direct3d9/gamma)

## Check a renderer

With the x86 MSVCRT llvm-mingw compiler on the build host's path, run
`sh win2k/d3d/build-probes.sh build/probes`. Place `tex8.exe` or
`tex9.exe` beside its matching three runtime DLLs and run it in Windows.
Each program prints the loaded DLL path, adapter description, shader
versions and hardware device type. It uploads a 4×4 colour texture,
draws it twice with different colours and checks all 4,096 pixels of
each back buffer, including the border. A failed comparison, draw or
presentation returns a nonzero exit code.
On Direct3D 9 it also switches sRGB writes on and off and checks the
result of each clear when the format advertises that capability.

The same script builds `dx9shade.exe`. Run it beside the Direct3D 9 DLLs.
It changes vertex and pixel shader constants independently across 16 queued
draws and checks 16 pixels from each result.

Record `rvdstat.exe` before and after the program to check that Ripple
submitted work to the GPU and released the resources. For a control,
put just the probe executable in a separate directory and run it there.
Windows will look for its installed Direct3D runtime.

## Guest results

The following runs used 32-bit Windows in UTM on Apple silicon. Direct3D
Quake 0.81 imports `d3d8.dll` directly. Its stock executable played the
969-frame shareware `demo1` in a 640×480 window with sound disabled.
[Direct3D Quake](https://dxquake.sourceforge.net/) · [Quake shareware data](https://www.libsdl.org/projects/quake/)

| Guest | Direct3D 8 textured frames | Direct3D 9 textures, sRGB clears and shaders | Direct3D Quake |
| --- | --- | --- | ---: |
| Windows XP Professional SP3 | 8,192 pixels passed | 16,384 pixels and all 16 shader cases passed | 21.4 fps |
| Windows 2000 SP4 | 8,192 pixels passed | 16,384 pixels and all 16 shader cases passed | 21.2 fps |

Both APIs created a hardware device, loaded `ripplegl.dll` and submitted
commands through the miniport. Direct3D 8 reported vertex shader 1.1 and
pixel shader 1.4. Direct3D 9 reported shader model 3.0 for both stages.
Running the same probes outside the game folder found no hardware device
with XP's system DLLs. The stock Windows 2000 guest lacked both system DLLs.

Both game runs exited with code zero. The saved and current desktops read
1024×768 at 32-bit colour and 60 Hz. The miniport counted 2,256 successful
GPU submissions during the XP game and 2,210 on Windows 2000. After each
run it reported zero contexts, buffers and pages held.

Run the timedemo from the game directory with the shareware `id1` directory beside the executable.

```bat
D3DQuake.exe -window -width 640 -height 480 -nosound -nocdaudio -nojoy -condebug +timedemo demo1
```

### Direct3D 8 performance on XP

The stock Direct3D Quake executable replayed the 969-frame `demo1` on the
same XP SP3 boot. Each run used the same driver, game data, 32-bit desktop
and sound settings. The runner checked the actual game window dimensions
and sampled which process owned the foreground window.

| Window | Pixels per frame | First run | Repeat |
| --- | ---: | ---: | ---: |
| 320×240 | 76,800 | 21.4 fps | 21.1 fps |
| 640×480 | 307,200 | 20.3 fps | 20.6 fps |
| 800×600 | 480,000 | 19.8 fps | 19.7 fps |

The runs went from 640×480 to 320×240 to 800×600 and then repeated in
reverse order. Quake held the foreground in every sample. Increasing
pixel count 6.25 times lowered the average frame rate by about 7%.
Over each complete run the display driver spent 1.02–1.05 seconds reading
pixels back at 320×240 and 1.99–2.01 seconds at 800×600. All GPU submissions
and pixel reads succeeded.

Quake asks its Direct3D 8 hardware device to use software vertex processing.
At 640×480 a diagnostic DLL honoured that request for the first and last runs
and forced hardware vertex processing for the two runs between them. The
default produced 20.8 and 21.1 fps. The hardware override produced 19.0 and
18.8 fps. The driver, executable, data and other runtime DLLs were identical.
[Direct3D Quake source](https://dxquake.sourceforge.net/)

The game calls `Sleep(1)` in its main loop, including during the timedemo.
With the stock runtime at 640×480, requesting a 1 ms Windows timer period
gave 21.8 and 21.9 fps. Restoring the default gave 21.2 fps. A clock on the
host measured 49.6, 48.8 and 49.9 seconds respectively from game launch to
the score. Finer timer resolution made little difference to elapsed time.

At 640×480, `r_norefresh 1` skips the 3D scene while the game continues to
draw the status bar and present each frame. The same demo then took 17.2
seconds (56.2 fps). Restoring the scene with `r_norefresh 0` took 46.2
seconds (21.0 fps). Drawing the scene added about 30 ms per frame. Across
each complete run, XP charged the game process 5.2 seconds of CPU time with
the scene skipped and 34.5 seconds with it visible. The matched renderer
test below measures how much of that time the Direct3D path accounts for.

### The same game on Linux QEMU

Linux QEMU 9.2 ran the same 969-frame `demo1` in a 640×480 window with
the sound disabled. Both hosts used the same stock XP SP3 disk, test data,
game executable and Direct3D DLLs. The guest had one Pentium III CPU,
512 MiB and the `pc-i440fx-2.12` machine type on both hosts. Both Linux
benchmarks booted fresh overlays of one driver installation. XP checked that
the installed miniport, display DLL and OpenGL DLL matched the test files.
On the i9-9900K, KVM ran the guest code directly on the CPU. QEMU's Tiny
Code Generator (TCG) emulated the guest CPU in software on that same machine.

| XP host and CPU mode | 3D scene visible | 3D scene skipped |
| --- | ---: | ---: |
| UTM on Apple silicon, emulated x86 | 21.0 fps | 56.2 fps |
| Linux QEMU with KVM, Intel UHD 630 | 56.6–57.6 fps | 59.6–59.7 fps |
| Linux QEMU with TCG, Intel UHD 630 | 13.9–14.2 fps | 56.0–56.4 fps |

Each Linux range covers three runs with the scene visible or two with it
skipped, all using the default Windows timer. A separate 1 ms timer trial
with the scene visible produced 60.0 fps under KVM and 11.8 fps under TCG.
The game logged the applied `r_norefresh` value on every run. The Linux guest
framebuffers showed textured level geometry and the player's weapon with
both CPU modes. Both modes also passed the 8,192-pixel Direct3D 8 check.

Drawing the scene added 52–54 ms per frame under Linux CPU emulation,
compared with 30 ms in UTM. The Linux host clock measured 18–19 seconds
from game launch to the full-scene score with KVM and 72–74 seconds with
TCG, including startup. XP charged Quake about 3 seconds of CPU time per
complete run with KVM and 56–58 seconds with TCG. Changing the CPU mode
alone reproduced the slowdown with the same host GPU and renderer.

### Direct3D 8 and direct OpenGL on the same XP guest

Two builds of Direct3D Quake 0.81 replayed the same 969 frames in a
640×480 window in UTM on an M1 Max. Both used the same compiler settings,
game data, driver and 32-bit desktop. Both called `Sleep(1)` each frame.
The test fixed the simulation step at 50 ms while measuring elapsed time
independently. Both builds used two texture units for lightmapping. The
OpenGL build used Mesa's ARB multitexture entry points for the operations
the original Direct3D port exposes under their older SGIS names.
[Direct3D Quake source](https://dxquake.sourceforge.net/)

| Renderer | Scene visible, two runs | XP main-thread CPU per timedemo | Scene skipped |
| --- | ---: | ---: | ---: |
| Direct OpenGL through Ripple | 45.1, 47.2 fps | 5.4–6.2 s | 63.2 fps |
| Direct3D 8 through WineD3D and Ripple | 22.6, 22.7 fps | 27.4–27.6 s | 56.3 fps |

The two pairs ran in opposite orders. The unmodified Direct3D executable
scored 21.1 fps before them and 21.7 fps after. Both rebuilt renderers
logged the same per-frame geometry checksum, 214,047 world polygons and
807,647 model triangles. Both used Ripple's OpenGL DLL. Only the Direct3D
run loaded the app-local Direct3D 8 and WineD3D DLLs. The independent
Direct3D texture check matched all 8,192 pixels.

| Elapsed time per sampled frame | Direct OpenGL | Direct3D 8 |
| --- | ---: | ---: |
| Draw the 3D scene | 8.82 ms | 31.92 ms |
| Draw the world, included in the scene | 5.37 ms | 13.15 ms |
| Draw entities, included in the scene | 1.80 ms | 11.82 ms |
| Present the frame | 3.96 ms | 5.48 ms |

These runs timed one frame in eight, 122 frames per renderer. Across the
whole run the Direct3D port made about 517,000 `DrawPrimitiveUP` calls,
roughly 534 per frame and three primitives per call. Time spent inside
that API averaged 27.54 ms per sampled frame. Selecting the vertex format
cost 0.76 ms and updating the port's dirty render state cost 0.52 ms.
These call timings overlap the scene and presentation rows.

The UTM emulator loaded Apple's `AGXMetalG13X` driver. The M1 Max GPU's
device utilization had a median of 7% with either
renderer during the full-scene runs. Ten samples after the final game
exited had a median of 0%. These counters cover the whole host. On the
same XP boot the Direct3D path used about 22 extra seconds of game-thread
CPU and spent 23.1 ms more drawing each sampled scene. The difference
in presentation was 1.52 ms.

The original game executable also ran against a diagnostic build of the
same WineD3D runtime. The profiler sampled every 127th `DrawPrimitiveUP`
across 800 consecutive frames, about 418,000 calls in each run.
Two detailed runs each sampled 3,292 calls, all of which took the OpenGL
vertex-array path. The game held the foreground and scored 21.4 fps in
both runs. A separate control with the timers disabled scored 21.5 fps.
An earlier 11.9 fps run held the foreground for two of 85 samples and was
excluded from the comparison.

| Work inside one sampled Direct3D draw | Elapsed time, two runs |
| --- | ---: |
| Copy the caller's vertices and unmap the streaming buffer | 22.6–23.2 µs |
| Acquire WineD3D's drawing context | 14.4–14.5 µs |
| Apply drawing state | 12.9–13.7 µs |
| Issue the OpenGL draw and check its result | 5.0–5.2 µs |

The last row measures time in the guest issuing the draw. Each sampled
interval includes a timer read of about 0.85 µs. Two further runs kept
only the outer timers. The copy and unmap then took 21.3–21.8 µs, and
the complete draw dispatch took 31.2–31.4 µs. Streaming a few vertices
and preparing the context and state for the next draw account for most
of the measured time.

WineD3D normally checks for OpenGL errors after many API calls. With
the original DLLs, setting `WINEDEBUG=err-all` for Quake alone gave
22.3 and 22.5 fps. The default setting gave 21.5 fps immediately before
the change and 21.6 fps after restoring it. Disabling that class of error
checks accounted for about 4% on this workload.

#### Reusing the OpenGL context within one draw

The XP experiment acquires the WineD3D context at the start of
`DrawPrimitiveUP`, keeps it active across the vertex upload and draw,
and releases it before returning to the game. This lets the nested
operations share the current context. Both settings ran the original
Quake executable with the same two experimental DLLs on one XP boot.

| Context handling | Timedemo fps, three runs | Median fps | Host time from launch to score | Median XP process CPU |
| --- | ---: | ---: | ---: | ---: |
| Existing behavior | 20.3, 20.1, 21.0 | 20.3 | 50.2–53.0 s | 35.8 s |
| Keep the context through upload and draw | 24.1, 23.8, 24.4 | 24.1 | 43.7–45.4 s | 28.3 s |

The median frame rate rose 18.7%. Two enabled runs were bracketed by
controls, then the guest repeated the enabled and control settings once
more. All six runs held the foreground and exited normally. At the 900th
presentation the enabled runs had taken the new path for every one of
about 432,500 draws. The two settings each matched all 8,192 pixels in
the separate texture test. The desktop returned to 1024×768 and the
driver released its contexts, buffers and pages. The next compatibility
checks are Windows 2000 and applications that switch between their own
WGL context and Direct3D.

To repeat the trial, append the linked patch to
`win2k/d3d/clear-srgb.patch` in a scratch checkout, then use the loose
runtime build described above. Set `RBQGROUP=0` in the game process for
the control and `RBQGROUP=1` for context reuse. At the 900th presentation
the test DLL writes `rbctx.log` with the number of draws that used the
scope. [Tested WineD3D patch](../../win2k/d3d/experiments/scoped-draw-context.patch)

#### GPU presentation on NT5

The NT5 windowed path still reads frames back and copies them through GDI.
The Windows 9x full-screen path keeps the frame on the GPU by publishing a
scanout resource. Its Mesa entry point expects an offscreen renderbuffer,
and the NT5 miniport lacks the matching GPU present operation. Applying
the same idea to windows also needs a GPU-backed desktop and a copy
clipped to the window's visible region. The current presentation timings
give a baseline for measuring that work. [NT5 OpenGL frame time](development.md#frame-time) · [Windows 9x full-screen GPU presentation](../win9x/directdraw.md#full-screen-gpu-presentation) · [Snow Leopard window presentation](../re/ioaccelerator-interface.md#step-2-the-kext-shows-a-buffer-on-such-a-surface-2026-11-12)

### Direct3D 9 games on XP

DarkPlaces, build 20140513, ran the same shareware `demo1` in a 640×480
window at **63.1 fps** (969 frames in 15.353 seconds). Its log identified
the D3D9 device and recorded successful HLSL compilation for lightmaps,
model lighting and shadow maps. The game exited with code zero. Ripple
counted 2,002 GPU submissions, zero failures and no resources held after
exit. The desktop returned to 1024×768×32 at 60 Hz. This executable imports
`SetDllDirectoryA`, which Microsoft supports starting with XP SP1.
[DarkPlaces Windows build](https://icculus.org/twilight/darkplaces/download.html) · [Windows API requirements](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setdlldirectorya)

Place the `d3d9` runtime beside `darkplaces.exe` and the game's `libjpeg-8.dll`.
The test used Microsoft's `d3dx9_37.dll` for the game's shader compiler.
The game writes the renderer to `id1\qconsole.log` and the score to
`id1\benchmark.log`. [DirectX helper libraries](https://www.microsoft.com/en-us/download/details.aspx?id=8109)

```bat
darkplaces.exe -nohome -window -width 640 -height 480 -nosound -condebug +vid_dx9 1 -benchmark demo1
```

DirectQ 1.5 created a hardware context and submitted work, then stopped
before its timedemo finished. A repeat run located the unhandled error
(`0xc0000005`) in `DirectQ.exe`. The cause remains open.
[DirectQ project](https://sourceforge.net/projects/direct3dquake/)
