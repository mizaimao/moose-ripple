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
