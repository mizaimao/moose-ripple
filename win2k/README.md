# Windows 2000 display driver and GPU probe

This x86 driver draws a Windows 2000 desktop on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`). The miniport programs the compatibility display
registers and maps video memory. The display DLL gives GDI a top-down
32-bit bitmap backed by that memory. GDI draws directly into the visible
framebuffer. The driver performs DirectDraw colour fills and whole-surface
copies on the host for up to four XRGB32 offscreen surfaces, each up to
1024×768. Applications can copy a source rectangle from these surfaces
to a window using a DirectDraw primary surface and an HWND clipper.
In exclusive fullscreen mode the driver offers one back buffer when two
pages fit in the adapter's video memory. A flip changes the adapter's display-start
offset, and releasing the chain returns scanout to the desktop page.
A separate diagnostic draws one pixel on the host
through the same virtio control queue.

On the Linux build host, run `win2k/build.sh` with the Windows 2000 DDK
and Visual C++ 6.0 under Wine. The script locates the kits under
`.tools/win9x`, or under the directory named by `WIN9X_TOOLS`. Its default
output directory is `build/win2k`.

Copy the six output files into one directory the test guest can read.
Run `w2kprobe.exe` to see the hardware ID and current modes. Run
`nt5inst.exe` as a local administrator to install the driver, then restart
Windows. The helper obtains the adapter's complete PCI hardware ID from
SetupAPI before calling the native device installer.

The preferred desktop is 1024×768 at 32 bits and 60 Hz. The installer
supplies defaults for a new device and preserves an existing selection.
Use Display Properties to save 1024×768 on an upgraded guest. The driver
also supports 640×480 and 800×600. Run `w2kprobe.exe /pattern` to
sample four colours at 1024×768 and restore the original mode. Use
`/pattern800` to exercise the same change and restoration at 800×600.

Run `w2kprobe.exe /gpu` to exercise the host renderer. The miniport creates
a private 1×1 texture, clears it to orange, waits for completion, and
reads four bytes back. The expected BGRA pixel is `0xffff8000`. The tool
prints the stage and error code on failure and returns zero on success.
The desktop continues to use the mapped framebuffer during this test.

On Windows 2000 Professional SP4 with driver 0.0.6.0 and an Intel UHD 630 host, the adapter
initially had PCI bus mastering disabled. The miniport enables it for the
probe and restores the previous setting after resetting the queue. The guest
retrieved the exact orange pixel on two successive runs. QEMU traced both
command submissions and readbacks, and the Intel driver's per-process render
time advanced on each run. Afterward, the framebuffer still showed all four
test colours and the guest switched to 1024×768 and back to 800×600.

The GPU run used a previously installed Ripple service. The two driver
files were replaced on the powered-down test disk and verified after boot.
On a separate stock VGA installation, Windows Setup installed the same
build after the unsigned-driver prompt. Both files matched after reboot,
and Windows offered all three 32-bit modes. The first boot retained the
old four-bit setting. Select a 32-bit mode before running the GPU test.
An in-place upgrade from the previous driver still needs qualification.

`w2kprobe.exe /ddraw` tests the application-facing DirectDraw API. It
requests a 17×13 hardware-only offscreen surface, fills it with two
different colours, then takes one read-only lock and checks all 221 pixels.
It repeats the operation on an emulated system-memory surface. When the
driver exposes its transfer counters, the test checks for two GPU fills,
zero earlier readbacks, one readback at lock, and zero uploads. Run on an
idle desktop so unrelated DirectDraw clients do not contribute to the totals.

Run `w2kprobe.exe /ddraw-full` at 1024×768×32 to check a pair of
desktop-sized surfaces. It fills the source, copies it to the destination,
and overwrites the source before locking the destination. A pass requires
all 786,432 destination pixels to retain the first colour on both paths.
The hardware path must report two fills, one host-side copy, no uploads,
and a single readback at the lock.

On Windows 2000 Professional SP4 under Apple-silicon UTM, 0.0.8.0 passed
this test on a 1024×768×32 desktop at 60 Hz. QEMU recorded both textures,
the two fills and the copy, then one transfer from the destination when
the application locked it. A separate two-surface test verified edits
across scanlines at 1024×768 and isolation from a 1019×753 surface.
The guest retained its 1024×768 mode across a cold boot.

`w2kprobe.exe /window` opens a 640×480 test window and attaches a
DirectDraw clipper to it. It presents a solid colour, then a cropped
pattern from a host texture, first with another window overlapping it
and again after uncovering it. The probe checks the image, the window
margins and the overlapping window through GDI. It repeats the test
using DirectDraw software emulation as a control.

`w2kprobe.exe /flip` requests exclusive fullscreen mode, creates a primary
surface with one attached back buffer, and writes a different pattern to
each page. It flips five times and checks every front-buffer pixel after
each swap. On hardware it also reads the adapter's display-start register
to verify which page is visible. The hardware test runs at 640×480 and
1024×768, then checks that Windows has restored the original desktop mode.

`w2kprobe.exe /ddraw-pair` keeps one windowed DirectDraw surface alive
while a second process creates and reads its own. After the second process
exits, the first checks that its original colour is intact.

On Windows 2000 SP4 under UTM, the 32-bit hardware paths matched all
5,468,160 pixels checked across ten fullscreen flips. The adapter trace
recorded each page switch, the return to page zero, and zero resource
uploads or readbacks. Windows restored 1024×768×32 at 60 Hz. Only the
owner of a fullscreen chain maps both video pages. Two simultaneous
windowed clients retained their own offscreen pixels.

On Windows 2000 SP4 in UTM at 1024×768×32, both paths passed. Known solid
colours needed no GPU readback. For a patterned image the primary shares
GDI's framebuffer, so the driver reads back the visible rectangles. The
overlapping window left 286,833 pixels visible, requiring 1,147,332
bytes across four rectangles. Removing the overlap yielded one rectangle
of 298,936 pixels. The application uploaded its source pattern once
during setup. Presenting the window required no uploads.

When GDI or an application takes a writable lock after a solid fill, the
display DLL fills the application's staging buffer from the known colour.
A partial lock preserves pixels outside the locked rectangle. A patterned
surface still requires a readback if the caller needs its existing pixels.

Microsoft’s unmodified DirectX 7 Switcher sample displayed its text and
bouncing ball in a window on the 1024×768 desktop. Before this change,
each of 58 clears was followed by a full 640×480 readback for GDI. With the
change, 72 clears produced zero lock readbacks, saving 1,228,800 bytes per
frame. Each frame still uploaded the GDI edits and read its visible
rectangles for presentation.

## OpenGL

The display DLL answers `OPENGL_GETINFO` with the name `RIPPLEGL`, so
Windows' own `opengl32.dll` loads the driver registered under that name in
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers`. With the
Windows 98 build of `ripplegl.dll` (Mesa 24.3.4, `win9x/mesa`) copied to
`system32` and registered both as a string value and as a key (`Dll`,
`DriverVersion` 1, `Flags` 1, `Version` 2), it draws on the host GPU.

Mesa's virgl winsys uses the interface of the Windows 98 kernel driver
(`win9x/minivdd/rvd.h`). On Windows 2000 it sends the same calls to the
display DLL with `ExtEscape` (`rb2krvd.h`, `display_rvd.c`), and the miniport
carries them out (`gpu_rvd.c`): host contexts, resources, command batches of
up to 384 KB and transfers, one exchange with the host at a time. Each
buffer has guest pages of its own, attached to the host resource for its
whole life and mapped into the process that made it (`gpu_memory.c`). Mesa's
command stream moves data between the two by itself, so the pages cannot be
lent for single transfers. Mesa does not always destroy its context when a program ends, so the
miniport frees the contexts and buffers of ended processes at the next
creation and before the GPU self-test. While an
OpenGL context is open the transport stays up, and a mode change frees only
DirectDraw's surfaces.

On Windows 2000 SP4 in UTM, `gltest` reported `virgl` as the renderer and
passed all its pixel checks at about 590 frames a second at 320×240. The
scene is small, so this mostly measures a frame's way to the window: a
readback and a GDI copy. Microsoft's software OpenGL (`GDI Generic`) passed
at about 430 frames a second and Mesa's softpipe at about 9. A screen
capture of a 640×480 run showed the whole scene, clipped by an overlapping
window.

The Quake III Arena demo runs on the host GPU too, from its files copied
to `C:` (it needs no installer). `timedemo` with `demo001`, default
settings, 32-bit colour, one emulated Pentium III in UTM on Apple silicon:

| Mode | frames a second |
| --- | ---: |
| 640×480 in a window | 85 |
| 1024×768 full-screen | 82 |

    quake3.exe +set logfile 2 +set timedemo 1 +set s_initsound 0 +set r_mode 3
        +set r_fullscreen 0 +set nextdemo quit +demo demo001
    (the result is in demoq3\qconsole.log)

With 2.6 times the pixels the frame rate hardly changes, so on this host
the frame's readback and copy are not the limit; the emulated CPU or
the cost of each request to the host is, which is not yet measured. Screen captures of the windowed run showed the
level's textures, lighting and the game's text. Windows 98 reached about
300 frames a second in a window on a Proxmox VE host whose CPU is not
emulated; it has not been timed in UTM.

## Speed

`ddbench.exe` times the same DirectDraw frames through the driver
(hardware-only DirectDraw, video-memory surfaces) and through Windows'
software path (emulation-only DirectDraw, system-memory surfaces), for
three seconds each. It writes to standard output when that is redirected,
otherwise to COM1. Measured on Windows 2000 SP4 in UTM on Apple silicon
(one emulated Pentium III), 1024×768×32 desktop, 640×480 frames:

| Frame | Driver | Software |
| --- | ---: | ---: |
| Fill a surface, copy it to a window | 315 fps | 755 fps |
| CPU writes every pixel, copy it to a window | 92 fps | 454 fps |
| Fullscreen: fill the back buffer, flip | 845 fps | 914 fps |
| Fullscreen: fill, 100 64×64 sprites, flip | fails | 487 fps |
| Same with a source colour key | fails | 496 fps |

In a window the driver is two to five times slower than Windows' own
software path. The desktop is guest memory that the CPU draws into, so
each copy from a host texture to the screen is a GPU readback, and each
CPU-written frame is also an upload. Fullscreen fills and flips run on the
CPU in video memory on both paths. Copying part of a video-memory surface
into the back buffer fails with `DDERR_UNSUPPORTED`, and the driver offers
no colour keys. Microsoft's Donuts sample still draws its sprites
correctly with the driver: its frame matched the software path's.

The earlier 0.0.7.0 build supported full-surface fills on textures of
at most 4 KiB each. GDI continues to draw the primary desktop. On 0.0.6.0
the DirectDraw capability check reported no hardware acceleration and the
software control passed (exit 3).

On Windows 2000 Professional SP4 (build 2195), the stock VGA driver offered
640×480 and 800×600 at four bits. The Ripple miniport started after reboot,
and Windows switched to 800×600×32 at 60 Hz. Independent pixel samples
from GDI and the host display agreed on all four colours. The guest
restored its previous mode. Windows also cold-booted at the saved 800×600×32
setting, switched to 1024×768×32 and restored 800×600×32.

Console windows open on the 32-bit desktop. The display DLL reports
`TC_RA_ABLE` in its text capabilities, so GDI realizes raster fonts on the
framebuffer surface, as in the DDK's framebuffer sample. Builds without it
listed the raster fonts, Terminal among them, but `AllocConsole` failed with
Windows error 31; the console's default face is the raster Terminal font. On
Windows 2000 SP4 in UTM at 1024×768×32, the build with the flag allocated and
freed a console, and Command Prompt drew its banner in the Terminal font. The
miniport was byte-identical in both builds.
