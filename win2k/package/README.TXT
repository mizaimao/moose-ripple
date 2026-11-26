moose-ripple display driver for Windows 2000 and Windows XP, version @VERSION@

A display driver for QEMU's virtio-vga-gl device with host 3D acceleration
(virglrenderer). It provides the desktop, DirectDraw, Direct3D 7, the
Direct3D 8 and 9 driver interface and OpenGL (Mesa), drawn by the host GPU.

What works
- Direct3D 7, on by default, including hardware transform and lighting.
- Direct3D 8 and 9 through the DirectX 8 level driver interface: pixel
  shaders 1.1 to 1.3 and 2.0, fixed-function stages, stencil, 24-bit depth,
  back buffers with alpha, full-screen devices, render target textures,
  DXT textures (decoded by the host GPU), cube and volume textures.
- OpenGL through Windows' own opengl32.dll (Mesa 24.3.4).
- 3DMark03 runs to the end on Windows XP.

Not yet
- Many 3DMark03 pictures are still wrong (dark scenes, white water).
- Vertex shaders are off by default; when on, they have no flow control.
- 1920x1080 at 32 bits per pixel does not fit in 8 MB of video memory.
- Direct3D 8 and 9 on Windows 2000 are untested.

Install
1. Start the guest with a virtio-vga-gl device and a display with OpenGL
   (for example -device virtio-vga-gl -display gtk,gl=on, or sdl,gl=on).
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
- RippleHostDxt        1 (default): DXT textures decoded by the host GPU;
                       0 decodes them in the guest.
- RippleVertexShaders  0 (default): 1 runs vertex shaders on the host GPU
                       (needed by DirectQ and 3DMark05).
- RippleHardwareCursor, RippleGpuDesktop, RippleDirectDrawGpuSurfaces and
  RippleD3D7WaitCurrent are for tests and stay 0.

Licences: LICENSE.TXT (this driver, MIT); THIRDPTY.TXT (Mesa, in
ripplegl.dll). Source: https://github.com/mizaimao/moose-ripple
