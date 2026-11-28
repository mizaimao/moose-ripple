moose-ripple @VERSION@ display driver for Windows 9x
Windows 95 OSR2.5, Windows 98 Second Edition and Windows Me

Install or update

1. Configure the VM with QEMU's virtio-vga-gl display and enable host 3D
   acceleration. Attach the ISO as a CD or extract the ZIP to a folder
   Windows can read. Both contain the same eight files.
2. In Control Panel > System > Device Manager, find the PCI display
   adapter with vendor ID 1AF4 and device ID 1050. If a fresh installation
   shows only generic VGA and there is no PCI bus under System devices,
   use Control Panel > Add New Hardware > System devices and select PCI
   bus. Let Windows detect the devices and restart when prompted. Keep
   the original Windows installation files available.
3. In the hardware wizard for the PCI display adapter, choose Have Disk
   and browse to RIPPLE9X.INF on the CD or in the extracted folder.
   Select "moose-ripple virtio-vga". To update an existing installation,
   open the adapter's properties in Device Manager, select the Driver
   tab, then Update Driver or Change Driver and Have Disk.
4. Restart Windows. In Display Properties > Settings, choose 16-bit or
   32-bit colour. If only 640x480 appears, check the monitor type.
   The INF reports version @VERSION@.0.

If Windows reports that the display adapter conflicts with another
device, Device Manager still lists an earlier display adapter, for
example Cirrus Logic from before the VM's display was changed. Remove
that entry under Display adapters and restart.

Direct3D 8 and 9

Since 0.6.0 the driver offers the Direct3D 8 and 9 driver interface by
default: pixel shaders 1.1 to 1.3 and 2.0, vertex shaders 1.1 and 2.0
(without flow control), fixed-function vertex processing on the GPU,
DXT, cube, volume and bump map textures. Programs need Microsoft's
DirectX 8.1 runtime, or DirectX 9.0c (December 2006 is the last that
installs on Windows 98 and Me) for Direct3D 9.

To go back to the behaviour of earlier releases, where the DirectX
runtime processes vertices itself and drives the Direct3D 7 interface,
set this registry value and restart Windows:

  HKEY_LOCAL_MACHINE\Software\moose-ripple
  "D3D8"="0"

"1" offers the Direct3D 8 interface with the runtime's vertex
processing; "2", or no value, is the default.

DirectX runtime

DirectDraw and Direct3D acceleration need DirectX 7 or later. With the
DirectX 6.1 that ships with Windows 98 SE, DirectDraw offers no video
memory or page flipping. On Windows 98 SE and Me, install DirectX 9.0c
(December 2006) and restart. On Windows 95 OSR2.5, install DirectX 7 or
8.1 and restart; it also supplies DirectInput and MSVCRT.DLL. The
Internet Explorer 4 installation included with OSR2.5 also supplies
MSVCRT.DLL for OpenGL. OpenGL itself needs no DirectX.

Hard disk: switch DMA on

Windows 95, 98 and Me leave DMA off for an IDE hard disk, and the VM
then writes to its disk at under 1 MB a second. Programs that need more
memory than the guest has (most Direct3D 9 programs on a guest of
256 MB) then take minutes to load and stutter. In Control Panel >
System > Device Manager > Disk drives, open the disk's Properties, tick
DMA on the Settings tab and restart Windows.

Full-screen Direct3D games

By default, 3D rendering runs on the host and the finished frame is
shown from the GPU for full-screen programs. RIPPLE_D3D_SCANOUT=0 in a
Windows DOS prompt, before starting the program from that prompt, copies
each frame to the guest VGA display instead. For DOS games using planar
VGA or custom display starts, boot Windows fresh.

Source and installation guide

https://github.com/mizaimao/moose-ripple/tree/win9x-v@VERSION@
See docs/win9x/display-driver.md for setup and docs/win9x/directdraw.md
for test results and compatibility details. LICENSE.TXT and THIRDPTY.TXT
contain the distribution notices.
