# Windows 2000 display driver and GPU probe

This x86 driver draws a Windows 2000 desktop on QEMU's `virtio-vga-gl`
adapter (PCI `1AF4:1050`). The miniport programs the compatibility display
registers and maps video memory. The display DLL gives GDI a top-down
32-bit bitmap backed by that memory. GDI draws directly into the visible
framebuffer. The 0.0.7.0 candidate adds DirectDraw callbacks for small
offscreen surfaces. A separate diagnostic draws one pixel on the host
through the same virtio control queue.

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

0.0.7.0 targets full-surface fills on up to four XRGB32 surfaces of at
most 4 KiB each. GDI continues to draw the primary desktop. On 0.0.6.0
the DirectDraw capability check reported no hardware acceleration and the
software control passed (exit 3).

On Windows 2000 Professional SP4 (build 2195), the stock VGA driver offered
640×480 and 800×600 at four bits. The Ripple miniport started after reboot,
and Windows switched to 800×600×32 at 60 Hz. Independent pixel samples
from GDI and the host display agreed on all four colours. The guest
restored its previous mode. Windows also cold-booted at the saved 800×600×32
setting, switched to 1024×768×32 and restored 800×600×32.

A cold boot using the 32-bit framebuffer exposed a console limitation.
Explorer and Calculator start, and a detached `cmd.exe` runs successfully.
Creating a new console window fails with Windows error 31 from
`AllocConsole`. The same test under the same account succeeded on the
stock VGA desktop at 640×480 in 16 colours. A build that supplied all three
default font descriptions also failed to open a console on the 32-bit desktop.

A traced miniport received no requests during the failed console allocation.
An immediately following successful resize generated mode-setting and memory
mapping requests. This bounds the failure to a path that issues no new
miniport request during that call. The underlying cause is still open.
