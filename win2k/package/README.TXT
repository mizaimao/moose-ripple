moose-ripple display driver for Windows 2000 and Windows XP, version @VERSION@

A display driver for QEMU's virtio-vga-gl device with host 3D acceleration
(virglrenderer). It provides the desktop, DirectDraw, Direct3D 7, the
Direct3D 8 and 9 driver interface and OpenGL (Mesa), drawn by the host GPU.

What works
- Direct3D 7, on by default, including hardware transform and lighting.
- Direct3D 8 and 9, also through the Direct3D 9 driver interface: pixel
  shaders 1.1 to 1.4 and 2.0, vertex shaders 1.1 and 2.0 with flow control
  on the host GPU, eight fixed-function stages, stencil, 24-bit depth, clip
  planes, the scissor test, full-screen devices, back buffers with alpha,
  render targets up to 2048x2048 (also cube maps), DXT textures (decoded by
  the host GPU), cube and volume textures, mipmaps of managed textures,
  plain surfaces (StretchRect, ColorFill, UpdateSurface), vertex and index
  buffers kept on the host GPU, points and point sprites.
  On Windows 2000 they need the DirectX 9.0c runtime; the last DirectX
  redistributable that installs there is the one of February 2010.
- OpenGL through Windows' own opengl32.dll (Mesa 24.3.4).
- 3DMark 2001 SE, 3DMark03 and 3DMark05 run to the end on Windows XP, and
  every 3DMark03 test draws the right picture.

Not yet
- Speed: on an Intel UHD 630 host 3DMark03 scores about a third of what a
  GeForce 9600 GT in the same kind of guest does (11009 against 33348).
- 1920x1080 at 32 bits per pixel does not fit in 8 MB of video memory.
- Wireframe and point fill modes and some fixed-function texture stage
  combinations are not drawn.
- A full-screen program that reads its back buffer before showing it gets
  an older picture.
- Shader model 3.0.

Install
1. Start the guest with a virtio-vga-gl device and a display with OpenGL
   (for example -device virtio-vga-gl -display gtk,gl=on, or sdl,gl=on),
   and with the clock option -rtc base=localtime,driftfix=slew: without
   driftfix=slew a guest that draws a heavy scene loses time, and
   benchmarks print scores that are too high.
2. Copy all files of this package to a folder in the guest.
3. Device Manager, Display adapters, the adapter's Properties, Driver,
   Update Driver; choose to pick the driver yourself, Have Disk, select
   RB2K.INF in that folder, and confirm the unsigned driver.
4. Restart Windows. The desktop comes up at 1024x768, 32 bits per pixel.

Uninstall
In Device Manager, uninstall the display adapter and restart; Windows
returns to its standard VGA driver. The files rb2kdisp.dll, rbntmode.dll
and ripplegl.dll in system32 and rb2kmp.sys in system32\drivers may then
be deleted.

Registry switches (DWORD values in the adapter's key under
HKLM\SYSTEM\CurrentControlSet\Control\Video\{...}\0000; restart after a
change):
- RippleNativeD3D7     1 (default): Direct3D drawn on the host GPU.
- RippleD3D8           1 (default): the Direct3D 8 and 9 interface.
- RippleD3D9           1 (default): the Direct3D 9 driver interface; 0
                       leaves Direct3D 9 programs on the DirectX 8 level
                       interface.
- RippleVertexShaders  1 (default): vertex shaders on the host GPU; 0
                       leaves them to the runtime.
- RippleHostBuffers    1 (default): vertex and index buffers kept on the
                       host GPU; 0 sends the vertices with every draw.
- RippleHostDxt        1 (default): DXT textures decoded by the host GPU;
                       0 decodes them in the guest.
- RippleTextureMB      256 (default): host memory for Direct3D textures,
                       in megabytes (16 to 1024).
- RippleHardwareCursor, RippleGpuDesktop, RippleDirectDrawGpuSurfaces and
  RippleD3D7WaitCurrent are for tests and stay 0.

Licences: LICENSE.TXT (this driver, MIT); THIRDPTY.TXT (Mesa, in
ripplegl.dll). Source: https://github.com/mizaimao/moose-ripple
