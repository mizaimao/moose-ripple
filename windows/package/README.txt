moose-ripple display driver for Windows Vista and Windows 7, version @VERSION@

A display driver for 64-bit Windows Vista SP2 and Windows 7 guests on QEMU's
virtio-vga-gl device with host 3D acceleration (virglrenderer). The desktop,
Aero, Direct3D 9 and OpenGL are drawn by the host's graphics card; on a host
with OpenGL 3.3 or later, Windows 7 gets Direct3D 10 too.

  vista\   the driver for Windows Vista SP2 x64
  win7\    the driver for Windows 7 x64

The full guide, with what works and what does not yet:
https://github.com/mizaimao/moose-ripple/blob/windows/docs/windows/install.md


TEST MODE IS NEEDED

64-bit Windows loads a kernel driver only when it carries a signature that
chains to a certificate Microsoft issued for that purpose. This driver has
no such certificate. It is signed with the project's own test certificate,
and Windows accepts a test signature only in test mode.

Test mode lets every test-signed kernel driver load, not only this one. Use
it in a virtual machine kept for this purpose, not on a machine whose
safety matters to you.

Switch it on in a Command Prompt started with "Run as administrator", then
restart Windows:

    bcdedit /set testsigning on

"Test Mode" then shows in the corners of the desktop.


INSTALL

1. Give the virtual machine the virtio-vga-gl display device and an OpenGL
   display (for example: -vga none -device virtio-vga-gl -display sdl,gl=on).
2. Switch test mode on and restart (above).
3. Copy this folder into the guest.
4. Open Device Manager, Display adapters. Right-click the adapter
   (Standard VGA Graphics Adapter), Update Driver Software, Browse my
   computer for driver software, Let me pick from a list, Have Disk, and
   choose viogpu3d.inf in the vista or the win7 folder.
5. Windows warns that it cannot verify the publisher. Choose "Install this
   driver software anyway".
6. Restart Windows.

Without test mode the driver does not start: Device Manager shows the
adapter with error code 52 and Windows stays on its plain VGA driver.


REMOVE

1. Device Manager, Display adapters, right-click "moose-ripple virtio-gpu
   3D", Uninstall, tick "Delete the driver software for this device", OK.
2. Restart Windows. It returns to the Standard VGA Graphics Adapter.
3. Switch test mode off if nothing else needs it, and restart:

       bcdedit /set testsigning off

4. Windows 7 only: the driver switched sleep and hibernation off for the
   machine, because it cannot bring the picture back after them yet. To
   allow them again, as administrator:

       reg delete "HKLM\SOFTWARE\Policies\Microsoft\Power\PowerSettings\abfc2519-3608-4c2a-94ea-171b0ed546ab" /f
       powercfg -h on

If the screen stays black after installing, start Windows in Safe Mode
(F8 before Windows starts), which uses the plain VGA driver, and remove
the driver there.


LICENCES

The driver is MIT licensed (LICENSE.txt). The kernel driver is derived from
virtio-win's viogpu3d (BSD-3-Clause); the user-mode driver is Mesa 24.3.4
(MIT) with statically linked runtime code. Their notices are in
THIRD-PARTY-NOTICES.txt.
