# Windows 2000 framebuffer probe

This x86 driver draws a Windows 2000 desktop on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`). The miniport programs the compatibility display
registers and maps video memory. The display DLL gives GDI a top-down
32-bit bitmap backed by that memory. GDI draws directly into the visible
framebuffer. Hardware 3D is a separate milestone.

On the Linux build host, run `win2k/build.sh` with the Windows 2000 DDK
and Visual C++ 6.0 under Wine. The script locates the kits under
`.tools/win9x`, or under the directory named by `WIN9X_TOOLS`. Its default
output directory is `build/win2k`.

Copy the five output files into one directory the test guest can read.
Run `w2kprobe.exe` to see the hardware ID and current modes. Run
`nt5inst.exe` as a local administrator to install the driver, then restart
Windows. The helper obtains the adapter's complete PCI hardware ID from
SetupAPI before calling the native device installer.

Run `w2kprobe.exe /pattern` to switch temporarily to 800×600×32, sample
red, green, blue and white on the visible window and restore the prior mode.
The miniport also advertises 640×480 and 1024×768 at 32 bits.

On Windows 2000 Professional SP4 (build 2195), the stock VGA driver offered
640×480 and 800×600 at four bits. The Ripple miniport started after reboot,
and Windows switched to 800×600×32 at 60 Hz. Independent pixel samples
from GDI and the host display agreed on all four colours. The guest
restored its previous mode. Windows also cold-booted at the saved 800×600×32
setting, switched to 1024×768×32 and restored 800×600×32.

A cold boot using the 32-bit framebuffer exposed a console limitation.
Explorer and Calculator start, and a detached `cmd.exe` runs successfully.
Creating a new console window fails with Windows error 31 from
`AllocConsole`. A control build that supplied all three default font
descriptions gave the same result.

A traced miniport received no requests during the failed console allocation.
An immediately following successful resize generated mode-setting and memory
mapping requests. This bounds the failure to a path that issues no new
miniport request during that call. The underlying cause is still open.
